[CmdletBinding()]
param([switch]$VerifyOnly, [string]$BuildDirectory = 'build')

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$workspace = [IO.Path]::GetFullPath($PSScriptRoot)
. (Join-Path $workspace 'scripts\Verify-SymbolPair.ps1')

$version = '0.7.7'
$baseName = "LAN3DShare-$version"
$productionNames = @('LAN3DShare.exe', 'LAN3DCapture.dll')
$buildRoot = [IO.Path]::GetFullPath((Join-Path $workspace $BuildDirectory))
$inputDirectory = Join-Path $buildRoot 'package'
$symbolDirectory = Join-Path $buildRoot 'symbols'
$pairs = @()
foreach ($name in $productionNames) {
    $pdbName = [IO.Path]::ChangeExtension($name, '.pdb')
    $pairs += Test-Lan3DSymbolPair -Binary (Join-Path $inputDirectory $name) -Symbols (Join-Path $symbolDirectory $pdbName)
}
if ($VerifyOnly) { $pairs; return }

# Only this explicit list is packaged. Never recurse over build/package, which
# also contains CLI utilities, tests, and fake vendor runtime fixtures.
$documentation = @(
    @{ source = 'docs\TECHNICAL_NOTES.zh-CN.md'; destination = 'docs\TECHNICAL_NOTES.zh-CN.md' },
    @{ source = 'docs\screenshots\idle.png'; destination = 'docs\screenshots\idle.png' },
    @{ source = 'docs\screenshots\picker.png'; destination = 'docs\screenshots\picker.png' },
    @{ source = 'docs\screenshots\quality.png'; destination = 'docs\screenshots\quality.png' },
    @{ source = 'docs\screenshots\sending.png'; destination = 'docs\screenshots\sending.png' },
    @{ source = 'docs\screenshots\receiving.png'; destination = 'docs\screenshots\receiving.png' },
    @{ source = 'docs\screenshots\settings.png'; destination = 'docs\screenshots\settings.png' },
    @{ source = 'docs\screenshots\viewer.png'; destination = 'docs\screenshots\viewer.png' },
    @{ source = 'build-opt\test-results\audio-protection.xml'; destination = 'validation\audio-protection.xml' },
    @{ source = 'build-opt\test-results\audio-protection-sender-observation.json'; destination = 'validation\async-scan-live-observation.json' },
    @{ source = 'build-opt\test-results\async-process-scan.xml'; destination = 'validation\async-process-scan.xml' },
    @{ source = 'build-opt\test-results\desktop-perf\comparison.json'; destination = 'validation\gpu-pipeline-comparison.json' },
    @{ source = 'build-opt\test-results\cross-gpu-end-to-end\result.json'; destination = 'validation\cross-gpu-sbs.json' },
    @{ source = 'build-opt\test-results\cross-gpu-end-to-end\sender.stdout.log'; destination = 'validation\cross-gpu-sbs-sender.log' },
    @{ source = 'build-opt\test-results\gpu-pacing-regression.xml'; destination = 'validation\gpu-pacing-regression.xml' },
    @{ source = 'build-opt\test-results\gpu-pipeline-regression.xml'; destination = 'validation\gpu-pipeline-regression.xml' },
    @{ source = 'build-opt\test-results\gpu-selection\result.json'; destination = 'validation\gpu-selection.json' },
    @{ source = 'README.md'; destination = 'README.md' },
    @{ source = 'VALIDATION.md'; destination = 'VALIDATION.md' },
    @{ source = 'src\net\Protocol.md'; destination = 'Protocol.md' },
    @{ source = 'src\session\Protocol.md'; destination = 'DiscoveryProtocol.md' },
    @{ source = 'src\capture\README.md'; destination = 'Capture.md' },
    @{ source = 'src\receiver\README.md'; destination = 'Receiver.md' },
    @{ source = 'third_party\minhook\LICENSE.txt'; destination = 'licenses\MinHook.txt' },
    @{ source = 'third_party\nvcodec\LICENSE'; destination = 'licenses\nv-codec-headers.txt' },
    @{ source = 'third_party\nvcodec\README.md'; destination = 'licenses\nv-codec-headers-source.md' },
    @{ source = 'third_party\webview2\LICENSE.txt'; destination = 'licenses\WebView2.txt' },
    @{ source = 'third_party\webview2\README.md'; destination = 'licenses\WebView2-source.md' },
    @{ source = 'third_party\nlohmann\LICENSE.MIT'; destination = 'licenses\nlohmann-json.txt' },
    @{ source = 'build\test-results\web-ui\results.json'; destination = 'validation\web-ui.json' },
    @{ source = 'build-opt\test-results\stream-recovery\result.json'; destination = 'validation\stream-recovery.json' },
    @{ source = 'build-opt\test-results\audio-system\result.json'; destination = 'validation\audio-system.json' },
    @{ source = 'build-opt\test-results\audio-volume\result.json'; destination = 'validation\audio-volume.json' },
    @{ source = 'build-opt\test-results\audio-volume\before.log'; destination = 'validation\audio-volume-before.log' },
    @{ source = 'build-opt\test-results\audio-volume\reference.log'; destination = 'validation\audio-volume-reference.log' },
    @{ source = 'build-opt\test-results\audio-volume\live-gain.log'; destination = 'validation\audio-volume-live.log' },
    @{ source = 'build-opt\test-results\audio-gain-ui\result.json'; destination = 'validation\audio-gain-ui.json' },
    @{ source = 'build-opt\test-results\receiver-volume\ui.json'; destination = 'validation\receiver-volume-ui.json' },
    @{ source = 'build-opt\test-results\receiver-volume\tests.xml'; destination = 'validation\receiver-volume-tests.xml' },
    @{ source = 'build-opt\test-results\audio-meter\tests.xml'; destination = 'validation\audio-meter-tests.xml' },
    @{ source = 'build-opt\test-results\audio-meter\ui.json'; destination = 'validation\audio-meter-ui.json' },
    @{ source = 'build-opt\test-results\audio-meter\before.json'; destination = 'validation\audio-clock-before.json' },
    @{ source = 'build-opt\test-results\audio-meter\system-result.json'; destination = 'validation\audio-clock-system.json' },
    @{ source = 'build-opt\test-results\program-follow-072\fixed-launch-sender.log'; destination = 'validation\program-follow-launch.log' },
    @{ source = 'build-opt\test-results\program-follow-072\reuse-200-sender.log'; destination = 'validation\program-follow-reuse.log' },
    @{ source = 'build-opt\test-results\program-follow-072\loader-final.xml'; destination = 'validation\capture-loader.xml' },
    @{ source = 'build-opt\test-results\program-follow-072\old-loader-negative-final.log'; destination = 'validation\capture-loader-negative.log' },
    @{ source = 'build-opt\test-results\ui-source-state\result.json'; destination = 'validation\ui-source-state.json' },
    @{ source = 'build\benchmarks\hardware-results.json'; destination = 'validation\hardware-codec.json' },
    @{ source = 'build\test-results\bandwidth\lan-result.txt'; destination = 'validation\lan-bandwidth.txt' },
    @{ source = 'build\test-results\perf-baseline\result.json'; destination = 'validation\performance-baseline.json' },
    @{ source = 'build\test-results\perf-baseline\sender.log'; destination = 'validation\performance-baseline-sender.log' },
    @{ source = 'build-gpu\benchmarks\sender-pipeline-baseline.json'; destination = 'validation\cpu-pipeline-baseline.json' },
    @{ source = 'build-media-gpu\test-results\nvenc-gpu.xml'; destination = 'validation\gpu-codec-timing.xml' },
    @{ source = 'build-gpu-pipeline\test-results\gpu-end-to-end\result.json'; destination = 'validation\gpu-end-to-end.json' },
    @{ source = 'build-gpu-pipeline\test-results\gpu-sender-wire-sink\result.json'; destination = 'validation\gpu-sender-wire-sink.json' },
    @{ source = 'build-opt\test-results\live-gpu-070\performance-summary.json'; destination = 'validation\live-gpu-performance.json' },
    @{ source = 'build-opt\test-results\live-gpu-070\pixel-comparison.json'; destination = 'validation\live-gpu-pixels.json' },
    @{ source = 'build-opt\test-results\live-gpu-070\wire-default-split-long\sender.log'; destination = 'validation\live-gpu-sender.log' },
    @{ source = 'build-opt\test-results\live-gpu-070\wire-default-split-long\wire-sink.log'; destination = 'validation\live-gpu-wire.log' },
    @{ source = 'build-gpu-pipeline\test-results\nvenc-auto-split-071\result.json'; destination = 'validation\nvenc-auto-split.json' },
    @{ source = 'build-gpu-pipeline\test-results\nvenc-auto-split-071\automatic.log'; destination = 'validation\nvenc-auto-split.log' },
    @{ source = 'build-gpu-pipeline\test-results\nvenc-auto-split-071\override0.log'; destination = 'validation\nvenc-single-engine.log' },
    @{ source = 'build\test-results\ctest.xml'; destination = 'validation\ctest.xml' },
    @{ source = 'build\test-results\build-info.json'; destination = 'validation\build-info.json' },
    @{ source = 'build\test-results\live-hotattach-061\result.json'; destination = 'validation\live-hotattach.json' },
    @{ source = 'build\test-results\live-hotattach-061\pixel-comparison.json'; destination = 'validation\live-pixels.json' }
)
foreach ($item in $documentation) {
    if (-not (Test-Path -LiteralPath (Join-Path $workspace $item.source) -PathType Leaf)) {
        throw "Packaging input is missing: $($item.source)"
    }
}

