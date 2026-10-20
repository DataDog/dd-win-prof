# dd-win-prof

> **⚠️ EARLY VERSION WARNING**
> 
> This is an early version of the Windows profiler and is **not recommended for production use**. 
> The software is provided as-is for evaluation and development purposes only.
> 
> **Use in production environments at your own risk.** Please thoroughly test in your specific 
> environment before considering any production deployment.

## Architecture Overview

This is a Windows native profiler that samples CPU and wall time and exports profiles to Datadog. The profiler is implemented as a DLL that hooks into the target process and periodically captures thread stacks.

### Entry Point and Initialization

**`dllmain.cpp`** - Main DLL entry point
- Hooks into process/thread lifecycle events (`DLL_PROCESS_ATTACH`, `DLL_THREAD_ATTACH`, etc.)
- Creates the singleton `Profiler` instance on process attach
- Automatically tracks new threads as they are created
- Manages thread cleanup on thread detach

**`dd-win-prof.cpp/.h`** - Public external API for applications to control profiling
- Exports `SetupProfiler()`, `StartProfiler()` and `StopProfiler()` functions for controlling the profiler
- Supports multiple sequential profiling sessions in one process (`Start` → optional UI-hang monitoring → `Stop` → `Start` again)
- Rejects repeated starts, monitoring outside a running session, multiple monitored windows in one session, and stops without a running session
- Exports `EnterView()` and `LeaveCurrentView()` for RUM view navigation

**`include/internal/dd-win-rum-private.h`** - SDK-internal API shipped for dependency consumers
- Exports `SetupProfilingDataCallback()` for SDK-managed profile delivery
- Exports `SetRumSession()` and `SetRumView()` for fine-grained RUM context management
- Pass `nullptr` to `SetRumSession()` to end the current session

### Core Profiler Management

**`Profiler.cpp/.h`** - Central coordinator and singleton
- The singleton is process-scoped; each start/stop pair creates and destroys a separate profiling session
- Serializes lifecycle operations and resets process-scoped thread sampling state between sessions
- UI-hang registration is session-scoped and must be repeated after a restart
- Manages the overall profiler lifecycle and orchestrates all components
- Creates and configures:
  - `ThreadList` for tracking active threads
  - `StackSamplerLoop` for periodic sampling
  - `CpuTimeProvider` for CPU time samples collection
  - `WallTimeProvider` for wall-time and wait-time samples
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
- Manages walltime sampling timestamps
- Uses `ScopedHandle` for automatic handle cleanup

### Stack Sampling Engine

**`StackSamplerLoop.cpp/.h`** - Main sampling thread ("DD_StackSampler")
- Runs at configurable frequency (default: every 20ms)
- `Stop()` wakes the wait for the next iteration immediately and joins the thread; an instance runs at most once, so each profiling session creates a new loop
- Implements CPU-based sampling strategy:
  - Only samples threads currently consuming CPU or about to run
  - Limits concurrent sampling to number of logical cores
  - Detects and prevents CPU time overlap
- Coordinates with `StackFrameCollector` for call stack capture
- Integrates with `CpuTimeProvider` and `WallTimeProvider` for sample storage

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

**`WallTimeProvider.cpp/.h`** - Walltime sampling collector
- Inherits from `CollectorBase`
- Defines sample types: "wall-time" and "wait-time" (nanoseconds)
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

**`Symbolication.cpp/.h`** - Function name resolution
- Integrates with Windows Debug Help Library (DbgHelp)
- Resolves instruction pointers to function names and line numbers if requested (obfuscated by default - i.e. empty function name)
- Manages symbol handler initialization and cleanup
- Supports module refresh for dynamically loaded libraries


### Data Flow Summary

1. **Initialization**: `dllmain.cpp` creates `Profiler` singleton
2. **Thread Tracking**: New threads automatically registered in `ThreadList`
3. **Sampling**: `StackSamplerLoop` samples CPU-active threads and performs wall-time sampling
4. **Stack Capture**: `StackFrameCollector` captures call stacks from these threads
5. **Sample Storage**: `CpuTimeProvider` and `WallTimeProvider` store samples via `CollectorBase`
6. **Collection**: `SamplesCollector` worker thread collects samples every 60ms
7. **Aggregation**: `PprofAggregator` aggregate samples into a lobdatadog profile
8. **Export**: `SamplesCollector` exporter thread triggers profile export every 60s
9. **Upload**: `ProfileExporter` serialize profile into pprof format and upload it to Datadog via libdatadog

### Threading Model

- **Main Application Threads**: Monitored and sampled
- **DD_StackSampler**: Performs periodic stack sampling (default: every 20ms)
- **DD_worker**: Collects samples from providers (every 60ms) 
- **DD_exporter**: Exports profiles to backend (every 60s)

### Configuration

