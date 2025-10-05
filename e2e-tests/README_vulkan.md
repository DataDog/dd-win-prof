# Vulkan Examples with Datadog Profiler

Builds Vulkan examples with automatic Datadog profiler integration.

## Quick Start

### 1. Build your profiler
```cmd
msbuild src\WindowsProfiler.sln /p:Configuration=Release /p:Platform=x64
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
```

### 3. Build and run
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

After building, run examples manually from `vulkan_examples\Vulkan\build\bin\Release\`:

```cmd
# With profiler
.\run-triangle-with-profiler.bat
ProfilerInjector.exe gears.exe

# Without profiler  
triangle.exe
gears.exe
```
