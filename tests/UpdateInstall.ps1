param([Parameter(Mandatory=$true)][string]$OutputDir,[Parameter(Mandatory=$true)][string]$ParentFixture)
$ErrorActionPreference='Stop'
Set-StrictMode -Version 2
Import-Module "$PSHOME\Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1" -Force
Import-Module "$PSHOME\Modules\Microsoft.PowerShell.Management\Microsoft.PowerShell.Management.psd1" -Force
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
$root=[IO.Path]::GetFullPath($OutputDir)
[void][IO.Directory]::CreateDirectory($root)
$run=Join-Path $root ([Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($run)
$installer=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../src/update/InstallUpdate.ps1'))
$results=New-Object 'System.Collections.Generic.List[object]'
function Require($condition,$message){if(-not $condition){throw $message}}
function Fixture([string]$name,[string]$mode='normal') {
    $base=Join-Path $run $name
    $install=Join-Path $base 'app';$stage=Join-Path $base 'data/updates/test'
    [void][IO.Directory]::CreateDirectory($install);[void][IO.Directory]::CreateDirectory($stage)
    [IO.File]::WriteAllText((Join-Path $install 'LAN3DShare.exe'),'old-exe')
    [IO.File]::WriteAllText((Join-Path $install 'LAN3DCapture.dll'),'capture')
    [IO.File]::WriteAllText((Join-Path $install 'user-settings.ini'),'keep-me')
    $zip=Join-Path $stage 'package.zip'
    $records=@();$payload=[ordered]@{'LAN3DShare.exe'='new-exe';'LAN3DCapture.dll'='capture';'README.md'='new-readme'}
    if($mode -eq 'locked'){$payload['LAN3DCapture.dll']='changed-capture'}
    if($mode -eq 'traversal'){$payload['../outside.md']='unsafe'}
    $archive=[IO.Compression.ZipFile]::Open($zip,[IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach($name in $payload.Keys){
            $bytes=if($mode -eq 'handoff' -and $name -eq 'LAN3DShare.exe'){[IO.File]::ReadAllBytes($ParentFixture)}else{[Text.Encoding]::UTF8.GetBytes($payload[$name])};$sha=[Security.Cryptography.SHA256]::Create()
            try {$hash=([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
            $records+=@{path=$name;bytes=$bytes.Length;sha256=$hash}
            $stream=$archive.CreateEntry($name).Open();try{$stream.Write($bytes,0,$bytes.Length)}finally{$stream.Dispose()}
        }
        if($mode -eq 'badfile'){$records[0].sha256=('0'*64)}
        if($mode -eq 'duplicate'){$stream=$archive.CreateEntry('readme.md').Open();$stream.Dispose()}
        $manifest=@{product='LAN3DShare';platform='Windows x64';version='0.7.9';runtimeFiles=$records}|ConvertTo-Json -Depth 6
        $writer=[IO.StreamWriter]::new($archive.CreateEntry('manifest.json').Open());try{$writer.Write($manifest)}finally{$writer.Dispose()}
    } finally {$archive.Dispose()}
    $request=@{installDirectory=$install;archive=$zip;sha256=(Get-FileHash -LiteralPath $zip).Hash.ToLowerInvariant();version='0.7.9';parentPid=0;parentStart=0;readyEvent=''}
    if($mode -eq 'badzip'){$request.sha256=('0'*64)}
    $file=Join-Path $stage 'request.json'
    [IO.File]::WriteAllText($file,($request|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
    return @{install=$install;stage=$stage;request=$request;file=$file;result=(Join-Path $base 'data/update-result.json')}
}
function Run($fixture) {
    $log=Join-Path $fixture.stage 'output.log'
    $process=Start-Process -FilePath powershell.exe -ArgumentList @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',('"'+$installer+'"'),'-RequestFile',('"'+$fixture.file+'"'),'-NoRestart') -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput $log -RedirectStandardError ($log+'.err')
    return $process.ExitCode
}
foreach($mode in @('normal','badzip','badfile','traversal','duplicate','locked','identical-locked')) {
    $f=Fixture $mode $mode; $lock=$null
    try {
        if($mode -in @('locked','identical-locked')){$lock=[IO.File]::Open((Join-Path $f.install 'LAN3DCapture.dll'),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)}
        $code=Run $f
        $success=$mode -in @('normal','identical-locked')
        Require (($code -eq 0) -eq $success) "$mode exit=$code; see $($f.stage)"
        Require (([IO.File]::ReadAllText((Join-Path $f.install 'LAN3DShare.exe'))) -eq $(if($success){'new-exe'}else{'old-exe'})) "$mode EXE not restored"
        Require (([IO.File]::ReadAllText((Join-Path $f.install 'LAN3DCapture.dll'))) -eq 'capture') "$mode DLL changed"
        Require (([IO.File]::ReadAllText((Join-Path $f.install 'user-settings.ini'))) -eq 'keep-me') "$mode settings changed"
        Require ((Test-Path -LiteralPath (Join-Path $f.install 'README.md')) -eq $success) "$mode new-file rollback failed"
        $results.Add(@{case=$mode;passed=$true;exit=$code})
    } finally {if($lock){$lock.Dispose()}}
}
# Exercise the real process identity and ready/exit handshake in an isolated app directory.
$f=Fixture 'handoff' 'handoff'; Copy-Item -LiteralPath $ParentFixture -Destination (Join-Path $f.install 'LAN3DShare.exe') -Force
[IO.File]::AppendAllText((Join-Path $f.install 'LAN3DShare.exe'),'old-build-overlay')
$before=(Get-FileHash -LiteralPath (Join-Path $f.install 'LAN3DShare.exe')).Hash
$exitName='Local\Lan3DTestExit'+[Guid]::NewGuid().ToString('N')
$readyName='Local\Lan3DTestReady'+[Guid]::NewGuid().ToString('N')
$exitEvent=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$exitName)
$readyEvent=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$readyName)
$parent=$null;$helper=$null
$previousMarker=$env:LAN3D_UPDATE_TEST_RESTART_MARKER
try {
    $parent=Start-Process -FilePath (Join-Path $f.install 'LAN3DShare.exe') -ArgumentList @('--wait',$exitName) -WindowStyle Hidden -PassThru
    $f.request.parentPid=$parent.Id;$f.request.parentStart=$parent.StartTime.ToUniversalTime().ToFileTimeUtc();$f.request.readyEvent=$readyName
    [IO.File]::WriteAllText($f.file,($f.request|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
    $env:LAN3D_UPDATE_TEST_RESTART_MARKER=Join-Path $f.stage 'restarted.txt'
    $helper=Start-Process -FilePath powershell.exe -ArgumentList @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',('"'+$installer+'"'),'-RequestFile',('"'+$f.file+'"')) -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $f.stage 'handoff-error.log')
    Require ($readyEvent.WaitOne(15000)) 'Helper did not signal ready'
    Require ((Get-FileHash -LiteralPath (Join-Path $f.install 'LAN3DShare.exe')).Hash -eq $before) 'Files changed before parent exit'
    [void]$exitEvent.Set();Require ($parent.WaitForExit(5000)) 'Fixture parent did not exit'
    Require ($helper.WaitForExit(15000)) 'Helper did not complete'
    Require ((Get-FileHash -LiteralPath (Join-Path $f.install 'LAN3DShare.exe')).Hash -eq (Get-FileHash -LiteralPath $ParentFixture).Hash) 'Handoff update failed'
    $until=[DateTime]::UtcNow.AddSeconds(5)
    while(-not (Test-Path -LiteralPath $env:LAN3D_UPDATE_TEST_RESTART_MARKER) -and [DateTime]::UtcNow -lt $until){Start-Sleep -Milliseconds 100}
    Require (Test-Path -LiteralPath $env:LAN3D_UPDATE_TEST_RESTART_MARKER) 'Updated executable did not restart'
    $results.Add(@{case='parent-identity-ready-exit-restart';passed=$true})
} finally {
    $env:LAN3D_UPDATE_TEST_RESTART_MARKER=$previousMarker
    [void]$exitEvent.Set()
    if($parent -and -not $parent.HasExited){$parent.Kill()}
    if($helper -and -not $helper.HasExited){$helper.Kill()}
    $exitEvent.Dispose();$readyEvent.Dispose()
}
$results|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $root 'installer.json') -Encoding UTF8
$results|Format-Table
# Delete only this test run's verified temporary directory; retain summarized evidence.
Require ($run.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase)) 'Cleanup path escaped test root'
Remove-Item -LiteralPath $run -Recurse -Force
