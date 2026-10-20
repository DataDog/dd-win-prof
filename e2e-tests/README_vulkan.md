# Vulkan Examples with Datadog Profiler

Builds Vulkan examples with automatic Datadog profiler integration.

## Quick Start

### 1. Build your profiler

From the repository root:

```powershell
cmake -G "Visual Studio 17 2022" -A x64 -B build
cmake --build build --config Release --target dd-win-prof ProfilerInjector
```

### 2. Create .env file for profiler configuration
Create `.env` in the `e2e-tests` directory (will be copied to executables):

```env
DD_SITE=datadoghq.com
DD_API_KEY=your_api_key_here
# No Service is needed!
DD_ENV=development
DD_TRACE_DEBUG=1
DD_PROFILING_LOG_LEVEL=debug
DD_PROFILING_LOG_TO_CONSOLE=1
# Optional: save pprof files locally
# DD_INTERNAL_PROFILING_OUTPUT_DIR=e2e-tests/pprof
```

### 3. Build and run

Run from `e2e-tests`:

```powershell
.\vulcan_quickstart.ps1 -EnableProfiler
```

The script runs `computeheadless` automatically and shows command lines for other examples.

## Build Options

```powershell
# Build with profiling
.\vulcan_quickstart.ps1 -EnableProfiler

# Build specific examples only
.\vulcan_quickstart.ps1 -EnableProfiler -BuildTargets @("triangle", "gears")

# Clean rebuild
.\vulcan_quickstart.ps1 -Clean
.\vulcan_quickstart.ps1 -EnableProfiler
```

## Manual Execution

After building, run examples manually from `e2e-tests\vulkan_examples\build\bin\Release\`
(relative to the repository root):

```cmd
# With profiler
.\run-triangle-with-profiler.bat
ProfilerInjector.exe gears.exe

# Without profiler  
triangle.exe
gears.exe
```
