# Contributing to dd-win-prof

We welcome contributions to dd-win-prof! This guide will help you get started.
For integration and runtime configuration, see [README.md](README.md).

## Before You Start

⚠️ **Please note**: This is an early version project. Major architectural changes may occur.

## How to Contribute

### Reporting Bugs

If you find a bug:
1. Check if it's already reported in [Issues](https://github.com/DataDog/dd-win-prof/issues)
2. If not, create a new issue with:
   - Clear description of the problem
   - Steps to reproduce
   - Expected vs actual behavior
   - Windows version and build environment

### Suggesting Features

For new features:
1. Open an issue with `[Feature Request]` in the title
2. Describe the feature and why it would be useful
3. Wait for maintainer feedback before starting work

### Code Contributions

1. **Fork the repository**
2. **Create a feature branch** from `main`
3. **Make your changes**
4. **Test your changes** (build and run tests)
5. **Submit a pull request**

## Development Setup

### Prerequisites

- **Windows 10/11** or **Windows Server 2019/2022**
- **Visual Studio 2022** or later with C++ development tools (or **Build Tools for Visual Studio**)
- **Windows 10/11 SDK**
- **Visual Studio DIA SDK**: required by ObfSymbols, which is enabled in the default top-level build. Install it with Visual Studio, or disable obfuscation tools as shown below.
- **CMake 3.21** or later

### Dependencies

All dependencies are downloaded automatically by CMake at configure time via `FetchContent`:

- **libdatadog v19.0.0**: profile serialization and upload
- **spdlog v1.14.1**: logging
- **Google Test v1.14.0**: unit testing

### Building

```powershell
# Configure and build (Release)
cmake -G "Visual Studio 17 2022" -A x64 -B build
cmake --build build --config Release
```

If the DIA SDK is unavailable, omit obfuscation tools when configuring:

```powershell
cmake -G "Visual Studio 17 2022" -A x64 -B build `
  -DDD_WIN_PROF_BUILD_OBFUSCATION=OFF
```

### Build with Visual Studio

Run the helper script to generate and open the solution:

```powershell
.\scripts\generate-vs.ps1
```

This generates a Visual Studio solution in `build\` and opens it automatically. You can then build and debug directly from the IDE. Use `Get-Help .\scripts\generate-vs.ps1` for options (custom build directory, VS version, etc.).

The helper uses the generator's default architecture; it does not pass `-A x64`.
For an explicitly x64 solution, use the direct CMake command above in a fresh build
directory.

#### Building with AddressSanitizer

To get an ASan-instrumented solution, pass `-Asan` and use a separate build
directory so it does not clobber your regular `build\`:

```powershell
.\scripts\generate-vs.ps1 -BuildDir build-asan -Asan
```

This configures CMake with `-DDD_WIN_PROF_ENABLE_ASAN=ON` and opens the
resulting solution. Every configuration in that solution (Debug, Release, ...)
will be ASan-instrumented; pick Debug from the VS dropdown for typical use.

Requirements:

- The **C++ AddressSanitizer** component must be installed via the Visual
  Studio Installer (`Modify` → *Individual components* → search "AddressSanitizer").
- No Developer Command Prompt needed: the build locates the ASan runtime DLL
  (`clang_rt.asan_dynamic-x86_64.dll`) automatically via `vswhere` and copies
  it next to `Tests.exe` and `Runner.exe`.

To switch back to a non-ASan build, just open the original `build\` solution
(or generate one without `-Asan`).

### Development mode and using dd-win-prof as a dependency

`DD_WIN_PROF_DEVELOPMENT` is the umbrella option for working on dd-win-prof itself. It defaults to `ON` when dd-win-prof is the top-level project and `OFF` when it is included in another project via `FetchContent` or `add_subdirectory`, and it sets the default for:

| Option | Builds / enables |
|---|---|
| `DD_WIN_PROF_BUILD_TESTS` | Unit tests (also fetches GoogleTest) |
| `DD_WIN_PROF_BUILD_RUNNER` | The Runner test application |
| `DD_WIN_PROF_BUILD_INJECTOR` | The ProfilerInjector utility |
| `DD_WIN_PROF_BUILD_OBFUSCATION` | The obfuscation tools (requires the DIA SDK) |

Outputs are also staged into `src/reference` in development mode, or whenever the Runner is built (it consumes that directory). `DD_WIN_PROF_BUILD_INSTALL` (default: top-level only) controls all install rules: the release-package layout and the ObfSymbols install. Set any of these explicitly to override the umbrella.

A project that only needs the profiler library can therefore just do `FetchContent_MakeAvailable(dd-win-prof)`: it won't download GoogleTest, define test/tool targets, write into dd-win-prof's source tree, or add install rules. Consumers that need the runtime DLLs next to their own binaries should use `dd_win_prof_copy_runtime_deps()` rather than reading `src/reference`, which is no longer populated for them. Opting a consumer into `DD_WIN_PROF_BUILD_TESTS=ON` also brings in GoogleTest and CTest's cache entries.

Only `src/dd-win-prof/include/` (which holds the public `dd-win-prof.h`) is on the include path of projects that link the `dd-win-prof` target; keep anything else out of that directory. The helper targets are namespaced (`dd_win_prof_spdlog`, `dd_win_prof_libdatadog_static`, `dd_win_prof_libdatadog_dynamic`) so they can't collide with a consumer's own. The CI job "Consume as dependency" (`scripts/test-consumer.ps1`) builds a small project against dd-win-prof, exports and installs the target as the README describes, builds a second project against that installed export, and fails if any of this regresses; run it locally with `.\scripts\test-consumer.ps1`.

### Project structure and build outputs

Abbreviated source layout:

```
dd-win-prof/
├── CMakeLists.txt                  # Root CMake configuration
├── src/
│   ├── CMakeLists.txt              # Dependencies (libdatadog, spdlog, googletest via FetchContent)
│   ├── dd-win-prof/                # Main profiler DLL
│   ├── Runner/                     # Example app for testing the profiler
│   ├── Tests/                      # Unit tests (Google Test)
│   ├── integration-tests/          # Runner integration tests
│   ├── ProfilerInjector/           # Utility to inject profiler into a target process
│   └── reference/                  # Staged public header and runtime artifacts
├── obfuscation/                    # Symbol obfuscation tools (optional, requires DIA SDK)
│   ├── ObfSymbols/                 # PDB symbol extractor/obfuscator
│   ├── TestSymbols/                # Test executable for ObfSymbols
│   └── TestSymbolsDll/             # Test DLL for ObfSymbols
├── e2e-tests/                      # Runner, symbol, consumer, and optional Vulkan tests
│   ├── consumer/                  # CMake consumer/export checks
│   ├── runner-scenarios/          # Profile correctness expectations
│   ├── capture-pprof.ps1          # Capture profiles from Runner
│   └── validate-runner-sym.ps1    # Validate Runner's generated symbols
└── scripts/
    ├── generate-vs.ps1             # Generate and open VS solution from CMake
    └── test-consumer.ps1           # Run CMake consumer/export checks
