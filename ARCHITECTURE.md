# Architecture

The profiler is an in-process DLL for Windows x64. It samples CPU, wall time,
and wait time, aggregates the samples with libdatadog, and writes or uploads
pprof profiles.

## Lifecycle and thread tracking

[`dllmain.cpp`](src/dd-win-prof/dllmain.cpp) creates the `Profiler` singleton
on process attach. Thread attach/detach notifications update `ThreadList`;
`ThreadInfo` holds OS handles, CPU consumption, and sampling timestamps.
Thread registration remains active when sampling is disabled.

The application controls sampling through `SetupProfiler()`, `StartProfiler()`,
and `StopProfiler()` in the [public API](src/dd-win-prof/dd-win-prof.h).
Configuration must be applied before starting.

Thread discovery relies on DLL lifecycle notifications. Loading the DLL after
application threads have already started does not register those existing
threads.

## Sampling and export

1. [`StackSamplerLoop`](src/dd-win-prof/StackSamplerLoop.cpp) visits threads
   for CPU and wall-time sampling, with separate configured thread thresholds.
   CPU samples use consumption deltas, capped to elapsed time to avoid overlap.
2. [`StackFrameCollector`](src/dd-win-prof/StackFrameCollector.cpp) suspends
   the target, captures its context, and walks the stack with Windows unwind
   APIs. The sampling loop resumes the thread before constructing the sample.
3. `CpuTimeProvider` records `cpu-time` and `cpu-samples`; `WallTimeProvider`
   records `wall-time` and `wait-time`. `SampleValueTypeProvider` assigns value
   offsets, and the providers buffer samples through `CollectorBase`.
4. [`SamplesCollector`](src/dd-win-prof/SamplesCollector.cpp) drains providers
   into `ProfileExporter`. `PprofAggregator` wraps libdatadog profile creation
   and serialization.
5. [`ProfileExporter`](src/dd-win-prof/ProfileExporter.cpp) writes local files
   and uploads profiles according to independent output/export settings.
   Function-name symbolization is optional and disabled by default.

RUM context is attached to samples as view labels. Session/view transitions
also produce timeline records included in exports and local `.rum-views.json`
files. `EnterView()` and `LeaveCurrentView()` are public; session and explicit
view context are supplied through the SDK-facing
[`dd-win-rum-private.h`](src/dd-win-prof/dd-win-rum-private.h) API.

## Background threads

| Thread | Responsibility |
|--------|----------------|
| `DD_StackSampler` | CPU and wall-time sampling at the configured period |
| `DD_worker` | Drain sample providers |
| `DD_exporter` | Export at the configured upload interval |

Configuration defaults and environment-variable handling live in
[`Configuration`](src/dd-win-prof/Configuration.cpp). Public overrides and
sampling options are described in [`ProfilerConfig`](src/dd-win-prof/dd-win-prof.h).
Logging is initialized at DLL load time, before `SetupProfiler()`.

## Suspension constraints

A suspended application thread may hold allocator or other process locks.
Stack capture must avoid allocations and blocking operations while it is
suspended. Windows unwinding can itself acquire locks; there is no sampler
watchdog to recover a blocked unwind. See `StackFrameCollector` for the
remaining unwind/deadlock caveats.
