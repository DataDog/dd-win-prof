# dd-win-prof

Native CPU, wall-time, and wait-time sampling for 64-bit Windows applications.

**Early version: intended for evaluation and development, not production use.**

## Usage

Include [`dd-win-prof.h`](src/dd-win-prof/dd-win-prof.h), link `dd-win-prof.lib`,
and deploy `dd-win-prof.dll` alongside your application. The MSVC runtime must
also be available. Call `StartProfiler()` before the work to profile and
`StopProfiler()` before the application exits.

The profiler links libdatadog statically. `datadog_profiling_ffi.dll` is included
in the distribution but is a runtime dependency of the unit tests, not the
profiler DLL.

### CMake integration

```cmake
include(FetchContent)
FetchContent_Declare(
    dd-win-prof
    GIT_REPOSITORY https://github.com/DataDog/dd-win-prof.git
    GIT_TAG main
)
FetchContent_MakeAvailable(dd-win-prof)
target_link_libraries(your_application PRIVATE dd-win-prof)
dd_win_prof_copy_runtime_deps(your_application)
```

Pin `GIT_TAG` to the release or commit you intend to use. The copy helper places
runtime files beside the application; add `INCLUDE_PDBS` to copy debug symbols.

## Configuration

By default, configuration comes from environment variables. Without an API key,
profiles are sent through the Datadog Agent. Setting `DD_API_KEY` selects direct
upload to the site specified by `DD_SITE` (default: `datadoghq.com`).

| Variable | Purpose |
|----------|---------|
| `DD_SERVICE`, `DD_ENV`, `DD_VERSION` | Application metadata |
| `DD_API_KEY`, `DD_SITE` | Direct upload credentials and site |
| `DD_PROFILING_ENABLED=0` | Prevent profiling from starting |
| `DD_TRACE_LOG_DIRECTORY` | Log directory, set at DLL load time |
| `DD_INTERNAL_PROFILING_OUTPUT_DIR` | Write local pprof files |
| `DD_INTERNAL_PROFILING_EXPORT_ENABLED=0` | Disable backend upload |
| `DD_PROFILING_INTERNAL_SYMBOLIZE_CALLSTACKS=1` | Enable function-name symbolization |

Function names are obfuscated by default. Local pprof output and backend upload
are independent: setting an output directory does not disable upload.

To override settings in code, call `SetupProfiler()` before `StartProfiler()`:

```cpp
#include <cstdint>
#include "dd-win-prof.h"

ProfilerConfig config{};
config.size = sizeof(config);
config.serviceName = "my-windows-app";
config.serviceEnvironment = "production";
config.serviceVersion = "1.2.3";
if (!SetupProfiler(&config)) {
    return 1;
}
```

See the public header for all fields. `noEnvVars=true` resets profiler settings
to defaults before applying the struct and requires both `url` and `apiKey`.
Logging is still configured at DLL load time, independently of this struct.

## Build

Requires Windows x64, CMake 3.21+, and Visual Studio 2022 C++ tools (the IDE or
Build Tools). CMake fetches libdatadog, spdlog, and Google Test; see
[`src/CMakeLists.txt`](src/CMakeLists.txt) for pinned versions.

From the repository root:

```powershell
cmake -G "Visual Studio 17 2022" -A x64 -B build
cmake --build build --config Release
```

Top-level builds include the [obfuscation tools](obfuscation/readme.md), which
require the DIA SDK. To omit them, configure with
`-DDD_WIN_PROF_BUILD_OBFUSCATION=OFF`.

To generate and open the Visual Studio solution, use `scripts\generate-vs.ps1`.
For ASan builds, see the [unit-test instructions](src/Tests/README.md#addresssanitizer).

## Tests and tools

Build/run instructions live alongside each test or tool:

- [Unit tests and ASan](src/Tests/README.md)
- [Runner scenarios and CLI](src/Runner/README.md)
- [RUM integration tests](src/integration-tests/README.md)
- [Profile correctness and Runner symbol validation](e2e-tests/README.md)
- [Vulkan example builds](e2e-tests/README_vulkan.md)
- [Obfuscation tests](obfuscation/readme.md#build-and-test)
- [ProfilerInjector](src/ProfilerInjector/README.md)

See [ARCHITECTURE.md](ARCHITECTURE.md) for the profiling pipeline and
[CONTRIBUTING.md](CONTRIBUTING.md) for contribution guidelines.

## Packaging and releases

After a full Release build, stage the package outside the source tree:

```powershell
cmake --install build --config Release --prefix "$env:TEMP\dd-win-prof-stage"
```

`Profiler/` contains the public header, import library, DLLs, and PDBs;
`Obfuscation/` contains `ObfSymbols.exe`. License and notice files accompany
both. The layout is defined by the CMake `install()` rules.

The [release workflow](.github/workflows/release.yml) builds a draft release
from a `vX.Y.Z` tag. See the [release procedure](.claude/skills/dd-win-prof-release/SKILL.md)
for tagging, version bumps, and recovery.
