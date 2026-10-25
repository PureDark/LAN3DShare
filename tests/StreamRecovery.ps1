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
$senderStopName = 'Local\LAN3DShare.StreamRecovery.Sender.' + $identity
$receiverStopName = 'Local\LAN3DShare.StreamRecovery.Receiver.' + $identity
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
        Start-Sleep -Milliseconds 25
    }
    throw "Child $($Child.Process.Id) readiness timed out: $(Read-TestLog $Child.Stdout) $(Read-TestLog $Child.Stderr)"
}
function Wait-TestExit($Child, [int]$TimeoutMs) {
    if (-not $Child.Process.WaitForExit($TimeoutMs)) { throw "Child $($Child.Process.Id) did not stop: $(Read-TestLog $Child.Stdout) $(Read-TestLog $Child.Stderr)" }
    $Child.Process.WaitForExit(); $Child.Process.Refresh()
    if ($Child.Process.ExitCode -ne 0) { throw "Child $($Child.Process.Id) failed with $($Child.Process.ExitCode): $(Read-TestLog $Child.Stdout) $(Read-TestLog $Child.Stderr)" }
}
try {
    # Both media workers are started exactly once. Only their media sockets change.
    $sender = Start-TestProcess 'sender' 'LAN3DSender.exe' @('--test-pattern', '--test-audio', '--listen', '127.0.0.1', '--port', '0', '--source-width', '640', '--source-height', '360', '--width', '640', '--height', '360', '--fps', '20', '--bitrate', '12000000', '--codec', 'software', '--seconds', '20', '--session-token', $identity, '--recover-stream', '--stop-event', $senderStopName)
    $senderPort = Wait-TestReady $sender 'Listening on 127\.0\.0\.1:(\d+);' 5000
    $proxy = Start-TestProcess 'proxy' 'StreamRecoveryProxy.exe' @('--upstream-port', "$senderPort", '--stall-ms', '700', '--before-fault', '8', '--timeout-ms', '18000')
    $proxyPort = Wait-TestReady $proxy 'PROXY_READY port=(\d+)' 3000
    $receiver = Start-TestProcess 'receiver' 'LAN3DReceiver.exe' @('--connect', '127.0.0.1', '--port', "$proxyPort", '--preview', '--hidden', '--receive-frames', '40', '--timeout', '5', '--session-token', $identity, '--recover-stream', '--stop-event', $receiverStopName)
    Wait-TestExit $receiver 15000
    $stopWatch = [Diagnostics.Stopwatch]::StartNew()
    [void]$senderStop.Set()
    Wait-TestExit $sender 4000
    Wait-TestExit $proxy 4000
    $stopWatch.Stop()
    $received = (Read-TestLog $receiver.Stdout) + "`n" + (Read-TestLog $receiver.Stderr)
    $forwarded = Read-TestLog $proxy.Stdout
    if ($received -notmatch 'STREAM_RECOVERY\s') { throw "Receiver did not enter recovery: $received" }
    $resumed = [regex]::Match($received, 'STREAM_RESUMED attempt=(\d+) keyframe=1 key_sequence=(\d+) decoded_unique=(\d+) last_sequence=(\d+)')
    if (-not $resumed.Success) { throw "Receiver did not prove decoded keyframe recovery: $received" }
    $stats = [regex]::Match($received, 'Receiver stopped: decoded_unique=(\d+) last_sequence=(\d+) uploaded=(\d+) presented=(\d+)')
    if (-not $stats.Success) { throw "Receiver did not print final frame statistics: $received" }
    $decoded = [uint64]$stats.Groups[1].Value
    $audioReceived = [regex]::Match($received, 'AUDIO_RECEIVED packets=(\d+) sample_frames=(\d+) resets=(\d+)')
    if (-not $audioReceived.Success -or [uint64]$audioReceived.Groups[1].Value -ne $decoded -or [uint64]$audioReceived.Groups[2].Value -eq 0 -or [uint64]$audioReceived.Groups[3].Value -lt 2) {
        throw "Receiver did not retain complete paired PCM and reset the audio timeline across recovery: $received"
    }
    $resumedDecoded = [uint64]$resumed.Groups[3].Value
    if ($decoded -lt 40 -or $resumedDecoded -le 8 -or $decoded -le $resumedDecoded) { throw "Decoded unique count did not grow through and after recovery: resume=$resumedDecoded final=$decoded" }
    if ([uint64]$stats.Groups[2].Value -le [uint64]$resumed.Groups[4].Value) { throw 'Source sequence did not advance after resumed keyframe' }
    if ([regex]::Matches($forwarded, 'PROXY_FAULT ').Count -ne 1 -or $forwarded -notmatch 'PROXY_DISCARDED connection=1 backlog=closed') { throw "Proxy did not discard exactly one partial packet: $forwarded" }
    if ($forwarded -notmatch 'PROXY_FAULT connection=1 completed_packets=8 .*stall_ms=700') { throw "Proxy did not preserve the initial complete frames and requested stall: $forwarded" }
    if ($forwarded -notmatch 'PROXY_FAULT .*partial_audio=1 audio_frames=\d+') { throw 'Fault must truncate audio after the complete video, to check atomic A/V delivery' }
    $audioBefore = [regex]::Match($forwarded, 'PROXY_AUDIO connection=1 packets=(\d+) sample_frames=(\d+)')
    $audioAfter = [regex]::Match($forwarded, 'PROXY_AUDIO connection=2 packets=(\d+) sample_frames=(\d+)')
    if (-not $audioBefore.Success -or -not $audioAfter.Success -or [int]$audioBefore.Groups[1].Value -ne 8 -or [int]$audioAfter.Groups[1].Value -lt 8) { throw "PCM did not travel in both media generations: $forwarded" }
    if ($forwarded -notmatch 'PROXY_FIRST_PACKET connection=2 keyframe=1 sequence=(\d+)') { throw "Proxy did not see a keyframe on the fresh connection: $forwarded" }
    $wireKeySequence = [uint64]$Matches[1]
    if ($wireKeySequence -ne [uint64]$resumed.Groups[2].Value) { throw 'Receiver resume keyframe does not match the proxy wire evidence' }
    if ($forwarded -notmatch 'PROXY_FINISHED connections=2 faults=1 resumed_packets=(\d+)') { throw "Proxy did not finish the same two-connection run: $forwarded" }
    $result = [pscustomobject]@{
        Result='PASS'; SenderPid=$sender.Process.Id; ReceiverPid=$receiver.Process.Id; ProxyPid=$proxy.Process.Id;
        SenderPort=[int]$senderPort; ProxyPort=[int]$proxyPort; FaultCount=1; CompletePacketsBeforeFault=8; PartialPacketStallMs=700;
        ResumeAttempt=[int]$resumed.Groups[1].Value; ResumeKeySequence=$wireKeySequence; UniqueAtResume=$resumedDecoded; UniqueAtExit=$decoded;
        LastSequence=[uint64]$stats.Groups[2].Value; Presented=[uint64]$stats.Groups[4].Value; GracefulStopMs=$stopWatch.ElapsedMilliseconds;
        AudioPacketsBeforeFault=[int]$audioBefore.Groups[1].Value; AudioPacketsAfterFault=[int]$audioAfter.Groups[1].Value;
        AudioFramesBeforeFault=[uint64]$audioBefore.Groups[2].Value; AudioFramesAfterFault=[uint64]$audioAfter.Groups[2].Value;
        ReceivedAudioPackets=[uint64]$audioReceived.Groups[1].Value; ReceivedAudioFrames=[uint64]$audioReceived.Groups[2].Value; AudioTimelineResets=[uint64]$audioReceived.Groups[3].Value;
        Scope='Same production sender and receiver processes; real token/capability handshake; one truncated PCM payload after complete video; silent synthetic PCM; preview-only, no Leia runtime'
    }
    [IO.File]::WriteAllText((Join-Path $outputRoot 'result.json'), ($result | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
    Write-Output "StreamRecovery PASS: one partial packet stalled 700ms; same receiver resumed from keyframe $wireKeySequence; unique $resumedDecoded -> $decoded; graceful stop $($stopWatch.ElapsedMilliseconds)ms."
} finally {
    [void]$senderStop.Set(); [void]$receiverStop.Set()
    foreach ($process in $owned) {
        if (-not $process.HasExited) {
            if (-not $process.WaitForExit(1500)) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue; [void]$process.WaitForExit(3000) }
        }
        $process.Dispose()
    }
    $senderStop.Dispose(); $receiverStop.Dispose()
}
