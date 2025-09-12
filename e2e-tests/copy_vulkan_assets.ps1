# Simple Vulkan Assets Copier
# Copies assets and shaders to the bin directory where executables expect them

param(
    [switch]$Debug  # Use Debug instead of Release
)

$VulkanSrc = Join-Path $PSScriptRoot "vulkan_examples\Vulkan"
$Config = if ($Debug) { "Debug" } else { "Release" }
$BinPath = Join-Path $PSScriptRoot "vulkan_examples\build\bin\$Config"

Write-Host "=== Copying Vulkan Assets to Bin Directory ===" -ForegroundColor Cyan

# Check source paths
$assetsSource = Join-Path $VulkanSrc "assets"
$shadersSource = Join-Path $VulkanSrc "shaders"

if (!(Test-Path $VulkanSrc)) {
    Write-Host "ERROR: Vulkan source not found at $VulkanSrc" -ForegroundColor Red
    exit 1
}

if (!(Test-Path $BinPath)) {
    Write-Host "ERROR: Bin directory not found at $BinPath" -ForegroundColor Red
    Write-Host "Build the project first!" -ForegroundColor Red
    exit 1
}

Write-Host "Copying assets to bin directory where executable expects them..."
Write-Host ""

# Copy assets to bin directory
if (Test-Path $assetsSource) {
    $assetsDest = Join-Path $BinPath "assets"
    Write-Host "Copying assets..." -ForegroundColor Yellow
    Write-Host "  From: $assetsSource" 
    Write-Host "  To:   $assetsDest"
    
    if (Test-Path $assetsDest) {
        Remove-Item $assetsDest -Recurse -Force
    }
    Copy-Item $assetsSource $assetsDest -Recurse -Force
    Write-Host "  Assets copied successfully" -ForegroundColor Green
} else {
    Write-Host "WARNING: Assets not found at $assetsSource" -ForegroundColor Yellow
}

# Copy shaders to bin directory
if (Test-Path $shadersSource) {
    $shadersDest = Join-Path $BinPath "shaders"
    Write-Host "Copying shaders..." -ForegroundColor Yellow
    Write-Host "  From: $shadersSource"
    Write-Host "  To:   $shadersDest"
    
    if (Test-Path $shadersDest) {
        Remove-Item $shadersDest -Recurse -Force
    }
    Copy-Item $shadersSource $shadersDest -Recurse -Force
    Write-Host "  Shaders copied successfully" -ForegroundColor Green
} else {
    Write-Host "WARNING: Shaders not found at $shadersSource" -ForegroundColor Yellow
}

# Test the fix
Write-Host ""
Write-Host "Testing computeheadless.exe..." -ForegroundColor Cyan
$computeheadless = Join-Path $BinPath "computeheadless.exe"

if (Test-Path $computeheadless) {
    Push-Location $BinPath
    try {
        $result = & .\computeheadless.exe 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-Host "computeheadless.exe works!" -ForegroundColor Green
        } else {
            Write-Host "computeheadless.exe still failing:" -ForegroundColor Red
            Write-Host $result -ForegroundColor Red
        }
    }
    finally {
        Pop-Location
    }
} else {
    Write-Host "computeheadless.exe not found - build first" -ForegroundColor Red
}

Write-Host ""
Write-Host "=== Asset Copy Complete ===" -ForegroundColor Cyan