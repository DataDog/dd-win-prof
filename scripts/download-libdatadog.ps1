#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Downloads libdatadog prebuilt binaries for Windows

.DESCRIPTION
    This script downloads libdatadog from GitHub releases and extracts it to the third-party directory.
    Supports both x64 and x86 platforms, and both dynamic and static linking.

.PARAMETER Version
    The version of libdatadog to download (default: 19.0.0)

.PARAMETER Platform
    The platform to download: x64 or x86 (default: x64)

.PARAMETER Force
    Force download even if files already exist

.EXAMPLE
    .\download-libdatadog.ps1
    Downloads libdatadog v19.0.0 for x64

.EXAMPLE
    .\download-libdatadog.ps1 -Version "18.0.0" -Platform x86
    Downloads libdatadog v18.0.0 for x86
#>

param(
    [string]$Version = "19.0.0",
    [ValidateSet("x64", "x86")]
    [string]$Platform = "x64",
    [switch]$Force
)

# Configuration
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RootDir = Split-Path -Parent $ScriptDir
$ThirdPartyDir = Join-Path $RootDir "third-party"
$LibDatadogDir = Join-Path $ThirdPartyDir "libdatadog"

# Derived values
$Filename = "libdatadog-${Platform}-windows"
$ZipFile = "${Filename}.zip"
$Url = "https://github.com/DataDog/libdatadog/releases/download/v${Version}/${ZipFile}"
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
if ((Test-Path $ExtractPath) -and -not $Force) {
    Write-WarningCustom "libdatadog ${Version} (${Platform}) already exists at: $ExtractPath"
    Write-Host "Use -Force to re-download" -ForegroundColor Gray
    exit 0
}

Write-Host "Downloading libdatadog ${Version} for ${Platform}..." -ForegroundColor Cyan
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

    # Cleanup ZIP file
    Remove-Item $ZipPath -Force
    Write-Status "Cleaned up ZIP file"

    # Verify structure
    Write-Host "Verifying structure..." -ForegroundColor Cyan
    
    $IncludeDir = Join-Path $ExtractPath "include"
    $ReleaseDir = Join-Path $ExtractPath "release"
    $DebugDir = Join-Path $ExtractPath "debug"

    if (Test-Path $IncludeDir) {
        $HeaderCount = (Get-ChildItem $IncludeDir -Recurse -File).Count
        Write-Status "Found include directory with $HeaderCount header files"
    } else {
        Write-ErrorCustom "Include directory not found!"
    }

    if (Test-Path $ReleaseDir) {
        $ReleaseFiles = Get-ChildItem $ReleaseDir -Recurse -File | Select-Object -ExpandProperty Name
        Write-Status "Found release directory with files: $($ReleaseFiles -join ', ')"
    } else {
        Write-ErrorCustom "Release directory not found!"
    }

    if (Test-Path $DebugDir) {
        $DebugFiles = Get-ChildItem $DebugDir -Recurse -File | Select-Object -ExpandProperty Name
        Write-Status "Found debug directory with files: $($DebugFiles -join ', ')"
    } else {
        Write-WarningCustom "Debug directory not found (might be normal)"
    }

    # Show final structure
    Write-Host "`nFinal structure:" -ForegroundColor Cyan
    Get-ChildItem $ExtractPath -Recurse | Select-Object @{
        Name='Path'
        Expression={$_.FullName.Replace($ExtractPath, "").TrimStart('\')}
    }, Length | Format-Table -AutoSize

    Write-Host "`nlibdatadog ${Version} (${Platform}) successfully downloaded!" -ForegroundColor Green
    Write-Host "Location: $ExtractPath" -ForegroundColor Gray
    
} catch {
    Write-ErrorCustom "Download failed: $($_.Exception.Message)"
    
    # Cleanup on failure
    if (Test-Path $ZipPath) {
        Remove-Item $ZipPath -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path $ExtractPath) {
        Remove-Item $ExtractPath -Recurse -Force -ErrorAction SilentlyContinue
    }
    
    exit 1
} 