param([switch]$SelfTest)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Path $dist -Force | Out-Null
$exe = Join-Path $dist 'LAN3DBandwidth.exe'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
& $compiler /nologo /target:winexe /platform:x64 /optimize+ /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll "/out:$exe" (Join-Path $PSScriptRoot 'LanBandwidth.cs')
if ($LASTEXITCODE -ne 0) { throw 'Compile failed' }
if ($SelfTest) {
    $evidence = Join-Path $root 'build\test-results\bandwidth'
    New-Item -ItemType Directory -Path $evidence -Force | Out-Null
    $portProbe = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $portProbe.Start()
    $port = $portProbe.LocalEndpoint.Port
    $portProbe.Stop()
    $serverResult = Join-Path $evidence 'server-result.txt'
    $clientResult = Join-Path $evidence 'client-result.txt'
    foreach ($item in @($serverResult, $clientResult, "$serverResult.ready")) { if (Test-Path -LiteralPath $item) { Remove-Item -LiteralPath $item -Force } }
    $server = $null
    $client = $null
    try {
        $server = Start-Process -FilePath $exe -ArgumentList @('--selftest-server', $port, ('"' + $serverResult + '"')) -WindowStyle Hidden -PassThru
        $null = $server.Handle
        $deadline = [DateTime]::UtcNow.AddSeconds(5)
        while (!(Test-Path -LiteralPath "$serverResult.ready")) { if ([DateTime]::UtcNow -ge $deadline) { throw 'Server did not start' }; Start-Sleep -Milliseconds 100 }
        $client = Start-Process -FilePath $exe -ArgumentList @('--selftest-client', $port, ('"' + $clientResult + '"')) -WindowStyle Hidden -PassThru
        $null = $client.Handle
        if (!$client.WaitForExit(16000) -or !$server.WaitForExit(16000)) { throw 'Self-test timeout' }
        if ($client.ExitCode -ne 0 -or $server.ExitCode -ne 0) { throw 'Self-test failed' }
        $a = Get-Content -LiteralPath $clientResult
        $b = Get-Content -LiteralPath $serverResult
        if ($a[0] -ne 'PASS' -or $b[0] -ne 'PASS') { throw 'Missing PASS result' }
        $clientSent = ($a | Where-Object { $_ -like 'OUT_BYTES=*' }) -replace 'OUT_BYTES=', ''
        $serverReceived = ($b | Where-Object { $_ -like 'IN_BYTES=*' }) -replace 'IN_BYTES=', ''
        $serverSent = ($b | Where-Object { $_ -like 'OUT_BYTES=*' }) -replace 'OUT_BYTES=', ''
        $clientReceived = ($a | Where-Object { $_ -like 'IN_BYTES=*' }) -replace 'IN_BYTES=', ''
        if ($clientSent -ne $serverReceived -or $serverSent -ne $clientReceived) { throw 'Receiver byte acknowledgement mismatch' }
        "PASS: two-process localhost; forward=$clientSent bytes; reverse=$serverSent bytes; both receiver acknowledgements matched. This is not a LAN bandwidth result." | Set-Content -LiteralPath (Join-Path $evidence 'selftest.txt') -Encoding UTF8
        Get-Content -LiteralPath (Join-Path $evidence 'selftest.txt')
    } finally {
        foreach ($process in @($client, $server)) { if ($null -ne $process -and !$process.HasExited) { Stop-Process -Id $process.Id -Force } }
        if (Test-Path -LiteralPath "$serverResult.ready") { Remove-Item -LiteralPath "$serverResult.ready" -Force }
    }
}
Get-Item -LiteralPath $exe | Select-Object FullName, Length, LastWriteTime
