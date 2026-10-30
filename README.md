# dd-win-prof

> **⚠️ EARLY VERSION WARNING**
> 
> This is an early version of the Windows profiler and is **not recommended for production use**. 
> The software is provided as-is for evaluation and development purposes only.
> 
> **Use in production environments at your own risk.** Please thoroughly test in your specific 
> environment before considering any production deployment.

Implementation of a Windows CPU and wall-time profiler

## Usage : how to profile your 64-bit Windows application

In your project, you need the following files:

- **dd-win-prof.h**: header containing the declaration of the `StartProfiler` and `StopProfiler` functions to be called when the application starts and when it stops.
- **dd-win-prof.lib**: library containing the stub for the two functions pointing to **dd-win-prof.dll**

Deploy **dd-win-prof.dll** next to your application's executable. It links
libdatadog statically for profile serialization and upload.

The separate **datadog_profiling_ffi.dll** is needed by the unit-test executable,
not by applications that only link `dd-win-prof.lib`. The corresponding `.pdb`
files are available for debugging.

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

To copy the profiler beside your application's binary after each build, call
`dd_win_prof_copy_runtime_deps`. The helper also copies the FFI DLL used by tests:

```
dd_win_prof_copy_runtime_deps(your_application)
```

(You can optionally supply `INCLUDE_PDBS` to copy `.pdb` files for these libraries as well.)

For installed builds, add `dd-win-prof` to your export set and install the public header:

```cmake
install(TARGETS dd-win-prof
   EXPORT YourApplicationTargets
   RUNTIME DESTINATION bin
   ARCHIVE DESTINATION lib
)
# The exported target's include path is <prefix>/include, so install the public header
# there too. ${dd-win-prof_SOURCE_DIR} is set by FetchContent; with add_subdirectory
# use the path you added.
install(FILES "${dd-win-prof_SOURCE_DIR}/src/dd-win-prof/include/dd-win-prof.h" DESTINATION include)
```

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

**No-env-vars mode** (`config.noEnvVars = true`): Uses struct values and defaults
instead of environment-based profiler settings. In this mode, `url` and `apiKey`
are **mandatory**; `SetupProfiler` returns `false` if either is missing.
Set these fields before calling `SetupProfiler`. `url` is a Datadog site name,
not a full intake URL:

```C++
    config.noEnvVars = true;
    config.url = "datadoghq.com";
    config.apiKey = "your_datadog_api_key_here";
    config.serviceName = "my-windows-app";
```

**ProfilerConfig fields** (see `dd-win-prof.h`):

| Field | Description |
|-------|-------------|
| `noEnvVars` | If true, use struct values and defaults instead of environment-based profiler settings (default: false) |
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

When `DD_API_KEY` is set, profiling data is sent directly to the Datadog intake without requiring a local Datadog Agent.

- `DD_SERVICE=your-app-name` - Application name
- `DD_SITE=datadoghq.com` - Datadog site (varies by region, defaults to `datadoghq.com`)
- `DD_API_KEY=your-api-key` - Your Datadog API key

#### Optional (recommended)

- `DD_VERSION=1.0.0` - Application version
- `DD_ENV=production` - Environment (dev, staging, production)
- `DD_TRACE_LOG_DIRECTORY` - Log output directory
- `DD_INTERNAL_PROFILING_OUTPUT_DIR` - Local pprof debug output directory

### Example configurations

Set these variables before launching the application. For example, in PowerShell:

**Agentless:**
```powershell
$env:DD_SERVICE = "my-windows-app"
$env:DD_SITE = "datadoghq.com"
$env:DD_API_KEY = "your_datadog_api_key_here"
$env:DD_VERSION = "1.2.3"
$env:DD_ENV = "production"
```

**Agent-based** (leave `DD_API_KEY` unset):
```powershell
$env:DD_SERVICE = "my-windows-app"
$env:DD_VERSION = "1.2.3"
$env:DD_ENV = "production"
```

Call [StartProfiler](./src/dd-win-prof/include/dd-win-prof.h) when ready; [StopProfiler](./src/dd-win-prof/include/dd-win-prof.h) when done. With environment-based configuration, use `DD_PROFILING_ENABLED=0` to disable profiling even if `StartProfiler` is called.

Call `StopProfiler()` before process exit to export the final profile. During
process termination, the DLL skips profiler cleanup because Windows may already
have terminated threads holding its locks. This does not add support for dynamic
DLL unloading with `FreeLibrary()`.

**NOTE:** Method names are obfuscated by default. Add `DD_PROFILING_INTERNAL_SYMBOLIZE_CALLSTACKS=1` to enable symbolization.

## Contributing

For build instructions, development setup, testing, and releases, see
[CONTRIBUTING.md](CONTRIBUTING.md).
