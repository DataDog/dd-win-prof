# Vulkan Quickstart Script - Re-run Behavior Guide

## Overview
The enhanced `vulcan_quickstart.ps1` script now handles re-runs intelligently to avoid unnecessary work while ensuring reliability.

## Re-run Behavior

### 🔄 **Normal Re-run** 
```powershell
.\vulcan_quickstart.ps1
```
**What it does:**
- ✅ Skips dependency setup if already completed (uses `.glm_setup_complete` marker)
- ✅ Skips CMake configuration if `CMakeCache.txt` exists
- ✅ Only rebuilds if source changes detected
- ⚡ **Fast** - typically completes in seconds

### 🧹 **Clean Start**
```powershell
.\vulcan_quickstart.ps1 -Clean
```
**What it does:**
- 🗑️ Removes build directory
- 🗑️ Removes dependency markers (`.glm_setup_complete`, etc.)
- ✨ Next run will perform full setup from scratch

### 🔧 **Force Dependency Re-setup**
```powershell
.\vulcan_quickstart.ps1 -ForceDependencies
```
**What it does:**
- 🔄 Re-checks and re-installs GLM
- 🔄 Re-initializes git submodules
- 🔄 Re-runs vcpkg package installation
- ⚠️ Use when dependency issues occur

### ⚙️ **Force CMake Reconfiguration**
```powershell
.\vulcan_quickstart.ps1 -ForceReconfigure
```
**What it does:**
- 🗑️ Removes CMakeCache.txt and CMakeFiles/
- 🔄 Re-runs CMake configuration
- ⚠️ Use when CMake configuration issues occur

### 🔄 **Complete Reset**
```powershell
.\vulcan_quickstart.ps1 -Clean
.\vulcan_quickstart.ps1 -ForceDependencies
```
**What it does:**
- 🧹 Complete clean slate
- 🔄 Full dependency and build setup
- 🐌 **Slow** - but most reliable

## When to Use Each Option

| Scenario | Command | Why |
|----------|---------|-----|
| **Normal development** | `.\vulcan_quickstart.ps1` | Fast, only rebuilds what's needed |
| **First time setup** | `.\vulcan_quickstart.ps1` | Automatically detects and sets up everything |
| **GLM errors appeared** | `.\vulcan_quickstart.ps1 -ForceDependencies` | Re-installs dependencies |
| **CMake errors** | `.\vulcan_quickstart.ps1 -ForceReconfigure` | Clears CMake cache |
| **Everything broken** | `-Clean` then `-ForceDependencies` | Nuclear option - start fresh |
| **CI/Automation** | `.\vulcan_quickstart.ps1 -CI` | Headless mode, full setup |

## State Files Created

The script creates these marker files to track setup state:

- `.glm_setup_complete` - GLM dependency setup completed
- `.vcpkg_setup_complete` - vcpkg setup completed (if applicable)
- `vulkan_examples/build/CMakeCache.txt` - CMake configuration completed

## Troubleshooting

### "GLM not found" on re-run
```powershell
.\vulcan_quickstart.ps1 -ForceDependencies
```

### "CMake configuration failed" on re-run
```powershell
.\vulcan_quickstart.ps1 -ForceReconfigure
```

### Build fails with old errors
```powershell
.\vulcan_quickstart.ps1 -Clean
.\vulcan_quickstart.ps1
```

### Complete environment issues
```powershell
.\vulcan_quickstart.ps1 -Clean
.\vulcan_quickstart.ps1 -ForceDependencies -ForceReconfigure
```

## CI Integration

For CI systems, use:
```powershell
.\vulcan_quickstart.ps1 -CI -Verbose
```

This provides:
- Full dependency setup on first run
- Fast re-runs on subsequent builds
- Detailed logging for troubleshooting
- Headless operation (no GUI windows)

## Performance

| Run Type | Typical Duration | What's Done |
|----------|------------------|-------------|
| **First run** | 5-15 minutes | Full setup + build |
| **Normal re-run** | 30 seconds - 2 minutes | Build only |
| **Force dependencies** | 2-5 minutes | Dependency setup + build |
| **Clean + rebuild** | 5-15 minutes | Complete reset + build |

The script is now optimized for development workflows while maintaining reliability for CI environments.
