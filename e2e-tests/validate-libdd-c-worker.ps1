param(
    [Parameter(Mandatory=$true)][string]$LibddCSourceDir,
    [Parameter(Mandatory=$true)][string]$BaselineSourceDir,
    [Parameter(Mandatory=$true)][string]$ArtifactDirectory,
    [string]$TestZstdSourceDir = '',
    [switch]$RemainingChecksOnly
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($root.StartsWith('Z:', [StringComparison]::OrdinalIgnoreCase)) { throw 'Build from C:\Repos, not SMB' }
New-Item $ArtifactDirectory -ItemType Directory -Force | Out-Null
Start-Transcript -Path (Join-Path $ArtifactDirectory 'validation.log') -Force
function Run([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe failed: $LASTEXITCODE" }
}
try {
    $capture = Join-Path $root 'build-libdd-c-capture'
    if ($RemainingChecksOnly) {
        if (-not (Test-Path (Join-Path $capture 'independent-decode.json'))) { throw 'Primary validation evidence is required before resuming.' }
        Write-Host 'REUSING primary integration validation for the previously verified source snapshot.'
        Run 'cmake' @('--build', (Join-Path $root 'build-libdd-c'), '--config', 'Release', '--target', 'Tests', '--parallel', '4')
    } else {
        & (Join-Path $PSScriptRoot 'validate-c-aggregation.ps1') -LibddCSourceDir $LibddCSourceDir -TestZstdSourceDir $TestZstdSourceDir
    }
    $baselineBuild = Join-Path $BaselineSourceDir 'build-libdd-c-baseline'
    Run 'cmake' @('-S', $BaselineSourceDir, '-B', $baselineBuild, '-G', 'Visual Studio 17 2022', '-A', 'x64',
        '-DDD_WIN_PROF_DEVELOPMENT=OFF', '-DDD_WIN_PROF_BUILD_TESTS=ON')
    Run 'cmake' @('--build', $baselineBuild, '--config', 'Release', '--target', 'Tests', '--parallel', '4')
    $filter = '--gtest_filter=PprofAggregatorTest.CustomPeriodAndDefinitionsSurviveInputLifetimeAndReset:PprofAggregatorTest.CustomSampleTypesPreserveRepeatedAndDistinctDefinitions:PprofAggregatorTest.EmbeddedNulDefinitionsSurviveReset'
    foreach ($entry in @(@('before', (Join-Path $baselineBuild 'src/Tests/Release/Tests.exe')), @('after', (Join-Path $root 'build-libdd-c/src/Tests/Release/Tests.exe')))) {
        $env:DD_WIN_PROF_TEST_PROFILE_DIR = Join-Path $capture $entry[0]
        New-Item $env:DD_WIN_PROF_TEST_PROFILE_DIR -ItemType Directory -Force | Out-Null
        Run $entry[1] @($filter)
    }
    Remove-Item Env:DD_WIN_PROF_TEST_PROFILE_DIR
    Run 'libdd-c-profile-check' @('compare', (Join-Path $capture 'before'), (Join-Path $capture 'after'))
    $asan = Join-Path $root 'build-libdd-c-asan'
    Run 'cmake' @('-S', $root, '-B', $asan, '-G', 'Visual Studio 17 2022', '-A', 'x64',
        '-DDD_WIN_PROF_DEVELOPMENT=OFF', '-DDD_WIN_PROF_BUILD_TESTS=ON', '-DDD_WIN_PROF_ENABLE_ASAN=ON',
        "-DDD_WIN_PROF_LIBDD_C_SOURCE_DIR=$LibddCSourceDir")
    Run 'cmake' @('--build', $asan, '--config', 'RelWithDebInfo', '--target', 'Tests', '--parallel', '4')
    Run (Join-Path $asan 'src/Tests/RelWithDebInfo/Tests.exe') @('--gtest_color=no', "--gtest_output=xml:$(Join-Path $capture 'asan-tests.xml')")
    if ($RemainingChecksOnly) {
        Write-Host 'PASS: deterministic before/after decode and MSVC ASan; primary integration validation reused.'
    } else {
        Write-Host 'PASS: fresh Windows libdd-c integration, deterministic before/after decode and MSVC ASan'
    }
} finally {
    foreach ($directory in @('build-libdd-c-capture', 'build-libdd-c/libdd-c-profiling/Testing', 'build-libdd-c-asan/Testing')) {
        $path = Join-Path $root $directory
        if (Test-Path $path) { Copy-Item $path $ArtifactDirectory -Recurse -Force }
    }
    foreach ($directory in @('build-libdd-c', 'build-libdd-c-consumer', 'build-libdd-c-asan')) {
        $cache = Join-Path $root "$directory/CMakeCache.txt"
        if (Test-Path $cache) { Copy-Item $cache (Join-Path $ArtifactDirectory "$directory-cache.txt") -Force }
    }
    Stop-Transcript
}
