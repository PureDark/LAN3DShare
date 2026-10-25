param(
    [Parameter(Mandatory=$true)][string]$BinaryDir,
    [Parameter(Mandatory=$true)][string]$OutputDir
)
$ErrorActionPreference = 'Stop'
$BinaryDir = (Resolve-Path -LiteralPath $BinaryDir).Path
[IO.Directory]::CreateDirectory($OutputDir) | Out-Null
$OutputDir = (Resolve-Path -LiteralPath $OutputDir).Path
$owned = [Collections.Generic.List[Diagnostics.Process]]::new()
$utf8 = [Text.UTF8Encoding]::new($false)
function Read-TestLog([string]$path) {
    if (-not [IO.File]::Exists($path)) { return '' }
    $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8, $true)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}
function Start-TestProcess([string]$name, [string]$executable, [string[]]$arguments) {
    $stdout = Join-Path $OutputDir ($name + '.stdout.log')
    $stderr = Join-Path $OutputDir ($name + '.stderr.log')
    $process = Start-Process -FilePath (Join-Path $BinaryDir $executable) -ArgumentList $arguments -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    [void]$process.Handle # Retain the PS 5 process handle before exit.
    $owned.Add($process)
    return @{ Process=$process; Stdout=$stdout; Stderr=$stderr }
}
function Unused-Port {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    try { return $listener.LocalEndpoint.Port } finally { $listener.Stop() }
}
try {
    $results = @()
    foreach ($case in @('normal-eof', 'insufficient-frames')) {
        $port = Unused-Port
        $sender = Start-TestProcess ($case + '-sender') 'LAN3DSender.exe' @('--test-pattern', '--listen', '127.0.0.1', '--port', "$port", '--seconds', '2', '--fps', '20', '--source-width', '640', '--source-height', '360')
        $deadline = [DateTime]::UtcNow.AddSeconds(6)
        while ((Read-TestLog $sender.Stdout) -notmatch 'Listening on ') {
            if ($sender.Process.HasExited) { throw "Sender failed before readiness: $(Read-TestLog $sender.Stderr)" }
            if ([DateTime]::UtcNow -gt $deadline) { throw 'Sender readiness timed out' }
            Start-Sleep -Milliseconds 25
        }
        $arguments = @('--connect', '127.0.0.1', '--port', "$port", '--preview', '--hidden', '--timeout', '5')
        if ($case -eq 'insufficient-frames') { $arguments += @('--receive-frames', '100000') }
        $receiver = Start-TestProcess ($case + '-receiver') 'LAN3DReceiver.exe' $arguments
        if (-not $receiver.Process.WaitForExit(10000)) { throw "Receiver did not exit after EOF: $case" }
        if (-not $sender.Process.WaitForExit(5000)) { throw "Sender did not finish: $case" }
        $receiver.Process.Refresh(); $sender.Process.Refresh()
        $output = Read-TestLog $receiver.Stdout
        $errors = Read-TestLog $receiver.Stderr
        if ($sender.Process.ExitCode -ne 0) { throw "Sender failed: $(Read-TestLog $sender.Stderr)" }
        if ($case -eq 'normal-eof') {
            if ($receiver.Process.ExitCode -ne 0) { throw "Clean EOF was reported as a failure: $errors" }
            if ($output -notmatch 'Receiver stopped: decoded_unique=(\d+) last_sequence=(\d+) uploaded=(\d+) presented=(\d+)') { throw 'Clean EOF did not print receiver statistics' }
            $decoded = [uint64]$Matches[1]; $presented = [uint64]$Matches[4]
            if ($decoded -eq 0 -or $presented -eq 0) { throw 'Clean EOF test produced no decoded/presented frames' }
        } else {
            if ($receiver.Process.ExitCode -ne 1) { throw 'Insufficient receive-frames incorrectly succeeded' }
            if ($errors -notmatch 'Sender ended before --receive-frames target: decoded (\d+) of 100000') { throw "Unexpected insufficient-frame error: $errors" }
            $decoded = [uint64]$Matches[1]; $presented = $null
            if ($decoded -eq 0) { throw 'Insufficient-frame test did not receive any complete frame' }
        }
        $results += [pscustomobject]@{ Case=$case; ReceiverExitCode=$receiver.Process.ExitCode; Decoded=$decoded; Presented=$presented }
    }
    [IO.File]::WriteAllText((Join-Path $OutputDir 'result.json'), ($results | ConvertTo-Json), $utf8)
    Write-Output 'Receiver disconnect PASS: clean EOF prints statistics; insufficient receive-frames fails'
} finally {
    foreach ($process in $owned) {
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue; $process.WaitForExit(3000) | Out-Null }
        $process.Dispose()
    }
}
