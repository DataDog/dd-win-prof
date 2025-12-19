# Patch Vulkan Examples for Datadog Profiler Integration
# Automatically modifies the CMakeLists.txt to add profiling support

param(
    [switch]$Verbose,
    [switch]$Undo,  # Remove the patches
    [string]$ProfilerDllDir = ""  # Required: folder containing dd-win-prof.dll
)

$VulkanRoot = Join-Path $PSScriptRoot "vulkan_examples\Vulkan"
$CMakeFile = Join-Path $VulkanRoot "examples\CMakeLists.txt"
$BackupFile = "$CMakeFile.backup"

# Convert profiler DLL dir to CMake forward-slash format
if ([string]::IsNullOrWhiteSpace($ProfilerDllDir)) {
    throw "ProfilerDllDir parameter is required"
}
$cmakeProfilerDllDir = ($ProfilerDllDir -replace '\\','/')

function Write-Log {
    param([string]$Message, [string]$Level = "INFO")
    $timestamp = Get-Date -Format "HH:mm:ss"
    # Only print INFO messages when -Verbose is set; always print WARN/ERROR/OK
    if ($Level -eq "INFO" -and -not $Verbose) { return }
    switch ($Level) {
        "ERROR" { Write-Host "[$timestamp] [ERROR] $Message" -ForegroundColor Red }
        "WARN"  { Write-Host "[$timestamp] [WARN] $Message" -ForegroundColor Yellow }
        "OK"    { Write-Host "[$timestamp] [OK] $Message" -ForegroundColor Green }
        default { Write-Host "[$timestamp] [INFO] $Message" -ForegroundColor Cyan }
    }
}

function Test-AlreadyPatched {
    if (!(Test-Path $CMakeFile)) {
        return $false
    }
    
    $content = Get-Content $CMakeFile -Raw
    return $content -match "DD_PROFILER_ENABLED"
}

function Backup-CMakeFile {
    if (!(Test-Path $BackupFile)) {
        Copy-Item $CMakeFile $BackupFile
        Write-Log "Created backup: $BackupFile" "OK"
    } else {
        Write-Log "Backup already exists: $BackupFile" "OK"
    }
}

function Restore-CMakeFile {
    if (Test-Path $BackupFile) {
        Copy-Item $BackupFile $CMakeFile -Force
        Write-Log "Restored original CMakeLists.txt from backup" "OK"
        return $true
    } else {
        Write-Log "No backup file found at: $BackupFile" "ERROR"
        return $false
    }
}

function Apply-Patches {
    Write-Log "Applying Datadog profiler patches to CMakeLists.txt..."
    
    $content = Get-Content $CMakeFile -Raw
    
    # Patch 1: Add profiler configuration at the top
    $profilerConfig = @"
# Datadog Profiler Integration
option(ENABLE_DD_PROFILER "Enable Datadog profiler integration" OFF)

if(ENABLE_DD_PROFILER)
    # Profiler DLL directory provided by patch script
    set(DD_PROFILER_DLL_DIR "${cmakeProfilerDllDir}" CACHE PATH "Folder containing dd-win-prof.dll")
    set(DD_PROFILER_DLL "`${DD_PROFILER_DLL_DIR}/dd-win-prof.dll")
    
    message(STATUS "Datadog profiler integration enabled")
    message(STATUS "Profiler DLL dir: `${DD_PROFILER_DLL_DIR}")
    message(STATUS "Profiler DLL: `${DD_PROFILER_DLL}")
    
    if(NOT EXISTS `${DD_PROFILER_DLL})
        message(WARNING "Datadog profiler DLL not found: `${DD_PROFILER_DLL}")
    endif()
endif()

"@
    
    # Insert after the copyright header
    $content = $content -replace "(# SPDX-License-Identifier: MIT\s*\n)", "`$1`n$profilerConfig"
    
    # No post-build copies or script generation - the quickstart will handle profiler invocation
    
    # Write the modified content
    Set-Content $CMakeFile $content -NoNewline
    
    Write-Log "Patches applied successfully" "OK"
}

# Main execution
try {
    Write-Log "=== Vulkan CMake Profiler Patcher ==="
    
    if (!(Test-Path $VulkanRoot)) {
        Write-Log "Vulkan examples not found at: $VulkanRoot" "ERROR"
        Write-Log "Run the vulcan_quickstart.ps1 script first to clone the examples" "ERROR"
        exit 1
    }
    
    if (!(Test-Path $CMakeFile)) {
        Write-Log "CMakeLists.txt not found at: $CMakeFile" "ERROR"
        exit 1
    }
    
    if ($Undo) {
        Write-Log "Undoing patches..."
        if (Restore-CMakeFile) {
            Write-Log "Patches removed successfully" "OK"
        } else {
            Write-Log "Failed to remove patches" "ERROR"
            exit 1
        }
    } else {
        if (Test-AlreadyPatched) {
            Write-Log "CMakeLists.txt appears to already be patched" "WARN"
            Write-Log "Use -Undo to remove existing patches first" "WARN"
            exit 0
        }
        
        Backup-CMakeFile
        Apply-Patches
        
        Write-Log ""
        Write-Log "=== Patching Complete ==="
        Write-Log "To build with profiling enabled, use:"
        Write-Log "  cmake -DENABLE_DD_PROFILER=ON ..."
        Write-Log ""
        Write-Log "To build without profiling (default):"
        Write-Log "  cmake ..."
        Write-Log ""
        Write-Log "To undo patches:"
        Write-Log "  .\patch_vulkan_for_profiling.ps1 -Undo"
    }
}
catch {
    Write-Log "Patching failed: $($_.Exception.Message)" "ERROR"
    exit 1
}
