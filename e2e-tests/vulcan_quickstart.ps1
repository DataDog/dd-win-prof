# Vulkan Quickstart Script - PowerShell Version
# Enhanced environment setup for better tool detection
# Runs headless examples automatically and provides command lines for windowed examples
#
# Usage Examples:
#   .\vulcan_quickstart.ps1                    # Normal run (skips setup if already done)
#   .\vulcan_quickstart.ps1 -Clean             # Clean build directory and dependency cache
#   .\vulcan_quickstart.ps1 -ForceReconfigure  # Force CMake reconfiguration (regenerates bat files)
#   .\vulcan_quickstart.ps1 -ForceDependencies # Force dependency re-setup (GLM, etc.)
#   .\vulcan_quickstart.ps1 -CI                # CI mode (headless)
#   .\vulcan_quickstart.ps1 -Verbose           # Verbose output
#   .\vulcan_quickstart.ps1 -EnableProfiler    # Enable Datadog profiler integration
#   .\vulcan_quickstart.ps1 -BuildTargets @("triangle")  # Build only specific examples
#
# The script will:
# - Copy .env files from various locations to the binary directory for profiler configuration
# - Copy dd-win-prof.dll and ProfilerInjector.exe to the binary directory for profiling
# - Generate *_with_profiler.bat files that use local copies of profiler artifacts
# - Re-copy profiler artifacts and regenerate bat files when -ForceReconfigure is used
# - Run only headless examples (computeheadless) automatically
# - Display command lines for running windowed examples manually

param(
    [switch]$CI,
    [switch]$Clean,
    [switch]$Verbose,
    [switch]$ForceReconfigure,  # Force CMake reconfiguration
    [switch]$ForceDependencies,  # Force dependency re-setup
    [switch]$EnableProfiler,     # Enable Datadog profiler integration
    [string]$ProfilerDllDir = "",  # Optional: folder containing dd-win-prof.dll (defaults to src/x64/Release)
    [string]$RepoRef = "",        # Optional: git ref/commit/tag to checkout for Vulkan examples cache keying
    [string[]]$BuildTargets = @("triangle", "computeheadless", "gears")  # Vulkan examples to build
)

# Configuration
$RepoUrl = "https://github.com/SaschaWillems/Vulkan.git"
$Root = Join-Path $PSScriptRoot "vulkan_examples"
$Src = Join-Path $Root "Vulkan"
$Build = Join-Path $Root "build"
$Config = "Release"

# Determine profiler DLL directory - look in src/x64/Release or src/dd-win-prof/x64/Release
if ([string]::IsNullOrWhiteSpace($ProfilerDllDir)) {
    $srcDir = Join-Path $PSScriptRoot "..\src"
    $candidates = @(
        (Join-Path $srcDir "x64\$Config"),
        (Join-Path $srcDir "dd-win-prof\x64\$Config")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path (Join-Path $candidate "dd-win-prof.dll")) {
            $ProfilerDllDir = $candidate
            break
        }
    }
    if ([string]::IsNullOrWhiteSpace($ProfilerDllDir)) {
        $ProfilerDllDir = $candidates[0]  # Default to first candidate
    }
}

# Also determine injector path
$ProfilerInjectorExe = $null
$injectorCandidates = @(
    (Join-Path (Split-Path $ProfilerDllDir -Parent) "ProfilerInjector.exe"),
    (Join-Path $ProfilerDllDir "ProfilerInjector.exe")
)
foreach ($candidate in $injectorCandidates) {
    if (Test-Path $candidate) {
        $ProfilerInjectorExe = $candidate
        break
    }
}

function Write-Step {
    param([string]$Message, [string]$Level = "INFO")
    $timestamp = Get-Date -Format "HH:mm:ss"
    if ($Level -eq "INFO" -and -not $Verbose) { return }
    switch ($Level) {
        "ERROR" { Write-Host "[$timestamp] [ERROR] $Message" -ForegroundColor Red }
        "WARN"  { Write-Host "[$timestamp] [WARN] $Message" -ForegroundColor Yellow }
        "OK"    { Write-Host "[$timestamp] [OK] $Message" -ForegroundColor Green }
        default { Write-Host "[$timestamp] [INFO] $Message" -ForegroundColor Cyan }
    }
}

