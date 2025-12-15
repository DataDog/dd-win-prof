# Symbols Solution Build Instructions

This solution contains the ObfSymbols project which extracts and obfuscate symbols from PDB files.

## Solution Structure

```
Symbols/
├── Symbols.slnx              # Visual Studio solution file
├── build.ps1                 # PowerShell build script
├── build.bat                 # Batch build script
├── test.ps1                  # PowerShell test script
├── BUILD_INSTRUCTIONS.md     # This file
├── ObfSymbols/              # Main project folder
│   ├── ObfSymbols.cpp       # Source code
│   ├── ObfSymbols.vcxproj   # Project file
│   └── x64/                 # Build outputs
│       ├── Debug/
│       └── Release/
└── TestSymbols/             # Test project folder
    ├── TestSymbols.cpp      # Test source with various symbols
    ├── TestSymbols.vcxproj  # Test project file
    └── x64/                 # Test outputs
        ├── Debug/
        └── Release/
```

## Build Scripts

### 1. PowerShell Script: `build.ps1` (Recommended)

**Basic Usage:**
```powershell
.\build.ps1
```

**Advanced Usage:**
```powershell
# Build Debug x64 (default)
.\build.ps1

# Build Release x64
.\build.ps1 Release

# Build Debug x86
.\build.ps1 Debug x86

# Build Release x86
.\build.ps1 Release x86

# Clean rebuild (forces recompilation)
.\build.ps1 Debug x64 -Clean
.\build.ps1 Release -Clean
```

### 2. Batch Script: `build.bat`

**Basic Usage:**
```cmd
build.bat
```

**Advanced Usage:**
```cmd
REM Build Debug x64 (default)
build.bat

REM Build Release x64
build.bat Release

REM Build Debug x86
build.bat Debug x86

REM Build Release x86
build.bat Release x86

REM Clean rebuild
build.bat Debug x64 clean
build.bat Release x64 clean
```

## Quick Reference

| Command | Configuration | Platform | Action |
|---------|--------------|----------|--------|
| `.\build.ps1` | Debug | x64 | Incremental build |
| `.\build.ps1 Release` | Release | x64 | Incremental build |
| `.\build.ps1 -Clean` | Debug | x64 | Full rebuild |
| `.\build.ps1 Release -Clean` | Release | x64 | Full rebuild |

## Output Locations

After building, the executable will be located at:
- **Debug x64**: `ObfSymbols\x64\Debug\ObfSymbols.exe`
- **Release x64**: `ObfSymbols\x64\Release\ObfSymbols.exe`
- **Debug x86**: `ObfSymbols\Debug\ObfSymbols.exe`
- **Release x86**: `ObfSymbols\Release\ObfSymbols.exe`

## Running the Application

```cmd
.\ObfSymbols\x64\Debug\ObfSymbols.exe --pdb <path_to_pdb_file> --out <output_file> [--obf <obfuscated_output_file>]
```

**Example:**
```cmd
# Generate both symbol file and obfuscated file (auto-generated name)
.\ObfSymbols\x64\Debug\ObfSymbols.exe --pdb MyApp.pdb --out symbols.sym

# Specify custom obfuscated file name
.\ObfSymbols\x64\Debug\ObfSymbols.exe --pdb MyApp.pdb --out symbols.sym --obf obfuscated.sym
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
# Test Debug x64 (default)
.\test.ps1

# Test Release x64
.\test.ps1 Release

# Test Debug x86
.\test.ps1 Debug x86
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

You can also open the solution in Visual Studio 2022:
1. Open `Symbols.slnx` in Visual Studio 2022
2. Select the desired configuration (Debug/Release) and platform (x64/x86)
3. Build → Build Solution (or press Ctrl+Shift+B)

## Requirements

- **Visual Studio 2022** (Professional, Community, or Enterprise)
- **Platform Toolset**: v143 (Visual Studio 2022)
- **C++ Standard**: C++20
- **Windows SDK**: 10.0 or higher
- **DIA SDK**: Included with Visual Studio 2022 (used for all PDB operations)
  - Headers: `$(VSInstallDir)DIA SDK\include`
  - Libraries: `$(VSInstallDir)DIA SDK\lib\amd64`
  - Runtime: `msdia140.dll` (automatically loaded from Visual Studio installation)