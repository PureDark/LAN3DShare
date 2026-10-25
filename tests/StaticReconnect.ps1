[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BinaryDir,
    [Parameter(Mandatory = $true)][string]$OutputDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$binaryRoot = (Resolve-Path -LiteralPath $BinaryDir).Path
$outputRoot = [IO.Path]::GetFullPath($OutputDir)
[IO.Directory]::CreateDirectory($outputRoot) | Out-Null
$children = [Collections.Generic.List[Diagnostics.Process]]::new()
$portReservation = $null

function Read-TestLog([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $reader = $null
    try {
        $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8, $true)
        return $reader.ReadToEnd()
    }
    finally {
        if ($null -ne $reader) { $reader.Dispose() }
        $stream.Dispose()
    }
}

function Start-TestProcess([string]$Name, [string[]]$Arguments, [string]$LogPrefix) {
    $executable = Join-Path $binaryRoot $Name
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Missing test executable: $executable" }
    $stdout = Join-Path $outputRoot ($LogPrefix + '.stdout.log')
    $stderr = Join-Path $outputRoot ($LogPrefix + '.stderr.log')
    $parameters = @{
        FilePath = $executable
        WorkingDirectory = $binaryRoot
        WindowStyle = 'Hidden'
        PassThru = $true
        RedirectStandardOutput = $stdout
        RedirectStandardError = $stderr
    }
    if ($null -ne $Arguments -and $Arguments.Count) { $parameters.ArgumentList = $Arguments }
    $process = Start-Process @parameters
    $children.Add($process)
    # Windows PowerShell 5 must retain the process handle before the child exits
    # or its later ExitCode property can be null despite successful completion.
    $null = $process.Handle
    return [PSCustomObject]@{ Process = $process; Stdout = $stdout; Stderr = $stderr }
}

function Wait-TestReady($Child, [string]$Pattern, [int]$TimeoutMs) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $TimeoutMs) {
        if ((Read-TestLog $Child.Stdout) -match $Pattern) { return }
        if ($Child.Process.HasExited) {
            throw "Process $($Child.Process.Id) exited before readiness: $(Read-TestLog $Child.Stderr) $(Read-TestLog $Child.Stdout)"
        }
        Start-Sleep -Milliseconds 40
    }
    throw "Readiness timeout for process $($Child.Process.Id): $(Read-TestLog $Child.Stderr) $(Read-TestLog $Child.Stdout)"
}

function Wait-TestExit($Child, [int]$TimeoutMs) {
    if (-not $Child.Process.WaitForExit($TimeoutMs)) {
        throw "Process $($Child.Process.Id) timed out: $(Read-TestLog $Child.Stderr) $(Read-TestLog $Child.Stdout)"
    }
    $Child.Process.WaitForExit()
    $Child.Process.Refresh()
    if ($Child.Process.ExitCode -ne 0) {
        throw "Process $($Child.Process.Id) failed with $($Child.Process.ExitCode): $(Read-TestLog $Child.Stderr) $(Read-TestLog $Child.Stdout)"
    }
}

try {
    # Reserve an OS-selected free loopback port, then release it immediately
    # before starting our server. Readiness uses logs, not a probe connection.
    $portReservation = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $portReservation.Start()
    $port = $portReservation.LocalEndpoint.Port
    $portReservation.Stop()
    $portReservation = $null

    $source = Start-TestProcess 'StaticSource.exe' @() 'source'
    Wait-TestReady $source 'StaticSource READY .*published=1 sequence=1 width=640 height=360' 3000

    $senderArguments = @('--no-inject', '--pid', $source.Process.Id.ToString(), '--listen', '127.0.0.1',
        '--port', $port.ToString(), '--seconds', '10')
    $sender = Start-TestProcess 'LAN3DSender.exe' $senderArguments 'sender'
    Wait-TestReady $sender ('Listening on 127\.0\.0\.1:' + $port + ';') 3000

    $results = @()
    foreach ($connection in 1..2) {
        $receiverArguments = @('--connect', '127.0.0.1', '--port', $port.ToString(),
            '--preview', '--hidden', '--frames', '5', '--timeout', '4')
        $receiver = Start-TestProcess 'LAN3DReceiver.exe' $receiverArguments ('receiver-' + $connection)
        Wait-TestExit $receiver 5000
        $text = Read-TestLog $receiver.Stdout
        $match = [regex]::Match($text,
            'Receiver stopped: decoded_unique=(\d+) last_sequence=(\d+) uploaded=(\d+) presented=(\d+)')
        if (-not $match.Success -or [uint64]$match.Groups[1].Value -ne 1 -or
            [uint64]$match.Groups[2].Value -ne 1 -or [uint64]$match.Groups[3].Value -lt 1 -or
            [uint64]$match.Groups[4].Value -ne 5) {
            throw "Receiver $connection did not display the single static source frame: $text"
        }
        $results += [PSCustomObject]@{
            Connection = $connection
            DecodedUnique = [uint64]$match.Groups[1].Value
            LastSequence = [uint64]$match.Groups[2].Value
            Uploaded = [uint64]$match.Groups[3].Value
            Presented = [uint64]$match.Groups[4].Value
        }
    }

    Wait-TestExit $sender 11000
    $senderText = Read-TestLog $sender.Stdout
    if ([regex]::Matches($senderText, 'Receiver connected').Count -ne 2) {
        throw "Expected exactly two accepted receiver connections: $senderText"
    }
    $sent = [regex]::Match($senderText, 'Stopped\. Sent (\d+) frames\.')
    if (-not $sent.Success -or [uint64]$sent.Groups[1].Value -lt 2) {
        throw "Sender did not send the paused image to both connections: $senderText"
    }
    [PSCustomObject]@{
        Result = 'PASS'
        SourcePublished = 1
        SourceSequence = 1
        SenderAccepted = 2
        SenderSent = [uint64]$sent.Groups[1].Value
        Receivers = $results
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $outputRoot 'result.json') -Encoding utf8
    Write-Output 'StaticReconnect PASS: one source frame, two sequential clients, each decoded_unique=1 last_sequence=1 presented=5.'
    Write-Output 'The paused-source refresh detects the first disconnect; the second client receives the existing source frame without a producer update.'
}
finally {
    if ($null -ne $portReservation) { $portReservation.Stop() }
    # Only processes started by this test are eligible for termination. Do not
    # search by executable name or touch any pre-existing user application.
    for ($index = $children.Count - 1; $index -ge 0; --$index) {
        $child = $children[$index]
        try {
            if (-not $child.HasExited) { $child.Kill(); $child.WaitForExit(2000) | Out-Null }
        }
        catch { Write-Warning "Could not terminate owned test process $($child.Id): $_" }
        finally { $child.Dispose() }
    }
}
