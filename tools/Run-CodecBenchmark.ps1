param([string]$CMake = 'C:\Program Files\CMake\bin\cmake.exe')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build\benchmarks'
& $CMake -S (Join-Path $PSScriptRoot 'codec-benchmark') -B $build -G 'Visual Studio 18 2026' -A x64
if ($LASTEXITCODE) { throw 'Benchmark configuration failed' }
& $CMake --build $build --config Release -- /m:1 /nr:false
if ($LASTEXITCODE) { throw 'Benchmark build failed' }
& (Join-Path $build 'Release\CodecBenchmark.exe') (Join-Path $build 'results.json')
if ($LASTEXITCODE) { throw 'Benchmark reported failures; inspect results.json' }
