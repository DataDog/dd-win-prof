<#
.SYNOPSIS
    Generates a Visual Studio solution from CMake.

.DESCRIPTION
    Configures the CMake project and generates a Visual Studio solution in the
    build/ directory. Opens the solution in Visual Studio when done.

.PARAMETER BuildDir
    Output directory for the generated solution (default: build).

.PARAMETER Generator
    CMake generator to use (default: "Visual Studio 17 2022").

.PARAMETER NoOpen
    Skip opening the solution in Visual Studio after generation.

.EXAMPLE
    .\scripts\generate-vs.ps1
    .\scripts\generate-vs.ps1 -BuildDir out\vs -NoOpen
#>
param(
    [string]$BuildDir = "build",
    [string]$Generator = "Visual Studio 17 2022",
    [switch]$NoOpen
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $ProjectRoot
try {
    Write-Host "Generating Visual Studio solution in '$BuildDir'..." -ForegroundColor Cyan
    cmake -G "$Generator" -B "$BuildDir"
    if ($LASTEXITCODE -ne 0) {
        Write-Error "CMake configure failed."
        exit 1
    }

    $sln = Get-ChildItem -Path $BuildDir -Filter "*.sln" | Select-Object -First 1
    if (-not $sln) {
        Write-Error "No .sln file found in $BuildDir"
        exit 1
    }

    Write-Host "Solution generated: $($sln.FullName)" -ForegroundColor Green

    if (-not $NoOpen) {
        Write-Host "Opening in Visual Studio..." -ForegroundColor Cyan
        Start-Process $sln.FullName
    }
}
finally {
    Pop-Location
}
