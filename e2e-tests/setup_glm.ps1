# GLM Setup Script for Vulkan Examples
# Downloads and sets up GLM (OpenGL Mathematics) library if not found

param(
    [string]$TargetPath = "",
    [switch]$Force,
    [switch]$Verbose
)

function Write-Log {
    param([string]$Message, [string]$Level = "INFO")
    $timestamp = Get-Date -Format "HH:mm:ss"
    switch ($Level) {
        "ERROR" { Write-Host "[$timestamp] [ERROR] $Message" -ForegroundColor Red }
        "WARN"  { Write-Host "[$timestamp] [WARN] $Message" -ForegroundColor Yellow }
        "OK"    { Write-Host "[$timestamp] [OK] $Message" -ForegroundColor Green }
        default { Write-Host "[$timestamp] [INFO] $Message" -ForegroundColor Cyan }
    }
}

# Determine target path
if (!$TargetPath) {
    $vulkanExamplesPath = Join-Path $PSScriptRoot "vulkan_examples\Vulkan"
    if (Test-Path $vulkanExamplesPath) {
        $TargetPath = Join-Path $vulkanExamplesPath "external\glm"
    } else {
        $TargetPath = Join-Path $PSScriptRoot "glm"
    }
}

Write-Log "=== GLM Setup Script ==="
Write-Log "Target path: $TargetPath"

try {
    # Check if GLM is already present
    if ((Test-Path $TargetPath) -and !$Force) {
        Write-Log "GLM already exists at $TargetPath" "OK"
        
        # Verify it's a valid GLM installation
        $glmHpp = Join-Path $TargetPath "glm\glm.hpp"
        if (Test-Path $glmHpp) {
            Write-Log "GLM installation verified (found glm.hpp)" "OK"
            exit 0
        } else {
            Write-Log "GLM directory exists but glm.hpp not found. Re-downloading..." "WARN"
        }
    }
    
    # Create target directory
    $parentDir = Split-Path $TargetPath
    if (!(Test-Path $parentDir)) {
        New-Item -ItemType Directory -Path $parentDir -Force | Out-Null
        Write-Log "Created parent directory: $parentDir"
    }
    
    # Download GLM
    Write-Log "Downloading GLM from GitHub..."
    $glmUrl = "https://github.com/g-truc/glm/releases/download/0.9.9.8/glm-0.9.9.8.zip"
    $tempZip = Join-Path $env:TEMP "glm.zip"
    $tempExtract = Join-Path $env:TEMP "glm_extract"
    
    try {
        # Download
        Write-Log "Downloading: $glmUrl"
        Invoke-WebRequest -Uri $glmUrl -OutFile $tempZip -UseBasicParsing
        Write-Log "Download completed" "OK"
        
        # Extract
        Write-Log "Extracting GLM..."
        if (Test-Path $tempExtract) {
            Remove-Item $tempExtract -Recurse -Force
        }
        Expand-Archive -Path $tempZip -DestinationPath $tempExtract -Force
        
        # Find the GLM directory in the extracted content
        $extractedGlm = Get-ChildItem $tempExtract -Directory | Where-Object { $_.Name -like "glm*" } | Select-Object -First 1
        if (!$extractedGlm) {
            throw "Could not find GLM directory in extracted archive"
        }
        
        Write-Log "Found GLM directory: $($extractedGlm.FullName)"
        
        # Copy to target location
        if (Test-Path $TargetPath) {
            Remove-Item $TargetPath -Recurse -Force
        }
        
        Copy-Item $extractedGlm.FullName $TargetPath -Recurse -Force
        Write-Log "GLM copied to: $TargetPath" "OK"
        
        # Verify installation
        $glmHpp = Join-Path $TargetPath "glm\glm.hpp"
        if (Test-Path $glmHpp) {
            Write-Log "GLM installation verified successfully" "OK"
            
            # Show version info if verbose
            if ($Verbose) {
                $versionContent = Get-Content $glmHpp | Select-String "#define GLM_VERSION" | Select-Object -First 3
                Write-Log "GLM version information:"
                $versionContent | ForEach-Object { Write-Log "  $_" }
            }
        } else {
            throw "GLM installation verification failed - glm.hpp not found"
        }
        
    }
    finally {
        # Cleanup
        if (Test-Path $tempZip) { Remove-Item $tempZip -Force }
        if (Test-Path $tempExtract) { Remove-Item $tempExtract -Recurse -Force }
    }
    
    Write-Log "GLM setup completed successfully" "OK"
    Write-Log "You can now build Vulkan examples that depend on GLM"
    
    # Show usage instructions
    Write-Log ""
    Write-Log "GLM is now available at: $TargetPath"
    Write-Log "If using CMake, you may need to set GLM_ROOT_DIR or add to CMAKE_PREFIX_PATH"
    Write-Log "Example: cmake -DGLM_ROOT_DIR=`"$TargetPath`" ..."
    
}
catch {
    Write-Log "GLM setup failed: $($_.Exception.Message)" "ERROR"
    Write-Log "" 
    Write-Log "Manual installation options:" "ERROR"
    Write-Log "1. Download GLM from: https://github.com/g-truc/glm/releases" "ERROR"
    Write-Log "2. Extract to: $TargetPath" "ERROR"  
    Write-Log "3. Or use vcpkg: vcpkg install glm:x64-windows" "ERROR"
    exit 1
}
