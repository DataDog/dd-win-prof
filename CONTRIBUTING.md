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
- Visual Studio 2022 (or compatible)
- Windows 10/11 SDK
- PowerShell (for dependency scripts)

### Building
```bash
# Download dependencies
.\scripts\download-libdatadog.ps1
.\scripts\download-spdlog.ps1

# Build with Visual Studio or MSBuild
msbuild src/InprocProfiling/InprocProfiling.vcxproj /p:Configuration=Release
```

### Testing
```bash
# Build and run tests
msbuild src/Tests/Tests.vcxproj /p:Configuration=Debug
src/Tests/x64/Debug/Tests.exe
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
