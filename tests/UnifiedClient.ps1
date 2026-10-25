param([Parameter(Mandatory)][string]$BinaryDir, [Parameter(Mandatory)][string]$OutputDir, [switch]$Desktop)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$reservations = @()
$children = @()
try {
    foreach ($n in 1, 2) {
        $socket = [System.Net.Sockets.UdpClient]::new([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Loopback, 0))
        $reservations += $socket
    }
    $firstPort = $reservations[0].Client.LocalEndPoint.Port
    $secondPort = $reservations[1].Client.LocalEndPoint.Port
    $reservations | ForEach-Object { $_.Dispose() }
    $reservations = @()
    foreach ($role in 'receiver', 'sender') {
        $result = Join-Path $OutputDir "$role-result.txt"
        if (Test-Path -LiteralPath $result) { Remove-Item -LiteralPath $result }
        $port = if ($role -eq 'sender') { $firstPort } else { $secondPort }
        $target = if ($role -eq 'sender') { $secondPort } else { $firstPort }
        $parameters = @('--ui-test', $role, '--test-discovery-port', "$port", '--test-target-port', "$target", '--test-results', ('"' + $OutputDir + '"'))
        if ($Desktop) { $parameters += '--test-desktop' }
        $process = Start-Process -FilePath (Join-Path $BinaryDir 'LAN3DShare.exe') -ArgumentList $parameters -WindowStyle Hidden -PassThru
        $children += $process
        $null = $process.Handle
    }
    foreach ($child in $children) {
        if (-not $child.WaitForExit(28000)) { throw 'Unified client did not complete its session' }
        $child.WaitForExit()
        if ($child.ExitCode -ne 0) { throw "Unified client failed ($($child.ExitCode)); inspect $OutputDir" }
    }
    foreach ($role in 'sender', 'receiver') {
        $result = Get-Content -LiteralPath (Join-Path $OutputDir "$role-result.txt") -Raw -Encoding UTF8
        if ($result -notmatch "^PASS role=$role discovered=[1-9]\d* connected=1") { throw "Unexpected result: $result" }
        Write-Output $result.Trim()
    }
    $receiverLog = Get-ChildItem -LiteralPath (Join-Path $OutputDir 'logs') -File | Where-Object { $_.Name -match 'receive' } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $receiverLog) { throw 'Missing receiver worker log' }
    $text = Get-Content -LiteralPath $receiverLog.FullName -Raw -Encoding UTF8
    if ($text -notmatch 'decoded_unique=([1-9]\d*)') { throw "Receiver did not decode a frame: $text" }
    if ($text -match 'Runtime:') { throw 'Windowed receiver unexpectedly initialized LeiaSR' }
    if ($Desktop) {
        if ($text -notmatch 'DESKTOP \(2D\)') { throw 'Desktop frames did not retain their content kind at the receiver' }
        $senderLog = Get-ChildItem -LiteralPath (Join-Path $OutputDir 'logs') -File | Where-Object { $_.Name -match 'send' } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        $senderText = Get-Content -LiteralPath $senderLog.FullName -Raw -Encoding UTF8
        if ($senderText -notmatch 'SOURCE_KIND Desktop' -or $senderText -match 'SOURCE_KIND LeiaSbs') { throw 'Desktop capture was incorrectly identified as Leia SBS' }
    }
    Write-Output 'PASS two identical GUI clients: discovery -> clicked sender -> automatic receiver -> authenticated video -> disconnect, without loading LeiaSR.'
} finally {
    foreach ($socket in $reservations) { $socket.Dispose() }
    foreach ($child in $children) {
        if (-not $child.HasExited) { $child.Kill(); $child.WaitForExit() }
        $child.Dispose()
    }
}
