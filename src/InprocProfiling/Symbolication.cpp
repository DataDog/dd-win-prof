#include "pch.h"
#include "Symbolication.h"
#include <DbgHelp.h>

#pragma comment(lib, "dbghelp.lib")

// Symbolication implementation
Symbolication::Symbolication()
    : _isInitialized(false)
{
}

Symbolication::~Symbolication()
{
    Cleanup();
}

bool Symbolication::Initialize()
{
    if (_isInitialized)
        return true;

    if (!InitializeSymbolHandler())
        return false;

    _isInitialized = true;
    return true;
}

void Symbolication::Cleanup()
{
    if (_isInitialized)
    {
        CleanupSymbolHandler();
        _isInitialized = false;
    }
}

std::optional<CachedSymbolInfo> Symbolication::SymbolicateAndIntern(uint64_t address, ddog_prof_ManagedStringStorage& stringStorage)
{
    // Handle uninitialized state - return nullopt (major failure)
    if (!_isInitialized)
    {
        return std::nullopt;
    }

    // Buffer for symbol information
    const size_t maxNameLength = 256;
    char symbolBuffer[sizeof(SYMBOL_INFO) + maxNameLength] = { 0 };
    PSYMBOL_INFO pSymbol = reinterpret_cast<PSYMBOL_INFO>(symbolBuffer);
    pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSymbol->MaxNameLen = maxNameLength;

    CachedSymbolInfo result;
    result.Address = address;

    // Initialize string IDs to zero (invalid)
    result.FunctionNameId = ddog_prof_ManagedStringId{0};
    result.FileNameId = ddog_prof_ManagedStringId{0};

    DWORD64 displacement64 = 0;
    if (SymFromAddr(GetCurrentProcess(), address, &displacement64, pSymbol))
    {
        // Intern function name
        ddog_CharSlice functionNameSlice = { pSymbol->Name, strlen(pSymbol->Name) };
        auto functionNameResult = ddog_prof_ManagedStringStorage_intern(stringStorage, functionNameSlice);
        if (functionNameResult.tag != DDOG_PROF_MANAGED_STRING_STORAGE_INTERN_RESULT_OK)
        {
            // String interning failed - major failure, return nullopt
            return std::nullopt;
        }

        result.FunctionNameId = functionNameResult.ok;
        result.displacement = static_cast<uint64_t>(displacement64);
        result.isValid = true;

        // Try to get line information
        IMAGEHLP_LINE64 line = { 0 };
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD displacement32 = 0;

        if (SymGetLineFromAddr64(GetCurrentProcess(), address, &displacement32, &line))
        {
            // Intern file name
            ddog_CharSlice fileNameSlice = { line.FileName, strlen(line.FileName) };
            auto fileNameResult = ddog_prof_ManagedStringStorage_intern(stringStorage, fileNameSlice);
            if (fileNameResult.tag == DDOG_PROF_MANAGED_STRING_STORAGE_INTERN_RESULT_OK)
            {
                result.FileNameId = fileNameResult.ok;
                result.lineNumber = line.LineNumber;
            }
            // Note: If file name interning fails, we still return the function symbol
        }
        // if file is not found, add an empty string to the filename

        return result;
    }
    else
    {
        // Todo: add a module name

        // SymFromAddr failed - address not found, return unknown symbol
        auto unknownSymbol = CreateUnknownSymbol(address, stringStorage);
        if (unknownSymbol.FunctionNameId.value == 0)
        {
            // Even unknown symbol creation failed (string interning failed) - major failure
            return std::nullopt;
        }
        return unknownSymbol;
    }
}

bool Symbolication::RefreshModules()
{
    if (!_isInitialized)
        return false;

    // This will refresh the module list by re-enumerating all loaded modules
    return SymRefreshModuleList(GetCurrentProcess()) != FALSE;
}

bool Symbolication::InitializeSymbolHandler()
{

    // Set symbol options for better debugging
    // todo: does order matter between init and set options ?
    DWORD options = SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS;
#ifdef _DEBUG
    // Enable debug output only in debug builds
    options |= SYMOPT_DEBUG;
#endif
    SymSetOptions(options);

    // Initialize symbol handler - TRUE means auto-enumerate all loaded modules
    if (!SymInitialize(GetCurrentProcess(), nullptr, TRUE))
    {
        return false;
    }

    return true;
}

void Symbolication::CleanupSymbolHandler()
{
    SymCleanup(GetCurrentProcess());
}

CachedSymbolInfo Symbolication::CreateUnknownSymbol(uint64_t address, ddog_prof_ManagedStringStorage& stringStorage)
{
    CachedSymbolInfo result;
    result.Address = address;
    result.isValid = true; // Always valid, even if unknown

    // Initialize IDs to zero (invalid) in case interning fails
    result.FunctionNameId = ddog_prof_ManagedStringId{0};
    result.FileNameId = ddog_prof_ManagedStringId{0};

    // Intern unknown function name
    const char* unknownFunction = "<unknown>";
    ddog_CharSlice unknownFunctionSlice = { unknownFunction, strlen(unknownFunction) };
    auto functionNameResult = ddog_prof_ManagedStringStorage_intern(stringStorage, unknownFunctionSlice);
    if (functionNameResult.tag == DDOG_PROF_MANAGED_STRING_STORAGE_INTERN_RESULT_OK)
    {
        result.FunctionNameId = functionNameResult.ok;
    }

    // Intern unknown filename - ProfileExporter expects this to be set
    const char* unknownFile = "<unknown>";
    ddog_CharSlice unknownFileSlice = { unknownFile, strlen(unknownFile) };
    auto fileNameResult = ddog_prof_ManagedStringStorage_intern(stringStorage, unknownFileSlice);
    if (fileNameResult.tag == DDOG_PROF_MANAGED_STRING_STORAGE_INTERN_RESULT_OK)
    {
        result.FileNameId = fileNameResult.ok;
    }

    // Note: If even the unknown string interning fails, we still return a valid symbol
    // but with zero IDs which ProfileExporter should handle gracefully
    return result;
}
