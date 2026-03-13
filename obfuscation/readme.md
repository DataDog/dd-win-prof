# Obfuscation Tools Build Instructions

This directory contains the ObfSymbols project which extracts and obfuscates symbols from PDB files.

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

## Output Locations

After building, the executable will be located at:
- **Debug**: `build\obfuscation\ObfSymbols\Debug\ObfSymbols.exe`
- **Release**: `build\obfuscation\ObfSymbols\Release\ObfSymbols.exe`

## Running the Application

```cmd
build\obfuscation\ObfSymbols\Debug\ObfSymbols.exe --pdb <path_to_pdb_file> --out <output_file> [--obf <obfuscated_output_file>]
```

**Example:**
```cmd
# Generate both symbol file and obfuscated file (auto-generated name)
build\obfuscation\ObfSymbols\Debug\ObfSymbols.exe --pdb MyApp.pdb --out symbols.sym

# Specify custom obfuscated file name
build\obfuscation\ObfSymbols\Debug\ObfSymbols.exe --pdb MyApp.pdb --out symbols.sym --obf obfuscated.sym
```

**Output Files:**

1. **Symbol file** (specified by `--out`): Contains visibility, addresses, sizes, obfuscated names, and function signatures
   ```
   PUBLIC/PRIVATE 0xADDRESS SIZE obf_XXXXXXXX FunctionSignature
   ```

   Example:
   ```
   PRIVATE 0x1000 11 obf_7120BD79 Circle::~Circle()
   PRIVATE 0x1010 14 obf_2BDA4789 Shape::Shape()
   PRIVATE 0x1050 452 obf_8C39C417 Shape::operator=(Shape&)
   PRIVATE 0x1490 4 obf_F10B2883 Add(int, int)
   PRIVATE 0x14A0 5 obf_9343A148 Add(double, double)
   PRIVATE 0x1580 8 obf_5835D4DE Max<int>(int, int)
   ```

   - `PUBLIC` = Exported/external symbols (visible outside the module)
   - `PRIVATE` = Static/internal symbols (only visible within the module)
   - `SIZE` = Size of the symbol in bytes
   - `obf_XXXXXXXX` = Obfuscated name (8-character hash)
   - `FunctionSignature` = Function name with parameter types (no return type)

   **Signature Extraction:**
   - Function signatures are extracted using the DIA (Debug Interface Access) SDK
   - Signatures include function names, parameter types, and qualifiers (const)
   - Return types are omitted

   **Known Limitation:**  The PUBLIC/PRIVATE column provides best-effort visibility information where available.

2. **Obfuscated symbol file** (auto-generated or specified by `--obf`): Contains only obfuscated names without revealing real function information
   ```
   PUBLIC/PRIVATE 0xADDRESS SIZE obf_XXXXXXXX
   ```

   Example:
   ```
   PRIVATE 0x1000 11 obf_7120BD79
   PRIVATE 0x1010 14 obf_2BDA4789
   PRIVATE 0x1050 452 obf_8C39C417
   PRIVATE 0x1490 4 obf_F10B2883
   PRIVATE 0x14A0 5 obf_9343A148
   PRIVATE 0x1580 8 obf_5835D4DE
   ```

   **Purpose:** This file is for true obfuscation - no function names or signatures are exposed.
   The regular `.sym` file serves as a mapping reference between obfuscated names and real signatures.

If `--obf` is not specified, the obfuscated file will be auto-generated with `_obf` suffix before the extension.

## Testing

### Automated Testing with `test.ps1`

A comprehensive test script is provided to validate ObfSymbols functionality:

**Basic Usage:**
```powershell
.\test.ps1
```

**Advanced Usage:**
```powershell
# Test Debug (default)
.\test.ps1

# Test Release
.\test.ps1 Release

# Test with custom build directory
.\test.ps1 Release -BuildDir ..\out
```

**What it tests:**
- ✓ Builds TestSymbols project with various symbol types
- ✓ Runs ObfSymbols on the generated PDB
- ✓ Validates both output files are created
- ✓ Verifies correct file formats (addresses, obfuscated names, signatures)
- ✓ Checks that .sym file includes function signatures with parameters
- ✓ Confirms _obf.sym file contains only obfuscated names (no signatures)
- ✓ Validates address sorting and uniqueness
- ✓ Checks line count consistency between files
- ✓ Analyzes content for expected symbol types

**Sample Output:**
```
=== Test Results Summary ===
Tests Passed: 25
Tests Failed: 0
Pass Rate: 100%

ALL TESTS PASSED! ✓
```

## Using Visual Studio

From the repository root, generate and open the VS solution:
```powershell
.\scripts\generate-vs.ps1
```

Then build from the IDE (Build → Build Solution or Ctrl+Shift+B). Make sure to enable `DD_WIN_PROF_BUILD_OBFUSCATION` in the CMake configure step if you need obfuscation tools.

## Requirements

- **Visual Studio 2022** (Professional, Community, or Enterprise)
- **CMake 3.21** or later
- **C++ Standard**: C++20
- **Windows SDK**: 10.0 or higher
- **DIA SDK**: Included with Visual Studio 2022 (used for all PDB operations)