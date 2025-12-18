// PdbParser.h - PDB file parser using DIA SDK

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

class PdbParser
{
public:
    explicit PdbParser(const std::wstring& pdbFilePath);
    ~PdbParser();

    bool IsValid() const { return _isValid; }
    bool ExtractModuleInfo(ModuleInfo& moduleInfo);
    bool ExtractSymbols(std::vector<SymbolInfo>& symbols);
    bool DumpAllSymbols();
    const std::wstring& GetPdbPath() const { return _pdbFilePath; }

private:
    bool InitializeDiaAndLoadPdb();
    bool ExtractEmbeddedDll(std::wstring& dllPath);
    std::wstring GetExecutableDirectory();
    std::wstring GetArchitectureString(DWORD machineType);
    std::wstring GetDiaTypeName(IDiaSymbol* pType);
    std::wstring GetDiaTypeString(IDiaSymbol* pType);
    std::wstring GetDiaFunctionSignature(IDiaSymbol* pSymbol, const std::wstring& functionName);
    bool LoadOMAPTables();
    DWORD TranslateRVAFromOriginal(DWORD rvaOriginal) const;
    DWORD TranslateRVAToOriginal(DWORD rvaOptimized) const;
    ULONG CalculateSizeWithOMAP(DWORD rvaOriginal, ULONGLONG sizeOriginal) const;

private:
    std::wstring _pdbFilePath;
    bool _isValid;
    bool _comInitialized;

    CComPtr<IDiaDataSource> _pSource;
    CComPtr<IDiaSession> _pSession;
    CComPtr<IDiaSymbol> _pGlobal;

    // OMAP tables for address translation
    std::vector<OMAPRVA> _omapFrom;  // Original RVA -> Optimized RVA
    std::vector<OMAPRVA> _omapTo;    // Optimized RVA -> Original RVA
    bool _hasOMAP;                   // Whether OMAP tables are present
};