function Test-Command {
    param([string]$Command)
    try {
        Get-Command $Command -ErrorAction Stop | Out-Null
        return $true
    }
    catch {
        return $false
    }
}

function Setup-VisualStudio {
    Write-Step "Setting up Visual Studio environment..."
    
    # Check if cl.exe is already available
    if (Test-Command "cl") {
        Write-Step "MSVC compiler found in PATH" "OK"
        return $true
    }
    
    # Try to find Visual Studio using vswhere
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        Write-Step "Found vswhere, locating Visual Studio..."
        try {
            $vsPath = & $vswhere -latest -property installationPath
            if ($vsPath) {
                Write-Step "Found Visual Studio at: $vsPath"
                $vsDevCmd = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
                if (Test-Path $vsDevCmd) {
                    Write-Step "Loading Visual Studio Developer Environment..."
                    
                    # Import VS environment variables
                    $tempFile = [System.IO.Path]::GetTempFileName()
                    cmd /c "`"$vsDevCmd`" -arch=x64 -no_logo && set" > $tempFile
                    
                    Get-Content $tempFile | ForEach-Object {
                        if ($_ -match "^([^=]+)=(.*)$") {
                            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2])
                        }
                    }
                    Remove-Item $tempFile
                    
                    if (Test-Command "cl") {
                        Write-Step "MSVC environment loaded successfully" "OK"
                        return $true
                    }
                }
            }
        }
        catch {
            Write-Step "Error using vswhere: $($_.Exception.Message)" "WARN"
        }
    }
    
    # Try common Visual Studio paths
    $vsPaths = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional", 
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools"
    )
    
    foreach ($vsPath in $vsPaths) {
        $vsDevCmd = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
        if (Test-Path $vsDevCmd) {
            Write-Step "Found VS at: $vsPath"
            try {
                # Import VS environment variables
                $tempFile = [System.IO.Path]::GetTempFileName()
                cmd /c "`"$vsDevCmd`" -arch=x64 -no_logo && set" > $tempFile
                
                Get-Content $tempFile | ForEach-Object {
                    if ($_ -match "^([^=]+)=(.*)$") {
                        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2])
                    }
                }
                Remove-Item $tempFile
                
                if (Test-Command "cl") {
                    Write-Step "MSVC environment loaded from $vsPath" "OK"
                    return $true
                }
            }
            catch {
                Write-Step "Error loading VS environment from $vsPath`: $($_.Exception.Message)" "WARN"
            }
        }
    }
    
    Write-Step "Could not setup MSVC environment" "ERROR"
    Write-Step "Please install Visual Studio 2022 with C++ development tools" "ERROR"
    return $false
}

function Setup-CMake {
    Write-Step "Setting up CMake..."
    
    # Check if cmake is already available
    if (Test-Command "cmake") {
        Write-Step "CMake found in PATH" "OK"
        return $true
    }
    
    # Try common CMake installation paths
    $cmakePaths = @(
        "${env:ProgramFiles}\CMake\bin",
        "${env:ProgramFiles(x86)}\CMake\bin",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
    )
    
    foreach ($cmakePath in $cmakePaths) {
        $cmakeExe = Join-Path $cmakePath "cmake.exe"
        if (Test-Path $cmakeExe) {
            Write-Step "Found CMake at: $cmakePath"
            $env:PATH = "$cmakePath;$env:PATH"
            if (Test-Command "cmake") {
                Write-Step "CMake added to PATH" "OK"
                return $true
            }
        }
    }
    
    Write-Step "Could not find CMake installation" "ERROR"
    Write-Step "Please install CMake from https://cmake.org/download/" "ERROR"
    Write-Step "Or install via Visual Studio Installer (Individual components -> CMake tools)" "ERROR"
    return $false
}

