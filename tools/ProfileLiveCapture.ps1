[CmdletBinding()]
param(
    [int]$SourcePid = 38040,
    [string]$BinaryDir = 'build/package',
    [string]$OutputDir = 'build/test-results/perf-baseline',
    [ValidateRange(1, 3600)][int]$Seconds = 20,
    [switch]$Gpu,
    [switch]$WireSink,
    [string]$SinkPath = 'build-gpu-pipeline/package/GpuPipelineProducer.exe',
    [string]$CaptureDllPath,
    [ValidateRange(0, 240)][int]$CaptureFps = 0,
    [ValidateRange(1, 120)][int]$SinkSeconds = 10,
    [switch]$WithAudio
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$BinaryDir = (Resolve-Path -LiteralPath $BinaryDir).Path
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
$senderPath = Join-Path $BinaryDir 'LAN3DSender.exe'
$capturePath = if ($CaptureDllPath) { (Resolve-Path -LiteralPath $CaptureDllPath).Path } else { Join-Path $BinaryDir 'LAN3DCapture.dll' }
$receiverPath = if ($WireSink) { (Resolve-Path -LiteralPath $SinkPath).Path } else { Join-Path $BinaryDir 'LAN3DReceiver.exe' }
$null = New-Item -ItemType Directory -Force -Path $OutputDir
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
if (-not ('Lan3dProfileDpi' -as [type])) {
    Add-Type 'public static class Lan3dProfileDpi { [System.Runtime.InteropServices.DllImport("user32.dll")] public static extern System.IntPtr SetThreadDpiAwarenessContext(System.IntPtr value); }'
}

function Artifact([string]$Path) {
    [ordered]@{ Path=[IO.Path]::GetFullPath($Path); Sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
}
function Read-Log([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { return '' }
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8, $true)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}
$screenshots = [Collections.Generic.List[object]]::new()
function Save-Screens([string]$Name) {
    $oldDpi = [Lan3dProfileDpi]::SetThreadDpiAwarenessContext([IntPtr](-4))
    try {
    $virtual = [Windows.Forms.SystemInformation]::VirtualScreen
    foreach ($shot in @(
        @{ Path=(Join-Path $OutputDir ($Name + '.png')); X=3420; Y=0; Width=420; Height=100; Kind='fps-crop' },
        @{ Path=(Join-Path $OutputDir ($Name + '-desktop.png')); X=$virtual.X; Y=$virtual.Y; Width=$virtual.Width; Height=$virtual.Height; Kind='virtual-desktop' }
    )) {
        $bitmap = [Drawing.Bitmap]::new($shot.Width, $shot.Height)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CopyFromScreen($shot.X, $shot.Y, 0, 0, $bitmap.Size)
            $bitmap.Save($shot.Path, [Drawing.Imaging.ImageFormat]::Png)
            $screenshots.Add([ordered]@{ Stage=$Name; Kind=$shot.Kind; Path=$shot.Path; X=$shot.X; Y=$shot.Y; Width=$shot.Width; Height=$shot.Height; Taken=[DateTime]::Now.ToString('o') })
        } finally { $graphics.Dispose(); $bitmap.Dispose() }
    }
    } finally { if ($oldDpi -ne [IntPtr]::Zero) { $null = [Lan3dProfileDpi]::SetThreadDpiAwarenessContext($oldDpi) } }
}
$owned = [Collections.Generic.List[object]]::new()
function Start-Helper([string]$Name, [string]$Executable, [string[]]$Arguments) {
    $stdout = Join-Path $OutputDir ($Name + '.log')
    $stderr = Join-Path $OutputDir ($Name + '.err')
    $process = Start-Process -FilePath $Executable -ArgumentList $Arguments -WorkingDirectory $BinaryDir -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $null = $process.Handle
    $child = [pscustomobject]@{ Name=$Name; Process=$process; Pid=$process.Id; Started=$process.StartTime; Executable=$Executable; Arguments=$Arguments; Stdout=$stdout; Stderr=$stderr; ExitCode=$null; ForcedStop=$false }
    $owned.Add($child)
    return $child
}
function Observe-Exit($Child) {
    $Child.Process.Refresh()
    if (-not $Child.Process.HasExited) { return $false }
    $Child.Process.WaitForExit()
    $Child.ExitCode = $Child.Process.ExitCode
    return $true
}
$game = Get-Process -Id $SourcePid
$gameStart = $game.StartTime
$gamePath = $game.Path
$artifacts = [ordered]@{ Sender=(Artifact $senderPath); Capture=(Artifact $capturePath); Receiver=(Artifact $receiverPath) }
if ($gamePath) { $artifacts.Source = Artifact $gamePath }
function Same-Source {
    try { return (Get-Process -Id $SourcePid -ErrorAction Stop).StartTime -eq $gameStart } catch { return $false }
}
$sender = $null
$receiver = $null
$failure = ''
$captureStopped = $false
$stopMethod = 'not-started'
$sourceIdentityPreserved = $true
$started = [DateTime]::Now
try {
    Save-Screens 'before'
    $senderArgs = @('--pid', $SourcePid, '--listen', '127.0.0.1', '--port', '48746', '--width', '3840', '--height', '2160', '--fps', '60', '--bitrate', '200000000', '--codec', 'auto', '--no-audio', '--profile', '--seconds', $Seconds)
    if ($WithAudio) { $senderArgs = $senderArgs | Where-Object { $_ -ne '--no-audio' } }
    $senderArgs += @('--capture-dll', ('"' + $capturePath + '"'))
    if ($CaptureFps) { $senderArgs += @('--capture-fps', $CaptureFps) }
    if (-not $Gpu) { $senderArgs += '--capture-cpu' }
    # Baseline binaries before the GPU switch was added do not know this option.
    if (-not $Gpu -and $OutputDir -like '*perf-baseline*') { $senderArgs = $senderArgs | Where-Object { $_ -ne '--capture-cpu' } }
    $sender = Start-Helper 'sender' $senderPath $senderArgs
    $deadline = [DateTime]::UtcNow.AddSeconds(12)
    $listening = $false
    do {
        if (Observe-Exit $sender) { throw "Sender exited before listening: $(Read-Log $sender.Stderr)" }
        if ((Read-Log $sender.Stdout) -match 'Listening on') { $listening = $true; break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if (-not $listening) { throw 'Sender did not become ready within 12 seconds' }
    if ($WireSink) {
        $sinkArgs = @('--verify-port', '48746', '--wire-sink')
        if ($SinkSeconds -ne 10) { $sinkArgs += @('--measure-seconds', $SinkSeconds) }
        $receiver = Start-Helper 'wire-sink' $receiverPath $sinkArgs
    } else {
        $receiver = Start-Helper 'receiver' $receiverPath @('--connect', '127.0.0.1', '--port', '48746', '--preview', '--hidden', '--receive-frames', '100000', '--timeout', ($Seconds + 8))
    }
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $during1 = $false; $during2 = $false
    # The sink completes its fixed sample interval before the sender's bounded
    # session. Always wait for the sender as well so its capture stop is observed.
    while (-not (Observe-Exit $sender)) {
        if (-not $during1 -and $watch.Elapsed.TotalSeconds -ge 7) { Save-Screens 'during-1'; $during1 = $true }
        if (-not $during2 -and $watch.Elapsed.TotalSeconds -ge 12) { Save-Screens 'during-2'; $during2 = $true }
        if ($watch.Elapsed.TotalSeconds -gt $Seconds + 10) { throw 'Sender did not finish its bounded interval' }
        Start-Sleep -Milliseconds 100
    }
    if ($sender.ExitCode) { throw "Sender exit $($sender.ExitCode): $(Read-Log $sender.Stderr)" }
    if ($WireSink) {
        if (-not $receiver.Process.WaitForExit(6000)) { throw 'Wire sink did not finish after the sender ended' }
        $null = Observe-Exit $receiver
        if ($receiver.ExitCode) { throw "Wire sink exit $($receiver.ExitCode): $(Read-Log $receiver.Stderr)" }
    }
} catch {
    $failure = $_.Exception.Message
} finally {
    if ($sender) {
        $captureStopped = (Read-Log $sender.Stdout) -match ("Capture stopped in PID " + $SourcePid + '(?:;|\s|$)')
        if ($captureStopped) { $stopMethod = 'sender-confirmed' }
        else {
            try {
                if (-not (Same-Source)) { throw 'Source process identity changed; no stop command was sent to a different process' }
                if ((Get-FileHash -LiteralPath $senderPath).Hash -ne $artifacts.Sender.Sha256 -or
                    (Get-FileHash -LiteralPath $capturePath).Hash -ne $artifacts.Capture.Sha256) {
                    throw 'Sender/capture files changed during profiling; same-version stop cannot be confirmed'
                }
                $stopper = Start-Helper 'capture-stop' $senderPath @('--pid', $SourcePid, '--stop', '--capture-dll', ('"' + $capturePath + '"'))
                if (-not $stopper.Process.WaitForExit(10000)) { throw 'Capture stop helper timed out' }
                $null = Observe-Exit $stopper
                $captureStopped = $stopper.ExitCode -eq 0 -and (Read-Log $stopper.Stdout) -match 'Capture stopped;'
                $stopMethod = 'same-version-stop-helper'
                if (-not $captureStopped) { throw "Capture stop not confirmed: $(Read-Log $stopper.Stderr)" }
            } catch {
                $stopMethod = 'unconfirmed'
                $failure = ($failure + '; ' + $_.Exception.Message).Trim('; ')
            }
        }
    }
    # Only helpers started above are candidates for termination. The game is
    # inspected for identity, but is never stopped, restarted or sent UI input.
    foreach ($child in $owned) {
        try {
            if (-not (Observe-Exit $child)) {
                if ($child.Process.StartTime -ne $child.Started) { throw "Helper PID $($child.Pid) was reused" }
                Stop-Process -InputObject $child.Process -Force
                $child.ForcedStop = $true
                if ($child.Process.WaitForExit(5000)) { $null = Observe-Exit $child }
            }
        } catch { $failure = ($failure + '; helper cleanup: ' + $_.Exception.Message).Trim('; ') }
    }
    $sourceIdentityPreserved = Same-Source
    if (-not $sourceIdentityPreserved) { $failure = ($failure + '; Game identity changed').Trim('; ') }
    try {
        Start-Sleep -Seconds 3
        Save-Screens 'after'
    } catch { $failure = ($failure + '; after screenshot: ' + $_.Exception.Message).Trim('; ') }
    $helperResults = @($owned | ForEach-Object {
        [ordered]@{ Name=$_.Name; Pid=$_.Pid; Started=$_.Started.ToString('o'); Executable=$_.Executable; Arguments=$_.Arguments; ExitCode=$_.ExitCode; ForcedStop=$_.ForcedStop; Stdout=$_.Stdout; Stderr=$_.Stderr }
    })
    [ordered]@{
        Result=$(if ($failure) { 'FAIL' } else { 'PASS' }); Failure=$failure
        SourcePid=$SourcePid; SourceStart=$gameStart.ToString('o'); SourcePath=$gamePath; SourceIdentityPreserved=$sourceIdentityPreserved
        Mode=$(if ($WireSink) { 'WireSink' } else { 'Loopback' }); CaptureMode=$(if ($Gpu) { 'GPU' } else { 'CPU' }); Seconds=$Seconds
        CaptureFps=$CaptureFps; SinkSeconds=$SinkSeconds; NvencSplitDiagnostic=$env:LAN3D_NVENC_SPLIT
        SystemAudioEnabled=[bool]$WithAudio
        CaptureSha256=$artifacts.Capture.Sha256; Artifacts=$artifacts; Helpers=$helperResults
        CaptureStopped=$captureStopped; StopMethod=$stopMethod; Screenshots=$screenshots.ToArray()
        Scope=$(if ($WireSink) { 'Real game; local TCP wire sink, no decode/render; system audio selection recorded separately; GDI full desktop and FPS crops' } else { 'Real game; loopback receiver on same GPU, hidden preview; system audio selection recorded separately; GDI full desktop and FPS crops' })
        Started=$started.ToString('o'); Completed=[DateTime]::Now.ToString('o')
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDir 'result.json') -Encoding utf8
}
if ($failure) { throw $failure }
