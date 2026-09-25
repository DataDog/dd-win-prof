# Build and validate the profiler locally on Windows. No backend upload is enabled.
[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$originalPath = $env:PATH
Push-Location $root
try {
    . (Join-Path $PSScriptRoot "rum_test_helpers.ps1")
    $python = Find-Python
    $venv = Join-Path $root "build\ui-hang-venv"
    if (-not (Test-Path (Join-Path $venv "Scripts\python.exe"))) {
        if ($python -eq "py -3") {
            & py -3 -m venv $venv
        } else {
            & $python -m venv $venv
        }
        if ($LASTEXITCODE -ne 0) { throw "Python venv creation failed: $LASTEXITCODE" }
    }
    & (Join-Path $venv "Scripts\python.exe") -m pip install -r (Join-Path $PSScriptRoot "requirements.txt")
    if ($LASTEXITCODE -ne 0) { throw "Python dependencies failed: $LASTEXITCODE" }
    $env:PATH = (Join-Path $venv "Scripts") + ";" + $env:PATH

    & cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
    & cmake --build build --config $Config
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed: $LASTEXITCODE" }
    & ctest --test-dir build -C $Config --verbose --no-tests=error --timeout 180
    if ($LASTEXITCODE -ne 0) { throw "Native tests failed: $LASTEXITCODE" }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $PSScriptRoot "test_ui_hangs.ps1") -Config $Config -KeepArtifacts
    if ($LASTEXITCODE -ne 0) { throw "UI hang integration tests failed: $LASTEXITCODE" }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $PSScriptRoot "test_ui_hangs.ps1") -Config $Config `
        -HangDurationMs 6000 -Cycles 1 -KeepArtifacts
    if ($LASTEXITCODE -ne 0) { throw "Long UI hang integration tests failed: $LASTEXITCODE" }
} finally {
    $env:PATH = $originalPath
    Pop-Location
}
