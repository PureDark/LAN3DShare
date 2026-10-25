[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$BinaryDir,
    [Parameter(Mandatory=$true)][string]$OutputDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$binaryRoot = (Resolve-Path -LiteralPath $BinaryDir).Path
$outputRoot = [IO.Path]::GetFullPath($OutputDir)
[IO.Directory]::CreateDirectory($outputRoot) | Out-Null
$owned = [Collections.Generic.List[Diagnostics.Process]]::new()
$identity = [Guid]::NewGuid().ToString('N')
$silenceStopName = 'Local\LAN3DShare.AudioSystem.Source.' + $identity
$senderStopName = 'Local\LAN3DShare.AudioSystem.Sender.' + $identity
$receiverStopName = 'Local\LAN3DShare.AudioSystem.Receiver.' + $identity
$silenceStop = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $silenceStopName)
$senderStop = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $senderStopName)
$receiverStop = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $receiverStopName)

function Read-TestLog([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { return '' }
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8, $true)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}
function Start-TestProcess([string]$Name, [string]$Executable, [string[]]$Arguments) {
    $stdout = Join-Path $outputRoot ($Name + '.stdout.log')
    $stderr = Join-Path $outputRoot ($Name + '.stderr.log')
    $process = Start-Process -FilePath (Join-Path $binaryRoot $Executable) -WorkingDirectory $binaryRoot -ArgumentList $Arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    [void]$process.Handle
    $owned.Add($process)
    return [pscustomobject]@{ Process=$process; Stdout=$stdout; Stderr=$stderr }
}
function Wait-TestReady($Child, [string]$Pattern, [int]$TimeoutMs) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $TimeoutMs) {
        $text = Read-TestLog $Child.Stdout
        if ($text -match $Pattern) { return $Matches[1] }
        if ($Child.Process.HasExited) { throw "Child $($Child.Process.Id) exited before ready: $text $(Read-TestLog $Child.Stderr)" }
        Start-Sleep -Milliseconds 20
    }
    throw "Child $($Child.Process.Id) readiness timed out: $(Read-TestLog $Child.Stdout) $(Read-TestLog $Child.Stderr)"
}
function Wait-TestExit($Child, [int]$TimeoutMs) {
    if (-not $Child.Process.WaitForExit($TimeoutMs)) { throw "Child $($Child.Process.Id) did not stop: $(Read-TestLog $Child.Stdout) $(Read-TestLog $Child.Stderr)" }
    $Child.Process.WaitForExit(); $Child.Process.Refresh()
    if ($Child.Process.ExitCode -ne 0) { throw "Child $($Child.Process.Id) failed with $($Child.Process.ExitCode): $(Read-TestLog $Child.Stdout) $(Read-TestLog $Child.Stderr)" }
}
try {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $silence = Start-TestProcess 'silence-source' 'AudioSilenceSource.exe' @('--seconds', '15', '--stop-event', $silenceStopName)
    [void](Wait-TestReady $silence 'SILENCE_READY sample_rate=(48000)' 4000)
    # Use actual desktop/system capture, not --test-pattern or --test-audio.
    $sender = Start-TestProcess 'sender' 'LAN3DSender.exe' @('--desktop-monitor', '0', '--width', '640', '--height', '360', '--fps', '30', '--bitrate', '12000000', '--codec', 'software', '--listen', '127.0.0.1', '--port', '0', '--seconds', '12', '--stop-event', $senderStopName)
    $senderPort = Wait-TestReady $sender 'Listening on 127\.0\.0\.1:(\d+);' 4000
    $receiver = Start-TestProcess 'receiver' 'LAN3DReceiver.exe' @('--connect', '127.0.0.1', '--port', "$senderPort", '--preview', '--hidden', '--volume-percent', '600', '--receive-frames', '20', '--timeout', '5', '--stop-event', $receiverStopName)
    Wait-TestExit $receiver 10000
    [void]$senderStop.Set()
    Wait-TestExit $sender 2000
    [void]$silenceStop.Set()
    Wait-TestExit $silence 2000
    $watch.Stop()
    $sent = (Read-TestLog $sender.Stdout) + "`n" + (Read-TestLog $sender.Stderr)
    $received = (Read-TestLog $receiver.Stdout) + "`n" + (Read-TestLog $receiver.Stderr)
    $source = (Read-TestLog $silence.Stdout) + "`n" + (Read-TestLog $silence.Stderr)
    $sourceStats = [regex]::Match($source, 'SILENCE_STOPPED submitted_frames=(\d+) max_padding_frames=(\d+) samples=zero')
    $capture = [regex]::Match($sent, 'AUDIO_CAPTURE stopped captured_frames=(\d+) discarded_frames=(\d+) timestamp_errors=(\d+)')
    $transmitted = [regex]::Match($sent, 'AUDIO_SENT packets=(\d+) sample_frames=(\d+) format=48000/2/s16 source=system-loopback')
    $receivedAudio = [regex]::Match($received, 'AUDIO_RECEIVED packets=(\d+) sample_frames=(\d+) resets=(\d+)')
    $playback = [regex]::Match($received, 'AUDIO_SYNC generation=\d+ submitted_frames=(\d+) accepted_frames=(\d+)')
    $video = [regex]::Match($received, 'Receiver stopped: decoded_unique=(\d+) last_sequence=(\d+) uploaded=(\d+) presented=(\d+)')
    if (-not $sourceStats.Success -or [uint64]$sourceStats.Groups[1].Value -eq 0) { throw "Silence source did not submit to the real endpoint: $source" }
    if (-not $capture.Success -or [uint64]$capture.Groups[1].Value -eq 0) { throw "Production sender did not capture system-loopback audio: $sent" }
    if ($received -notmatch 'AUDIO_SYNC [^\r\n]* volume_percent=600') { throw 'Receiver volume was not configured' }
    if (-not $transmitted.Success -or [uint64]$transmitted.Groups[1].Value -eq 0 -or [uint64]$transmitted.Groups[2].Value -eq 0) { throw "Production sender did not extract/send actual captured PCM: $sent" }
    if (-not $receivedAudio.Success -or [uint64]$receivedAudio.Groups[1].Value -eq 0 -or [uint64]$receivedAudio.Groups[2].Value -eq 0) { throw "Production receiver did not receive captured PCM: $received" }
    if (-not $playback.Success -or [uint64]$playback.Groups[1].Value -ne 0 -or [uint64]$playback.Groups[2].Value -eq 0) { throw "Hidden receiver must enqueue real PCM but submit no audible playback: $received" }
    if (-not $video.Success -or [uint64]$video.Groups[1].Value -lt 20) { throw "Production desktop stream did not reach its decoded-frame target: $received" }
    if ([uint64]$receivedAudio.Groups[2].Value -gt [uint64]$transmitted.Groups[2].Value -or [uint64]$transmitted.Groups[2].Value -gt [uint64]$capture.Groups[1].Value) { throw 'Captured/sent/received sample counts violate the real-loopback data flow' }
    $result = [pscustomobject]@{
        Result='PASS'; DurationMs=$watch.ElapsedMilliseconds; SenderPort=[int]$senderPort;
        SilenceSourcePid=$silence.Process.Id; SenderPid=$sender.Process.Id; ReceiverPid=$receiver.Process.Id;
        SilenceSubmittedFrames=[uint64]$sourceStats.Groups[1].Value; CapturedFrames=[uint64]$capture.Groups[1].Value;
        SentPackets=[uint64]$transmitted.Groups[1].Value; SentSampleFrames=[uint64]$transmitted.Groups[2].Value;
        ReceivedPackets=[uint64]$receivedAudio.Groups[1].Value; ReceivedSampleFrames=[uint64]$receivedAudio.Groups[2].Value;
        HiddenReceiverAcceptedFrames=[uint64]$playback.Groups[2].Value; HiddenReceiverSubmittedFrames=[uint64]$playback.Groups[1].Value;
        DecodedUnique=[uint64]$video.Groups[1].Value; TimestampErrors=[uint64]$capture.Groups[3].Value;
        ReceiverVolumePercent=600;
        Scope='Synthetic silence activates the real default WASAPI render endpoint; production desktop sender captures original system-loopback PCM, extracts against video timestamps, sends v5 TCP, and hidden receiver configures 600% local volume and enqueues without playback; other live system sound may be captured; no audio files, acoustic or remote-LAN validation'
    }
    [IO.File]::WriteAllText((Join-Path $outputRoot 'result.json'), ($result | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
    Write-Output "AudioSystemEndToEnd PASS: actual system capture $($result.CapturedFrames), sent $($result.SentSampleFrames), received $($result.ReceivedSampleFrames) sample frames; hidden playback=0; desktop decoded=$($result.DecodedUnique); $($result.DurationMs)ms."
} finally {
    [void]$receiverStop.Set(); [void]$senderStop.Set(); [void]$silenceStop.Set()
    foreach ($process in $owned) {
        if (-not $process.HasExited) {
            if (-not $process.WaitForExit(1000)) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue; [void]$process.WaitForExit(2000) }
        }
        $process.Dispose()
    }
    $receiverStop.Dispose(); $senderStop.Dispose(); $silenceStop.Dispose()
}
