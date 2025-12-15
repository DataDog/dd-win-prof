# Build script for Symbols solution
# Usage: .\build.ps1 [Debug|Release] [x64|x86]

param(
    [Parameter(Position=0)]
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [Parameter(Position=1)]
    [ValidateSet("x64", "x86")]
    [string]$Platform = "x64",

    [switch]$Clean
)

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Building Symbols Solution" -ForegroundColor Cyan
Write-Host "Configuration: $Configuration" -ForegroundColor Yellow
Write-Host "Platform: $Platform" -ForegroundColor Yellow
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Check if Visual Studio Developer PowerShell is already initialized
if (-not (Get-Command msbuild -ErrorAction SilentlyContinue)) {
    Write-Host "Initializing Visual Studio Developer Environment..." -ForegroundColor Green

    # Try to find and load Visual Studio Developer PowerShell
    $vsDevShell = "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\Launch-VsDevShell.ps1"

    if (Test-Path $vsDevShell) {
        & $vsDevShell -Arch amd64 -SkipAutomaticLocation
        # Return to solution directory after VS initialization
        Set-Location $PSScriptRoot
    } else {
        Write-Host "ERROR: Visual Studio 2022 Professional not found!" -ForegroundColor Red
        Write-Host "Please install Visual Studio 2022 or update the path in this script." -ForegroundColor Red
        exit 1
    }
}

# Determine build target
$target = if ($Clean) { "Rebuild" } else { "Build" }

Write-Host "Running MSBuild ($target)..." -ForegroundColor Green
Write-Host ""

# Build the project (slnx files need to build via the project file)
$projectPath = "ObfSymbols\ObfSymbols.vcxproj"

# Run MSBuild
msbuild $projectPath /t:$target /p:Configuration=$Configuration /p:Platform=$Platform /v:minimal

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Green
    Write-Host "BUILD SUCCEEDED!" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "Output: ObfSymbols\$Platform\$Configuration\ObfSymbols.exe" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Usage: .\ObfSymbols\$Platform\$Configuration\ObfSymbols.exe --pdb <pdb_file> --out <output_file> [--obf <obfuscated_output_file>]" -ForegroundColor Cyan
    Write-Host "  If --obf is not specified, the obfuscated file will be auto-generated" -ForegroundColor Gray
} else {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Red
    Write-Host "BUILD FAILED!" -ForegroundColor Red
    Write-Host "========================================" -ForegroundColor Red
    exit $LASTEXITCODE
}

