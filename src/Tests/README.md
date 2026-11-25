# Tests

Unit tests for dd-win-prof, built with [Google Test](https://github.com/google/googletest).

## Prerequisites

- **NuGet CLI** to restore test dependencies (Google Test package)

## Building and Running

```cmd
# Restore test dependencies
nuget restore src\Tests\packages.config -PackagesDirectory src\packages

# Build tests
msbuild src\Tests\Tests.vcxproj /p:Configuration=Release /p:Platform=x64

# Run tests
src\Tests\x64\Release\Tests.exe
```

Or with CMake:

```cmd
cmake --build build --target Tests --config Debug
build\src\Tests\Debug\Tests.exe
```

## Test Files

| File | Description |
|------|-------------|
| `ConfigurationTests.cpp` | `Configuration` class defaults, env var handling, `ResetToDefaults`, `InitializeConfiguration`, `noEnvVars` mode, `ProfilerConfig` zero-init defaults |
| `ProfileExporterTests.cpp` | `ProfileExporter` initialization (including failure, rollback and retry), tag preparation, defaults-only and API-overridden configs, HTTP delivery, exactly-once final export and its statuses, `SamplesCollector::Stop` final status |
| `PprofAggregatorTests.cpp` | libdatadog pprof aggregation, sample types, profile serialization |
| `SymbolicationTests.cpp` | Call stack symbolization and function name resolution |
| `DynamicModuleTests.cpp` | Dynamically loaded module handling |
| `UuidTests.cpp` | UUID generation and formatting |
| `SamplesCollectorTests.cpp` | `SamplesCollector` rollback when either worker cannot be created, workers detached under a simulated loader lock, final export once (also with concurrent `Stop`), repeated `Stop`, per-instance shutdown state, and destruction |
| `StackSamplerLoopTests.cpp` | `StackSamplerLoop` one-shot runs, fresh loop per session, immediate wake-up on `Stop`, repeated `Stop`, no sample after a stop request, and destruction |
| `RumContextTests.cpp` | RUM context structs, `Profiler` RUM state management, `Sample` view context, `ProfileExporter` RUM tags/labels |
| `UiHangDetectorLifetimeTests.cpp` | `UiHangDetector` activation rollback and retry for each stage, active-instance lifetime, teardown order, and hook callback or probe post racing `Stop`/destruction, driven through a fake runtime |

## Integration Tests

For end-to-end tests that run the Runner executable and validate pprof file content, see [`src/integration-tests/README.md`](../integration-tests/README.md).
