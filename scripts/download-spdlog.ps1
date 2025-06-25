#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Downloads spdlog header-only library from GitHub releases

.DESCRIPTION
    This script downloads spdlog from GitHub releases and extracts it to the third-party directory.
    spdlog is a header-only library, so only the include directory is needed.

.PARAMETER Version
    The version of spdlog to download (default: 1.14.1)

.PARAMETER Force
    Force download even if files already exist

.EXAMPLE
    .\download-spdlog.ps1
    Downloads spdlog v1.14.1

.EXAMPLE
    .\download-spdlog.ps1 -Version "1.13.0"
    Downloads spdlog v1.13.0
#>

param(
    [string]$Version = "1.14.1",
    [switch]$Force
)

# Configuration
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RootDir = Split-Path -Parent $ScriptDir
$ThirdPartyDir = Join-Path $RootDir "third-party"
$SpdlogDir = Join-Path $ThirdPartyDir "spdlog"

# Derived values
$Filename = "spdlog-${Version}"
$ZipFile = "v${Version}.zip"
$Url = "https://github.com/gabime/spdlog/archive/refs/tags/${ZipFile}"
$ZipPath = Join-Path $ThirdPartyDir $ZipFile
$ExtractPath = Join-Path $ThirdPartyDir $Filename

function Write-Status {
    param([string]$Message, [string]$Color = "Green")
    Write-Host "[OK] $Message" -ForegroundColor $Color
}

function Write-WarningCustom {
    param([string]$Message)
    Write-Host "[WARNING] $Message" -ForegroundColor Yellow
}

function Write-ErrorCustom {
    param([string]$Message)
    Write-Host "[ERROR] $Message" -ForegroundColor Red
}

# Check if already downloaded
if ((Test-Path $SpdlogDir) -and -not $Force) {
    Write-WarningCustom "spdlog ${Version} already exists at: $SpdlogDir"
    Write-Host "Use -Force to re-download" -ForegroundColor Gray
    exit 0
}

Write-Host "Downloading spdlog ${Version}..." -ForegroundColor Cyan
Write-Host "URL: $Url" -ForegroundColor Gray

try {
    # Create directories
    if (-not (Test-Path $ThirdPartyDir)) {
        New-Item -ItemType Directory -Force -Path $ThirdPartyDir | Out-Null
        Write-Status "Created third-party directory"
    }

    # Download
    Write-Host "Downloading..." -ForegroundColor Cyan
    $ProgressPreference = 'SilentlyContinue'  # Suppress progress bar for faster download
    Invoke-WebRequest -Uri $Url -OutFile $ZipPath -ErrorAction Stop
    Write-Status "Downloaded $ZipFile"

    # Extract
    Write-Host "Extracting..." -ForegroundColor Cyan
    if (Test-Path $ExtractPath) {
        Remove-Item $ExtractPath -Recurse -Force
    }
    Expand-Archive $ZipPath -DestinationPath $ThirdPartyDir -Force
    Write-Status "Extracted to $ExtractPath"

    # Move to final location (GitHub archives extract to spdlog-{version} folder)
    if (Test-Path $SpdlogDir) {
        Remove-Item $SpdlogDir -Recurse -Force
    }
    Move-Item $ExtractPath $SpdlogDir
    Write-Status "Moved to final location: $SpdlogDir"

    # Cleanup ZIP file
    Remove-Item $ZipPath -Force
    Write-Status "Cleaned up ZIP file"

    # Verify structure
    Write-Host "Verifying structure..." -ForegroundColor Cyan
    
    $IncludeDir = Join-Path $SpdlogDir "include"
    $SpdlogIncludeDir = Join-Path $IncludeDir "spdlog"

    if (Test-Path $SpdlogIncludeDir) {
        $HeaderCount = (Get-ChildItem $SpdlogIncludeDir -Recurse -File -Filter "*.h").Count
        Write-Status "Found spdlog include directory with $HeaderCount header files"
    } else {
        Write-ErrorCustom "spdlog include directory not found!"
    }

    # Show final structure
    Write-Host "`nFinal structure:" -ForegroundColor Cyan
    Get-ChildItem $SpdlogDir -Recurse -Directory | Select-Object @{
        Name='Path'
        Expression={$_.FullName.Replace($SpdlogDir, "").TrimStart('\')}
    } | Format-Table -AutoSize

    Write-Host "`nspdlog ${Version} successfully downloaded!" -ForegroundColor Green
    Write-Host "Location: $SpdlogDir" -ForegroundColor Gray
    Write-Host "Include path: $IncludeDir" -ForegroundColor Gray
    
} catch {
    Write-ErrorCustom "Download failed: $($_.Exception.Message)"
    
    # Cleanup on failure
    if (Test-Path $ZipPath) {
        Remove-Item $ZipPath -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path $ExtractPath) {
        Remove-Item $ExtractPath -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path $SpdlogDir) {
        Remove-Item $SpdlogDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    
    exit 1
} 