$dist = [IO.Path]::GetFullPath((Join-Path $workspace 'dist'))
if (Test-Path -LiteralPath $dist) {
    $distItem = Get-Item -LiteralPath $dist -Force
    if (-not $distItem.PSIsContainer -or ($distItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'dist must be a regular directory inside this workspace, not a link.'
    }
} else { New-Item -ItemType Directory -Path $dist | Out-Null }

function Assert-Lan3DDistChild {
    param([string]$Path)
    $resolved = [IO.Path]::GetFullPath($Path)
    $prefix = $dist.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Refusing an output path outside dist: $resolved" }
    return $resolved
}

function Get-Lan3DPayloadHashes {
    param([string]$Directory)
    $prefix = $Directory.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    foreach ($file in (Get-ChildItem -LiteralPath $Directory -File -Recurse | Sort-Object FullName)) {
        [pscustomobject][ordered]@{
            path = $file.FullName.Substring($prefix.Length).Replace('\', '/')
            bytes = $file.Length
            sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
}

$stage = Assert-Lan3DDistChild (Join-Path $dist ('.stage-' + $baseName + '-' + [Guid]::NewGuid().ToString('N')))
$runtimeStage = Join-Path $stage 'runtime'
$symbolsStage = Join-Path $stage 'symbols'
$utf8 = New-Object Text.UTF8Encoding($false)
try {
    New-Item -ItemType Directory -Path $runtimeStage, $symbolsStage | Out-Null
    foreach ($pair in $pairs) {
        $binaryCopy = Join-Path $runtimeStage $pair.binary
        $pdbCopy = Join-Path $symbolsStage $pair.pdb
        Copy-Item -LiteralPath (Join-Path $inputDirectory $pair.binary) -Destination $binaryCopy
        Copy-Item -LiteralPath (Join-Path $symbolDirectory $pair.pdb) -Destination $pdbCopy
        # Reject a concurrent rebuild after pair verification instead of writing
        # a manifest for different bytes than those put into the archives.
        if ((Get-FileHash -LiteralPath $binaryCopy -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pair.binarySha256 -or
            (Get-FileHash -LiteralPath $pdbCopy -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pair.pdbSha256) {
            throw "Build output changed while packaging $($pair.binary); finish building and retry."
        }
    }
    foreach ($item in $documentation) {
        $destination = Join-Path $runtimeStage $item.destination
        $parent = Split-Path -Parent $destination
        if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent | Out-Null }
        Copy-Item -LiteralPath (Join-Path $workspace $item.source) -Destination $destination
    }
    $manifest = [ordered]@{
        product = 'LAN3DShare'
        version = $version
        platform = 'Windows x64'
        createdUtc = [DateTime]::UtcNow.ToString('o')
        symbolVerification = 'PE CodeView RSDS GUID+age equals MSF7 PDB stream 1 GUID+age'
        pairs = $pairs
        runtimeFiles = @(Get-Lan3DPayloadHashes $runtimeStage)
        symbolFiles = @(Get-Lan3DPayloadHashes $symbolsStage)
    }
    $manifestJson = $manifest | ConvertTo-Json -Depth 8
    [IO.File]::WriteAllText((Join-Path $runtimeStage 'manifest.json'), $manifestJson, $utf8)
    [IO.File]::WriteAllText((Join-Path $symbolsStage 'manifest.json'), $manifestJson, $utf8)

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $runtimeZipName = "$baseName-win64.zip"
    $symbolZipName = "$baseName-symbols.zip"
    $runtimeZip = Join-Path $stage $runtimeZipName
    $symbolZip = Join-Path $stage $symbolZipName
    [IO.Compression.ZipFile]::CreateFromDirectory($runtimeStage, $runtimeZip, [IO.Compression.CompressionLevel]::Optimal, $false)
    [IO.Compression.ZipFile]::CreateFromDirectory($symbolsStage, $symbolZip, [IO.Compression.CompressionLevel]::Optimal, $false)
    foreach ($zip in @($runtimeZip, $symbolZip)) {
        $destination = Assert-Lan3DDistChild (Join-Path $dist ([IO.Path]::GetFileName($zip)))
        if (Test-Path -LiteralPath $destination -PathType Container) { throw "Archive output is a directory: $destination" }
        Move-Item -LiteralPath $zip -Destination $destination -Force
    }
    $manifestName = "$baseName-manifest.json"
    $manifestOutput = Assert-Lan3DDistChild (Join-Path $dist $manifestName)
    [IO.File]::WriteAllText($manifestOutput, $manifestJson, $utf8)
    $checksumLines = foreach ($name in @($runtimeZipName, $symbolZipName, $manifestName)) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $dist $name) -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $name"
    }
    $checksumsOutput = Assert-Lan3DDistChild (Join-Path $dist "$baseName-SHA256SUMS.txt")
    [IO.File]::WriteAllLines($checksumsOutput, [string[]]$checksumLines, $utf8)
    [pscustomobject]@{
        Runtime = Join-Path $dist $runtimeZipName
        Symbols = Join-Path $dist $symbolZipName
        Manifest = $manifestOutput
        Checksums = $checksumsOutput
        VerifiedPairs = $pairs.Count
    }
} finally {
    $cleanup = Assert-Lan3DDistChild $stage
    if (Test-Path -LiteralPath $cleanup) {
        $stageItem = Get-Item -LiteralPath $cleanup -Force
        if (-not $stageItem.PSIsContainer -or ($stageItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Refusing to recursively remove a non-directory or linked packaging stage.'
        }
        Remove-Item -LiteralPath $cleanup -Recurse -Force
    }
}