function Setup-Vcpkg {
    Write-Step "Checking for vcpkg package manager..."
    
    # Check if vcpkg is already available
    if (Test-Command "vcpkg") {
        Write-Step "vcpkg found in PATH" "OK"
        return $true
    }
    
    # Check common vcpkg installation locations
    $vcpkgPaths = @(
        "${env:VCPKG_ROOT}\vcpkg.exe",
        "C:\vcpkg\vcpkg.exe",
        "C:\tools\vcpkg\vcpkg.exe",
        "${env:ProgramFiles}\vcpkg\vcpkg.exe"
    )
    
    foreach ($vcpkgPath in $vcpkgPaths) {
        if (Test-Path $vcpkgPath) {
            Write-Step "Found vcpkg at: $vcpkgPath"
            $vcpkgDir = Split-Path $vcpkgPath
            $env:PATH = "$vcpkgDir;$env:PATH"
            $env:VCPKG_ROOT = $vcpkgDir
            Write-Step "vcpkg added to PATH" "OK"
            return $true
        }
    }
    
    # Try to install vcpkg if git is available
    if (Test-Command "git") {
        Write-Step "vcpkg not found. Installing vcpkg..." "WARN"
        $vcpkgInstallPath = "C:\vcpkg"
        
        try {
            if (!(Test-Path $vcpkgInstallPath)) {
                Write-Step "Cloning vcpkg to $vcpkgInstallPath..."
                & git clone https://github.com/Microsoft/vcpkg.git $vcpkgInstallPath
                if ($LASTEXITCODE -ne 0) { throw "vcpkg clone failed" }
            }
            
            Push-Location $vcpkgInstallPath
            Write-Step "Bootstrapping vcpkg..."
            & .\bootstrap-vcpkg.bat
            if ($LASTEXITCODE -ne 0) { throw "vcpkg bootstrap failed" }
            
            # Integrate with Visual Studio
            & .\vcpkg integrate install
            if ($LASTEXITCODE -eq 0) {
                Write-Step "vcpkg Visual Studio integration completed" "OK"
            }
            
            Pop-Location
            
            $env:PATH = "$vcpkgInstallPath;$env:PATH"
            $env:VCPKG_ROOT = $vcpkgInstallPath
            Write-Step "vcpkg installed and configured successfully" "OK"
            return $true
        }
        catch {
            Write-Step "Failed to install vcpkg: $($_.Exception.Message)" "ERROR"
            if ($PWD.Path -eq $vcpkgInstallPath) { Pop-Location }
        }
    }
    
    Write-Step "vcpkg not available. GLM dependency may need manual installation." "WARN"
    return $false
}

function Test-Tools {
    Write-Step "Checking required tools..."
    
    $tools = @("git", "cmake")
    $allFound = $true
    
    foreach ($tool in $tools) {
        if (Test-Command $tool) {
            Write-Step "Tool found: $tool" "OK"
        } else {
            Write-Step "Required tool not found: $tool" "ERROR"
            $allFound = $false
            
            switch ($tool) {
                "git" {
                    Write-Step "Install Git from: https://git-scm.com/download/win" "ERROR"
                }
                "cmake" {
                    Write-Step "Install CMake from: https://cmake.org/download/" "ERROR"
                }
            }
        }
    }
    
    # Setup vcpkg (optional but helpful for dependencies)
    Setup-Vcpkg | Out-Null
    
    return $allFound
}

function Invoke-VulkanClone {
    Write-Step "Cloning Vulkan examples (shallow)..." "WARN"
    if (!(Test-Path $Root)) {
        New-Item -ItemType Directory -Path $Root -Force | Out-Null
    }
    $cloneArgs = @("--recurse-submodules", "--depth=1", "--filter=blob:none", $RepoUrl, $Src)
    & git clone @cloneArgs
    if ($LASTEXITCODE -ne 0) { throw "Git clone failed" }
    if ($RepoRef) {
        Push-Location $Src
        & git checkout --force $RepoRef
        if ($LASTEXITCODE -ne 0) {
            Pop-Location
            throw "Git checkout failed"
        }
        Pop-Location
    }
}

