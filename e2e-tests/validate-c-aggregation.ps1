# Self-contained Windows validation. Run from the mirrored experimental checkout.
# No credentials, external HTTP uploads, shared worker edits or SDK source changes.
param([string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$pwsh = Get-Command pwsh.exe -CommandType Application -ErrorAction SilentlyContinue
if (-not $pwsh) { throw 'PowerShell 7+ (pwsh.exe) is required for UI-hang integration validation; install it and retry.' }
$pwshPath = $pwsh.Source
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build-c-aggregation'
$consumer = Join-Path $root 'build-c-aggregation-consumer'
$stage = Join-Path $root 'build-c-aggregation-stage'
$capture = Join-Path $root 'build-c-aggregation-capture'
function Run([string]$exe, [string[]]$arguments) {
    Write-Host ("RUN " + $exe + " " + ($arguments -join ' '))
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "${exe} failed with exit code $LASTEXITCODE" }
}
function NoRustFiles([string]$path) {
    $bad = @(Get-ChildItem $path -Recurse -File | Where-Object { $_.Name -match 'datadog_profiling_ffi|libdatadog' })
    if ($bad.Count -gt 0) { throw "Unexpected libdatadog artifact: $($bad.FullName -join ', ')" }
}
# Fresh stage/capture only; never delete unrelated checkout/build directories.
foreach ($dir in @($stage, $capture)) {
    if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
    New-Item $dir -ItemType Directory -Force | Out-Null
}
Run 'cmake' @('-S', $root, '-B', $build, '-G', 'Visual Studio 17 2022', '-A', 'x64',
    '-DDD_WIN_PROF_DEVELOPMENT=OFF', '-DDD_WIN_PROF_BUILD_TESTS=ON', '-DDD_WIN_PROF_BUILD_RUNNER=ON', '-DDD_WIN_PROF_BUILD_UIAPP=ON',
    '-DDD_WIN_PROF_BUILD_INJECTOR=OFF', '-DDD_WIN_PROF_BUILD_OBFUSCATION=OFF', '-DDD_WIN_PROF_BUILD_INSTALL=ON',
    '-DPA_BUILD_TESTS=ON', '-DPA_BUILD_BENCHMARK=ON', '-DPE_BUILD_TESTS=ON')
# Repair only the known file artifact from the early runtime-copy bug. Never
# remove an existing output directory or any unrelated path.
$runnerOutput = Join-Path $build "src/Runner/$Configuration"
if (Test-Path $runnerOutput -PathType Leaf) { Remove-Item $runnerOutput -Force }
Run 'cmake' @('--build', $build, '--config', $Configuration, '--target', 'Tests', 'Runner', 'UIApp', 'aggregation_tests', 'aggregation_collision_tests', 'aggregation_bench', 'exporter_protocol_tests', 'exporter_probe', '--parallel', '4')
$uiAppOutput = Join-Path $build "src/UIApp/$Configuration"
NoRustFiles $uiAppOutput
Run (Join-Path $build "packages/profile-aggregation/$Configuration/aggregation_tests.exe") @((Join-Path $capture 'core-native.zst.pprof'))
Run (Join-Path $build "packages/profile-aggregation/$Configuration/aggregation_collision_tests.exe") @()
Run (Join-Path $build "packages/profile-exporter/$Configuration/exporter_protocol_tests.exe") @()
Run (Join-Path $build "src/Tests/$Configuration/Tests.exe") @('--gtest_color=no')
Run (Join-Path $build "packages/profile-aggregation/$Configuration/aggregation_bench.exe") @('100000')
Run 'cmake' @('--install', $build, '--config', $Configuration, '--prefix', $stage)
NoRustFiles $stage
# Build as a source SDK dependency with development options OFF. Its executable
# retains callback bytes after StopProfiler and records a real busy-thread capture.
Run 'cmake' @('-S', (Join-Path $root 'e2e-tests/consumer'), '-B', $consumer, '-G', 'Visual Studio 17 2022', '-A', 'x64',
    "-DDD_WIN_PROF_SOURCE_DIR=$root", '-DDD_WIN_PROF_DEVELOPMENT=OFF', '-DDD_WIN_PROF_BUILD_INSTALL=OFF',
    '-DPA_BUILD_TESTS=OFF', '-DPA_BUILD_BENCHMARK=OFF', '-DPE_BUILD_TESTS=OFF')
Run 'cmake' @('--build', $consumer, '--config', $Configuration, '--target', 'consumer', '--parallel', '4')
$env:DD_C_AGGREGATION_CAPTURE_DIR = $capture
try { Run (Join-Path $consumer "$Configuration/consumer.exe") @() }
finally { Remove-Item Env:DD_C_AGGREGATION_CAPTURE_DIR -ErrorAction SilentlyContinue }
Run 'cmake' @('--install', $consumer, '--config', $Configuration, '--prefix', (Join-Path $stage 'consumer'))
NoRustFiles (Join-Path $consumer $Configuration)
NoRustFiles (Join-Path $stage 'consumer')
Run 'cmake' @('-S', (Join-Path $root 'e2e-tests/consumer/downstream'), '-B', (Join-Path $root 'build-c-aggregation-downstream'),
    '-G', 'Visual Studio 17 2022', '-A', 'x64', "-DDD_WIN_PROF_CONSUMER_PREFIX=$(Join-Path $stage 'consumer')")
Run 'cmake' @('--build', (Join-Path $root 'build-c-aggregation-downstream'), '--config', $Configuration)
# Inspect import tables with dumpbin from the VS build tools when not on PATH.
$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if (-not $dumpbin) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $dumpbinPath = Get-ChildItem (Join-Path $vs 'VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe') | Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
} else { $dumpbinPath = $dumpbin.Source }
if (-not $dumpbinPath) { throw 'dumpbin.exe not found; dependency validation incomplete' }
$symbols = & $dumpbinPath /SYMBOLS (Join-Path $build "packages/profile-aggregation/$Configuration/profile_aggregation.lib")
if ($LASTEXITCODE -ne 0) { throw 'dumpbin archive symbol inspection failed' }
$definitions = @($symbols | Where-Object { $_ -match 'External' -and $_ -notmatch '\bUNDEF\b' } | ForEach-Object {
    if ($_ -match '\|\s+(\S+)') { $Matches[1] }
})
$unexpectedSymbols = @($definitions | Where-Object { $_ -notmatch '^_?(pa_|ddpa_)|^__|^@|^\?\?_C@' })
if ($unexpectedSymbols.Count -gt 0) { throw "Unprefixed archive definitions: $($unexpectedSymbols -join ', ')" }
Write-Host "PASS: $($definitions.Count) archive definitions use pa_/ddpa_ or compiler-reserved names"
foreach ($binary in @((Join-Path $build "src/dd-win-prof/$Configuration/dd-win-prof.dll"), (Join-Path $build "src/Tests/$Configuration/Tests.exe"), (Join-Path $uiAppOutput "UIApp.exe"))) {
    $imports = & $dumpbinPath /DEPENDENTS $binary
    if ($LASTEXITCODE -ne 0) { throw 'dumpbin failed' }
    $imports | Write-Host
    if (($imports -join "`n") -match 'datadog_profiling_ffi|libdatadog') { throw "Rust dependency in $binary" }
}
# Standalone local-debug path uses the identical C core. HTTP stays disabled.
$previousExport = $env:DD_INTERNAL_PROFILING_EXPORT_ENABLED
$env:DD_INTERNAL_PROFILING_EXPORT_ENABLED = '0'
try { Run (Join-Path $build "src/Runner/$Configuration/Runner.exe") @('--scenario', '1', '--iterations', '1', '--symbolize', '--pprofdir', (Join-Path $capture 'standalone')) }
finally { $env:DD_INTERNAL_PROFILING_EXPORT_ENABLED = $previousExport }
# A mirrored worktree has no usable macOS .git pointer. Initialize only this
# local mirror so the existing consumer script can compare its source state.
if (-not (Test-Path (Join-Path $root '.git'))) { Run 'git' @('init', $root) }
Run 'powershell' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'scripts/test-consumer.ps1'),
    '-Config', $Configuration, '-WorkDir', (Join-Path (Split-Path -Parent $root) 'prof-16455-ci-consumer'))
