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
