param([string]$BinaryDir='build-opt/package',[string]$OutputDir='build-opt/test-results/probe-startup',[string]$Gpu='1')
$ErrorActionPreference='Stop'
$bin=[IO.Path]::GetFullPath($BinaryDir);$out=[IO.Path]::GetFullPath($OutputDir)
[void][IO.Directory]::CreateDirectory($out)
$id=[Guid]::NewGuid().ToString('N')
$fixtureStop=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,('Local\ProbeFixtureStop'+$id))
$fixtureReady=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,('Local\ProbeFixtureReady'+$id))
$senderStop=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,('Local\ProbeSenderStop'+$id))
$owned=@()
function Read-Log([string]$name) {
    $stream=[IO.File]::Open((Join-Path $out $name),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    $reader=[IO.StreamReader]::new($stream)
    try {return $reader.ReadToEnd()} finally {$reader.Dispose()}
}
try {
    $fixturePath=Join-Path $bin 'SourceProcessTests.exe'
    $fixture=Start-Process -FilePath $fixturePath -ArgumentList @('--fixture-wait',('Local\ProbeFixtureStop'+$id),('Local\ProbeFixtureReady'+$id)) -PassThru -WindowStyle Hidden
    $owned+=$fixture
    if(-not $fixtureReady.WaitOne(3000)){throw 'Fixture did not become ready'}
    # Selected program is this non-rendering fixture, never a running game.
    # It stays before LeiaSR initialization while ordinary desktop video flows.
    $sender=Start-Process -FilePath (Join-Path $bin 'LAN3DSender.exe') -ArgumentList @('--launch',('"'+$fixturePath+'"'),'--auto-source','--gpu',$Gpu,'--listen','127.0.0.1','--port','0','--codec','auto','--width','3840','--height','2160','--fps','60','--bitrate','400000000','--no-audio','--profile','--seconds','12','--stop-event',('Local\ProbeSenderStop'+$id)) -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $out 'sender.log') -RedirectStandardError (Join-Path $out 'sender-error.log')
    $owned+=$sender
    $until=[DateTime]::UtcNow.AddSeconds(10);$port=$null
    while([DateTime]::UtcNow -lt $until){
        $text=Read-Log 'sender.log'
        if($text -match 'Listening on 127\.0\.0\.1:(\d+)'){$port=$Matches[1];break}
        if($sender.HasExited){throw 'Sender exited before listening'}
        Start-Sleep -Milliseconds 100
    }
    if(-not $port){throw 'Sender startup timeout'}
    $receiver=Start-Process -FilePath (Join-Path $bin 'LAN3DReceiver.exe') -ArgumentList @('--connect','127.0.0.1','--port',$port,'--preview','--hidden','--no-audio','--receive-frames','180','--timeout','8') -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $out 'receiver.log') -RedirectStandardError (Join-Path $out 'receiver-error.log')
    $owned+=$receiver
    if(-not $receiver.WaitForExit(12000) -or $receiver.ExitCode -ne 0){throw 'Receiver failed'}
    [void]$senderStop.Set();if(-not $sender.WaitForExit(5000) -or $sender.ExitCode -ne 0){throw 'Sender failed'}
    $text=Read-Log 'sender.log'
    $received=Read-Log 'receiver.log'
    if([regex]::Matches($text,'GPU_PROBE ').Count -ne 1 -or $text.IndexOf('GPU_PROBE startup ready=1') -lt 0 -or $text.IndexOf('GPU_PROBE startup') -gt $text.IndexOf('Listening on')){throw 'Probe did not run exactly once before streaming'}
    if($text -notmatch 'cross_adapter=1' -or $text -notmatch 'ENCODER NVENC AV1 Desktop 3840x2160' -or $text -match '0x887[Aa]0007|input quarantined'){throw 'Cross-adapter GPU path failed'}
    if($received -notmatch 'decoded_unique=180\b'){throw 'Receiver did not decode 180 frames'}
    @{passed=$true;scope='Local desktop -> second GPU NVENC -> loopback NVDEC; fixture waiting for LeiaSR; no game injection';frames=180;startupProbeCount=1;profiles=@([regex]::Matches($text,'(?m)^PROFILE [^\r\n]+')|ForEach-Object {$_.Value})}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $out 'result.json') -Encoding UTF8
    Write-Output 'PASS: single startup probe, 4K cross-GPU encoding, 180 decoded frames'
} finally {
    [void]$fixtureStop.Set();[void]$senderStop.Set()
    foreach($process in $owned){if(-not $process.HasExited -and -not $process.WaitForExit(3000)){$process.Kill()}}
    $fixtureStop.Dispose();$fixtureReady.Dispose();$senderStop.Dispose()
}