```

After building, the key artifacts are:

| Artifact | Location |
|----------|----------|
| `dd-win-prof.dll` / `.lib` / `.pdb` | `build\src\dd-win-prof\<Config>\` |
| `datadog_profiling_ffi.dll` (unit-test dependency) | `build\_deps\libdatadog-src\release\dynamic\` |
| `Runner.exe` | `build\src\Runner\<Config>\` |
| `Tests.exe` | `build\src\Tests\<Config>\` |
| `ProfilerInjector.exe` | `build\src\ProfilerInjector\<Config>\` |
| `dd-win-prof.h` (staged public header) | `src\reference\` (copied before compilation) |
| **All runtime DLLs together** | `src\reference\` (copied by POST_BUILD) |

The libdatadog examples show a Release build with the default `FETCHCONTENT_BASE_DIR`.
CI sets it to `.deps`, so the DLL is instead at
`.deps\libdatadog-src\release\dynamic\datadog_profiling_ffi.dll`.

The `src\reference\` directory stages the public header and runtime artifacts. After a successful build it contains `dd-win-prof.h`, `dd-win-prof.dll`, `dd-win-prof.lib`, `dd-win-prof.pdb`, `datadog_profiling_ffi.dll`, and `datadog_profiling_ffi.pdb`. It is only populated when dd-win-prof is built as the top-level project (or with `DD_WIN_PROF_DEVELOPMENT=ON` / `DD_WIN_PROF_BUILD_RUNNER=ON`). Projects that consume dd-win-prof via `FetchContent` or `add_subdirectory` should call `dd_win_prof_copy_runtime_deps(<target>)` instead.

## Testing

Instructions live beside the tests:

- [Unit tests](src/Tests/README.md)
- [Runner build and CLI examples](src/Runner/README.md)
- [RUM integration tests](src/integration-tests/README.md)
- [Runner profile/symbol checks, consumer checks, and Vulkan tests](e2e-tests/README.md)

[`.github/workflows/test.yml`](.github/workflows/test.yml) is the primary CI
workflow and calls [`.github/workflows/e2e.yml`](.github/workflows/e2e.yml) for Vulkan tests.

## Releases

### Packaging a release

Reproduce the release zip layout locally with `cmake --install`:

```powershell
cmake --install build --config Release --prefix .\stage\dd-win-prof
```

Layout is owned by `install()` rules in the relevant `CMakeLists.txt` files.

### Cutting a release

[`.github/workflows/release.yml`](.github/workflows/release.yml) runs on `vX.Y.Z` tag push and produces a draft GitHub Release with `dd-win-prof.zip`. `main` always sits at the version about to be released, so the tag points at `main` as-is.

1. Sign and push the tag: `git tag -s vX.Y.Z -m "vX.Y.Z" && git push origin vX.Y.Z`. (`-s` signs the annotated tag; lightweight unsigned tags are discouraged.) The workflow asserts `version.h` matches the tag and fails fast otherwise.
2. Open a follow-up PR bumping `src/dd-win-prof/version.h` to the next dev version, so `main` is ready for the next release cycle.
3. Edit the auto-generated notes on the draft release and publish.

## Code Guidelines

- Follow existing code style and patterns
- Add tests for new functionality
- Update documentation as needed
- Ensure builds pass on Windows

## Review Process

All pull requests require review from code owners (@chrisnas @r1viollet) before merging.

## Questions?

Feel free to open an issue for questions or reach out to the maintainers.