$pythonEnv = Join-Path $root 'build-c-aggregation-python'
Run 'python' @('-m', 'venv', $pythonEnv)
$python = Join-Path $pythonEnv 'Scripts/python.exe'
Run $python @('-m', 'pip', 'install', '-r', (Join-Path $root 'src/integration-tests/requirements.txt'))
$previousTestsExe = $env:DD_WIN_PROF_TESTS_EXE
$env:DD_WIN_PROF_TESTS_EXE = Join-Path $build "src/Tests/$Configuration/Tests.exe"
try { Run $python @('-m', 'unittest', 'discover', '-s', (Join-Path $root 'src/integration-tests'), '-p', 'test_*.py', '-v') }
finally { $env:DD_WIN_PROF_TESTS_EXE = $previousTestsExe }
$mock = Join-Path $root 'packages/profile-exporter/tests/mock_intake.py'
$received = Join-Path $capture 'winhttp-received.zst.pprof'
Run $python @($mock, '--probe', (Join-Path $build "packages/profile-exporter/$Configuration/exporter_probe.exe"), '--output', $received)
Run $python @($mock, '--decode', $received)
$previousPath = $env:PATH
$env:PATH = (Join-Path $pythonEnv 'Scripts') + ';' + $env:PATH
try {
    Run 'powershell' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'src/integration-tests/test_rum_scenario.ps1'),
        '-Config', $Configuration, '-Iterations', '3', '-RunnerPath', (Join-Path $build "src/Runner/$Configuration/Runner.exe"))
    Run $pwshPath @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'src/integration-tests/test_ui_hangs.ps1'),
        '-Config', $Configuration, '-UIAppPath', (Join-Path $uiAppOutput 'UIApp.exe'),
        '-OutputRoot', (Join-Path $capture 'ui-hangs'), '-KeepArtifacts')
} finally { $env:PATH = $previousPath }
Write-Host "PASS: Windows core, behavioral tests, callback lifecycle/capture, WinHTTP loopback/rejection/redirect/timeout/refusal and decoded pprof, local standalone/RUM/UI-hang capture and restart, existing consumer isolation, install/export/import inventories. Artifacts: $capture"
Write-Host 'NOT VALIDATED: dd-sdk-cpp dirty checkout install rules, live SDK intake, real standalone intake/backend indexing, integrated memory budget.'
