param([Parameter(Mandatory)][string]$BinaryDir, [Parameter(Mandatory)][string]$OutputDir, [switch]$Hardware,
    [ValidateRange(20,1000)][int]$BitrateMbps = 400, [switch]$Automatic)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = $listener.LocalEndpoint.Port
$listener.Stop()
$sender = $null
$receiver = $null
try {
    $senderLog = Join-Path $OutputDir 'sender.log'
    $senderError = Join-Path $OutputDir 'sender-error.log'
    $arguments = @('--test-pattern', '--listen', '127.0.0.1', '--port', $port, '--source-width', '640', '--source-height', '360', '--seconds', '12')
    if ($Hardware) {
        $target = if ($Automatic) { 0 } else { $BitrateMbps * 1000000 }
        # This fixture generates 4K pixels on the CPU. Leave enough time for
        # 150 frames even while a game occupies the GPU; GPU throughput has its
        # own shared-texture test. Keep both reconnect/frame-count assertions.
        $senderSeconds = if ($Automatic) { '28' } else { '16' }
        $arguments = @('--test-pattern', '--listen', '127.0.0.1', '--port', $port, '--source-width', '3840', '--source-height', '2160',
            '--width', '3840', '--height', '2160', '--fps', '60', '--bitrate', "$target", '--codec', 'auto', '--seconds', $senderSeconds)
    }
    $sender = Start-Process -FilePath (Join-Path $BinaryDir 'LAN3DSender.exe') -WindowStyle Hidden -PassThru `
        -ArgumentList $arguments `
        -RedirectStandardOutput $senderLog -RedirectStandardError $senderError
    $null = $sender.Handle
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        if ($sender.HasExited) { throw "Sender exited early: $(Get-Content -LiteralPath $senderError -Raw)" }
        if ((Test-Path -LiteralPath $senderLog) -and (Get-Content -LiteralPath $senderLog -Raw) -match 'Listening on') { break }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $deadline)
    if ([DateTime]::UtcNow -ge $deadline) { throw 'Sender did not become ready' }
    foreach ($run in 1, 2) {
        $output = Join-Path $OutputDir "receiver-$run.log"
        $errorLog = Join-Path $OutputDir "receiver-$run-error.log"
        $frameTarget = if ($Automatic) { 150 } else { 30 }
        $receiver = Start-Process -FilePath (Join-Path $BinaryDir 'LAN3DReceiver.exe') -WindowStyle Hidden -PassThru `
            -ArgumentList @('--connect', '127.0.0.1', '--port', $port, '--preview', '--hidden', '--receive-frames', "$frameTarget", '--timeout', '5') `
            -RedirectStandardOutput $output -RedirectStandardError $errorLog
        $null = $receiver.Handle
        $receiveDeadlineMs = if ($Automatic) { 12000 } else { 8000 }
        if (-not $receiver.WaitForExit($receiveDeadlineMs)) { throw "Receiver $run timed out" }
        $receiver.WaitForExit()
        if ($receiver.ExitCode -ne 0) { throw "Receiver $run failed: $(Get-Content -LiteralPath $errorLog -Raw)" }
        $result = Get-Content -LiteralPath $output -Raw
        if ($result -notmatch 'decoded_unique=(\d+)' -or [int]$Matches[1] -lt $frameTarget) { throw "Receiver $run did not decode $frameTarget unique frames: $result" }
        if ($Hardware) {
            $senderText = Get-Content -LiteralPath $senderLog -Raw
            if ($senderText -match 'FALLBACK Software') { Write-Output 'SKIP: hardware pair unavailable; software fallback worked'; exit 77 }
            $initialRate = if ($Automatic) { 200 } else { $BitrateMbps }
            if ($senderText -notmatch "ENCODER NVENC (AV1|HEVC|H\.264) SBS 3840x2160 target=60fps/${initialRate}Mbps") { throw "Hardware sender did not negotiate 4K60/${initialRate}Mbps: $senderText" }
            $selected = $Matches[1]
            if ($result -notmatch ('DECODER NVDEC ' + [regex]::Escape($selected))) { throw "Receiver did not use the negotiated hardware decoder: $result" }
            if ($Automatic -and ($senderText -notmatch 'AUTO_BITRATE target=200000000 reason=initial' -or $senderText -match 'reconfigure-unavailable')) { throw "Automatic bitrate did not remain available: $senderText" }
        }
        Write-Output $result.Trim()
        $receiver.Dispose()
        $receiver = $null
    }
    if (-not $sender.WaitForExit(15000)) { throw 'Sender did not stop at its time limit' }
    if ($sender.ExitCode -ne 0) { throw "Sender failed: $(Get-Content -LiteralPath $senderError -Raw)" }
    Write-Output 'PASS production sender -> TCP -> production receiver -> GPU upload/preview, including reconnect'
} finally {
    foreach ($process in $receiver, $sender) {
        if ($null -ne $process) {
            if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
            $process.Dispose()
        }
    }
}
