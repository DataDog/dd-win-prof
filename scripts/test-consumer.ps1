<#
.SYNOPSIS
    Build a small project that consumes dd-win-prof via FetchContent and check that
    nothing meant only for dd-win-prof developers leaks into it.

.DESCRIPTION
    Configures, builds and installs scripts/consumer-test (a C++17 executable that links
    the dd-win-prof target) with the Visual Studio generator, then verifies that:
      - GoogleTest was not downloaded
      - no test / tool projects (Tests, Runner, ProfilerInjector, ObfSymbols, gtest...)
        were generated
      - dd-win-prof's source tree was not modified (e.g. no files staged into
        src/reference)
      - `cmake --install` installs only the consumer's own executable
      - none of the cache variables dd-win-prof sets for spdlog / GoogleTest leaked
        into the consumer's CMake cache
    The compile step additionally fails if dd-win-prof exposes internal headers (see
    consumer-test/main.cpp) or if dd-win-prof.h isn't self-contained.

    Run by the "Consume as dependency" job in .github/workflows/test.yml.

.PARAMETER Config
    Build configuration. Default: Release.

.PARAMETER WorkDir
    Scratch directory for the consumer build. Deleted first. Default: a temp directory.

.PARAMETER FetchContentBaseDir
    Optional FETCHCONTENT_BASE_DIR, to reuse a dependency cache (used by CI).

.EXAMPLE
    .\scripts\test-consumer.ps1
#>
[CmdletBinding()]
param(
    [string]$Config = 'Release',
    [string]$WorkDir = (Join-Path ([IO.Path]::GetTempPath()) 'dd-win-prof-consumer'),
    [string]$FetchContentBaseDir = ''
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $WorkDir 'build'
$installDir = Join-Path $WorkDir 'install'
$depsDir = if ($FetchContentBaseDir) { $FetchContentBaseDir } else { Join-Path $buildDir '_deps' }

function Invoke-Checked {
    & $args[0] $args[1..($args.Length - 1)]
    if ($LASTEXITCODE -ne 0) {
        Write-Error "'$($args -join ' ')' failed with exit code $LASTEXITCODE"
        exit 1
    }
}

# Snapshot of the working tree, ignoring the dependency cache CI keeps inside it.
function Get-SourceTreeState {
    git -C $repoRoot status --porcelain --untracked-files=all -- . ':(exclude).deps'
}

if (Test-Path $WorkDir) { Remove-Item -Recurse -Force $WorkDir }
$stateBefore = @(Get-SourceTreeState)

# FetchContent mishandles backslashes in paths it writes to generated files.
$configureArgs = @(
    '-G', 'Visual Studio 17 2022', '-A', 'x64',
    '-S', (Join-Path $PSScriptRoot 'consumer-test'),
    '-B', $buildDir,
    ('-DDD_WIN_PROF_SOURCE_DIR=' + ($repoRoot -replace '\\', '/'))
)
if ($FetchContentBaseDir) {
    $configureArgs += ('-DFETCHCONTENT_BASE_DIR=' + ($FetchContentBaseDir -replace '\\', '/'))
}

Invoke-Checked cmake @configureArgs
Invoke-Checked cmake --build $buildDir --config $Config
Invoke-Checked cmake --install $buildDir --config $Config --prefix $installDir

$failures = @()

# GoogleTest must not have been fetched.
$gtest = @(Get-ChildItem $depsDir -Directory -Filter 'googletest*' -ErrorAction SilentlyContinue)
if ($gtest.Count -gt 0) {
    $failures += "GoogleTest was fetched: $($gtest.Name -join ', ')"
}

# No test or tool projects may have been generated.
$unwanted = '^(Tests|Runner|Runner_runtime_deps|ProfilerInjector|ObfSymbols|TestSymbols.*|gtest.*|gmock.*|Continuous|Experimental|Nightly.*)$'
$projects = @(
    Get-ChildItem $buildDir -Recurse -Filter '*.vcxproj' |
        Where-Object { $_.FullName -notmatch '-subbuild|CMakeFiles' } |
        ForEach-Object { $_.BaseName }
)
$leaked = @($projects | Where-Object { $_ -match $unwanted })
if ($leaked.Count -gt 0) {
    $failures += "Developer-only projects were generated: $($leaked -join ', ')"
}

# dd-win-prof's source tree must be untouched.
$stateAfter = @(Get-SourceTreeState)
$changed = @(Compare-Object $stateBefore $stateAfter | ForEach-Object { $_.InputObject })
if ($changed.Count -gt 0) {
    $failures += "The source tree was modified by the consumer build:`n  $($changed -join "`n  ")"
}

# Only the consumer's own executable may be installed.
$installed = @(
    Get-ChildItem $installDir -Recurse -File |
        ForEach-Object { $_.FullName.Substring($installDir.Length + 1) -replace '\\', '/' }
)
if (($installed -join ',') -ne 'bin/consumer.exe') {
    $failures += "Unexpected install contents: $($installed -join ', ')"
}

# Settings dd-win-prof applies to spdlog / GoogleTest must not leak into our cache.
$leakedCache = @(
    Select-String -Path (Join-Path $buildDir 'CMakeCache.txt') `
        -Pattern '^(SPDLOG_INSTALL|SPDLOG_BUILD_EXAMPLE|SPDLOG_BUILD_TESTS|SPDLOG_BUILD_BENCH|INSTALL_GTEST|gtest_force_shared_crt):' |
        ForEach-Object { $_.Line }
)
if ($leakedCache.Count -gt 0) {
    $failures += "Cache variables leaked into the consumer: $($leakedCache -join ', ')"
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Host "FAIL: $_" -ForegroundColor Red }
    exit 1
}
Write-Host 'OK: dd-win-prof is consumable as a dependency without leaking developer-only content' -ForegroundColor Green
