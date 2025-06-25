# dd-win-prof
Implementation of a Windows CPU profiler

## Architecture Overview

This is a Windows native profiler that performs CPU sampling and exports profiles to Datadog. The profiler is implemented as a DLL that hooks into the target process and periodically samples cpu-bound threads stack.

### Entry Point and Initialization

**`dllmain.cpp`** - Main DLL entry point
- Hooks into process/thread lifecycle events (`DLL_PROCESS_ATTACH`, `DLL_THREAD_ATTACH`, etc.)
- Creates the singleton `Profiler` instance on process attach
- Automatically tracks new threads as they are created
- Manages thread cleanup on thread detach

**`InprocProfiling.cpp/.h`** - Public external API for applications to control profiling
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

**`PprofAggregator.cpp/.h`** - libdatadog integration (InprocProfiling namespace)
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
