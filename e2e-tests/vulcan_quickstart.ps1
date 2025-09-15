# Vulkan Quickstart Script - PowerShell Version
# Enhanced environment setup for better tool detection
#
# Usage Examples:
#   .\vulcan_quickstart.ps1                    # Normal run (skips setup if already done)
#   .\vulcan_quickstart.ps1 -Clean             # Clean build directory and dependency cache
#   .\vulcan_quickstart.ps1 -ForceReconfigure  # Force CMake reconfiguration
#   .\vulcan_quickstart.ps1 -ForceDependencies # Force dependency re-setup (GLM, etc.)
#   .\vulcan_quickstart.ps1 -CI                # CI mode (headless)
#   .\vulcan_quickstart.ps1 -Verbose           # Verbose output
#   .\vulcan_quickstart.ps1 -EnableProfiler    # Enable Datadog profiler integration

param(
    [switch]$CI,
    [switch]$Clean,
    [switch]$Verbose,
    [switch]$ForceReconfigure,  # Force CMake reconfiguration
    [switch]$ForceDependencies,  # Force dependency re-setup
    [switch]$EnableProfiler     # Enable Datadog profiler integration
)

# Configuration
$RepoUrl = "https://github.com/SaschaWillems/Vulkan.git"
$Root = Join-Path $PSScriptRoot "vulkan_examples"
$Src = Join-Path $Root "Vulkan"
$Build = Join-Path $Root "build"
$Config = "Release"

