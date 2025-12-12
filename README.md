# dd-win-prof

> **⚠️ EARLY VERSION WARNING**
> 
> This is an early version of the Windows profiler and is **not recommended for production use**. 
> The software is provided as-is for evaluation and development purposes only.
> 
> **Use in production environments at your own risk.** Please thoroughly test in your specific 
> environment before considering any production deployment.

Implementation of a Windows CPU profiler

## Usage : how to profile your 64-bit Windows application

In your project, you need the following files:

- **dd-win-prof.h**: header containing the declaration of the `StartProfiler` and `StopProfiler` functions to be called when the application starts and when it stops.
- **dd-win-prof.lib**: library containing the stub for the two functions pointing to **dd-win-prof.dll**

The following files are required in the folder from where the application runs:
- **dd-win-prof.dll**: profiling code
- **datadog_profiling_ffi.dll**: responsible for serializing and sending the profiles to Datadog via HTTP
*Note: the corresponding .pdb files are available for debugging purposes*

## Configuration

The profiler can be configured in three ways:

### Option 1: Code based
Before starting the profiler, define your configuration settings in a `ProfilerConfig` instance and pass it to `SetupProfiler` like the following:
```C++
    ProfilerConfig config;
    ::ZeroMemory(&config, sizeof(ProfilerConfig));
    config.size = sizeof(ProfilerConfig);
    config.serviceEnvironment = "production";
    config.serviceName = "my-windows-app";
    config.serviceVersion = "1.2.3";
    SetupProfiler(&config);
```
### NOTE: method names are obfuscated by default. Add `config.symbolizeCallstacks = true;` to enable symbolization.


### Option 2: Agent-based (with Datadog Agent)
**Required environment variables:**
- `DD_SERVICE=your-app-name` - Application name

### Option 3: Agentless (direct to Datadog)
**Required environment variables:**
- `DD_SERVICE=your-app-name` - Application name
- `DD_PROFILING_AGENTLESS=1` - Enable agentless mode
- `DD_SITE=datadoghq.com` - Datadog site (varies by region)
- `DD_API_KEY=your-api-key` - Your Datadog API key

### Optional (but recommended) variables:
- `DD_VERSION=1.0.0` - Application version for profile identification
- `DD_ENV=production` - Environment name (dev, staging, production, etc.)

Call [StartProfiler](./src/dd-win-prof/dd-win-prof.h) when you are ready to run the profiler.
[StopProfiler](./src/dd-win-prof/dd-win-prof.h) when you want to stop it. Note that, instead of using environment variables, it is possible to configure some settings via `SetupProfiler()` API.

### NOTE: use `DD_PROFILING_ENABLED=0`to disable profiling even if `StartProfiler` is called.

### Example configuration:

**Agentless setup:**
```bash
DD_SERVICE=my-windows-app
DD_PROFILING_AGENTLESS=1
DD_SITE=datadoghq.com
DD_API_KEY=your_datadog_api_key_here
DD_VERSION=1.2.3
DD_ENV=production
```

**Agent-based setup:**
```bash
DD_SERVICE=my-windows-app
DD_VERSION=1.2.3
DD_ENV=production
```
### NOTE: method names are obfuscated by default. Add `DD_PROFILING_INTERNAL_SYMBOLIZE_CALLSTACKS=1` to enable symbolization.


## How to build dd-win-prof

### Prerequisites

- **Windows 10/11** or **Windows Server 2019/2022**
- **Visual Studio 2022** or later with C++ development tools (or **Build Tools for Visual Studio**)
- **PowerShell 5.1** or later

### Dependencies

The profiler requires two third-party libraries that are automatically downloaded:

- **libdatadog v19.0.0** - Datadog profiling library for profile serialization and upload
- **spdlog v1.14.1** - Logging library

### Build Steps

1. **Download dependencies**:
   ```powershell
   .\scripts\download-libdatadog.ps1 -Version 19.0.0 -Platform x64
   .\scripts\download-spdlog.ps1 -Version 1.14.1
   ```

2. **Build the profiler**:
   ```cmd
   msbuild src\dd-win-prof\dd-win-prof.vcxproj /p:Configuration=Release /p:Platform=x64
   ```

   For debug builds:
   ```cmd
   msbuild src\dd-win-prof\dd-win-prof.vcxproj /p:Configuration=Debug /p:Platform=x64
   ```

3. **Output files** will be generated in:
   - `src\dd-win-prof\x64\Release\` (or `Debug\`)
   - Key files: `dd-win-prof.dll`, `dd-win-prof.lib`, `dd-win-prof.pdb`

### Optional: Build Test Runner

The **Runner** project provides an example C++ application for testing the profiler:

```cmd
msbuild src\Runner\Runner.vcxproj /p:Configuration=Release /p:Platform=x64
```

### Optional: Build and Run Tests

**Note**: Requires **NuGet CLI** to restore test dependencies.

```cmd
# Restore test dependencies
nuget restore src\Tests\packages.config -PackagesDirectory src\packages

# Build tests
msbuild src\Tests\Tests.vcxproj /p:Configuration=Release /p:Platform=x64

# Run tests
src\Tests\x64\Release\Tests.exe
```

### Automated Build

For reference, see the complete automated build process in [`.github/workflows/test.yml`](.github/workflows/test.yml).

The CI uses `windows-2022` runners which come pre-installed with **Visual Studio Enterprise 2022** and MSBuild tools.
