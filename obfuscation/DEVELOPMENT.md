# ObfSymbols Development

## Structure

```
obfuscation/
├── CMakeLists.txt            # CMake build configuration
├── build.ps1                 # PowerShell build script
├── build.bat                 # Batch build script
├── test.ps1                  # PowerShell test script
├── ObfSymbols/               # Main project
│   ├── ObfSymbols.cpp
│   └── CMakeLists.txt
├── TestSymbols/              # Test executable
│   ├── TestSymbols.cpp
│   └── CMakeLists.txt
└── TestSymbolsDll/           # Test DLL (used by ObfSymbols tests)
    ├── TestSymbolsDll.cpp
    └── CMakeLists.txt
```

## Requirements

- **Visual Studio 2022** (Professional, Community, or Enterprise)
- **CMake 3.21** or later
- **C++ Standard**: C++20
- **Windows SDK**: 10.0 or higher
- **DIA SDK**: Included with Visual Studio 2022 (used for all PDB operations)

## Building

### With build scripts

```powershell
# Build Debug (default)
.\build.ps1

# Build Release
.\build.ps1 Release

# Clean rebuild
.\build.ps1 Release -Clean
```

Or with the batch script:
```cmd
build.bat Release
build.bat Release clean
```

### With CMake directly

From the repository root:
```powershell
cmake -G "Visual Studio 17 2022" -A x64 -B build -DDD_WIN_PROF_BUILD_OBFUSCATION=ON
cmake --build build --config Release --target ObfSymbols
```

### Output Locations

After building, the executable will be at:
- **Debug**: `build\obfuscation\ObfSymbols\Debug\ObfSymbols.exe`
- **Release**: `build\obfuscation\ObfSymbols\Release\ObfSymbols.exe`

### Using Visual Studio

From the repository root, generate and open the VS solution:
```powershell
.\scripts\generate-vs.ps1
```

Then build from the IDE (Build → Build Solution or Ctrl+Shift+B). Make sure to enable `DD_WIN_PROF_BUILD_OBFUSCATION` in the CMake configure step.

## Testing

### Automated Testing with `test.ps1`

```powershell
# Test Debug (default)
.\test.ps1

# Test Release
.\test.ps1 Release

# Test with custom build directory
.\test.ps1 Release -BuildDir ..\out
```

**What it tests:**
- Builds TestSymbolsDll with various symbol types
- Runs ObfSymbols on the generated PDB
- Validates both output files are created
- Verifies correct file formats (addresses, obfuscated names, signatures)
- Checks that `.sym` file includes function signatures
- Confirms `_obf.sym` file contains only obfuscated names
- Validates address sorting and uniqueness

**Sample Output:**
```
=== Test Results Summary ===
Tests Passed: 25
Tests Failed: 0
Pass Rate: 100%

ALL TESTS PASSED! ✓
```