function Write-Step {
    param([string]$Message, [string]$Level = "INFO")
    $timestamp = Get-Date -Format "HH:mm:ss"
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
    if (!(Test-Path (Join-Path $Src ".git"))) {
        if (!(Test-Path $Root)) {
            New-Item -ItemType Directory -Path $Root -Force | Out-Null
        }
        & git clone --recursive --depth=1 $RepoUrl $Src
        if ($LASTEXITCODE -ne 0) { throw "Git clone failed" }
    } else {
        Push-Location $Src
        & git pull --ff-only
        if ($LASTEXITCODE -ne 0) { 
            Pop-Location
            throw "Git pull failed" 
        }
        
        # Check if submodules are initialized (specifically the assets submodule)
        $assetsPath = Join-Path $Src "assets"
        if ((Test-Path $assetsPath) -and ((Get-ChildItem $assetsPath -ErrorAction SilentlyContinue | Measure-Object).Count -eq 0)) {
            Write-Step "Assets submodule not initialized - updating submodules..."
            & git submodule update --init --recursive --depth=1
            if ($LASTEXITCODE -ne 0) { 
                Write-Step "Submodule update failed" "WARN"
            } else {
                Write-Step "Submodules updated successfully" "OK"
            }
        } elseif (Test-Path $assetsPath) {
            Write-Step "Assets already available" "OK"
        }
        
        Pop-Location
    }
    
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
        $dllPath = Join-Path $PSScriptRoot "..\src\x64\$Config\dd-win-prof.dll"
        $injectorPath = Join-Path $PSScriptRoot "..\src\x64\$Config\ProfilerInjector.exe"
        
        Write-Step "Checking for required profiler files..."
        
        if (Test-Path $dllPath) {
            Write-Step "dd-win-prof.dll found at: $dllPath" "OK"
        } else {
            Write-Step "ERROR: dd-win-prof.dll not found at: $dllPath" "ERROR"
            Write-Step "Please build the profiler solution first using Visual Studio:" "ERROR"
            Write-Step "  1. Open src/WindowsProfiler.sln in Visual Studio" "ERROR"
            Write-Step "  2. Build -> Build Solution (or press Ctrl+Shift+B)" "ERROR"
            Write-Step "  3. Ensure both dd-win-prof and ProfilerInjector projects build successfully" "ERROR"
            Write-Step "Batch files will NOT be generated without the DLL!" "ERROR"
            exit 1
        }
        
        if (Test-Path $injectorPath) {
            Write-Step "ProfilerInjector.exe found at: $injectorPath" "OK"
        } else {
            Write-Step "WARNING: ProfilerInjector.exe not found at: $injectorPath" "WARN"
            Write-Step "Please ensure ProfilerInjector project is built in Visual Studio" "WARN"
        }
        
        $patchScript = Join-Path $PSScriptRoot "patch_vulkan_for_profiling.ps1"
        if (Test-Path $patchScript) {
            try {
                # Force reapply the patch by undoing first, then applying
                Write-Step "Ensuring fresh profiler patch application..."
                & $patchScript -Undo 2>$null  # Ignore errors if no backup exists
                & $patchScript
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
    }
    
    $cmakeArgs = @(
        "-S", $Src,
        "-B", $Build, 
        "-G", "Visual Studio 17 2022",
        "-A", "x64",
        "-DUSE_RELATIVE_ASSET_PATH=ON"
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
    
    # Build only the essential targets for faster execution
    Write-Step "Building essential targets: triangle, computeheadless, gears..."
    & cmake --build $Build --config $Config --target triangle computeheadless gears
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
    
    Write-Step "[6.5/7] Setting up assets for runtime..."
    $binPath = Join-Path $Build "bin\$Config"
    $binParentPath = Join-Path $Build "bin"
    $assetsSource = Join-Path $Src "assets"
    $shadersSource = Join-Path $Src "shaders"
    
    # Executables are in bin/Release/, so assets should be in bin/ (one level up)
    # This gives the correct ../assets and ../shaders relative paths
    $assetsDest = Join-Path $binParentPath "assets"
    $shadersDest = Join-Path $binParentPath "shaders"
    
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
    
    Write-Step "[7/7] Running samples..."
    $exePath = Join-Path $binPath "computeheadless.exe"
    
    Write-Step "== computeheadless (no window; good for CI) =="
    Write-Step "Executable: $exePath"
    
    # Run from bin directory where assets are now located
    Push-Location $binPath
    try {
        # Check if profiler is enabled and use appropriate launcher
        if ($EnableProfiler -and (Test-Path "run-computeheadless-with-profiler.bat")) {
            Write-Step "Running computeheadless with profiler integration..." "INFO"
            & .\run-computeheadless-with-profiler.bat
            if ($LASTEXITCODE -eq 0) {
                Write-Step "computeheadless with profiler ran successfully!" "OK"
            } else {
                Write-Step "computeheadless with profiler failed (exit code: $LASTEXITCODE)" "WARN"
            }
        } elseif (Test-Path "computeheadless.exe") {
            Write-Step "Running computeheadless directly (no profiler)..." "INFO"
            & .\computeheadless.exe
            if ($LASTEXITCODE -eq 0) {
                Write-Step "computeheadless ran successfully!" "OK"
            } else {
                Write-Step "computeheadless failed (exit code: $LASTEXITCODE)" "WARN"
            }
        } else {
            Write-Step "computeheadless.exe not found" "ERROR"
        }
        
        # If computeheadless failed, try gears as fallback
        if ($LASTEXITCODE -ne 0) {
            Write-Step "Trying gears as fallback (will run until manually closed)..." "WARN"
            
            if ($EnableProfiler -and (Test-Path "run-gears-with-profiler.bat")) {
                Write-Step "Running gears with profiler integration..." "INFO"
                Write-Step "Close the gears window to continue..." "INFO"
                $process = Start-Process -FilePath ".\run-gears-with-profiler.bat" -PassThru -NoNewWindow
                $process.WaitForExit()
            } elseif (Test-Path "gears.exe") {
                Write-Step "Running gears directly..." "INFO"
                Write-Step "Close the gears window to continue..." "INFO"
                $process = Start-Process -FilePath ".\gears.exe" -PassThru -NoNewWindow
                $process.WaitForExit()
            } else {
                Write-Step "gears.exe not found" "WARN"
            }
        }
    }
    catch {
        Write-Step "Error running computeheadless: $($_.Exception.Message)" "WARN"
    }
    finally {
        Pop-Location
    }
    
    if (!$CI) {
        Write-Step "== triangle (opens a window; close it to continue) =="
        Push-Location $binPath
        
        # Check if profiler is enabled and use appropriate launcher
        if ($EnableProfiler -and (Test-Path "run-triangle-with-profiler.bat")) {
            Write-Step "Running triangle with profiler integration..." "INFO"
            try {
                & .\run-triangle-with-profiler.bat
                Start-Sleep 5
            } catch {
                Write-Step "Profiler script failed, trying direct execution..." "WARN"
                if (Test-Path "triangle.exe") {
                    Start-Process -FilePath "triangle.exe" -WorkingDirectory $binPath -NoNewWindow
                    Start-Sleep 5
                }
            }
        } elseif (Test-Path "triangle.exe") {
            Start-Process -FilePath "triangle.exe" -WorkingDirectory $binPath -NoNewWindow
            Start-Sleep 5
        } else {
            Write-Step "triangle.exe not found" "WARN"
        }
        
        Write-Step "== gears demo (runs until manually closed) =="
        
        # Check if profiler is enabled and use appropriate launcher
        if ($EnableProfiler -and (Test-Path "run-gears-with-profiler.bat")) {
            Write-Step "Running gears with profiler integration..." "INFO"
            Write-Step "Close the gears window to continue..." "INFO"
            try {
                $process = Start-Process -FilePath ".\run-gears-with-profiler.bat" -PassThru -NoNewWindow
                $process.WaitForExit()
                if ($process.ExitCode -eq 0) {
                    Write-Step "Gears completed successfully" "OK"
                } else {
                    Write-Step "Gears exited with code: $($process.ExitCode)" "WARN"
                }
            } catch {
                Write-Step "Profiler script failed: $($_.Exception.Message)" "WARN"
                Write-Step "Trying direct execution..." "WARN"
                if (Test-Path "gears.exe") {
                    $process = Start-Process -FilePath ".\gears.exe" -PassThru -NoNewWindow
                    $process.WaitForExit()
                }
            }
        } elseif (Test-Path "gears.exe") {
            Write-Step "Running gears directly..." "INFO"
            Write-Step "Close the gears window to continue..." "INFO"
            try {
                $process = Start-Process -FilePath ".\gears.exe" -PassThru -NoNewWindow
                $process.WaitForExit()
                if ($process.ExitCode -eq 0) {
                    Write-Step "Gears completed successfully" "OK"
                } else {
                    Write-Step "Gears exited with code: $($process.ExitCode)" "WARN"
                }
            }
            catch {
                Write-Step "Error running gears: $($_.Exception.Message)" "WARN"
            }
        } else {
            Write-Step "gears.exe not found" "WARN"
        }
        Pop-Location
    }
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
