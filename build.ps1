param([string]$SdkRoot = 'F:\GithubMods\LeiaSR-SDK-1.36.2-win64', [switch]$SkipTests)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    cmake --preset windows-x64 "-DLEIA_SDK_ROOT=$SdkRoot"
    if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
    cmake --build --preset release -- /m:1 /nr:false
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
    if (-not $SkipTests) {
        ctest --preset release --output-on-failure --output-junit test-results/ctest.xml
        if ($LASTEXITCODE -ne 0) { throw 'Offline checks failed' }
        & (Join-Path $PSScriptRoot 'scripts\Write-ValidationEvidence.ps1')
    }
    Write-Output "Binaries: $PSScriptRoot\build\package"
} finally { Pop-Location }
