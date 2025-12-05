# OMAP Implementation for ObfSymbols

## Overview

This document describes the explicit OMAP (Address Mapping) implementation added to ObfSymbols to correctly handle optimized binaries and their PDB files.

## What is OMAP?

OMAP (Object Module Address Mapping) is a mechanism used by the Windows linker to track how addresses change during link-time optimization. When link-time code generation (LTCG) or whole program optimization (WPO) is enabled, the linker may:

- Reorder functions
- Merge duplicate code
- Eliminate dead code
- Split or combine functions

OMAP tables store the mapping between:
- **Original addresses** (pre-optimization)
- **Optimized addresses** (post-optimization)

## OMAP Tables

There are two OMAP debug streams in optimized PDB files:

### 1. OMAPFROM Table
- Maps from **original RVA** → **optimized RVA**
- Used to translate addresses from the unoptimized binary to the optimized binary
- Format: Array of `OMAPRVA` entries sorted by `rva`

### 2. OMAPTO Table
- Maps from **optimized RVA** → **original RVA**
- Used to translate addresses from the optimized binary back to the original
- Format: Array of `OMAPRVA` entries sorted by `rva`

### OMAPRVA Structure
```cpp
struct OMAPRVA
{
    DWORD rva;      // Source RVA
    DWORD rvaTo;    // Target RVA (0 means code was eliminated)
};
```

## Implementation Details

### 1. Loading OMAP Tables

The `LoadOMAPTables()` function is called during PDB initialization:

```cpp
bool PdbParser::LoadOMAPTables()
{
    // 1. Get debug streams enumerator from DIA session
    // 2. Iterate through all debug streams
    // 3. Find and load OMAPTO and OMAPFROM streams
    // 4. Store entries in m_omapTo and m_omapFrom vectors
}
```

**Output Messages:**
- `"Loaded OMAPTO table with N entries"` - OMAPTO loaded successfully
- `"Loaded OMAPFROM table with N entries"` - OMAPFROM loaded successfully
- `"OMAP tables loaded successfully - this is an optimized binary"` - Both tables present
- `"No OMAP tables found - this is a non-optimized binary"` - No optimization applied

### 2. Address Translation

#### TranslateRVAFromOriginal (Original → Optimized)
```cpp
DWORD PdbParser::TranslateRVAFromOriginal(DWORD rvaOriginal) const
{
    // 1. Binary search in OMAPFROM table for entry with rva <= rvaOriginal
    // 2. Calculate offset: offset = rvaOriginal - entry.rva
    // 3. Apply offset to target: rvaOptimized = entry.rvaTo + offset
    // 4. Return 0 if code was eliminated (entry.rvaTo == 0)
}
```

#### TranslateRVAToOriginal (Optimized → Original)
```cpp
DWORD PdbParser::TranslateRVAToOriginal(DWORD rvaOptimized) const
{
    // Similar to above but uses OMAPTO table
}
```

**Special Cases:**
- If `rvaTo == 0`: Code was eliminated during optimization
- If no mapping found: Returns original address unchanged
- If OMAP not present: Returns address unchanged

### 3. Size Calculation with OMAP

The `CalculateSizeWithOMAP()` function handles function size calculation:

```cpp
ULONG PdbParser::CalculateSizeWithOMAP(DWORD rvaOriginal, ULONGLONG sizeOriginal) const
{
    // 1. Translate start address: rvaOptimizedStart = TranslateRVAFromOriginal(rvaOriginal)
    // 2. Translate end address: rvaOptimizedEnd = TranslateRVAFromOriginal(rvaOriginal + size)
    // 3. Calculate new size: sizeOptimized = rvaOptimizedEnd - rvaOptimizedStart
    // 4. Handle special cases:
    //    - If either address is 0: Code was eliminated, return 0
    //    - If end < start: Code was rearranged, return original size as best effort
}
```

**Why Size Matters:**
- Optimizations can change function sizes
- Code may be split across non-contiguous regions
- Some code may be eliminated entirely

### 4. Symbol Extraction with OMAP

The `ExtractSymbols()` function applies OMAP translation to all symbols:

```cpp
bool PdbParser::ExtractSymbols(std::vector<SymbolInfo>& symbols)
{
    // For each function symbol:
    // 1. Get original RVA and size from DIA
    // 2. If OMAP present:
    //    a. Translate RVA using TranslateRVAFromOriginal()
    //    b. Calculate size using CalculateSizeWithOMAP()
    //    c. Skip symbols with zero RVA or size (eliminated code)
    // 3. If OMAP not present:
    //    - Use addresses and sizes as-is
    // 4. Store translated symbols
}
```

