# Build script for ObfSymbols using CMake
# Usage: .\build.ps1 [Debug|Release] [-BuildDir <path>] [-Clean]

param(
    [Parameter(Position=0)]
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [string]$BuildDir = "",

    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot

if (-not $BuildDir) {
    $BuildDir = Join-Path $ProjectRoot "build"
}

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Building ObfSymbols (CMake)" -ForegroundColor Cyan
Write-Host "Configuration: $Configuration" -ForegroundColor Yellow
Write-Host "Build directory: $BuildDir" -ForegroundColor Yellow
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning build directory..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
}

# Configure if needed
if (-not (Test-Path (Join-Path $BuildDir "CMakeCache.txt"))) {
    Write-Host "Configuring CMake..." -ForegroundColor Green
    cmake -G "Visual Studio 17 2022" -A x64 -B "$BuildDir" -S "$ProjectRoot" -DDD_WIN_PROF_BUILD_OBFUSCATION=ON
    if ($LASTEXITCODE -ne 0) {
        Write-Error "CMake configure failed."
        exit 1
    }
}

Write-Host "Building ObfSymbols..." -ForegroundColor Green
cmake --build "$BuildDir" --config $Configuration --target ObfSymbols

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Green
    Write-Host "BUILD SUCCEEDED!" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Green
    Write-Host ""
    $exePath = Join-Path $BuildDir "obfuscation/ObfSymbols/$Configuration/ObfSymbols.exe"
    Write-Host "Output: $exePath" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Usage: $exePath --pdb <pdb_file> --out <output_file> [--obf <obfuscated_output_file>]" -ForegroundColor Cyan
    Write-Host "  If --obf is not specified, the obfuscated file will be auto-generated" -ForegroundColor Gray
} else {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Red
    Write-Host "BUILD FAILED!" -ForegroundColor Red
    Write-Host "========================================" -ForegroundColor Red
    exit $LASTEXITCODE
}