function Ensure-VulkanRepo {
    if (!(Test-Path (Join-Path $Src ".git"))) {
        Invoke-VulkanClone
        return
    }
    
    Push-Location $Src
    $fetchArgs = @("--tags", "--force", "--prune", "--depth=1", "--filter=blob:none", "origin")
    if ($RepoRef) { $fetchArgs += $RepoRef }
    & git fetch @fetchArgs
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "Git fetch failed" }
    
    if ($RepoRef) {
        & git checkout --force $RepoRef
    } else {
        $remoteHead = & git symbolic-ref --short refs/remotes/origin/HEAD 2>$null
        if (-not $remoteHead) { $remoteHead = "origin/master" }
        & git reset --hard $remoteHead
    }
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "Git checkout/reset failed" }
    
    & git submodule update --init --recursive --depth=1
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "Git submodule update failed" }
    
    Pop-Location
}

# Main execution
try {
    Write-Step "=== Vulkan Quickstart (Enhanced) ==="
    
    if ($Clean) {
        Write-Step "[CLEAN] Removing build directory and dependency cache..."
        if (Test-Path $Build) {
            Remove-Item $Build -Recurse -Force
            Write-Step "Build directory removed" "OK"
        }
        
        # Also clean dependency markers for fresh start
        $depMarkers = @(
            (Join-Path $PSScriptRoot ".glm_setup_complete"),
            (Join-Path $PSScriptRoot ".vcpkg_setup_complete")
        )
        
        foreach ($marker in $depMarkers) {
            if (Test-Path $marker) {
                Remove-Item $marker -Force
                Write-Step "Removed dependency marker: $(Split-Path $marker -Leaf)" "OK"
            }
        }
        
        Write-Step "Clean completed - next run will perform full dependency setup" "OK"
        exit 0
    }
    
    Write-Step "[1/7] Setting up development environment..."
    if (!(Setup-VisualStudio)) { exit 1 }
    if (!(Setup-CMake)) { exit 1 }
    
    Write-Step "[2/7] Checking required tools..."
    if (!(Test-Tools)) { exit 1 }
    
    Write-Step "[3/7] Cloning or updating repository..."
    Ensure-VulkanRepo
    
    Write-Step "[3.5/7] Setting up dependencies (GLM, etc.)..."
    
    # Check if we should skip dependency setup (unless forced)
    $glmMarker = Join-Path $PSScriptRoot ".glm_setup_complete"
    $skipDependencySetup = (Test-Path $glmMarker) -and !$ForceDependencies
    
    if ($skipDependencySetup) {
        Write-Step "Dependencies already configured (use -ForceDependencies to re-setup)" "OK"
    } else {
        Write-Step "Setting up dependencies..."
        
        # Check if the project has git submodules and initialize them
        Push-Location $Src
        try {
            if (Test-Path ".gitmodules") {
                Write-Step "Initializing git submodules..."
                & git submodule update --init --recursive
                if ($LASTEXITCODE -ne 0) {
                    Write-Step "Git submodule initialization failed, continuing anyway..." "WARN"
                } else {
                    Write-Step "Git submodules initialized successfully" "OK"
                }
            } else {
                Write-Step "No .gitmodules found, checking for external dependencies..." "WARN"
            }
            
            # Check if GLM is available, if not try to install it via vcpkg or suggest manual installation
            $glmPaths = @(
                "$Src\external\glm\glm\glm.hpp",
                "$Src\third_party\glm\glm\glm.hpp", 
                "$Src\libs\glm\glm\glm.hpp",
                "${env:VCPKG_ROOT}\installed\x64-windows\include\glm\glm.hpp" 
            )
            
            $glmFound = $false
            foreach ($glmPath in $glmPaths) {
                if (Test-Path $glmPath) {
                    Write-Step "Found GLM at: $(Split-Path $glmPath)" "OK"
                    $glmFound = $true
                    break
                }
            }
            
            if (!$glmFound) {
                Write-Step "GLM not found in expected locations. Trying to install via vcpkg..." "WARN"
                if (Test-Command "vcpkg") {
                    Write-Step "Installing GLM via vcpkg..."
                    & vcpkg install glm:x64-windows
                    if ($LASTEXITCODE -eq 0) {
                        Write-Step "GLM installed successfully via vcpkg" "OK"
                        $glmFound = $true
                    } else {
                        Write-Step "vcpkg GLM installation failed" "WARN"
                    }
                }
                
                if (!$glmFound) {
                    Write-Step "GLM not available. Trying automatic setup..." "WARN"
                    $setupGlmScript = Join-Path $PSScriptRoot "setup_glm.ps1"
                    if (Test-Path $setupGlmScript) {
                        Write-Step "Running GLM setup script..."
                        & $setupGlmScript
                        if ($LASTEXITCODE -eq 0) {
                            Write-Step "GLM setup completed successfully" "OK"
                            $glmFound = $true
                        } else {
                            Write-Step "GLM setup script failed" "ERROR"
                            Write-Step "Please manually install GLM or the build may fail" "ERROR"
                        }
                    } else {
                        Write-Step "GLM setup script not found" "WARN"
                        Write-Step "Solutions:" "WARN"
                        Write-Step "  1. Install vcpkg and run: vcpkg install glm:x64-windows" "WARN"  
                        Write-Step "  2. Download GLM from https://github.com/g-truc/glm and extract to $Src\external\glm" "WARN"
                        Write-Step "  3. The project may handle GLM via CMake FetchContent - continuing anyway..." "WARN"
                    }
                }
            }
            
            # Create dependency marker if GLM was found or installed
            if ($glmFound) {
                "GLM dependency setup completed on $(Get-Date)" | Out-File $glmMarker -Encoding UTF8
                Write-Step "Created dependency marker for future runs" "OK"
            }
        }
        finally {
            Pop-Location
        }
    }
    
    Write-Step "[3.8/7] Setting up profiler integration..."
    if ($EnableProfiler) {
        Write-Step "Enabling Datadog profiler integration..."
        
        # Check if required profiler files exist
        $dllPath = Join-Path $ProfilerDllDir "dd-win-prof.dll"
        
        Write-Step "Profiler DLL directory: $ProfilerDllDir"
        
        if (Test-Path $dllPath) {
            Write-Step "dd-win-prof.dll found at: $dllPath" "OK"
        } else {
            Write-Step "ERROR: dd-win-prof.dll not found at: $dllPath" "ERROR"
            Write-Step "Please build the profiler solution first using Visual Studio:" "ERROR"
            Write-Step "  1. Open src/WindowsProfiler.sln in Visual Studio" "ERROR"
            Write-Step "  2. Build -> Build Solution (or press Ctrl+Shift+B)" "ERROR"
            Write-Step "  3. Ensure dd-win-prof project builds successfully" "ERROR"
            exit 1
        }
        
        if ($ProfilerInjectorExe -and (Test-Path $ProfilerInjectorExe)) {
            Write-Step "ProfilerInjector.exe found at: $ProfilerInjectorExe" "OK"
        } else {
            Write-Step "WARNING: ProfilerInjector.exe not found" "WARN"
            Write-Step "Please ensure ProfilerInjector project is built in Visual Studio" "WARN"
        }
        
        # Add profiler DLL directory to PATH so it can be found at runtime
        $env:PATH = "$ProfilerDllDir;$env:PATH"
        Write-Step "Added profiler DLL directory to PATH" "OK"
        
        $patchScript = Join-Path $PSScriptRoot "patch_vulkan_for_profiling.ps1"
        if (Test-Path $patchScript) {
            try {
                # Force reapply the patch by undoing first, then applying
                Write-Step "Ensuring fresh profiler patch application..."
                & $patchScript -Undo -ProfilerDllDir $ProfilerDllDir -Verbose:$Verbose 2>$null  # Ignore errors if no backup exists
                & $patchScript -ProfilerDllDir $ProfilerDllDir -Verbose:$Verbose
                if ($LASTEXITCODE -eq 0) {
                    Write-Step "Vulkan examples patched for profiler integration" "OK"
                } else {
                    Write-Step "Failed to patch Vulkan examples for profiling" "WARN"
                }
            }
            catch {
                Write-Step "Error running profiler patcher: $($_.Exception.Message)" "WARN"
            }
        } else {
            Write-Step "Profiler patch script not found" "WARN"
        }
    }
    
    Write-Step "[4/7] Configuring CMake (Visual Studio 2022, x64)..."
    
    # Check if we need to reconfigure CMake
    $cmakeCache = Join-Path $Build "CMakeCache.txt"
    $needsReconfigure = $ForceReconfigure -or !(Test-Path $cmakeCache)
    
    if ($needsReconfigure -and (Test-Path $cmakeCache)) {
        Write-Step "Removing existing CMake cache for clean reconfiguration..." "WARN"
        Remove-Item $cmakeCache -Force
        
        # Also remove CMakeFiles directory
        $cmakeFiles = Join-Path $Build "CMakeFiles"
        if (Test-Path $cmakeFiles) {
            Remove-Item $cmakeFiles -Recurse -Force
            Write-Step "Removed CMakeFiles directory" "OK"
        }
        
        # Clean up old profiler artifacts so they get re-copied
        $binPathForCleanup = Join-Path $Build "bin\$Config"
        if (Test-Path $binPathForCleanup) {
            $oldBatFiles = Get-ChildItem $binPathForCleanup -Filter "*_with_profiler.bat" -ErrorAction SilentlyContinue
            foreach ($batFile in $oldBatFiles) {
                Remove-Item $batFile.FullName -Force
                Write-Step "Removed old profiler bat file: $($batFile.Name)" "OK"
            }
            
            # Also remove copied DLL and injector to force re-copy
            $oldDll = Join-Path $binPathForCleanup "dd-win-prof.dll"
            if (Test-Path $oldDll) {
                Remove-Item $oldDll -Force
                Write-Step "Removed old dd-win-prof.dll" "OK"
            }
            
            $oldInjector = Join-Path $binPathForCleanup "ProfilerInjector.exe"
            if (Test-Path $oldInjector) {
                Remove-Item $oldInjector -Force
                Write-Step "Removed old ProfilerInjector.exe" "OK"
            }
        }
    }
    
    $cmakeArgs = @(
        "-S", $Src,
        "-B", $Build, 
        "-G", "Visual Studio 17 2022",
        "-A", "x64",
        "-DUSE_RELATIVE_ASSET_PATH=ON",
        "-DCMAKE_CONFIGURATION_TYPES=Release"  # Only build Release, skip Debug
    )
    
    # Add additional options to help with asset/shader paths
    $cmakeArgs += "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$Build/bin"
    
    # Add vcpkg toolchain if available
    if ($env:VCPKG_ROOT -and (Test-Path "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake")) {
        $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
        Write-Step "Using vcpkg toolchain: $env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" "OK"
    }
    
    if ($needsReconfigure) {
        Write-Step "Performing CMake configuration..."
    } else {
        Write-Step "CMake cache exists, skipping configuration (use -ForceReconfigure to force)" "OK"
    }
    
    if ($needsReconfigure) {
        # Add profiler flag if enabled
        if ($EnableProfiler) {
            $cmakeArgs += "-DENABLE_DD_PROFILER=ON"
            Write-Step "Added profiler flag to CMake configuration" "OK"
        }
        
        & cmake @cmakeArgs
        if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed" }
    }
    
    Write-Step "[5/7] Building project (optimized build)..."
    
    # Build only the specified targets for faster execution
    $targetList = $BuildTargets -join " "
    Write-Step "Building targets: $targetList..."
    & cmake --build $Build --config $Config --target $BuildTargets
    if ($LASTEXITCODE -ne 0) { 
        Write-Step "Target build failed, trying full build as fallback..." "WARN"
        & cmake --build $Build --config $Config
        if ($LASTEXITCODE -ne 0) { throw "Build failed" }
    }
    
    Write-Step "[6/7] Optional CI/SwiftShader setup..."
    if ($CI -and $env:SWIFTSHADER_DIR) {
        $swiftShaderIcd = Join-Path $env:SWIFTSHADER_DIR "vk_swiftshader_icd.json"
        if (Test-Path $swiftShaderIcd) {
            $env:VK_DRIVER_FILES = $swiftShaderIcd
            Write-Step "Using SwiftShader ICD: $env:VK_DRIVER_FILES" "OK"
        }
    }
    
    Write-Step "[6.5/7] Setting up assets and environment for runtime..."
    $binPath = Join-Path $Build "bin\$Config"
    $binParentPath = Join-Path $Build "bin"
    $assetsSource = Join-Path $Src "assets"
    $shadersSource = Join-Path $Src "shaders"
    
    # Executables are in bin/Release/, so assets should be in bin/ (one level up)
    # This gives the correct ../assets and ../shaders relative paths
    $assetsDest = Join-Path $binParentPath "assets"
    $shadersDest = Join-Path $binParentPath "shaders"
    
    # Generate bat files for running examples with profiler (if profiler is enabled)
    if ($EnableProfiler -and $ProfilerInjectorExe -and (Test-Path $ProfilerInjectorExe)) {
        Write-Step "Copying profiler artifacts to bin directory..."
        
        # Copy DLL to bin directory (so injector can find it next to executables)
        $dllSource = Join-Path $ProfilerDllDir "dd-win-prof.dll"
        $dllDest = Join-Path $binPath "dd-win-prof.dll"
        Copy-Item $dllSource $dllDest -Force
        Write-Step "Copied dd-win-prof.dll to $dllDest" "OK"
        
        # Copy injector to bin directory for convenience
        $injectorDest = Join-Path $binPath "ProfilerInjector.exe"
        Copy-Item $ProfilerInjectorExe $injectorDest -Force
        Write-Step "Copied ProfilerInjector.exe to $injectorDest" "OK"
        
        # Get list of executables
        $exeFiles = Get-ChildItem $binPath -Filter "*.exe" -ErrorAction SilentlyContinue
        
        foreach ($exe in $exeFiles) {
            $batFileName = "$($exe.BaseName)_with_profiler.bat"
            $batFilePath = Join-Path $binPath $batFileName
            
            # Generate bat file content using local copies
            $batContent = @"
@echo off
REM Auto-generated profiler runner for $($exe.Name)
REM Uses local copies of profiler artifacts

cd /d "%~dp0"

if not exist "dd-win-prof.dll" (
    echo ERROR: Profiler DLL not found in current directory
    echo Please re-run: vulcan_quickstart.ps1 -EnableProfiler -ForceReconfigure
    exit /b 1
)

if not exist "ProfilerInjector.exe" (
    echo ERROR: ProfilerInjector.exe not found in current directory
    echo Please re-run: vulcan_quickstart.ps1 -EnableProfiler -ForceReconfigure
    exit /b 1
)

echo Running $($exe.Name) with Datadog profiler...
echo Working directory: %CD%
echo.

ProfilerInjector.exe $($exe.Name) %*
"@
            
            Set-Content -Path $batFilePath -Value $batContent -Encoding ASCII
            Write-Step "Created $batFileName" "OK"
        }
        
        Write-Step "Profiler runner bat files generated in $binPath" "OK"
    }
    
    if (Test-Path $assetsSource) {
        Write-Step "Copying assets one level up from executables ($assetsDest)..."
        if (Test-Path $assetsDest) {
            Remove-Item $assetsDest -Recurse -Force
        }
        Copy-Item $assetsSource $assetsDest -Recurse -Force
        Write-Step "Assets copied to $assetsDest" "OK"
    } else {
        Write-Step "Assets not found at $assetsSource" "WARN"
    }
    
    if (Test-Path $shadersSource) {
        Write-Step "Copying shaders one level up from executables ($shadersDest)..."
        if (Test-Path $shadersDest) {
            Remove-Item $shadersDest -Recurse -Force
        }
        Copy-Item $shadersSource $shadersDest -Recurse -Force
        Write-Step "Shaders copied to $shadersDest" "OK"
    } else {
        Write-Step "Shaders not found at $shadersSource" "WARN"
    }
    
    # Copy .env file if found (for profiler configuration)
    Write-Step "Checking for .env file..."
    $envSearchPaths = @(
        (Join-Path $PSScriptRoot ".env"),                    # Same directory as script
        (Join-Path $PSScriptRoot "..\..\.env"),             # Project root
        (Join-Path $Root ".env"),                           # Vulkan examples root
        (Join-Path $Src ".env")                             # Vulkan source root
    )
    
    $envFound = $false
    foreach ($envPath in $envSearchPaths) {
        if (Test-Path $envPath) {
            $envDest = Join-Path $binPath ".env"
            Write-Step "Found .env file at: $envPath"
            Copy-Item $envPath $envDest -Force
            Write-Step "Copied .env file to: $envDest" "OK"
            $envFound = $true
            break
        }
    }
    
    if (!$envFound) {
        Write-Step "No .env file found. Profiler will use environment variables or defaults." "WARN"
        Write-Step "You can create a .env file at any of these locations:" "WARN"
        foreach ($path in $envSearchPaths) {
            Write-Step "  $path" "WARN"
        }
    }
    
    Write-Step "[7/7] Build complete - examples ready to run..."
    
    if ($CI) {
        Write-Step "CI mode: Skipping execution (no GPU available)" "INFO"
        Write-Step "Build artifacts verified and ready for deployment" "OK"
    }
    
    # Show command lines to run examples
    if (-not $CI) {
        Write-Step ""
        Write-Step "=== How to Run Examples ===" "INFO"
        Write-Step ""
        Write-Step "Navigate to the build directory:" "INFO"
        Write-Step "  cd `"$binPath`"" "INFO"
        Write-Step ""
        
        # Show available executables
        Write-Step "Available executables:" "INFO"
        $availableExes = Get-ChildItem $binPath -Filter "*.exe" -ErrorAction SilentlyContinue
        foreach ($exe in $availableExes) {
            Write-Step "  $($exe.Name)" "INFO"
            if ($EnableProfiler -and $ProfilerInjectorExe) {
                $batFileName = "$($exe.BaseName)_with_profiler.bat"
                $batFilePath = Join-Path $binPath $batFileName
                if (Test-Path $batFilePath) {
                    Write-Step "    With profiler (via bat): .\$batFileName" "INFO"
                }
                Write-Step "    With profiler (direct): .\ProfilerInjector.exe $($exe.Name)" "INFO"
            } else {
                Write-Step "    Run directly: .\$($exe.Name)" "INFO"
            }
        }
        
        Write-Step ""
        Write-Step "Examples:" "INFO"
        Write-Step "  computeheadless - Headless compute shader example" "INFO"
        Write-Step "  triangle, gears - Basic windowed examples" "INFO"
        Write-Step ""
        Write-Step "Note: Windowed examples require a GPU and display." "WARN"
    }
    
    Write-Step ""
    Write-Step "All done!" "OK"
}
catch {
    Write-Step "Script failed: $($_.Exception.Message)" "ERROR"
    Write-Step "" 
    Write-Step "=== Troubleshooting Guide ===" "ERROR"
    Write-Step "1. Environment Issues:" "ERROR"
    Write-Step "   - Ensure Visual Studio 2022 with C++ tools is installed" "ERROR"
    Write-Step "   - Ensure CMake is installed (standalone or via VS)" "ERROR" 
    Write-Step "   - Ensure Git is installed and accessible" "ERROR"
    Write-Step ""
    Write-Step "2. Dependency Issues:" "ERROR"
    Write-Step "   - Try: .\vulcan_quickstart.ps1 -ForceDependencies" "ERROR"
    Write-Step "   - Try: .\vulcan_quickstart.ps1 -Clean" "ERROR"
    Write-Step "   - Manual GLM setup: .\setup_glm.ps1" "ERROR"
    Write-Step ""
    Write-Step "3. Asset/Shader Path Issues:" "ERROR"
    Write-Step "   - Assets are automatically copied to build/ directory during build" "ERROR"
    Write-Step "   - Executables expect assets at ../assets and ../shaders relative to bin/Release/" "ERROR"
    Write-Step ""
    Write-Step "4. Build Issues:" "ERROR"
    Write-Step "   - Try: .\vulcan_quickstart.ps1 -ForceReconfigure" "ERROR"
    Write-Step "   - Check build log above for specific errors" "ERROR"
    Write-Step ""
    Write-Step "5. Complete Reset:" "ERROR"
    Write-Step "   - Run: .\vulcan_quickstart.ps1 -Clean" "ERROR"
    Write-Step "   - Then: .\vulcan_quickstart.ps1 -ForceDependencies" "ERROR"
    exit 1
}
