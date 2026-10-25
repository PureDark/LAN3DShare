param([string]$BinaryDir='build-opt/package', [string]$OutputDir='build-opt/test-results/desktop-perf/before',
    [int]$Width=3840, [int]$Height=2160, [int]$BitrateMbps=400, [int]$Seconds=12, [switch]$ReceiverProfile, [string]$Gpu="auto")
$ErrorActionPreference='Stop'
$binary=(Resolve-Path -LiteralPath $BinaryDir).Path
$output=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force -Path $output | Out-Null
$sender=$null; $receiver=$null
try {
    $sender=Start-Process -FilePath (Join-Path $binary 'LAN3DSender.exe') -WorkingDirectory $binary -WindowStyle Hidden -PassThru -ArgumentList @('--gpu',$Gpu,'--desktop-monitor','0','--width',"$Width",'--height',"$Height",'--fps','60','--bitrate',"$($BitrateMbps*1000000)",'--codec','av1','--profile','--listen','127.0.0.1','--port','0','--seconds',"$Seconds") -RedirectStandardOutput (Join-Path $output 'sender.log') -RedirectStandardError (Join-Path $output 'sender.err')
    [void]$sender.Handle
    $until=[DateTime]::UtcNow.AddSeconds(6)
    do {
        $log=Get-Content -LiteralPath (Join-Path $output 'sender.log') -Raw -ErrorAction SilentlyContinue
        if ($log -match 'Listening on 127\.0\.0\.1:(\d+);') { $port=$Matches[1]; break }
        if ($sender.HasExited) { throw 'Sender exited before ready' }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $until)
    if (-not $port) { throw 'Sender not ready' }
    $arguments=@('--connect','127.0.0.1','--port',$port,'--hidden','--preview','--no-audio','--timeout','5')
    if ($ReceiverProfile) { $arguments+='--profile' }
    $receiver=Start-Process -FilePath (Join-Path $binary 'LAN3DReceiver.exe') -WorkingDirectory $binary -WindowStyle Hidden -PassThru -ArgumentList $arguments -RedirectStandardOutput (Join-Path $output 'receiver.log') -RedirectStandardError (Join-Path $output 'receiver.err')
    [void]$receiver.Handle
    if (-not $sender.WaitForExit(($Seconds+10)*1000)) { throw 'Sender timeout' }
    if (-not $receiver.WaitForExit(10000)) { throw 'Receiver timeout' }
    if ($sender.ExitCode -ne 0 -or $receiver.ExitCode -ne 0) { throw 'Profile child failed; see logs' }
    Get-Content -LiteralPath (Join-Path $output 'sender.log') | Select-String '^PROFILE_SUMMARY'
    Get-Content -LiteralPath (Join-Path $output 'receiver.log') | Select-String 'PROFILE|Receiver stopped'
} finally {
    foreach ($child in @($receiver,$sender)) { if ($child -and -not $child.HasExited) { Stop-Process -Id $child.Id -Force } }
}
