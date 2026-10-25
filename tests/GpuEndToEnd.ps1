param([Parameter(Mandatory)][string]$BinaryDir, [Parameter(Mandatory)][string]$OutputDir, [switch]$WireSink, [switch]$Modes, [string]$EncodeGpu="auto")
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$binaryRoot = (Resolve-Path -LiteralPath $BinaryDir).Path
$outputRoot = [IO.Path]::GetFullPath($OutputDir)
[IO.Directory]::CreateDirectory($outputRoot) | Out-Null
$owned = [Collections.Generic.List[Diagnostics.Process]]::new()
$identity = [Guid]::NewGuid().ToString('N')
$producerEventName = 'Local\LAN3DShare.GpuTest.Producer.' + $identity
$senderEventName = 'Local\LAN3DShare.GpuTest.Sender.' + $identity
$producerStop = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $producerEventName)
$senderStop = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $senderEventName)
function Read-Log([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { return '' }
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8, $true)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}
function Start-Child([string]$Name, [string]$Executable, [string[]]$Arguments) {
    $stdout = Join-Path $outputRoot ($Name + '.stdout.log')
    $stderr = Join-Path $outputRoot ($Name + '.stderr.log')
    $process = Start-Process -FilePath (Join-Path $binaryRoot $Executable) -WorkingDirectory $binaryRoot -ArgumentList $Arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    [void]$process.Handle
    $owned.Add($process)
    return [pscustomobject]@{ Process=$process; Stdout=$stdout; Stderr=$stderr }
}
function Wait-Ready($Child, [string]$Pattern, [int]$TimeoutMs) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $TimeoutMs) {
        $text = Read-Log $Child.Stdout
        if ($text -match $Pattern) { return $Matches[1] }
        if ($Child.Process.HasExited) {
            if ($Child.Process.ExitCode -eq 77) { Write-Output 'SKIP NVIDIA hardware unavailable'; exit 77 }
            throw "Child exited before ready: $text $(Read-Log $Child.Stderr)"
        }
        Start-Sleep -Milliseconds 25
    }
    throw "Child readiness timed out: $(Read-Log $Child.Stdout) $(Read-Log $Child.Stderr)"
}
function Wait-Exit($Child, [int]$TimeoutMs) {
    if (-not $Child.Process.WaitForExit($TimeoutMs)) { throw "Child did not stop: $(Read-Log $Child.Stdout) $(Read-Log $Child.Stderr)" }
    $Child.Process.WaitForExit(); $Child.Process.Refresh()
    if ($Child.Process.ExitCode -ne 0) { throw "Child failed with $($Child.Process.ExitCode): $(Read-Log $Child.Stdout) $(Read-Log $Child.Stderr)" }
}
try {
    $producer = Start-Child 'producer' 'GpuPipelineProducer.exe' @('--stop-event', $producerEventName)
    $producerPid = Wait-Ready $producer 'GPU_PRODUCER_READY pid=(\d+)' 8000
    $sender = Start-Child 'sender' 'LAN3DSender.exe' @('--gpu', $EncodeGpu, '--no-inject', '--pid', "$producerPid", '--listen', '127.0.0.1', '--port', '0',
        '--codec', 'auto', '--width', '3840', '--height', '2160', '--fps', '60', '--bitrate', '200000000', '--no-audio', '--profile',
        '--seconds', '45', '--stop-event', $senderEventName, '--recover-stream')
    $port = Wait-Ready $sender 'Listening on 127\.0\.0\.1:(\d+)' 8000
    if ($Modes) {
        $sink = Start-Child 'mode-sink' 'GpuPipelineProducer.exe' @('--verify-port', "$port", '--verify-modes')
        Wait-Exit $sink 30000
        [void]$senderStop.Set(); Wait-Exit $sender 5000
        [void]$producerStop.Set(); Wait-Exit $producer 5000
        $text = Read-Log $sink.Stdout
        if ($text -notmatch 'GPU_MODE_VERIFIED transitions=5') { throw "Mode verification failed: $text" }
        Write-Output $text
        return
    }
    if ($WireSink) {
        $sink = Start-Child 'wire-sink' 'GpuPipelineProducer.exe' @('--verify-port', "$port", '--wire-sink')
        Wait-Exit $sink 22000
        [void]$senderStop.Set(); Wait-Exit $sender 5000
        [void]$producerStop.Set(); Wait-Exit $producer 5000
        $sinkText = Read-Log $sink.Stdout
        $line = [regex]::Match($sinkText, '(?m)^GPU_WIRE_SINK [^\r\n]+')
        if (-not $line.Success) { throw "Wire sink did not produce its steady measurement: $sinkText" }
        $sentText = Read-Log $sender.Stdout
        if ($sentText -notmatch 'ENCODER NVENC (AV1|HEVC|H\.264) SBS 3840x2160 target=60fps/200Mbps gpu_shared=1') {
            throw "Wire sink sender did not encode GPU shared 4K: $sentText"
        }
        $result = [ordered]@{ Result='PASS'; Scope='Synthetic shared GPU sender and compressed wire sink; 2 s warmup then 10 s; no local decoding/rendering; not game capture or LAN acceptance';
            EncodedWidth=3840; EncodedHeight=2160; DisplayEyeWidth=3840; DisplayEyeHeight=2160; Codec=$Matches[1];
            ObservedSenderProfiles=@([regex]::Matches($sentText, '(?m)^PROFILE [^\r\n]+') | ForEach-Object { $_.Value });
            ProducerSummary=(Read-Log $producer.Stdout).Trim(); RawSinkSummary=$line.Value }
        foreach ($field in @('measured_frames', 'measured_seconds', 'fps', 'missing_source_sequences', 'packet_interval_p50_ms', 'packet_interval_p95_ms', 'frame_age_mean_ms', 'payload_mbps')) {
            $value = [regex]::Match($line.Value, '(?:^| )' + [regex]::Escape($field) + '=([\d.]+)')
            if (-not $value.Success) { throw "Wire sink missing $field" }
            $result[$field] = [double]::Parse($value.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)
        }
        [IO.File]::WriteAllText((Join-Path $outputRoot 'result.json'), ($result | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
        Write-Output ($result | ConvertTo-Json -Depth 4)
        return
    }
    $runs = [Collections.Generic.List[object]]::new()
    $firstSenderText = ''
    foreach ($run in 1, 2) {
        $target = if ($run -eq 1) { 240 } else { 60 }
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $receiver = Start-Child "receiver-$run" 'LAN3DReceiver.exe' @('--connect', '127.0.0.1', '--port', "$port", '--preview', '--hidden',
            '--no-audio', '--receive-frames', "$target", '--timeout', '8')
        Wait-Exit $receiver 22000
        $watch.Stop()
        $text = Read-Log $receiver.Stdout
        $stats = [regex]::Match($text, 'Receiver stopped: decoded_unique=(\d+) last_sequence=(\d+) uploaded=(\d+) presented=(\d+)')
        if (-not $stats.Success -or [int]$stats.Groups[1].Value -lt $target) { throw "Receiver did not decode $target unique frames: $text" }
        if ($text -notmatch 'DECODER NVDEC (AV1|HEVC|H\.264)') { throw "Receiver did not use NVDEC: $text" }
        $receiveRates = @([regex]::Matches($text, '\| receive (\d+) fps') | ForEach-Object { [double]$_.Groups[1].Value })
        $runs.Add([pscustomobject]@{ Run=$run; Decoded=[int]$stats.Groups[1].Value; LastSequence=[uint64]$stats.Groups[2].Value;
            Uploaded=[int]$stats.Groups[3].Value; ElapsedMs=$watch.ElapsedMilliseconds; ReceiveFpsSamples=$receiveRates })
        if ($run -eq 1) { $firstSenderText = Read-Log $sender.Stdout }
    }
    if ($runs[1].LastSequence -le $runs[0].LastSequence) { throw 'Source sequence failed to advance across receiver reconnect' }
    $verifier = Start-Child 'wire-verifier' 'GpuPipelineProducer.exe' @('--verify-port', "$port")
    Wait-Exit $verifier 12000
    $verified = Read-Log $verifier.Stdout
    if ($verified -notmatch 'GPU_WIRE_VERIFIED version=6 encoded=3840x2160 display_eye=3840x2160 keyframe=1 decoded=4') {
        throw "Wire v5 / decoded eye aspect verification failed: $verified"
    }
    $stopWatch = [Diagnostics.Stopwatch]::StartNew()
    [void]$senderStop.Set(); Wait-Exit $sender 5000
    [void]$producerStop.Set(); Wait-Exit $producer 5000
    $stopWatch.Stop()
    $sent = Read-Log $sender.Stdout
    if ([regex]::Matches($sent, 'ENCODER NVENC (AV1|HEVC|H\.264) SBS 3840x2160 target=60fps/200Mbps gpu_shared=1').Count -lt 3) {
        throw "Sender did not use GPU shared 4K NVENC for every connection: $sent"
    }
    if ($sent -match 'FALLBACK|Shared GPU encoding failed|quarantined') { throw "Unexpected fallback or GPU failure: $sent" }
    $fps = @([regex]::Matches($firstSenderText, '(?m)^PROFILE seconds=[\d.]+ sent=\d+ fps=([\d.]+)') | ForEach-Object {
        [double]::Parse($_.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)
    })
    $profileRows = @([regex]::Matches($firstSenderText, '(?m)^PROFILE [^\r\n]+') | ForEach-Object {
        $line = $_.Value
        $dimensions = [regex]::Match($line, 'input=(\d+)x(\d+) output=(\d+)x(\d+)')
        $row = [ordered]@{InputWidth=[int]$dimensions.Groups[1].Value; InputHeight=[int]$dimensions.Groups[2].Value;
            OutputWidth=[int]$dimensions.Groups[3].Value; OutputHeight=[int]$dimensions.Groups[4].Value}
        foreach ($field in @('seconds', 'sent', 'fps', 'read_ms', 'scale_ms', 'encode_ms', 'send_ms', 'gpu_input_submit_ms', 'gpu_encode_submit_ms', 'gpu_bitstream_wait_ms')) {
            $value = [regex]::Match($line, '(?:^| )' + [regex]::Escape($field) + '=([\d.]+)')
            if ($value.Success) { $row[$field] = [double]::Parse($value.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture) }
        }
        [pscustomobject]$row
    })
    $steady = if ($fps.Count -gt 1) { @($fps | Select-Object -Skip 1) } else { $fps }
    $mean = if ($steady.Count) { ($steady | Measure-Object -Average).Average } else { 0 }
    $result = [pscustomobject]@{
        Result='PASS'; Scope='Synthetic D3D12 source, production GPU sender, loopback TCP v6 and production NVDEC receiver; not game or LAN hardware acceptance';
        ProducerPid=[int]$producerPid; SenderPid=$sender.Process.Id; SourceWidth=7680; SourceHeight=2160; EncodedWidth=3840; EncodedHeight=2160;
        DisplayEyeWidth=3840; DisplayEyeHeight=2160; BitrateMbps=200; RequestedFps=60;
        FirstConnectionSenderFpsSamples=$fps; SteadySenderMeanFps=$mean; WithinOneFpsOfTarget=($mean -ge 59);
        ObservedSenderProfiles=$profileRows;
        ReceiverRuns=$runs.ToArray(); WireMetadataAndNvdecVerified=$true; Reconnected=$true; GracefulStopMs=$stopWatch.ElapsedMilliseconds
    }
    [IO.File]::WriteAllText((Join-Path $outputRoot 'result.json'), ($result | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
    Write-Output ($result | ConvertTo-Json -Depth 6)
} finally {
    [void]$senderStop.Set(); [void]$producerStop.Set()
    foreach ($process in $owned) {
        if (-not $process.HasExited) {
            if (-not $process.WaitForExit(1500)) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue; [void]$process.WaitForExit(3000) }
        }
        $process.Dispose()
    }
    $senderStop.Dispose(); $producerStop.Dispose()
}
