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
| `ProfileExporterTests.cpp` | `ProfileExporter` initialization, tag preparation, defaults-only and API-overridden configs |
| `PprofAggregatorTests.cpp` | libdatadog pprof aggregation, sample types, profile serialization |
| `SymbolicationTests.cpp` | Call stack symbolization and function name resolution |
| `DynamicModuleTests.cpp` | Dynamically loaded module handling |
| `UuidTests.cpp` | UUID generation and formatting |
| `RumContextTests.cpp` | RUM context structs, `Profiler` RUM state management, `Sample` view context, `ProfileExporter` RUM tags/labels |

## Integration Tests

For end-to-end tests that run the Runner executable and validate pprof file content, see [`src/integration-tests/README.md`](../integration-tests/README.md).
