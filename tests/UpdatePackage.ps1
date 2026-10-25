param([Parameter(Mandatory=$true)][string]$Archive,[Parameter(Mandatory=$true)][string]$Executable,[Parameter(Mandatory=$true)][string]$OutputDir)
$ErrorActionPreference='Stop'
Import-Module "$PSHOME\Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1" -Force
Import-Module "$PSHOME\Modules\Microsoft.PowerShell.Management\Microsoft.PowerShell.Management.psd1" -Force
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class UpdateResources {
 [DllImport("kernel32",CharSet=CharSet.Unicode,SetLastError=true)] public static extern IntPtr LoadLibraryEx(string file,IntPtr reserved,uint flags);
 [DllImport("kernel32",SetLastError=true)] public static extern IntPtr FindResource(IntPtr module,IntPtr name,IntPtr type);
 [DllImport("kernel32")] public static extern IntPtr LoadResource(IntPtr module,IntPtr resource);
 [DllImport("kernel32")] public static extern IntPtr LockResource(IntPtr resource);
 [DllImport("kernel32")] public static extern uint SizeofResource(IntPtr module,IntPtr resource);
 [DllImport("kernel32")] public static extern bool FreeLibrary(IntPtr module);
}
'@
$root=[IO.Path]::GetFullPath($OutputDir)
[void][IO.Directory]::CreateDirectory($root)
$run=Join-Path $root ('package-'+[Guid]::NewGuid().ToString('N'))
$stage=Join-Path $run 'data/updates/package';$install=Join-Path $run 'app'
[void][IO.Directory]::CreateDirectory($stage);[void][IO.Directory]::CreateDirectory($install)
$module=[UpdateResources]::LoadLibraryEx([IO.Path]::GetFullPath($Executable),[IntPtr]::Zero,2)
if($module -eq [IntPtr]::Zero){throw 'Cannot load executable resources'}
try {
    $resource=[UpdateResources]::FindResource($module,[IntPtr]202,[IntPtr]10)
    $length=[UpdateResources]::SizeofResource($module,$resource)
    if(-not $length){throw 'Embedded installer missing'}
    $bytes=New-Object byte[] $length
    [Runtime.InteropServices.Marshal]::Copy([UpdateResources]::LockResource([UpdateResources]::LoadResource($module,$resource)),$bytes,0,$length)
    $script=Join-Path $stage 'install.ps1';[IO.File]::WriteAllBytes($script,$bytes)
} finally {[void][UpdateResources]::FreeLibrary($module)}
$source=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../src/update/InstallUpdate.ps1'))
if((Get-FileHash -LiteralPath $script).Hash -ne (Get-FileHash -LiteralPath $source).Hash){throw 'Embedded installer differs from source'}
$zip=Join-Path $stage 'package.zip';Copy-Item -LiteralPath $Archive -Destination $zip
[IO.File]::WriteAllText((Join-Path $install 'LAN3DShare.exe'),'old-version')
[IO.File]::WriteAllText((Join-Path $install 'custom.ini'),'preserved')
$archiveHandle=[IO.Compression.ZipFile]::OpenRead($zip)
try {
    $reader=[IO.StreamReader]::new($archiveHandle.GetEntry('manifest.json').Open())
    try {$manifest=$reader.ReadToEnd()|ConvertFrom-Json}finally{$reader.Dispose()}
} finally {$archiveHandle.Dispose()}
$request=@{installDirectory=$install;archive=$zip;sha256=(Get-FileHash -LiteralPath $zip).Hash.ToLowerInvariant();version=$manifest.version;parentPid=0;parentStart=0;readyEvent=''}
$file=Join-Path $stage 'request.json';[IO.File]::WriteAllText($file,($request|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
$helper=Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',('"'+$script+'"'),'-RequestFile',('"'+$file+'"'),'-NoRestart') -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput (Join-Path $stage 'output.log') -RedirectStandardError (Join-Path $stage 'error.log')
if($helper.ExitCode -ne 0){Get-Content -LiteralPath (Join-Path $stage 'error.log');throw "Package installation failed: $run"}
foreach($record in $manifest.runtimeFiles){if((Get-FileHash -LiteralPath (Join-Path $install $record.path)).Hash -ine $record.sha256){throw "Installed hash mismatch: $($record.path)"}}
if([IO.File]::ReadAllText((Join-Path $install 'custom.ini')) -ne 'preserved'){throw 'Unrelated configuration modified'}
@{version=$manifest.version;passed=$true;embeddedInstallerMatchesSource=$true;verifiedFiles=$manifest.runtimeFiles.Count;archiveSha256=(Get-FileHash -LiteralPath $Archive).Hash.ToLowerInvariant();restarted=$false}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $root 'package-install.json') -Encoding UTF8
if(-not $run.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Cleanup escaped test root'}
Remove-Item -LiteralPath $run -Recurse -Force
Write-Output 'Release archive installed and verified using its embedded installer'
