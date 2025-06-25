#pragma once

#include "pch.h"
#include "datadog/profiling.h"
#include <optional>

/// <summary>
/// Symbolication result using libdatadog string IDs
/// This version stores string IDs that can be reused across function creation
/// </summary>
struct CachedSymbolInfo
{
    uint64_t Address;                    // Original address (used as cache key)
    ddog_prof_ManagedStringId FunctionNameId;   // Interned function name ID
    ddog_prof_ManagedStringId FileNameId;       // Interned filename ID
    uint64_t displacement;
    uint32_t lineNumber;
    bool isValid;

    CachedSymbolInfo() : Address(0), FunctionNameId{0}, FileNameId{0}, displacement(0), lineNumber(0), isValid(false) {}
};

class Symbolication
{
public:
    Symbolication();
    virtual ~Symbolication();

    // Initialize the symbolication engine
    bool Initialize();
    
    // Cleanup resources
    void Cleanup();

    // Symbolicate a single address and intern strings into libdatadog's managed storage
    // Returns cached symbol info with string IDs that can be reused, or nullopt if symbolication fails
    std::optional<CachedSymbolInfo> SymbolicateAndIntern(uint64_t address, ddog_prof_ManagedStringStorage& stringStorage);

    // Check if symbolication is initialized
    bool IsInitialized() const { return _isInitialized; }

    // Refresh the module list to pick up dynamically loaded modules
    bool RefreshModules();

private:
    bool _isInitialized;

    // Helper methods
    bool InitializeSymbolHandler();
    void CleanupSymbolHandler();
    CachedSymbolInfo CreateUnknownSymbol(uint64_t address, ddog_prof_ManagedStringStorage& stringStorage);
}; 
