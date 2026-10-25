[CmdletBinding()]
param([string[]]$Reports = @('build\test-results\ctest.xml'))
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$outputDirectory = Join-Path $workspace 'build\test-results'
$cases = [ordered]@{}
$inputs = @()
$merged = New-Object Xml.XmlDocument
$null = $merged.AppendChild($merged.CreateXmlDeclaration('1.0', 'utf-8', $null))
$suite = $merged.CreateElement('testsuite')
$suite.SetAttribute('name', 'LAN3DShare final validation')
$suite.SetAttribute('timestamp', [DateTime]::Now.ToString('s'))
$null = $merged.AppendChild($suite)
foreach ($report in $Reports) {
    $path = if ([IO.Path]::IsPathRooted($report)) { $report } else { Join-Path $workspace $report }
    [xml]$inputReport = Get-Content -LiteralPath $path -Raw -Encoding UTF8
    $inputs += [ordered]@{ Path = $report; SHA256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
    foreach ($test in $inputReport.testsuite.testcase) { $cases[$test.GetAttribute('name')] = $test }
}
if (-not $cases.Count) { throw 'No test cases in the supplied reports' }
$failures = 0; $skipped = 0; $seconds = 0.0
foreach ($test in $cases.Values) {
    if ($test.SelectSingleNode('failure') -or $test.SelectSingleNode('error')) { $failures++ }
    if ($test.SelectSingleNode('skipped')) { $skipped++ }
    $seconds += [double]::Parse($test.GetAttribute('time'), [Globalization.CultureInfo]::InvariantCulture)
    $null = $suite.AppendChild($merged.ImportNode($test, $true))
}
if ($failures) { throw "$failures test cases still fail; cannot publish successful validation evidence" }
$suite.SetAttribute('tests', [string]$cases.Count)
$suite.SetAttribute('failures', [string]$failures)
$suite.SetAttribute('skipped', [string]$skipped)
$suite.SetAttribute('time', $seconds.ToString('F3', [Globalization.CultureInfo]::InvariantCulture))
$reportOutput = Join-Path $outputDirectory 'ctest.xml'
$merged.Save($reportOutput)
$prefix = $workspace.TrimEnd('\') + '\'
$sources = @(
    Get-ChildItem -LiteralPath (Join-Path $workspace 'src'), (Join-Path $workspace 'tests'), (Join-Path $workspace 'scripts'), (Join-Path $workspace 'tools'), (Join-Path $workspace 'third_party') -Recurse -File
    Get-Item -LiteralPath (Join-Path $workspace 'CMakeLists.txt'), (Join-Path $workspace 'CMakePresets.json'), (Join-Path $workspace 'build.ps1'), (Join-Path $workspace 'package.ps1')
)
$hashes = foreach ($file in ($sources | Sort-Object FullName)) {
    [ordered]@{ Path = $file.FullName.Substring($prefix.Length); SHA256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash }
}
$info = [ordered]@{
    Version = '0.7.4'; BuiltAt = [DateTime]::Now.ToString('o'); Platform = 'Windows x64'; Configuration = 'Release'
    Toolset = 'MSVC 14.51.36231'; Sdk = 'LeiaSR 1.36.2'
    TestsPassed = $cases.Count - $skipped; TestsSkipped = $skipped; TestsFailed = $failures
    ReportComposition = 'Latest result by test name; later supplied reports replace earlier results.'
    InputReports = $inputs; TestReportSha256 = (Get-FileHash -LiteralPath $reportOutput -Algorithm SHA256).Hash
    SourceHashes = @($hashes)
}
$info | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outputDirectory 'build-info.json') -Encoding UTF8
Write-Output "Validation evidence: $($cases.Count - $skipped) passed, $skipped skipped."