**Optimization Awareness:**
- Tracks eliminated symbols: `"(skipped N eliminated symbols)"`
- Deduplicates symbols that map to the same optimized address
- Maintains correct address ordering after translation

## Usage

The implementation is automatic and transparent:

1. **Load PDB**: OMAP tables are automatically loaded if present
2. **Extract Symbols**: All addresses and sizes are automatically translated
3. **Output**: Symbols reflect actual optimized binary addresses

### Example Output

#### Non-Optimized Binary
```
No OMAP tables found - this is a non-optimized binary
Successfully extracted 82 function symbols
```

#### Optimized Binary
```
Loaded OMAPTO table with 450 entries
Loaded OMAPFROM table with 450 entries
OMAP tables loaded successfully - this is an optimized binary
Successfully extracted 68 function symbols (skipped 14 eliminated symbols)
```

## Benefits

### Before (DIA Automatic OMAP)
- Relied on DIA's internal OMAP handling
- Limited control over address translation
- No visibility into eliminated code
- Potential inaccuracies in size calculation

### After (Explicit OMAP)
- Full control over address translation using OMAPFROM/OMAPTO tables
- Accurate size calculation by translating both start and end addresses
- Explicit handling of eliminated code
- Better diagnostics and visibility
- Consistent results across different optimization levels

## Technical Considerations

### Binary Search Algorithm
- OMAP tables can be large (thousands of entries)
- Binary search provides O(log N) lookup time
- Tables are already sorted by RVA (guaranteed by linker)

### Edge Cases Handled
1. **No OMAP tables**: Non-optimized builds use addresses as-is
2. **Eliminated code**: Functions with `rvaTo == 0` are skipped
3. **Non-contiguous code**: Best-effort size calculation
4. **Duplicate mappings**: Multiple symbols at same address are deduplicated
5. **Empty OMAP**: Treated as non-optimized binary

### Memory Efficiency
- OMAP tables loaded once during initialization
- Stored in `std::vector` for efficient binary search
- Released automatically with PdbParser destructor

## Testing

The implementation has been tested with:

- ✓ Non-optimized builds (Debug configuration)
- ✓ Optimized builds (Release configuration)
- ✓ Builds with whole program optimization
- ✓ Large PDB files with thousands of symbols
- ✓ Both x86 and x64 architectures

## Code Location

### Header File: `PdbParser.h`
- `struct OMAPRVA` - OMAP entry structure
- `std::vector<OMAPRVA> m_omapFrom` - OMAPFROM table
- `std::vector<OMAPRVA> m_omapTo` - OMAPTO table
- `bool m_hasOMAP` - OMAP presence flag
- Function declarations for OMAP handling

### Implementation: `PdbParser.cpp`
- `LoadOMAPTables()` - Load OMAP streams from PDB
- `TranslateRVAFromOriginal()` - Original → Optimized translation
- `TranslateRVAToOriginal()` - Optimized → Original translation
- `CalculateSizeWithOMAP()` - Size calculation with OMAP
- `ExtractSymbols()` - Updated to use explicit OMAP translation

## References

- [Microsoft DIA SDK Documentation](https://docs.microsoft.com/en-us/visualstudio/debugger/debug-interface-access/debug-interface-access-sdk)
- [OMAP Data Structures](https://docs.microsoft.com/en-us/previous-versions/visualstudio/visual-studio-2010/z97w0355(v=vs.100))
- [Link-Time Code Generation](https://docs.microsoft.com/en-us/cpp/build/reference/ltcg-link-time-code-generation)

## Future Enhancements

Possible improvements:

1. **Caching**: Cache translation results for frequently accessed addresses
2. **Statistics**: Track and report optimization statistics (% code eliminated, avg size change)
3. **Visualization**: Generate OMAP mapping reports for analysis
4. **Validation**: Verify OMAP table integrity and detect corrupted tables

## Conclusion

The explicit OMAP implementation provides accurate address and size translation for optimized binaries, ensuring that symbol information correctly reflects the actual binary layout after link-time optimization. This is essential for accurate symbol extraction, debugging, and analysis of optimized Windows executables.

