[CmdletBinding()]
param(
    [string]$BinaryPath,
    [string]$PdbPath
)

Set-StrictMode -Version 2.0

function Assert-Lan3DByteRange {
    param([byte[]]$Data, [long]$Offset, [long]$Count)
    if ($Offset -lt 0 -or $Count -lt 0 -or $Offset -gt $Data.LongLength -or $Count -gt ($Data.LongLength - $Offset)) {
        throw "Binary structure extends beyond its file (offset $Offset, length $Count)."
    }
}

function Read-Lan3DU16 {
    param([byte[]]$Data, [long]$Offset)
    Assert-Lan3DByteRange $Data $Offset 2
    return [BitConverter]::ToUInt16($Data, [int]$Offset)
}

function Read-Lan3DU32 {
    param([byte[]]$Data, [long]$Offset)
    Assert-Lan3DByteRange $Data $Offset 4
    return [BitConverter]::ToUInt32($Data, [int]$Offset)
}

function Read-Lan3DGuid {
    param([byte[]]$Data, [long]$Offset)
    Assert-Lan3DByteRange $Data $Offset 16
    $guidBytes = New-Object byte[] 16
    [Array]::Copy($Data, $Offset, $guidBytes, 0, 16)
    return ([Guid]::new($guidBytes)).ToString('D').ToLowerInvariant()
}

function Convert-Lan3DRvaToOffset {
    param([byte[]]$Data, [long]$Rva, [long]$Count, [long]$SectionTable, [uint32]$SectionCount, [uint32]$HeaderBytes)
    if ($Rva -lt $HeaderBytes -and $Count -le ([long]$HeaderBytes - $Rva)) {
        Assert-Lan3DByteRange $Data $Rva $Count
        return $Rva
    }
    for ($i = 0; $i -lt $SectionCount; $i++) {
        $section = $SectionTable + $i * 40
        Assert-Lan3DByteRange $Data $section 40
        $address = [long](Read-Lan3DU32 $Data ($section + 12))
        $rawBytes = [long](Read-Lan3DU32 $Data ($section + 16))
        $rawOffset = [long](Read-Lan3DU32 $Data ($section + 20))
        $delta = $Rva - $address
        if ($delta -ge 0 -and $delta -le $rawBytes -and $Count -le ($rawBytes - $delta)) {
            Assert-Lan3DByteRange $Data ($rawOffset + $delta) $Count
            return ($rawOffset + $delta)
        }
    }
    throw "PE debug directory RVA $Rva is not backed by file data."
}

function Get-Lan3DCodeViewIdentity {
    param([Parameter(Mandatory = $true)][string]$Path)
    [byte[]]$data = [IO.File]::ReadAllBytes($Path)
    if ((Read-Lan3DU16 $data 0) -ne 0x5A4D) { throw "Not a PE image: $Path" }
    $pe = [long](Read-Lan3DU32 $data 60)
    if ((Read-Lan3DU32 $data $pe) -ne 0x00004550) { throw "Invalid PE signature: $Path" }
    if ((Read-Lan3DU16 $data ($pe + 4)) -ne 0x8664) { throw "Expected an x64 PE image: $Path" }
    $sections = Read-Lan3DU16 $data ($pe + 6)
    $optionalBytes = Read-Lan3DU16 $data ($pe + 20)
    $optional = $pe + 24
    Assert-Lan3DByteRange $data $optional $optionalBytes
    if ($optionalBytes -lt 168 -or (Read-Lan3DU16 $data $optional) -ne 0x20B -or (Read-Lan3DU32 $data ($optional + 108)) -lt 7) {
        throw "PE32+ debug data directory is missing: $Path"
    }
    $debugRva = [long](Read-Lan3DU32 $data ($optional + 160))
    $debugBytes = [long](Read-Lan3DU32 $data ($optional + 164))
    if ($debugRva -eq 0 -or $debugBytes -lt 28 -or $debugBytes -gt 1048576 -or ($debugBytes % 28) -ne 0) {
        throw "Invalid PE debug directory size: $Path"
    }
    $headerBytes = Read-Lan3DU32 $data ($optional + 60)
    $debugOffset = Convert-Lan3DRvaToOffset $data $debugRva $debugBytes ($optional + $optionalBytes) $sections $headerBytes
    $identity = $null
    for ($offset = $debugOffset; $offset -lt ($debugOffset + $debugBytes); $offset += 28) {
        if ((Read-Lan3DU32 $data ($offset + 12)) -ne 2) { continue }
        $recordBytes = [long](Read-Lan3DU32 $data ($offset + 16))
        $record = [long](Read-Lan3DU32 $data ($offset + 24))
        if ($recordBytes -lt 24) { continue }
        Assert-Lan3DByteRange $data $record $recordBytes
        if ((Read-Lan3DU32 $data $record) -ne 0x53445352) { continue }
        $next = [pscustomobject]@{
            Guid = Read-Lan3DGuid $data ($record + 4)
            Age = Read-Lan3DU32 $data ($record + 20)
        }
        if ($null -ne $identity -and ($identity.Guid -ne $next.Guid -or $identity.Age -ne $next.Age)) {
            throw "PE image contains conflicting RSDS identities: $Path"
        }
        $identity = $next
    }
    if ($null -eq $identity) { throw "PE image has no RSDS CodeView identity: $Path" }
    return $identity
}

