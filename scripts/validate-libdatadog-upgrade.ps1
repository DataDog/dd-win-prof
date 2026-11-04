<#
.SYNOPSIS
    Build and validate the standalone libdatadog upgrade without external uploads.
.DESCRIPTION
    Run from a local Windows source tree. Optional BaselineArchive is a git archive
    of the pre-upgrade commit; only the shared fixture is added to that snapshot.
    All generated sources, builds, packages and results stay under BuildRoot.
#>
[CmdletBinding()]
param(
    [string]$BuildRoot = '',
    [string]$BaselineArchive = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if ($repo -notmatch '^[A-Za-z]:\\' -or $repo -match '^Z:') {
    throw 'Validation must run from a local Windows drive, not the SMB share'
}
if (-not $BuildRoot) { $BuildRoot = Join-Path $repo 'build/prof16112-upgrade' }
New-Item -ItemType Directory -Force $BuildRoot | Out-Null
$results = Join-Path $BuildRoot 'results'
New-Item -ItemType Directory -Force $results | Out-Null
Set-Location $repo
Get-ChildItem Env: | Where-Object { $_.Name -like 'DD_*' } | ForEach-Object { Remove-Item "Env:$($_.Name)" }
$env:DD_PROFILING_AUTO_START = '0'

function Run {
    Write-Host "> $($args -join ' ')"
    & $args[0] $args[1..($args.Length - 1)]
    if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $($args -join ' ')" }
}
function Same([string]$left, [string]$right) {
    if ((Get-FileHash $left -Algorithm SHA256).Hash -ne (Get-FileHash $right -Algorithm SHA256).Hash) {
        throw "Artifact mismatch: $left / $right"
    }
}
function Manifest([string]$directory, [string]$output) {
    @(Get-ChildItem $directory -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{ path = $_.FullName.Substring($directory.Length + 1).Replace('\', '/');
            size = $_.Length; sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower() }
    }) | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 $output
}

$venv = Join-Path $BuildRoot 'venv'
if (-not (Test-Path "$venv/Scripts/python.exe")) { Run python -m venv $venv }
$python = Join-Path $venv 'Scripts/python.exe'
Run $python -m pip install -r "$repo/src/integration-tests/requirements.txt"
Run $python -m unittest discover -s "$repo/src/integration-tests" -p test_pprof_utils.py -v

$sources = [ordered]@{}
if ($BaselineArchive) {
    $baseline = Join-Path $BuildRoot 'baseline-source'
    if (-not (Test-Path "$baseline/CMakeLists.txt")) {
        Expand-Archive -Path $BaselineArchive -DestinationPath $baseline
    }
    Copy-Item "$repo/src/Tests/ProfileUpgradeTests.cpp" "$baseline/src/Tests/ProfileUpgradeTests.cpp" -Force
    $testCmake = Join-Path $baseline 'src/Tests/CMakeLists.txt'
    $text = [IO.File]::ReadAllText($testCmake)
    if ($text -notmatch 'ProfileUpgradeTests.cpp') {
        [IO.File]::WriteAllText($testCmake, $text.Replace('    main.cpp', "    main.cpp`n    ProfileUpgradeTests.cpp"))
    }
    $sources['v19'] = $baseline
}
$sources['v47'] = $repo
$summary = @()
foreach ($version in $sources.Keys) {
    $source = $sources[$version]
    $build = Join-Path $BuildRoot "$version-build"
    Run cmake -S $source -B $build -G 'Visual Studio 17 2022' -A x64 -DDD_WIN_PROF_BUILD_OBFUSCATION=ON
    foreach ($config in @('Debug', 'Release')) {
        Run cmake --build $build --config $config --parallel 4
        $exe = Join-Path $build "src/Tests/$config/Tests.exe"
        $xml = Join-Path $results "$version-$config-unit.xml"
        $env:GTEST_OUTPUT_FILE = $xml
        try {
            Write-Host "> $exe (full unit suite)"
            & $exe 2>&1 | Tee-Object -FilePath (Join-Path $results "$version-$config-unit.log")
            if ($LASTEXITCODE -ne 0) { throw "Unit tests failed: $version/$config" }
        } finally { Remove-Item Env:GTEST_OUTPUT_FILE }
        [xml]$unit = Get-Content $xml
        $fixtures = Join-Path $results "$version-$config-fixtures"
        $parityArgs = @("$repo/src/integration-tests/validate_libdatadog_upgrade.py", '--tests-exe', $exe,
            '--output', $fixtures, '--codec', $(if ($version -eq 'v19') { 'lz4' } else { 'zstd' }))
        if ($version -eq 'v47' -and $BaselineArchive) {
            $parityArgs += @('--baseline', (Join-Path $results "v19-$config-fixtures"))
        }
        Run $python @parityArgs
        $stage = Join-Path $BuildRoot "$version-$config-package/dd-win-prof"
        Run cmake --install $build --config $config --prefix $stage
        foreach ($file in @('dd-win-prof.dll', 'dd-win-prof.lib', 'dd-win-prof.pdb',
                'datadog_profiling_ffi.dll', 'datadog_profiling_ffi.pdb', 'dd-win-prof.h',
                'internal/dd-win-rum-private.h', 'internal/error.h')) {
            if (-not (Test-Path "$stage/Profiler/$file")) { throw "Missing $version/$config/$file" }
        }
        if (-not (Test-Path "$stage/Obfuscation/ObfSymbols.exe")) { throw 'Missing ObfSymbols' }
        $dependency = Join-Path $build '_deps/libdatadog-src'
        $variant = $config.ToLower()
        Same "$stage/Profiler/datadog_profiling_ffi.dll" "$dependency/$variant/dynamic/datadog_profiling_ffi.dll"
        Same "$stage/Profiler/datadog_profiling_ffi.pdb" "$dependency/$variant/dynamic/datadog_profiling_ffi.pdb"
        Same "$stage/Profiler/dd-win-prof.dll" "$build/src/dd-win-prof/$config/dd-win-prof.dll"
        Same "$stage/Profiler/internal/dd-win-rum-private.h" "$source/src/dd-win-prof/include/internal/dd-win-rum-private.h"
        if ($version -eq 'v47') {
            foreach ($file in @('LICENSE', 'NOTICE', 'LICENSE-3rdparty.csv')) {
                Same "$stage/licenses/libdatadog/$file" "$dependency/$file"
            }
            $baselineStage = Join-Path $BuildRoot "v19-$config-package/dd-win-prof"
            if ($BaselineArchive) {
                $before = @(Get-ChildItem $baselineStage -Recurse -File | ForEach-Object { $_.FullName.Substring($baselineStage.Length + 1) })
                $after = @(Get-ChildItem $stage -Recurse -File | ForEach-Object { $_.FullName.Substring($stage.Length + 1) } |
                    Where-Object { $_ -notlike 'licenses\libdatadog\*' })
                if (@(Compare-Object $before $after).Count) { throw 'Package file layout changed beyond dependency attribution' }
            }
        }
        $zip = Join-Path $results "$version-$config-dd-win-prof.zip"
        if (Test-Path $zip) { Remove-Item $zip }
        Compress-Archive -Path $stage -DestinationPath $zip
        Manifest $stage (Join-Path $results "$version-$config-package-manifest.json")
        $summary += [ordered]@{ version = $version; config = $config; tests = $unit.testsuites.tests;
            failures = $unit.testsuites.failures; disabled = $unit.testsuites.disabled;
            zipSha256 = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower() }
        Write-Host "PASS: $version/$config unit, codec, decoded fixtures, HTTP policy, package parity"
    }
}

# The supported consumer test uses git status to detect source-tree writes.
if (-not (Test-Path "$repo/.git")) { Run git -C $repo init }
foreach ($config in @('Debug', 'Release')) {
    $env:DD_TRACE_LOG_DIRECTORY = Join-Path $results "consumer-$config-logs"
    $env:DD_PROFILING_LOG_LEVEL = 'debug'
    $env:DD_PROFILING_LOG_TO_CONSOLE = '1'
    $work = Join-Path $BuildRoot "consumer-$config"
    Run powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$repo/scripts/test-consumer.ps1" -Config $config -WorkDir $work
    $smoke = Join-Path $BuildRoot "package-smoke-$config"
    New-Item -ItemType Directory -Force $smoke | Out-Null
    Copy-Item "$work/install/bin/consumer.exe" $smoke -Force
    Copy-Item "$BuildRoot/v47-$config-package/dd-win-prof/Profiler/dd-win-prof.dll" $smoke -Force
    if (Test-Path "$smoke/datadog_profiling_ffi.dll") { throw 'Static smoke must not have the FFI DLL' }
    Run "$smoke/consumer.exe"
    Write-Host "PASS: $config FetchContent/installed/downstream/package-static callback consumers"
}
$summary | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $results 'summary.json')
Write-Host 'PASS: standalone libdatadog upgrade validation complete'
