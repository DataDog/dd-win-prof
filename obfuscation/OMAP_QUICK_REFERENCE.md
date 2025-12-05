# OMAP Implementation Quick Reference

## What Changed?

ObfSymbols now explicitly uses OMAPTO and OMAPFROM debug streams to calculate correct addresses and sizes for function symbols in optimized binaries.

## Key Features

### ✓ Explicit OMAP Loading
- Loads OMAPTO and OMAPFROM tables directly from PDB debug streams
- Automatic detection of optimized vs non-optimized binaries

### ✓ Accurate Address Translation
- Translates addresses from original (pre-optimization) to optimized layout
- Uses binary search for efficient O(log N) lookups

### ✓ Correct Size Calculation
- Calculates function sizes by translating both start and end addresses
- Handles code elimination and rearrangement

### ✓ Eliminated Code Detection
- Detects and skips functions eliminated during optimization
- Reports count of eliminated symbols

## New Class Members

### PdbParser.h
```cpp
// OMAP structure
struct OMAPRVA {
    DWORD rva;      // Source RVA
    DWORD rvaTo;    // Target RVA (0 = eliminated)
};

// New member variables
std::vector<OMAPRVA> m_omapFrom;  // Original → Optimized
std::vector<OMAPRVA> m_omapTo;    // Optimized → Original
bool m_hasOMAP;                   // OMAP present flag

// New methods
bool LoadOMAPTables();
DWORD TranslateRVAFromOriginal(DWORD rvaOriginal) const;
DWORD TranslateRVAToOriginal(DWORD rvaOptimized) const;
ULONG CalculateSizeWithOMAP(DWORD rvaOriginal, ULONGLONG sizeOriginal) const;
```

## Diagnostic Output

### Non-Optimized Binary
```
No OMAP tables found - this is a non-optimized binary
Successfully extracted 82 function symbols
```

### Optimized Binary (with OMAP)
```
Loaded OMAPTO table with 450 entries
Loaded OMAPFROM table with 450 entries
OMAP tables loaded successfully - this is an optimized binary
Successfully extracted 68 function symbols (skipped 14 eliminated symbols)
```

## How It Works

### 1. Initialization
```
PdbParser constructor
  └─> InitializeDiaAndLoadPdb()
       └─> LoadOMAPTables()
            ├─> Read OMAPTO stream
            ├─> Read OMAPFROM stream
            └─> Set m_hasOMAP flag
```

### 2. Symbol Extraction
```
ExtractSymbols()
  └─> For each function:
       ├─> Get original RVA and size from DIA
       ├─> If m_hasOMAP:
       │    ├─> TranslateRVAFromOriginal(rva)
       │    ├─> CalculateSizeWithOMAP(rva, size)
       │    └─> Skip if code eliminated (rva == 0)
       └─> Store symbol with correct address and size
```

### 3. Address Translation Algorithm
```
TranslateRVAFromOriginal(rvaOriginal):
  1. Binary search m_omapFrom for entry where entry.rva <= rvaOriginal
  2. offset = rvaOriginal - entry.rva
  3. return entry.rvaTo + offset
  4. Special case: if entry.rvaTo == 0, code was eliminated
```

### 4. Size Translation Algorithm
```
CalculateSizeWithOMAP(rvaOriginal, sizeOriginal):
  1. rvaStart = TranslateRVAFromOriginal(rvaOriginal)
  2. rvaEnd = TranslateRVAFromOriginal(rvaOriginal + sizeOriginal)
  3. If either == 0: return 0 (eliminated)
  4. If rvaEnd > rvaStart: return rvaEnd - rvaStart
  5. Else: return sizeOriginal (non-contiguous code)
```

## Build & Test

### Build
```powershell
# Release build (with optimizations)
.\build.ps1 release -clean

# Debug build
.\build.ps1 debug -clean
```

### Test
```powershell
# Test with Release configuration
.\test.ps1 release

# Test with Debug configuration
.\test.ps1 debug
```

## Example Usage

```powershell
# Run on optimized PDB
.\ObfSymbols\x64\release\ObfSymbols.exe --pdb MyApp.pdb --out symbols.sym

# Output will automatically:
# - Detect OMAP presence
# - Load OMAP tables if present
# - Translate all addresses and sizes
# - Report eliminated symbols
```

## Verification

To verify OMAP handling is working:

1. **Check Output**: Look for "Loaded OMAP..." messages
2. **Check Symbol Count**: Compare with/without optimization
3. **Check Addresses**: Verify addresses match actual binary
4. **Check Sizes**: Verify sizes match disassembly

## Technical Details

### OMAP Entry Format
| Field | Type | Description |
|-------|------|-------------|
| `rva` | DWORD | Source address (RVA) |
| `rvaTo` | DWORD | Target address (RVA) or 0 if eliminated |

### Special Cases
| Case | Handling |
|------|----------|
| No OMAP tables | Use addresses as-is (non-optimized) |
| `rvaTo == 0` | Code eliminated, skip symbol |
| `rvaEnd < rvaStart` | Non-contiguous code, use original size |
| Empty OMAP | Treated as non-optimized |

### Performance
- **Table Loading**: One-time cost at initialization
- **Binary Search**: O(log N) per lookup
- **Memory**: ~8 bytes per OMAP entry
- **Typical OMAP size**: 100-1000 entries for medium projects

## Troubleshooting

### "No OMAP tables found" but binary is optimized
- Check if Link-Time Code Generation (LTCG) is enabled
- Verify /PROFILE or /DEBUG:FULL linker flags
- Some optimizations don't require OMAP

### Symbols have wrong addresses
- Ensure PDB matches the binary (check GUID)
- Verify PDB was generated with correct optimization settings
- Check if binary was modified post-linking

### Size is 0 for valid functions
- Function may have been eliminated during optimization
- Check if function is actually present in binary
- Verify function wasn't inlined everywhere

## Files Modified

| File | Changes |
|------|---------|
| `PdbParser.h` | Added OMAPRVA struct, member variables, and methods |
| `PdbParser.cpp` | Implemented OMAP loading and translation functions |
| `OMAP_IMPLEMENTATION.md` | Comprehensive documentation |
| `OMAP_QUICK_REFERENCE.md` | This quick reference |

## Summary

The explicit OMAP implementation ensures accurate symbol extraction from optimized binaries by:
- ✓ Loading OMAP tables directly from PDB
- ✓ Translating addresses using OMAPFROM
- ✓ Calculating sizes with start/end translation
- ✓ Detecting and handling eliminated code
- ✓ Providing diagnostic output for transparency

This replaces DIA's automatic OMAP handling with explicit, controlled translation for better accuracy and visibility.

