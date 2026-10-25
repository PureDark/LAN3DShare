param([Parameter(Mandatory)][int]$SourceProcessId,
    [Parameter(Mandatory)][string]$BinaryDir, [Parameter(Mandatory)][string]$OutputDir,
    [switch]$Snapshots)
$ErrorActionPreference = 'Stop'
$BinaryDir = [IO.Path]::GetFullPath($BinaryDir)
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$source = Get-Process -Id $SourceProcessId
$sourceStart = $source.StartTime
$capturePath = Join-Path $BinaryDir 'LAN3DCapture.dll'
$captureHash = (Get-FileHash -LiteralPath $capturePath -Algorithm SHA256).Hash
# Never reinterpret exports from an older resident build using a new DLL's RVAs.
if ($source.Modules | Where-Object { $_.ModuleName -eq 'LAN3DCapture.dll' }) {
    throw 'This validation requires a running source with no capture DLL loaded before the first attachment.'
}
$runtime = @($source.Modules | Where-Object { $_.ModuleName -match 'SimulatedRealityDirectX|TitanLeiaRuntime' } | ForEach-Object {
    [ordered]@{ Path = $_.FileName; SHA256 = (Get-FileHash -LiteralPath $_.FileName -Algorithm SHA256).Hash }
})
if (-not $runtime.Count) { throw 'The selected source has no LeiaSR DirectX runtime loaded.' }
$results = @()
$sender = $null; $receiver = $null
$snapshotProcess = $null
$observation = $null
$failure = $null
$cleanupErrors = [Collections.Generic.List[string]]::new()
function Save-CaptureSnapshots([string]$Directory, [bool]$ExpectSbs) {
    New-Item -ItemType Directory -Path $Directory -Force | Out-Null
    $report = Join-Path $Directory 'snapshots.jsonl'
    if (Test-Path -LiteralPath $report) { throw "Refusing to mix snapshot runs in $Directory" }
    $script:snapshotProcess = Start-Process -FilePath (Join-Path $BinaryDir 'CaptureSnapshots.exe') -WindowStyle Hidden -PassThru `
        -ArgumentList @('--pid', $SourceProcessId, '--output-dir', ('"' + $Directory + '"'),
            '--frames', '3', '--interval-ms', '2000', '--desktop', 'gdi') `
        -RedirectStandardOutput (Join-Path $Directory 'snapshot-helper.log') `
        -RedirectStandardError (Join-Path $Directory 'snapshot-helper-error.log')
    $null = $script:snapshotProcess.Handle
    if (-not $script:snapshotProcess.WaitForExit(22000)) { throw "Snapshot helper timed out: $Directory" }
    $script:snapshotProcess.WaitForExit()
    if ($script:snapshotProcess.ExitCode -ne 0) { throw "Snapshot helper failed in $Directory with exit code $($script:snapshotProcess.ExitCode)" }
    $samples = @(Get-Content -LiteralPath $report | Where-Object { $_.Trim() } | ForEach-Object { $_ | ConvertFrom-Json })
    if ($samples.Count -ne 3) { throw "Expected three snapshot samples in $Directory" }
    $desktopFiles = @(); $sbsFiles = @(); $sequences = @()
    foreach ($sample in $samples) {
        if ($sample.pid -ne $SourceProcessId -or $sample.desktop.captureApi -ne 'GDI' -or $sample.desktop.status -ne 'complete') {
            throw "Snapshot source or complete GDI desktop verification failed in $Directory"
        }
        $desktopFile = Join-Path $Directory $sample.desktop.file
        if (-not (Test-Path -LiteralPath $desktopFile -PathType Leaf)) { throw "Missing desktop snapshot: $desktopFile" }
        $desktopFiles += $desktopFile
        if ($ExpectSbs) {
            if ($sample.sbs.status -ne 'new_frame' -or ($sequences.Count -and $sample.sbs.sequence -le $sequences[-1])) {
                throw "Snapshot did not contain a new increasing SBS frame in $Directory"
            }
            $sbsFile = Join-Path $Directory $sample.sbs.file
            if (-not (Test-Path -LiteralPath $sbsFile -PathType Leaf)) { throw "Missing SBS snapshot: $sbsFile" }
            $sbsFiles += $sbsFile; $sequences += $sample.sbs.sequence
        } elseif ($sample.sbs.status -ne 'no_new_frame') {
            throw "Observation-only phase unexpectedly produced SBS in $Directory"
        }
    }
    $script:snapshotProcess.Dispose(); $script:snapshotProcess = $null
    return [ordered]@{ Directory = $Directory; Report = $report; Samples = $samples.Count; DesktopFiles = $desktopFiles; SbsFiles = $sbsFiles; SbsSequences = $sequences }
}
try {
    if ($Snapshots) {
        $observeDir = Join-Path $OutputDir 'observe-hooks'
        New-Item -ItemType Directory -Path $observeDir -Force | Out-Null
        $observeLog = Join-Path $observeDir 'sender.log'; $observeError = Join-Path $observeDir 'sender-error.log'
        $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
        $listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
        # Select an absent stream: the capture DLL observes existing objects,
        # but no weaver can append a readback copy during this diagnostic phase.
        $sender = Start-Process -FilePath (Join-Path $BinaryDir 'LAN3DSender.exe') -WindowStyle Hidden -PassThru `
            -ArgumentList @('--stream', '9999', '--pid', $SourceProcessId, '--listen', '127.0.0.1',
                '--port', $port, '--seconds', '8', '--no-audio') `
            -RedirectStandardOutput $observeLog -RedirectStandardError $observeError
        $null = $sender.Handle
        $deadline = [DateTime]::UtcNow.AddSeconds(10)
        do {
            if ($sender.HasExited) { throw "Observation sender exited early: $(Get-Content -LiteralPath $observeError -Raw)" }
            if ((Test-Path -LiteralPath $observeLog) -and (Get-Content -LiteralPath $observeLog -Raw) -match 'Listening on') { break }
            Start-Sleep -Milliseconds 50
        } while ([DateTime]::UtcNow -lt $deadline)
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Observation sender readiness timed out' }
        $observeSnapshots = Save-CaptureSnapshots $observeDir $false
        if (-not $sender.WaitForExit(12000)) { throw 'Observation sender did not stop at its time limit' }
        $sender.WaitForExit()
        if ($sender.ExitCode -ne 0) { throw "Observation sender failed: $(Get-Content -LiteralPath $observeError -Raw)" }
        if ((Get-Content -LiteralPath $observeLog -Raw) -notmatch ("Capture stopped in PID " + $SourceProcessId + "; the source application remains running")) {
            throw 'Observation sender did not confirm capture stopped in the existing source'
        }
        $currentSource = Get-Process -Id $SourceProcessId
        if ($currentSource.StartTime -ne $sourceStart) { throw 'Source process identity changed during hook observation' }
        $observation = [ordered]@{ Passed = $true; Stream = 9999; DurationSeconds = 8; SourceStillRunning = $true; Snapshots = $observeSnapshots }
        $sender.Dispose(); $sender = $null
        Write-Output "PASS observation-only hooks: no SBS published, three GDI desktop snapshots, same source PID $SourceProcessId"
    }
    foreach ($run in 1, 2) {
        $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
        $listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
        $sendLog = Join-Path $OutputDir "sender-$run.log"
        $sendError = Join-Path $OutputDir "sender-$run-error.log"
        $receiveLog = Join-Path $OutputDir "receiver-$run.log"
        $receiveError = Join-Path $OutputDir "receiver-$run-error.log"
        # No desktop fallback or test pattern: every frame must originate in the
        # already-running source's SBS bus. Hidden preview cannot toggle its display.
        $sender = Start-Process -FilePath (Join-Path $BinaryDir 'LAN3DSender.exe') -WindowStyle Hidden -PassThru `
            -ArgumentList @('--pid', $SourceProcessId, '--listen', '127.0.0.1', '--port', $port,
                '--width', '3840', '--height', '2160', '--fps', '60', '--bitrate', '200000000',
                '--codec', 'auto', '--no-audio', '--seconds', '20') `
            -RedirectStandardOutput $sendLog -RedirectStandardError $sendError
        $null = $sender.Handle
        $deadline = [DateTime]::UtcNow.AddSeconds(10)
        do {
            if ($sender.HasExited) { throw "Sender $run exited early: $(Get-Content -LiteralPath $sendError -Raw)" }
            if ((Test-Path -LiteralPath $sendLog) -and (Get-Content -LiteralPath $sendLog -Raw) -match 'Listening on') { break }
            Start-Sleep -Milliseconds 50
        } while ([DateTime]::UtcNow -lt $deadline)
        if ([DateTime]::UtcNow -ge $deadline) { throw "Sender $run readiness timed out" }
        $receiver = Start-Process -FilePath (Join-Path $BinaryDir 'LAN3DReceiver.exe') -WindowStyle Hidden -PassThru `
            -ArgumentList @('--connect', '127.0.0.1', '--port', $port, '--preview', '--hidden',
                '--receive-frames', '30', '--timeout', '15') `
            -RedirectStandardOutput $receiveLog -RedirectStandardError $receiveError
        $null = $receiver.Handle
        if (-not $receiver.WaitForExit(18000)) { throw "Receiver $run timed out" }
        $receiver.WaitForExit()
        if ($receiver.ExitCode -ne 0) { throw "Receiver $run failed: $(Get-Content -LiteralPath $receiveError -Raw)" }
        $receiveText = Get-Content -LiteralPath $receiveLog -Raw
        if ($receiveText -notmatch 'decoded_unique=(\d+)' -or [int]$Matches[1] -lt 30) { throw "Receiver $run decoded too few frames: $receiveText" }
        $decoded = [int]$Matches[1]
        $runSnapshots = $null
        if ($Snapshots) {
            if ($sender.HasExited) { throw "Sender $run ended before the SBS/desktop snapshot interval" }
            $runSnapshots = Save-CaptureSnapshots (Join-Path $OutputDir "run-$run") $true
        }
        if (-not $sender.WaitForExit(23000)) { throw "Sender $run did not stop at its time limit" }
        $sender.WaitForExit()
        if ($sender.ExitCode -ne 0) { throw "Sender $run failed: $(Get-Content -LiteralPath $sendError -Raw)" }
        $sendText = Get-Content -LiteralPath $sendLog -Raw
        if ($sendText -notmatch ("Capture stopped in PID " + $SourceProcessId + "; the source application remains running")) {
            throw "Sender $run did not confirm capture stopped in the existing source"
        }
        if ($sendText -notmatch 'SOURCE_KIND LeiaSbs' -or $sendText -match 'SOURCE_KIND Desktop') { throw "Run $run did not exclusively send real SBS: $sendText" }
        if ($sendText -notmatch 'ENCODER NVENC (AV1|HEVC|H\.264) SBS (\d+)x(\d+)') { throw "Run $run did not use hardware SBS encoding: $sendText" }
        $codec = $Matches[1]; $width = [int]$Matches[2]; $height = [int]$Matches[3]
        if ($receiveText -notmatch ('DECODER NVDEC ' + [regex]::Escape($codec))) { throw "Run $run hardware decoder mismatch" }
        $currentSource = Get-Process -Id $SourceProcessId
        if ($currentSource.StartTime -ne $sourceStart) { throw 'Source process identity changed' }
        $results += [ordered]@{ Run = $run; DecodedUnique = $decoded; Codec = $codec; Width = $width; Height = $height; Kind = 'LeiaSbs'; SourceStillRunning = $true; Snapshots = $runSnapshots }
        $sender.Dispose(); $sender = $null; $receiver.Dispose(); $receiver = $null
        Write-Output "PASS run $run : $decoded unique SBS frames, $codec ${width}x${height}, same source PID $SourceProcessId"
    }
    [ordered]@{
        Date = [DateTime]::Now.ToString('o'); SourcePid = $SourceProcessId; SourcePath = $source.Path
        SourceStartTime = $sourceStart.ToString('o'); CaptureSha256 = $captureHash; Runtime = $runtime
        Scope = 'Existing real game; two separate sender sessions; loopback NVENC/NVDEC; hidden preview; no desktop fallback; no audio or physical display acceptance'
        SnapshotsEnabled = [bool]$Snapshots; Observation = $observation; Runs = $results
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDir 'result.json') -Encoding UTF8
} catch {
    # Preserve the actual test failure even if stopping a helper also fails.
    $failure = $_
    try { $_ | Out-String | Set-Content -LiteralPath (Join-Path $OutputDir 'failure.log') -Encoding UTF8 }
    catch { $cleanupErrors.Add("Could not save the original failure: $_") }
} finally {
    # Killing a sender skips its C++ RAII cleanup. Stop the injected capture
    # first, using the exact same on-disk DLL that was loaded for this run.
    # Never touch the game lifetime, and never call new-build RVAs in an older DLL.
    if ($null -ne $sender -or $null -ne $failure) {
        $stopper = $null
        try {
            $currentSource = Get-Process -Id $SourceProcessId -ErrorAction SilentlyContinue
            if ($null -ne $currentSource) {
                if ($currentSource.StartTime -ne $sourceStart) { throw 'Cleanup refused: source process identity changed' }
                $loaded = @($currentSource.Modules | Where-Object { $_.ModuleName -eq 'LAN3DCapture.dll' })
                if ($loaded.Count) {
                    if ($loaded.Count -ne 1 -or -not [string]::Equals([IO.Path]::GetFullPath($loaded[0].FileName),
                        $capturePath, [StringComparison]::OrdinalIgnoreCase) -or
                        (Get-FileHash -LiteralPath $capturePath -Algorithm SHA256).Hash -ne $captureHash) {
                        throw 'Cleanup refused: resident capture path or build changed'
                    }
                    $stopper = Start-Process -FilePath (Join-Path $BinaryDir 'LAN3DSender.exe') -WindowStyle Hidden -PassThru `
                        -ArgumentList @('--stop', '--pid', $SourceProcessId) `
                        -RedirectStandardOutput (Join-Path $OutputDir 'cleanup-stop.log') `
                        -RedirectStandardError (Join-Path $OutputDir 'cleanup-stop-error.log')
                    $null = $stopper.Handle
                    if (-not $stopper.WaitForExit(10000)) { throw 'Cleanup capture stop timed out' }
                    $stopper.WaitForExit()
                    if ($stopper.ExitCode -ne 0) { throw "Cleanup capture stop failed with exit code $($stopper.ExitCode)" }
                }
            }
        } catch { $cleanupErrors.Add([string]$_) }
        finally {
            if ($null -ne $stopper) {
                try {
                    if (-not $stopper.HasExited) { $stopper.Kill(); $stopper.WaitForExit() }
                    $stopper.Dispose()
                } catch { $cleanupErrors.Add("Stop helper cleanup failed: $_") }
            }
        }
    }
    # Only helper processes created here are stopped. The source game is never terminated.
    foreach ($process in $snapshotProcess, $receiver, $sender) {
        if ($null -ne $process) {
            try {
                if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
                $process.Dispose()
            } catch { $cleanupErrors.Add("Stream helper cleanup failed: $_") }
        }
    }
}
if ($cleanupErrors.Count) {
    try { $cleanupErrors | Set-Content -LiteralPath (Join-Path $OutputDir 'cleanup-errors.log') -Encoding UTF8 }
    catch { Write-Warning "Could not save cleanup errors: $_" }
    Write-Warning ($cleanupErrors -join '; ')
}
if ($null -ne $failure) { throw $failure }
if ($cleanupErrors.Count) { throw 'Validation cleanup failed; see cleanup-errors.log' }
