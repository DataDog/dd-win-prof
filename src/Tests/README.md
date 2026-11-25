# Tests

Unit tests for dd-win-prof, built with [Google Test](https://github.com/google/googletest).

## Prerequisites

[Configure CMake](../../CONTRIBUTING.md#development-setup) with
`DD_WIN_PROF_BUILD_TESTS=ON` (the top-level default). GoogleTest is fetched by CMake;
NuGet is not used.

## Building and Running

From the repository root:

```powershell
cmake --build build --target Tests --config Debug
.\build\src\Tests\Debug\Tests.exe
```

Replace `Debug` with `Release` for a Release run. CMake copies its required
`datadog_profiling_ffi.dll` beside `Tests.exe`.

## Test Files

| File | Description |
|------|-------------|
| `ConfigurationTests.cpp` | `Configuration` class defaults, env var handling, `ResetToDefaults`, `InitializeConfiguration`, `noEnvVars` mode, `ProfilerConfig` zero-init defaults |
| `ProfileExporterTests.cpp` | `ProfileExporter` initialization (including failure, rollback and retry), tag preparation, defaults-only and API-overridden configs, HTTP delivery, exactly-once final export and its statuses, `SamplesCollector::Stop` final status |
| `PprofAggregatorTests.cpp` | libdatadog pprof aggregation, custom sample types, reset behavior, periods, and profile serialization |
| `CpuOverlapTests.cpp` | CPU interval overlap calculations and sub-millisecond precision |
| `UiHangProfileTests.cpp` | UI-hang duration values, phase context, callback/HTTP serialization, and isolation across flushes |
| `ProfilerShutdownTests.cpp` | Process termination skips cleanup even when the profiler lock is held |
| `ProfilingDataCallbackTests.cpp` | C callback registration and caller-owned context, borrowed profile bytes, error recovery, and final delivery |
| `ProfilingErrorMessageTests.cpp` | Static error descriptions and unknown-code fallback |
| `ThreadListTests.cpp` | Thread registration, iteration, removal, and sampling-state reset |
| `SymbolicationTests.cpp` | Call stack symbolization and function name resolution |
| `DynamicModuleTests.cpp` | Dynamically loaded module handling |
| `UuidTests.cpp` | UUID generation and formatting |
| `StackSamplerLoopTests.cpp` | `StackSamplerLoop` one-shot runs, fresh loop per session, immediate wake-up on `Stop`, repeated `Stop`, no sample after a stop request, and destruction |
| `RumContextTests.cpp` | RUM context structs, `Profiler` RUM state management, `Sample` view context, `ProfileExporter` RUM tags/labels |
| `UiHangDetectorLifetimeTests.cpp` | `UiHangDetector` activation rollback and retry for each stage, active-instance lifetime, teardown order, and hook callback or probe post racing `Stop`/destruction, driven through a fake runtime |

## Integration Tests

For end-to-end tests that run the Runner executable and validate pprof file content, see [`src/integration-tests/README.md`](../integration-tests/README.md).
