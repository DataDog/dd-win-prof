<#
.SYNOPSIS
    Build a small project that consumes dd-win-prof via FetchContent and check that
    nothing meant only for dd-win-prof developers leaks into it.

.DESCRIPTION
    Configures, builds and installs e2e-tests/consumer (a C++17 executable that links
    the dd-win-prof target) with the Visual Studio generator, then verifies that:
      - GoogleTest was not downloaded
      - no test / tool projects (Tests, Runner, ProfilerInjector, ObfSymbols, gtest...)
        were generated
      - dd-win-prof's source tree was not modified (e.g. no files staged into
        src/reference)
      - `cmake --install` installs only what the consumer asked for (its executable and
        the dd-win-prof export it declares, as the README documents), none of
        dd-win-prof's own install rules (Profiler/, Obfuscation/, licenses)
      - none of the cache variables dd-win-prof sets for spdlog / GoogleTest leaked
        into the consumer's CMake cache
      - a separate project (consumer/downstream) can configure, compile and link
        against the installed, exported dd-win-prof target (include path, header
        install and import library)
      - consumers can register the SDK callback and retain a profile after stop
        through both FetchContent and installed headers
    The compile steps additionally fail if dd-win-prof exposes implementation headers
    (see consumer/main.cpp) or either published header isn't self-contained.

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

# Like Invoke-Checked, but returns whether the command succeeded instead of exiting.
function Invoke-Soft {
    & $args[0] $args[1..($args.Length - 1)] | Out-Host
    return ($LASTEXITCODE -eq 0)
}

function Invoke-Consumer([string]$Path) {
    $previousAutoStart = $env:DD_PROFILING_AUTO_START
    try {
        $env:DD_PROFILING_AUTO_START = '0'
        & $Path | Out-Host
        return ($LASTEXITCODE -eq 0)
    } finally {
        $env:DD_PROFILING_AUTO_START = $previousAutoStart
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
    '-S', (Join-Path $repoRoot 'e2e-tests/consumer'),
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
if (-not (Invoke-Consumer (Join-Path $buildDir "$Config/consumer.exe"))) {
    $failures += 'FetchContent consumer failed SDK callback delivery'
}
if (-not (Invoke-Consumer (Join-Path $installDir 'bin/consumer.exe'))) {
    $failures += 'Installed consumer failed SDK callback delivery'
}

# A project that only has the installed prefix must be able to use the exported target:
# the include path has to resolve (dd-win-prof.h installed under <prefix>/include) and
# the import library has to be present.
$downstreamDir = Join-Path $WorkDir 'downstream'
$ok = Invoke-Soft cmake -G 'Visual Studio 17 2022' -A x64 `
    -S (Join-Path $repoRoot 'e2e-tests/consumer/downstream') -B $downstreamDir `
    ('-DDD_WIN_PROF_CONSUMER_PREFIX=' + ($installDir -replace '\\', '/'))
if ($ok) { $ok = Invoke-Soft cmake --build $downstreamDir --config $Config }
if ($ok) { $ok = Invoke-Consumer (Join-Path $downstreamDir "$Config/downstream.exe") }
if (-not $ok) {
    $failures += 'A project using the installed, exported dd-win-prof target failed to configure, build or run its SDK callback'
}

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

# Only what the consumer's own install rules ask for may be installed; anything else
# (e.g. Profiler/, Obfuscation/, licenses) comes from dd-win-prof's own install rules.
$expectedInstalled = @(
    'bin/consumer.exe',
    'bin/dd-win-prof.dll',
    'include/dd-win-prof.h',
    'include/dd-win-prof-diagnostics.h',
    'include/internal/dd-win-rum-private.h',
    'lib/cmake/consumer/ConsumerTargets.cmake',
    "lib/cmake/consumer/ConsumerTargets-$($Config.ToLower()).cmake",
    'lib/dd-win-prof.lib'
) | Sort-Object
$installed = @(
    Get-ChildItem $installDir -Recurse -File |
        ForEach-Object { $_.FullName.Substring($installDir.Length + 1) -replace '\\', '/' } |
        Sort-Object
)
if (($installed -join ',') -ne ($expectedInstalled -join ',')) {
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
