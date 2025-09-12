# Patch Vulkan Examples for Datadog Profiler Integration
# Automatically modifies the CMakeLists.txt to add profiling support

param(
    [switch]$Verbose,
    [switch]$Undo  # Remove the patches
)

$VulkanRoot = Join-Path $PSScriptRoot "vulkan_examples\Vulkan"
$CMakeFile = Join-Path $VulkanRoot "examples\CMakeLists.txt"
$BackupFile = "$CMakeFile.backup"

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
    # Set paths to your profiler (relative to Vulkan examples root)
    set(DD_PROFILER_ROOT "`${CMAKE_SOURCE_DIR}/../../../src/dd-win-prof" CACHE PATH "Path to dd-win-prof")
    set(DD_PROFILER_INCLUDE_DIR "`${DD_PROFILER_ROOT}")
    set(DD_PROFILER_LIB "`${CMAKE_SOURCE_DIR}/../../../src/x64/Release/dd-win-prof.lib")
    set(DD_PROFILER_DLL "`${CMAKE_SOURCE_DIR}/../../../src/x64/Release/dd-win-prof.dll")
    
    # Verify profiler files exist
    if(NOT EXISTS `${DD_PROFILER_LIB})
        message(WARNING "Datadog profiler library not found: `${DD_PROFILER_LIB}")
    endif()
    if(NOT EXISTS `${DD_PROFILER_DLL})
        message(WARNING "Datadog profiler DLL not found: `${DD_PROFILER_DLL}")
        message(WARNING "Batch files will NOT be generated without the DLL!")
    endif()
    
    message(STATUS "Datadog profiler integration enabled")
    message(STATUS "Profiler lib: `${DD_PROFILER_LIB}")
    message(STATUS "Profiler DLL: `${DD_PROFILER_DLL}")
    
    # Debug: Show what files actually exist in the profiler directory
    get_filename_component(DD_PROFILER_DIR `${DD_PROFILER_DLL} DIRECTORY)
    message(STATUS "Checking profiler directory: `${DD_PROFILER_DIR}")
    if(EXISTS `${DD_PROFILER_DIR})
        message(STATUS "Profiler directory exists")
    else()
        message(WARNING "Profiler directory does not exist: `${DD_PROFILER_DIR}")
    endif()
endif()

"@
    
    # Insert after the copyright header
    $content = $content -replace "(# SPDX-License-Identifier: MIT\s*\n)", "`$1`n$profilerConfig"
    
    # Patch 2: Modify the buildExample function to add profiler linking
    $profilerIntegration = @"
        
        # Add Datadog profiler integration
        message(STATUS "Checking profiler integration for `${EXAMPLE_NAME}...")
        message(STATUS "ENABLE_DD_PROFILER: `${ENABLE_DD_PROFILER}")
        message(STATUS "DD_PROFILER_DLL exists: `${DD_PROFILER_DLL}")
        if(EXISTS `${DD_PROFILER_DLL})
            message(STATUS "DLL exists, proceeding with profiler integration")
        else()
            message(WARNING "DLL does not exist, skipping profiler integration for `${EXAMPLE_NAME}")
        endif()
        
        if(ENABLE_DD_PROFILER AND EXISTS `${DD_PROFILER_DLL})
            message(STATUS "Generating profiler batch files for `${EXAMPLE_NAME}")
            # No linking or code changes needed - we use DLL injection instead
            
            # Copy profiler DLL and injector to output directory
            add_custom_command(TARGET `${EXAMPLE_NAME} POST_BUILD
                COMMAND `${CMAKE_COMMAND} -E copy_if_different
                `${DD_PROFILER_DLL}
                `$<TARGET_FILE_DIR:`${EXAMPLE_NAME}>
                COMMENT "Copying Datadog profiler DLL for `${EXAMPLE_NAME}")
            
            # Copy ProfilerInjector.exe if it exists
            set(PROFILER_INJECTOR "`${CMAKE_SOURCE_DIR}/../../../src/x64/Release/ProfilerInjector.exe")
            if(EXISTS `${PROFILER_INJECTOR})
                add_custom_command(TARGET `${EXAMPLE_NAME} POST_BUILD
                    COMMAND `${CMAKE_COMMAND} -E copy_if_different
                    `${PROFILER_INJECTOR}
                    `$<TARGET_FILE_DIR:`${EXAMPLE_NAME}>
                    COMMENT "Copying ProfilerInjector.exe for `${EXAMPLE_NAME}")
            endif()
            
             # Set up environment variables for profiler auto-start and debug logging
             # Note: DD_SERVICE is auto-set by ProfilerInjector from executable name
             set_target_properties(`${EXAMPLE_NAME} PROPERTIES
                 VS_DEBUGGER_ENVIRONMENT "DD_PROFILING_AUTO_START=1;DD_TRACE_DEBUG=1")
            
            # Create a batch script for easy command-line execution with profiler
            set(BATCH_SCRIPT_PATH "`$<TARGET_FILE_DIR:`${EXAMPLE_NAME}>/run-`${EXAMPLE_NAME}-with-profiler.bat")
            set(BATCH_CONTENT "@echo off
echo Starting `${EXAMPLE_NAME} with Datadog profiler (zero-code injection)...
echo Note: Create a .env file in this directory to configure DD_SITE, DD_API_KEY, etc.
ProfilerInjector.exe `${EXAMPLE_NAME}.exe %*
")
            file(GENERATE OUTPUT "`${BATCH_SCRIPT_PATH}" CONTENT "`${BATCH_CONTENT}")
            
            # Also create a simple PowerShell version for better cross-compatibility
            set(PS_SCRIPT_PATH "`$<TARGET_FILE_DIR:`${EXAMPLE_NAME}>/run-`${EXAMPLE_NAME}-with-profiler.ps1")
            set(PS_CONTENT "Write-Host 'Starting `${EXAMPLE_NAME} with Datadog profiler (zero-code injection)...'
Write-Host 'Note: Create a .env file in this directory to configure DD_SITE, DD_API_KEY, etc.'
& '.\\ProfilerInjector.exe' '.\\`${EXAMPLE_NAME}.exe' `$args
")
            file(GENERATE OUTPUT "`${PS_SCRIPT_PATH}" CONTENT "`${PS_CONTENT}")
        endif()
"@
    
    # Insert profiler integration after the target_link_libraries line for Windows
    $content = $content -replace "(target_link_libraries\(\$\{EXAMPLE_NAME\} base \$\{Vulkan_LIBRARY\} \$\{WINLIBS\}\))", "`$1$profilerIntegration"
    
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