function Get-Lan3DPdbIdentity {
    param([Parameter(Mandatory = $true)][string]$Path)
    [byte[]]$data = [IO.File]::ReadAllBytes($Path)
    Assert-Lan3DByteRange $data 0 56
    $magic = [Text.Encoding]::ASCII.GetString($data, 0, 32)
    if ($magic -cne "Microsoft C/C++ MSF 7.00`r`n$([char]26)DS$([char]0)$([char]0)$([char]0)") {
        throw "Expected an MSF 7.0 PDB: $Path"
    }
    $blockBytes = [long](Read-Lan3DU32 $data 32)
    $blockCount = [long](Read-Lan3DU32 $data 40)
    $directoryBytes = [long](Read-Lan3DU32 $data 44)
    $blockMap = [long](Read-Lan3DU32 $data 52)
    if ($blockBytes -lt 512 -or $blockBytes -gt 65536 -or ($blockBytes -band ($blockBytes - 1)) -ne 0 -or
        $blockCount -lt 1 -or ($blockCount * $blockBytes) -gt $data.LongLength -or $blockMap -ge $blockCount -or
        $directoryBytes -lt 12 -or $directoryBytes -gt 67108864) {
        throw "Invalid PDB MSF superblock: $Path"
    }
    $directoryBlocks = [long][Math]::Ceiling($directoryBytes / [double]$blockBytes)
    if (($directoryBlocks * 4) -gt $blockBytes) { throw "PDB directory uses an unsupported oversized block map: $Path" }
    $directory = New-Object byte[] ([int]$directoryBytes)
    for ($i = 0; $i -lt $directoryBlocks; $i++) {
        $block = [long](Read-Lan3DU32 $data ($blockMap * $blockBytes + $i * 4))
        if ($block -ge $blockCount) { throw "Invalid PDB directory block: $Path" }
        $copyBytes = [Math]::Min($blockBytes, $directoryBytes - $i * $blockBytes)
        Assert-Lan3DByteRange $data ($block * $blockBytes) $copyBytes
        [Array]::Copy($data, $block * $blockBytes, $directory, $i * $blockBytes, $copyBytes)
    }
    $streams = [long](Read-Lan3DU32 $directory 0)
    if ($streams -lt 2 -or $streams -gt 1000000) { throw "PDB info stream is absent: $Path" }
    Assert-Lan3DByteRange $directory 4 ($streams * 4)
    $streamZeroBytes = [long](Read-Lan3DU32 $directory 4)
    $infoBytes = [long](Read-Lan3DU32 $directory 8)
    if ($infoBytes -lt 28 -or $infoBytes -eq 4294967295) { throw "PDB info stream is truncated: $Path" }
    $zeroBlocks = 0L
    if ($streamZeroBytes -ne 4294967295) { $zeroBlocks = [long][Math]::Ceiling($streamZeroBytes / [double]$blockBytes) }
    $infoBlocks = [long][Math]::Ceiling($infoBytes / [double]$blockBytes)
    $infoMap = 4 + $streams * 4 + $zeroBlocks * 4
    Assert-Lan3DByteRange $directory $infoMap ($infoBlocks * 4)
    # The fixed 28-byte stream-1 header always fits in its first MSF page.
    $infoBlock = [long](Read-Lan3DU32 $directory $infoMap)
    if ($infoBlock -ge $blockCount) { throw "Invalid PDB info stream block: $Path" }
    $infoOffset = $infoBlock * $blockBytes
    Assert-Lan3DByteRange $data $infoOffset 28
    return [pscustomobject]@{
        Guid = Read-Lan3DGuid $data ($infoOffset + 12)
        Age = Read-Lan3DU32 $data ($infoOffset + 8)
        Version = Read-Lan3DU32 $data $infoOffset
    }
}

function Test-Lan3DSymbolPair {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Binary,
        [Parameter(Mandatory = $true)][string]$Symbols
    )
    $binaryFull = (Resolve-Path -LiteralPath $Binary -ErrorAction Stop).ProviderPath
    $pdbFull = (Resolve-Path -LiteralPath $Symbols -ErrorAction Stop).ProviderPath
    $codeView = Get-Lan3DCodeViewIdentity $binaryFull
    $pdb = Get-Lan3DPdbIdentity $pdbFull
    if ($codeView.Guid -ne $pdb.Guid -or $codeView.Age -ne $pdb.Age) {
        throw "Symbol mismatch: $binaryFull RSDS=$($codeView.Guid)/$($codeView.Age); $pdbFull PDB=$($pdb.Guid)/$($pdb.Age)"
    }
    return [pscustomobject][ordered]@{
        binary = [IO.Path]::GetFileName($binaryFull)
        pdb = [IO.Path]::GetFileName($pdbFull)
        identity = [ordered]@{ guid = $codeView.Guid; age = $codeView.Age }
        pdbInfoVersion = $pdb.Version
        binaryBytes = (Get-Item -LiteralPath $binaryFull).Length
        pdbBytes = (Get-Item -LiteralPath $pdbFull).Length
        binarySha256 = (Get-FileHash -LiteralPath $binaryFull -Algorithm SHA256).Hash.ToLowerInvariant()
        pdbSha256 = (Get-FileHash -LiteralPath $pdbFull -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

if ($BinaryPath -or $PdbPath) {
    if (-not $BinaryPath -or -not $PdbPath) { throw 'Supply both -BinaryPath and -PdbPath.' }
    Test-Lan3DSymbolPair -Binary $BinaryPath -Symbols $PdbPath
}
