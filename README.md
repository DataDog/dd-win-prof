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

The profiler can be configured in two ways:

### Option 1: Agent-based (with Datadog Agent)
**Required environment variables:**
- `DD_SERVICE=your-app-name` - Application name

### Option 2: Agentless (direct to Datadog)
**Required environment variables:**
- `DD_SERVICE=your-app-name` - Application name
- `DD_PROFILING_AGENTLESS=1` - Enable agentless mode
- `DD_SITE=datadoghq.com` - Datadog site (varies by region)
- `DD_API_KEY=your-api-key` - Your Datadog API key

### Optional (but recommended) variables:
- `DD_VERSION=1.0.0` - Application version for profile identification
- `DD_ENV=production` - Environment name (dev, staging, production, etc.)


Call [StartProfiler](./src/dd-win-prof/dd-win-prof.h) when you are ready to run the profiler.
[StopProfiler](./src/dd-win-prof/dd-win-prof.h) when you want to stop it.

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

## Architecture Overview

This is a Windows native profiler that performs CPU sampling and exports profiles to Datadog. The profiler is implemented as a DLL that hooks into the target process and periodically samples cpu-bound threads stack.

### Entry Point and Initialization

**`dllmain.cpp`** - Main DLL entry point
- Hooks into process/thread lifecycle events (`DLL_PROCESS_ATTACH`, `DLL_THREAD_ATTACH`, etc.)
- Creates the singleton `Profiler` instance on process attach
- Automatically tracks new threads as they are created
- Manages thread cleanup on thread detach

**`dd-win-prof.cpp/.h`** - Public external API for applications to control profiling
- Exports `StartProfiler()` and `StopProfiler()` functions for controlling the profiler

### Core Profiler Management

**`Profiler.cpp/.h`** - Central coordinator and singleton
- Manages the overall profiler lifecycle and orchestrates all components
- Creates and configures:
  - `ThreadList` for tracking active threads
  - `StackSamplerLoop` for periodic sampling
  - `CpuTimeProvider` for CPU time samples collection
  - `SamplesCollector` for collecting samples from all providers
  - `ProfileExporter` for exporting profiles
- Handles profiler startup/shutdown sequences

### Thread Management

**`ThreadList.cpp/.h`** - Thread registry and iteration
- Maintains a list of active threads (`std::vector<std::shared_ptr<ThreadInfo>>`)
- Provides thread iteration with multiple concurrent iterators
- Thread-safe operations with recursive mutex

**`ThreadInfo.cpp/.h`** - Per-thread state tracking
- Stores thread ID, OS handle, name, CPU consumption, and timestamps
- Tracks CPU usage over time for delta calculations
- Manages walltime sampling timestamps (WIP)
- Uses `ScopedHandle` for automatic handle cleanup

### Stack Sampling Engine

**`StackSamplerLoop.cpp/.h`** - Main sampling thread ("DD_StackSampler")
- Runs at configurable frequency (default: every 10ms)
- Implements CPU-based sampling strategy:
  - Only samples threads currently consuming CPU or about to run
  - Limits concurrent sampling to number of logical cores
  - Detects and prevents CPU time overlap
- Coordinates with `StackFrameCollector` for call stack capture
- Integrates with `CpuTimeProvider` for sample storage

**`StackFrameCollector.cpp/.h`** - 64-bit stack walking
- Suspends/resumes target threads safely for stack capture
- Performs Windows-specific stack unwinding using `RtlVirtualUnwind` API
- Validates stack pointers and handles exceptions
- Captures up to 512 frames per sample
- Uses dedicated Windows APIs (`GetThreadContext`, `RtlLookupFunctionEntry`)
- avoid memory allocation to limit deadlock (i.e. suspended thread might own the malloc lock)

### Sample Collection and Providers

**`ISamplesProvider.h`** - Provider interface
- Abstract interface for sample providers
- Defines `MoveSamples()` for batch sample retrieval
- Implemented by collectors like `CpuTimeProvider`

**`CollectorBase.h`** - Base collector implementation
- Thread-safe sample storage with mutex protection
- Implements `ISamplesProvider` interface
- Provides `Add()` method for collectors to store samples
- Handles sample type values offset management

**`CpuTimeProvider.cpp/.h`** - CPU sampling collector
- Inherits from `CollectorBase`
- Defines sample types: "cpu" (nanoseconds) and "cpu-samples" (count)
- Stores samples collected by `StackSamplerLoop`

