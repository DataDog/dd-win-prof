// PdbParser.h - PDB file parser using DIA SDK
// Handles COM initialization and maintains DIA session for efficient access

#pragma once

#include <string>
#include <vector>
#include <Windows.h>
#include <atlbase.h>
#include <dia2.h>

// OMAP entry structure (from MSDN)
struct OMAPRVA
{
    DWORD rva;          // source RVA
    DWORD rvaTo;        // target RVA
};

// Structure to hold symbol information
struct SymbolInfo
{
    DWORD rva;          // relative virtual address
    ULONG size;         // size of the symbol in bytes
    std::wstring name;  // function name
    std::wstring signature;  // function signature with parameters
    bool isPublic;      // true for public/exported symbols, false for private/static
    WORD conflictCount; // number of OTHER symbols that also mapped to same RVA (0 = no conflict, >0 = conflict)
};

// Structure to hold module information
struct ModuleInfo
{
    std::wstring os;            // Operating system (win, linux, mac)
    std::wstring architecture;  // Architecture (x86, x64, arm64)
    std::wstring buildId;       // Build identifier (GUID)
    std::wstring moduleName;    // Module name (DLL/EXE name)
};

// PDB Parser class - maintains DIA session for efficient parsing
class PdbParser
{
public:
    // Constructor - initializes COM and loads PDB file
    explicit PdbParser(const std::wstring& pdbFilePath);

    // Destructor - cleans up COM resources
    ~PdbParser();

    // Check if parser is ready (PDB loaded successfully)
    bool IsValid() const { return m_isValid; }

    // Extract module information (OS, architecture, build ID, module name)
    bool ExtractModuleInfo(ModuleInfo& moduleInfo);

    // Extract all function symbols from the PDB
    bool ExtractSymbols(std::vector<SymbolInfo>& symbols);

    // Get the PDB file path
    const std::wstring& GetPdbPath() const { return m_pdbFilePath; }

private:
    // Helper: Initialize DIA SDK and load PDB
    bool InitializeDiaAndLoadPdb();

    // Helper: Extract embedded msdia140.dll if needed
    bool ExtractEmbeddedDll(std::wstring& dllPath);

    // Helper: Get executable directory
    std::wstring GetExecutableDirectory();

    // Helper: Get architecture string from machine type
    std::wstring GetArchitectureString(DWORD machineType);

    // Helper: Get type name from DIA type symbol
    std::wstring GetDiaTypeName(IDiaSymbol* pType);

    // Helper: Get complete type string (with pointers, references, etc.)
    std::wstring GetDiaTypeString(IDiaSymbol* pType);

    // Helper: Get function signature (without return type)
    std::wstring GetDiaFunctionSignature(IDiaSymbol* pSymbol, const std::wstring& functionName);

    // Helper: Load OMAP tables from PDB
    bool LoadOMAPTables();

    // Helper: Translate RVA using OMAPFROM (original -> optimized)
    DWORD TranslateRVAFromOriginal(DWORD rvaOriginal) const;

    // Helper: Translate RVA using OMAPTO (optimized -> original)
    DWORD TranslateRVAToOriginal(DWORD rvaOptimized) const;

    // Helper: Calculate size with OMAP translation
    ULONG CalculateSizeWithOMAP(DWORD rvaOriginal, ULONGLONG sizeOriginal) const;

private:
    std::wstring m_pdbFilePath;
    bool m_isValid;
    bool m_comInitialized;

    // DIA SDK objects (maintained for reuse)
    CComPtr<IDiaDataSource> m_pSource;
    CComPtr<IDiaSession> m_pSession;
    CComPtr<IDiaSymbol> m_pGlobal;

    // OMAP tables for address translation
    std::vector<OMAPRVA> m_omapFrom;  // Original RVA -> Optimized RVA
    std::vector<OMAPRVA> m_omapTo;    // Optimized RVA -> Original RVA
    bool m_hasOMAP;                   // Whether OMAP tables are present
};

