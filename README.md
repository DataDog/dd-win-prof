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

## CMake Setup

If your project is built with CMake, you can include **dd-win-prof** as a dependency using `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
   dd-win-prof
   GIT_REPOSITORY https://github.com/DataDog/dd-win-prof.git
   GIT_TAG        main
)
FetchContent_MakeAvailable(dd-win-prof)
```

Then you can use `target_link_libraries` to configure your project's include paths and linker inputs appropriately:

```cmake
target_link_libraries(your_application PRIVATE dd-win-prof)
```

To ensure that your development builds will have the requisite DLLs present alongside the application binary, you can call `dd_win_prof_copy_runtime_deps`, which adds a `POST_BUILD` command to copy `dd-win-prof.dll` and `datadog_profiling_ffi.dll` alongside your application's binaries:

```
dd_win_prof_copy_runtime_deps(your_application)
```

(You can optionally supply `INCLUDE_PDBS` to copy `.pdb` files for these libraries as well.)

Finally, if you're generating installed builds of your CMake project, you'll also want to ensure that `dd-win-prof` is added to the export set (to make `dd-win-prof.dll` available alongside your binaries, and to make the `dd-win-prof` CMake target accessible), and that `datadog_profiling_ffi.dll` is also copied to the same directory:

```cmake
install(TARGETS dd-win-prof
   EXPORT YourApplicationTargets
   RUNTIME DESTINATION bin
)
install(FILES "$<TARGET_FILE:libdatadog_dynamic>" DESTINATION bin)
```

If you're _not_ using CMake, you may continue building from `src/WindowsProfiler.sln` (with accompanying PowerShell scripts) and configuring your project's dependencies manually. See [**How to build dd-win-prof**](#how-to-build-dd-win-prof) below for manual build instructions.

## Configuration

The profiler can be configured in two main ways: **code-based** (via `ProfilerConfig` and `SetupProfiler`) or **environment variables**. Code-based settings override environment variables when both are provided.

### Option 1: Code-based configuration

Define your configuration in a `ProfilerConfig` instance and pass it to `SetupProfiler` before calling `StartProfiler`:

```C++
    ProfilerConfig config;
    ::ZeroMemory(&config, sizeof(ProfilerConfig));
    config.size = sizeof(ProfilerConfig);
    config.serviceEnvironment = "production";
    config.serviceName = "my-windows-app";
    config.serviceVersion = "1.2.3";
    if (!SetupProfiler(&config)) {
        // Setup failed (e.g. mandatory fields missing when noEnvVars=true)
        return -1;
    }
```

**No-env-vars mode** (`config.noEnvVars = true`): Ignores all environment variables and uses only values from the struct. In this mode, `url` and `apiKey` are **mandatory**; `SetupProfiler` returns `false` if either is missing.

```C++
    config.noEnvVars = true;
    config.url = "https://intake.datadoghq.com";
    config.apiKey = "your_datadog_api_key_here";
    config.serviceName = "my-windows-app";
```

**ProfilerConfig fields** (see `dd-win-prof.h`):

| Field | Description |
|-------|-------------|
| `noEnvVars` | If true, ignore env vars; use only struct values (default: false) |
| `serviceName`, `serviceEnvironment`, `serviceVersion` | Application metadata |
| `url`, `apiKey` | Mandatory when `noEnvVars=true`; otherwise can come from env |
| `pprofOutputDirectory` | Override for local pprof debug output (default: empty) |
| `tags`, `symbolizeCallstacks` | Tags and symbolization options |

**Note:** Log output directory is **not** configurable via `ProfilerConfig`. It is set at DLL load time via the `DD_TRACE_LOG_DIRECTORY` environment variable (default: `%PROGRAMDATA%\Datadog Tracer\logs`).

**NOTE:** Method names are obfuscated by default. Add `config.symbolizeCallstacks = true` to enable symbolization.

### Option 2: Environment variables

#### Agent-based (with Datadog Agent)

- `DD_SERVICE=your-app-name` - Application name

#### Agentless (direct to Datadog)

- `DD_SERVICE=your-app-name` - Application name
- `DD_PROFILING_AGENTLESS=1` - Enable agentless mode
- `DD_SITE=datadoghq.com` - Datadog site (varies by region)
- `DD_API_KEY=your-api-key` - Your Datadog API key

#### Optional (recommended)

- `DD_VERSION=1.0.0` - Application version
- `DD_ENV=production` - Environment (dev, staging, production)
- `DD_TRACE_LOG_DIRECTORY` - Log output directory
- `DD_INTERNAL_PROFILING_OUTPUT_DIR` - Local pprof debug output directory

### Example configurations

**Agentless via env vars:**
```bash
DD_SERVICE=my-windows-app
DD_PROFILING_AGENTLESS=1
DD_SITE=datadoghq.com
DD_API_KEY=your_datadog_api_key_here
DD_VERSION=1.2.3
DD_ENV=production
```

**Agent-based:**
```bash
DD_SERVICE=my-windows-app
DD_VERSION=1.2.3
DD_ENV=production
```

Call [StartProfiler](./src/dd-win-prof/dd-win-prof.h) when ready; [StopProfiler](./src/dd-win-prof/dd-win-prof.h) when done. Use `DD_PROFILING_ENABLED=0` to disable profiling even if `StartProfiler` is called.

**NOTE:** Method names are obfuscated by default. Add `DD_PROFILING_INTERNAL_SYMBOLIZE_CALLSTACKS=1` to enable symbolization.


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

### Optional: Build and Run Test Runner

The **Runner** project is an example C++ application for testing the profiler with four built-in scenarios. It supports both environment-variable and `--noenvvars` modes, plus PowerShell helper scripts.

```cmd
msbuild src\Runner\Runner.vcxproj /p:Configuration=Debug /p:Platform=x64
```

See [`src/Runner/README.md`](src/Runner/README.md) for full CLI reference, PowerShell scripts, and examples.

### Optional: Build and Run Tests

```cmd
nuget restore src\Tests\packages.config -PackagesDirectory src\packages
msbuild src\Tests\Tests.vcxproj /p:Configuration=Release /p:Platform=x64
src\Tests\x64\Release\Tests.exe
```

See [`src/Tests/README.md`](src/Tests/README.md) for details on prerequisites and test coverage.

### Optional: Run Integration Tests

Integration tests exercise the full profiling pipeline (Runner execution, pprof output, log validation). They require **Python 3.9+** with a few packages.

```powershell
# Install Python dependencies (one-time)
src\integration-tests\install-dependencies.ps1

# Run the RUM context integration test
src\integration-tests\test_rum_scenario.ps1
```

See [`src/integration-tests/README.md`](src/integration-tests/README.md) for details.

### Automated Build

For reference, see the complete automated build process in [`.github/workflows/test.yml`](.github/workflows/test.yml).

The CI uses `windows-2022` runners which come pre-installed with **Visual Studio Enterprise 2022** and MSBuild tools.
