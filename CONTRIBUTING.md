# Contributing to dd-win-prof

We welcome contributions to dd-win-prof! This guide will help you get started.

## Before You Start

⚠️ **Please note**: This is an early version project. Major architectural changes may occur.

## How to Contribute

### Reporting Bugs

If you find a bug:
1. Check if it's already reported in [Issues](../../issues)
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

- Visual Studio 2022 (or compatible) with C++ development tools
- Windows 10/11 SDK
- CMake 3.21 or later

### Building

```powershell
# Configure and build
cmake -G "Visual Studio 17 2022" -A x64 -B build
cmake --build build --config Release
```

To omit obfuscation tools from the build, pass `-DDD_WIN_PROF_BUILD_OBFUSCATION=OFF` when configuring.

#### Development mode and using dd-win-prof as a dependency

`DD_WIN_PROF_DEVELOPMENT` is the umbrella option for working on dd-win-prof itself. It defaults to `ON` when dd-win-prof is the top-level project and `OFF` when it is included in another project via `FetchContent` or `add_subdirectory`, and it sets the default for:

| Option | Builds / enables |
|---|---|
| `DD_WIN_PROF_BUILD_TESTS` | Unit tests (also fetches GoogleTest) |
| `DD_WIN_PROF_BUILD_RUNNER` | The Runner test application |
| `DD_WIN_PROF_BUILD_INJECTOR` | The ProfilerInjector utility |
| `DD_WIN_PROF_BUILD_OBFUSCATION` | The obfuscation tools (requires the DIA SDK) |

Outputs are also staged into `src/reference` in development mode, or whenever the Runner is built (it consumes that directory). `DD_WIN_PROF_BUILD_INSTALL` (default: top-level only) controls all install rules: the release-package layout and the ObfSymbols install. Set any of these explicitly to override the umbrella.

A project that only needs the profiler library can therefore just do `FetchContent_MakeAvailable(dd-win-prof)`: it won't download GoogleTest, define test/tool targets, write into dd-win-prof's source tree, or add install rules. Consumers that need the runtime DLLs next to their own binaries should use `dd_win_prof_copy_runtime_deps()` rather than reading `src/reference`, which is no longer populated for them. Opting a consumer into `DD_WIN_PROF_BUILD_TESTS=ON` also brings in GoogleTest and CTest's cache entries.

Only `src/dd-win-prof/include/` (which holds the public `dd-win-prof.h`) is on the include path of projects that link the `dd-win-prof` target; keep anything else out of that directory. The helper targets are namespaced (`dd_win_prof_spdlog`, `dd_win_prof_libdatadog_static`, `dd_win_prof_libdatadog_dynamic`) so they can't collide with a consumer's own. The CI job "Consume as dependency" (`scripts/test-consumer.ps1`) builds a small project against dd-win-prof and fails if any of this regresses; run it locally with `.\scripts\test-consumer.ps1`.

To open the project in Visual Studio, use the helper script:

```powershell
.\scripts\generate-vs.ps1
```

### Testing

```powershell
cmake --build build --config Debug
build\src\Tests\Debug\Tests.exe
```

## Code Guidelines

- Follow existing code style and patterns
- Add tests for new functionality
- Update documentation as needed
- Ensure builds pass on Windows

## Review Process

All pull requests require review from code owners (@chrisnas @r1viollet) before merging.

## Questions?

Feel free to open an issue for questions or reach out to the maintainers.
