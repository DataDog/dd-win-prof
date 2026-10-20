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
'' | & .\build\src\Tests\Debug\Tests.exe
```

Replace `Debug` with `Release` for a Release run. The blank input avoids the test
executable's pause after completion. CMake copies its required
`datadog_profiling_ffi.dll` beside `Tests.exe`.

## Test Files

| File | Description |
|------|-------------|
| `ConfigurationTests.cpp` | `Configuration` class defaults, env var handling, `ResetToDefaults`, `InitializeConfiguration`, `noEnvVars` mode, `ProfilerConfig` zero-init defaults |
| `ProfileExporterTests.cpp` | `ProfileExporter` initialization, tag preparation, defaults-only and API-overridden configs |
| `PprofAggregatorTests.cpp` | libdatadog pprof aggregation, sample types, profile serialization |
| `SymbolicationTests.cpp` | Call stack symbolization and function name resolution |
| `DynamicModuleTests.cpp` | Dynamically loaded module handling |
| `UuidTests.cpp` | UUID generation and formatting |
| `RumContextTests.cpp` | RUM context structs, `Profiler` RUM state management, `Sample` view context, `ProfileExporter` RUM tags/labels |

## Integration Tests

For end-to-end tests that run the Runner executable and validate pprof file content, see [`src/integration-tests/README.md`](../integration-tests/README.md).