Configuration is managed in `Configuration.cpp`. Values come from two sources, with code-based overrides taking precedence:

1. **Environment variables** (`EnvironmentVariables.h`): sampling period (default 20ms), threads to sample (default 5 for walltime, 64 for CPU), upload period (default 60s), service metadata, API key, agent URL, etc.

2. **`SetupProfiler(const ProfilerConfig*)`** (`dd-win-prof.cpp`): Callers pass a `ProfilerConfig` struct; `InitializeConfiguration()` applies its fields to the shared `Configuration` instance.

**Compatibility**: The lifecycle and SDK-private callback APIs use C linkage. `ProfilingData.struct_size` supports append-only extension, and `DD_WIN_PROF_CALL` makes the callback calling convention explicit. Seven zero-filled reserved bytes make the one-byte `is_last` tail occupy a full 64-bit unit; future fields are appended after that array so they increase `struct_size` instead of consuming implicit tail padding.

**SDK callback registration**: `Profiler` stores the function pointer and caller-owned context registered through `SetupProfilingDataCallback()`. `(NULL, NULL)` unregisters; `(NULL, non-NULL)` is rejected without replacing the previous registration. Registration survives configuration resets and stop/start cycles. Each `ProfileExporter` snapshots both pointers for its run. Stop and unregister before destroying the context.

**Callback delivery and errors**: `ProfilingData.pprof_bytes` is immutable, library-owned, and valid only during the synchronous callback. Retaining consumers copy it before returning. `ProfilingData.result` is zero for a profile; errors have null bytes and size zero. `GetProfilingErrorMessage(uint32_t)` returns static explanatory text, including a fallback for unknown codes. Final export errors have `is_last` set, just like final profiles. An unexpected exception during the final export is reported as `ProfilingErrorCode::InternalError` (5), unless the callback was already invoked for that attempt, so consumers always receive exactly one `is_last` callback per session. Errors do not trigger automatic shutdown.

**Exporter lifecycle and final delivery**: `ProfileExporter::Initialize()` commits its state only after every step succeeds. A failure releases everything it created, so the exporter can be retried or destroyed. `ExportFinal()` makes at most one final attempt and returns a `FinalExportStatus`: delivered; a profile-build, callback, network (transport error or non-2xx response) or internal failure; `LocalWriteFailed` when only the debug pprof file could not be written; or `NotExported` when there is no callback and HTTP export is disabled. `ExportFinal()` is the only final export path; `Export()` is periodic only and returns false when the profile could not be built or delivered (any non-2xx response or transport error), then resets the profile so the next export can recover. Calling `ExportFinal()` again returns `AlreadyFinalized` without sending. The attempt consumes the buffered samples and the RUM records it read, whatever its outcome. A failed final profile is dropped, never retried and never reported as delivered, so a transient network error at stop time loses the final profile; this is intended so that it is sent at most once. After the attempt, the exporter rejects `Add` and `Export`, so nothing reaches a later session, which always gets a new exporter. `Cleanup()` and the noexcept destructor only release resources and never export. The exporter is not thread-safe: `SamplesCollector` serializes every call under its export lock. `StopProfiler()` logs a final status other than `Delivered`.

**No-env-vars mode** (`ProfilerConfig.noEnvVars == true`): `ResetToDefaults()` is called first, then only explicit struct fields are applied. Environment-based profiler settings are ignored; logging is still configured at DLL load time. For HTTP delivery, `url` and `apiKey` are mandatory; `SetupProfiler` returns `false` if either is missing. Callback delivery does not require them.

**ProfilerConfig fields**: `serviceName`, `serviceEnvironment`, `serviceVersion`, `url`, `apiKey`, `tags`, `pprofOutputDirectory`, `symbolizeCallstacks`, plus tuning parameters (`cpuWallTimeSamplingPeriodNs`, `walltimeThreadsThreshold`, etc.). The **log directory** is not configurable via ProfilerConfig; it is set at DLL load time from `DD_TRACE_LOG_DIRECTORY` in `Log.h`.

Function names are not symbolized by default. To enable symbolization, set `ProfilerConfig.symbolizeCallstacks` to `true` before calling `SetupProfiler`.

**Hardcoded settings**:
- Collection period: 60ms  
- Max stack frames: 512

## Next steps 
- lock provider: detect wait pauses (use Wait Chain Transversal Windows API)
- thread lifetime events (start and stop) to identify short lived threads

## Limitations

- Possible issues with thread safety
We will need to ensure no allocations take place in the samples. Could need a `StackSamplerLoopManager` to detect deadlocks

- Thread lists are always ON (even if Profiling is off)
If we do not instrument thread creation, we can not be ready to sample.

- If sample collector is too slow, it can not keep up with samples being added. We should consider adjusting sampling periods if this happens.

- If you Control-C right at the time of thread creation from libdatadog, you will get a PANIC