**`SampleValueTypeProvider.cpp/.h`** - Sample type registry
- Manages registration and deduplication of sample types values
- Provides offset mapping for different sample types values
- Ensures consistent value type definitions across providers

### Sample Data Structure

**`Sample.cpp/.h`** - Individual sample representation
- Contains:
  - `_timestamp` - High-precision sample timestamp
  - `_callstack` - Vector of instruction pointer addresses (max 512)
  - `_values` - Metrics values (CPU time, sample count, etc.)
  - `_threadInfo` - Associated thread information
- Configurable values array size (default: 16 slots)

### Sample Aggregation and Export

**`SamplesCollector.cpp/.h`** - Sample aggregation coordinator
- Runs two background threads:
  - **"DD_worker"** - Periodically collects samples from providers (60ms interval)
  - **"DD_exporter"** - Periodically exports profiles (60s interval)
- Registers multiple `ISamplesProvider` instances
- Thread-safe sample collection with export mutex
- Forwards samples to `ProfileExporter`

**`ProfileExporter.cpp/.h`** - Profile export manager
- Receives samples from `SamplesCollector`
- Manages libdatadog profile creation with labels/values and export
- Generates unique runtime IDs for profile identification

**`PprofAggregator.cpp/.h`** - libdatadog integration
- C++ wrapper around libdatadog profiling APIs
- Handles profile initialization with sample types
- Supports multiple sample types: CPU_SAMPLES, CPU_TIME_NS, WALL_TIME_NS, etc.
- Provides profile serialization to pprof format
- Manages libdatadog resource lifecycle

### Utility and Platform Components

**`OsSpecificApi.cpp/.h`** - Windows-specific system APIs
- Thread User + Kernel CPU time measurement using `GetThreadTimes`
- Thread state detection via `NtQueryInformationThread`
- Processor count detection
- Thread scheduling state determination

**`OpSysTools.cpp/.h`** - OS utility functions
- Thread naming using `SetThreadDescription`/`GetThreadDescription` 
- High-precision timestamp generation
- Dynamic API loading for Windows version compatibility

**`Symbolication.cpp/.h`** - Debug symbol resolution
- Integrates with Windows Debug Help Library (DbgHelp)
- Resolves instruction pointers to function names and line numbers
- Manages symbol handler initialization and cleanup
- Supports module refresh for dynamically loaded libraries


### Data Flow Summary

1. **Initialization**: `dllmain.cpp` creates `Profiler` singleton
2. **Thread Tracking**: New threads automatically registered in `ThreadList`
3. **Sampling**: `StackSamplerLoop` periodically samples CPU-active threads
4. **Stack Capture**: `StackFrameCollector` captures call stacks from these threads
5. **Sample Storage**: `CpuTimeProvider` stores samples via `CollectorBase`
6. **Collection**: `SamplesCollector` worker thread collects samples every 60ms
7. **Aggregation**: `PprofAggregator` aggregate samples into a lobdatadog profile
8. **Export**: `SamplesCollector` exporter thread triggers profile export every 60s
9. **Upload**: `ProfileExporter` serialize profile into pprof format and upload it to Datadog via libdatadog

### Threading Model

- **Main Application Threads**: Monitored and sampled
- **DD_StackSampler**: Performs periodic stack sampling (every 10ms)
- **DD_worker**: Collects samples from providers (every 60ms) 
- **DD_exporter**: Exports profiles to backend (every 60s)

### Configuration

Configuration is retrieved in `Configuration.cpp` from environment variables defined in `EnvironmentVariables.h` such as sampling period (default 10ms), number of threads to sample (default 5) or upload period (default 60s)
Other settings are hardcoded:
- Collection period: 60ms  
- Max stack frames: 512

## Next steps 
- wall time provider: don't filter out not running/scheduled threads
- thread lifetime events (start and stop) to identify short lived threads

## Limitations

- Possible issues with thread safety
We will need to ensure no allocations take place in the samples. Could need a `StackSamplerLoopManager` to detect deadlocks

- Thread lists are always ON (even if Profiling is off)
If we do not instrument thread creation, we can not be ready to sample.

- If sample collector is too slow, it can not keep up with samples being added. We should consider adjusting sampling periods if this happens.

- If you Control-C right at the time of thread creation from libdatadog, you will get a PANIC

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
