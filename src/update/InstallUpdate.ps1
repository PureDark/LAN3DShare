param([Parameter(Mandatory=$true)][string]$RequestFile, [switch]$NoRestart)
# Embedded in the application. No credentials are passed to this process.
$ErrorActionPreference='Stop'
Set-StrictMode -Version 2
Import-Module "$PSHOME\Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1" -Force
Import-Module "$PSHOME\Modules\Microsoft.PowerShell.Management\Microsoft.PowerShell.Management.psd1" -Force
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$changed=New-Object 'System.Collections.Generic.List[object]'
$tempFiles=New-Object 'System.Collections.Generic.List[string]'
$parentExited=$false; $installed=$false; $ready=$null; $archive=$null; $resultFile=$null; $backup=$null
function Inside([string]$root,[string]$relative) {
    $path=[IO.Path]::GetFullPath((Join-Path $root $relative))
    if(-not $path.StartsWith($root.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw '更新路径越界'}
    return $path
}
function NoLinks([string]$path) {
    $current=$path
    while($current) {
        if(Test-Path -LiteralPath $current) {
            if((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw '更新目录不能包含符号链接或重解析点'}
        }
        $next=Split-Path -Parent $current
        if($next -eq $current){break}; $current=$next
    }
}
function PayloadPath([string]$name) {
    if(-not $name -or $name -notmatch '^[A-Za-z0-9_./-]+$'){throw '更新包路径无效'}
    foreach($part in $name.Split('/')) {
        if(-not $part -or $part -eq '.' -or $part -eq '..' -or $part.EndsWith('.') -or $part -match '^(?i:CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\.|$)'){throw '更新包含不安全的文件名'}
    }
    if($name -notmatch '^(LAN3DShare\.exe|LAN3DCapture\.dll|manifest\.json|[A-Za-z0-9_.-]+\.md|(?:docs|licenses|validation)/.+)$'){throw '更新包含未允许的程序文件'}
    return $name
}
function Result([bool]$ok,[string]$message) {
    if($resultFile) { [IO.File]::WriteAllText($resultFile,(@{ok=$ok;message=$message;version=$request.version}|ConvertTo-Json),[Text.UTF8Encoding]::new($false)) }
}
try {
    $requestPath=[IO.Path]::GetFullPath($RequestFile)
    $stage=Split-Path -Parent $requestPath; NoLinks $stage
    $request=[IO.File]::ReadAllText($requestPath) | ConvertFrom-Json
    $dataRoot=Split-Path -Parent (Split-Path -Parent $stage)
    if((Split-Path -Leaf (Split-Path -Parent $stage)) -ne 'updates'){throw '无效的更新缓存目录'}
    $resultFile=Inside $dataRoot 'update-result.json'
    $install=[IO.Path]::GetFullPath([string]$request.installDirectory).TrimEnd('\')
    NoLinks $install
    $exe=Inside $install 'LAN3DShare.exe'
    if(-not (Test-Path -LiteralPath $exe -PathType Leaf)){throw '找不到原客户端'}
    $zip=Inside $stage 'package.zip'
    if([IO.Path]::GetFullPath([string]$request.archive) -ne $zip){throw '无效的更新缓存文件'}
    if($request.version -notmatch '^\d{1,5}\.\d{1,5}\.\d{1,5}$' -or $request.sha256 -notmatch '^[0-9a-f]{64}$'){throw '无效的更新版本或校验值'}
    if((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ine $request.sha256){throw '更新包校验失败，请重新下载'}
    $parentProcess=$null
    if([long]$request.parentPid -gt 0) {
        $parentProcess=Get-Process -Id ([int]$request.parentPid) -ErrorAction Stop
        if($parentProcess.StartTime.ToUniversalTime().ToFileTimeUtc() -ne [long]$request.parentStart -or
            [IO.Path]::GetFullPath($parentProcess.MainModule.FileName) -ne $exe){throw '更新请求不属于当前客户端进程'}
        $ready=[Threading.EventWaitHandle]::OpenExisting([string]$request.readyEvent)
    } elseif(-not $NoRestart) { throw '更新缺少客户端退出握手' }
    foreach($process in @(Get-Process -Name LAN3DShare -ErrorAction SilentlyContinue)) {
        if($process.Id -ne [int]$request.parentPid -and $process.Path -and [IO.Path]::GetFullPath($process.Path) -eq $exe){throw '请先结束共享并关闭同目录的其他客户端'}
    }
    $archive=[IO.Compression.ZipFile]::OpenRead($zip)
    if($archive.Entries.Count -gt 512){throw '更新包文件数超限'}
    $entries=New-Object 'System.Collections.Generic.Dictionary[string,System.IO.Compression.ZipArchiveEntry]' ([StringComparer]::OrdinalIgnoreCase)
    $total=0L
    foreach($entry in $archive.Entries) {
        $name=PayloadPath $entry.FullName
        if($entries.ContainsKey($name) -or (($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000){throw '更新包含重复文件或链接'}
        $total+=$entry.Length
        if($entry.Length -gt 268435456 -or $total -gt 536870912){throw '更新包解压大小超限'}
        $entries.Add($name,$entry)
    }
    if(-not $entries.ContainsKey('manifest.json') -or $entries['manifest.json'].Length -gt 2097152){throw '更新包缺少校验清单'}
    $reader=[IO.StreamReader]::new($entries['manifest.json'].Open())
    try {$manifest=$reader.ReadToEnd()|ConvertFrom-Json} finally {$reader.Dispose()}
    if($manifest.product -ne 'LAN3DShare' -or $manifest.platform -ne 'Windows x64' -or $manifest.version -ne $request.version){throw '更新清单产品或版本不匹配'}
    $records=New-Object 'System.Collections.Generic.Dictionary[string,object]' ([StringComparer]::OrdinalIgnoreCase)
    foreach($file in $manifest.runtimeFiles) {
        $name=PayloadPath ([string]$file.path)
        if($name -eq 'manifest.json' -or $records.ContainsKey($name) -or -not $entries.ContainsKey($name) -or
            $file.sha256 -notmatch '^[0-9a-f]{64}$' -or [long]$file.bytes -ne $entries[$name].Length){throw '更新清单文件信息无效'}
        $records.Add($name,$file)
    }
    if(-not $records.ContainsKey('LAN3DShare.exe') -or -not $records.ContainsKey('LAN3DCapture.dll') -or $records.Count+1 -ne $entries.Count){throw '更新包程序不完整或包含未校验文件'}
    $unpack=Inside $stage ('unpack-'+[Guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($unpack)
    foreach($entry in $entries.Values) {
        $destination=Inside $unpack $entry.FullName
        [void][IO.Directory]::CreateDirectory((Split-Path -Parent $destination))
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$destination,$false)
        if($entry.FullName -ne 'manifest.json' -and (Get-FileHash -LiteralPath $destination).Hash -ine $records[$entry.FullName].sha256){throw '解压后文件校验失败'}
    }
    $archive.Dispose();$archive=$null
    # A same-directory backup allows rename/rollback on the installation volume.
    $backup=Inside $install ('.lan3d-update-backup-'+[Guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($backup)
    $probe=Inside $backup 'write-probe';[IO.File]::WriteAllText($probe,'ok');Remove-Item -LiteralPath $probe
    if($parentProcess) {
        [void]$ready.Set()
        if(-not $parentProcess.WaitForExit(30000)){throw '客户端未及时退出，更新已取消'}
    }
    $parentExited=$true
    foreach($entry in $entries.Values) {
        $name=$entry.FullName; $target=Inside $install $name; NoLinks $target
        $source=Inside $unpack $name
        # Identical capture DLLs may still be loaded by the game; do not touch them.
        if((Test-Path -LiteralPath $target -PathType Leaf) -and (Get-FileHash -LiteralPath $target).Hash -eq (Get-FileHash -LiteralPath $source).Hash){continue}
        [void][IO.Directory]::CreateDirectory((Split-Path -Parent $target))
        $temporary=$target+'.update-'+[Guid]::NewGuid().ToString('N');$tempFiles.Add($temporary)
        [IO.File]::Copy($source,$temporary,$false)
        $saved=Inside $backup $name; [void][IO.Directory]::CreateDirectory((Split-Path -Parent $saved))
        $hadOld=Test-Path -LiteralPath $target
        if($hadOld){[IO.File]::Move($target,$saved)}
        $changed.Add(@{target=$target;saved=$saved;hadOld=$hadOld})
        [IO.File]::Move($temporary,$target)
    }
    $installed=$true
    Result $true ('已更新到 '+$request.version)
    if(-not $NoRestart){Start-Process -FilePath $exe -WorkingDirectory $install -WindowStyle Normal | Out-Null}
    # Only remove our own verified staging/backup folders after successful restart.
    NoLinks $backup; if($backup -ne (Inside $install (Split-Path -Leaf $backup))){throw '无效备份路径'}
    Remove-Item -LiteralPath $backup -Recurse -Force
    NoLinks $unpack; if(-not $unpack.StartsWith($stage+'\',[StringComparison]::OrdinalIgnoreCase)){throw '无效缓存路径'}
    Remove-Item -LiteralPath $unpack -Recurse -Force
    Remove-Item -LiteralPath $zip -Force
    exit 0
} catch {
    $errorText=$_.Exception.Message
    $rollbackOk=$true
    if(-not $installed) {
        for($index=$changed.Count-1;$index -ge 0;$index--) {
            $item=$changed[$index]
            try {
                if(Test-Path -LiteralPath $item.target){Remove-Item -LiteralPath $item.target -Force}
                if($item.hadOld){[IO.File]::Move($item.saved,$item.target)}
            } catch {$rollbackOk=$false}
        }
    }
    foreach($temporary in $tempFiles){if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue}}
    $message=if($installed){'更新已安装，但自动重启或缓存清理未完成，请重新打开客户端。'}elseif($rollbackOk){'更新未完成，已保留原版本。'}else{'更新未完成，自动恢复失败，请使用安装目录内的 .lan3d-update-backup 备份恢复。'}
    try {Result $false ($message+' '+$errorText)} catch {}
    if($parentExited -and -not $NoRestart -and $rollbackOk -and -not $installed){Start-Process -FilePath $exe -WorkingDirectory $install -WindowStyle Normal -ErrorAction SilentlyContinue | Out-Null}
    if($NoRestart){Write-Error ($message+' '+$errorText) -ErrorAction Continue}
    exit 1
} finally {if($archive){$archive.Dispose()};if($ready){$ready.Dispose()}}
