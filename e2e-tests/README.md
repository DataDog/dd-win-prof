# Vulkan Examples with Datadog Profiler

A wrapper that sets up the Sascha Willems Vulkan Examples with automatic Datadog profiler integration.

## What This Does

This script automatically:
- Downloads and builds Vulkan examples
- Integrates your Datadog profiler (dd-win-prof.dll)
- Creates launcher scripts for profiled execution
- Handles all environment setup (Visual Studio, CMake, dependencies)

## Quick Start

### 1. Build your profiler first
```cmd
msbuild src\WindowsProfiler.sln /p:Configuration=Release /p:Platform=x64
```

### 2. Build Vulkan examples with profiling
```powershell
.\vulcan_quickstart.ps1 -EnableProfiler
```

### 3. Run examples
The script builds everything and runs the examples automatically. When profiling is enabled, you'll see:

```
Running computeheadless with profiler integration...
Auto-set DD_SERVICE=vulkan-computeheadless (from executable name)
Successfully injected profiler DLL into process 12345
```

## Configuration

Create a `.env` file in the build directory (`vulkan_examples\Vulkan\build\bin\Release\.env`):

```env
DD_SITE=datadoghq.com
DD_API_KEY=your_api_key_here
DD_TRACE_DEBUG=1
```

Note: DD_SERVICE is automatically set from the executable name.

## Build Options

```powershell
# Build with profiling
.\vulcan_quickstart.ps1 -EnableProfiler

# Build without profiling (default)
.\vulcan_quickstart.ps1

# Clean and rebuild
.\vulcan_quickstart.ps1 -Clean
.\vulcan_quickstart.ps1 -EnableProfiler

# CI mode (headless)
.\vulcan_quickstart.ps1 -EnableProfiler -CI

# Force reconfiguration
.\vulcan_quickstart.ps1 -EnableProfiler -ForceReconfigure
```

## Manual Script Usage

After building with profiler integration, you can run examples manually:

```cmd
# Navigate to build directory
cd vulkan_examples\Vulkan\build\bin\Release

# Run with profiler using batch scripts
run-triangle-with-profiler.bat
run-gears-with-profiler.bat
run-computeheadless-with-profiler.bat

# Or using PowerShell scripts
.\run-triangle-with-profiler.ps1
.\run-gears-with-profiler.ps1
.\run-computeheadless-with-profiler.ps1

# Or directly with ProfilerInjector
ProfilerInjector.exe triangle.exe
ProfilerInjector.exe gears.exe
ProfilerInjector.exe computeheadless.exe

# Run without profiler
triangle.exe
gears.exe
computeheadless.exe
```

## Features

- API keys are hidden in console output
- Service names auto-set from executable name
- Smart re-runs skip setup if already done
- Zero-code integration using DLL injection

## Requirements

- Windows 10/11
- Visual Studio 2022 with C++ development tools
- Your profiler built (see step 1 above)

## Troubleshooting

**Profiler DLL not found:**
```powershell
msbuild src\WindowsProfiler.sln /p:Configuration=Release /p:Platform=x64
```

**Script files missing:**
```powershell
.\vulcan_quickstart.ps1 -EnableProfiler -ForceReconfigure
```

**Build errors:**
```powershell
.\vulcan_quickstart.ps1 -Clean
.\vulcan_quickstart.ps1 -EnableProfiler
```

**Environment issues:**
Make sure you have Visual Studio 2022 with C++ tools installed.
