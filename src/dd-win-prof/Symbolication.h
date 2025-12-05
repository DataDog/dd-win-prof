// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "pch.h"
#include "datadog/profiling.h"
#include <optional>
#include <unordered_map>
#include <cstdint>

// Boost-style hash_combine for combining hash values
inline void hash_combine(uint64_t& seed, uint64_t value)
{
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

/// <summary>
/// Symbolication result using libdatadog string IDs
/// This version stores string IDs that can be reused across function creation
/// </summary>
struct CachedSymbolInfo
{
    uint64_t Address;                    // Original address (used as cache key)
    ddog_prof_ManagedStringId FunctionNameId;   // Interned function name ID
    ddog_prof_ManagedStringId FileNameId;       // Interned filename ID
    ddog_prof_ManagedStringId ModuleNameId;     // Interned module name ID
    ddog_prof_ManagedStringId BuildIdId;        // Interned build ID (PDB GUID + Age)
    uint64_t ModuleBaseAddress;          // Module base address (for mapping)
    uint32_t ModuleSize;                 // Module size in bytes (for mapping)
    uint64_t displacement;
    uint32_t lineNumber;
    bool isValid;

    CachedSymbolInfo() : Address(0), FunctionNameId{0}, FileNameId{0}, ModuleNameId{0}, BuildIdId{0},
                         ModuleBaseAddress(0), ModuleSize(0), displacement(0), lineNumber(0), isValid(false) {}
};

/// <summary>
/// Cached module information to avoid repeated PE header parsing
/// </summary>
struct CachedModuleInfo
{
    ddog_prof_ManagedStringId ModuleNameId;     // Interned module name ID
    ddog_prof_ManagedStringId BuildIdId;        // Interned build ID from PE header
    uint64_t ModuleBaseAddress;                 // Module base address
    uint32_t ModuleSize;                        // Module size in bytes

    CachedModuleInfo() : ModuleNameId{0}, BuildIdId{0}, ModuleBaseAddress(0), ModuleSize(0) {}
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

    // Module cache - key is hash of (BaseOfImage, ImageSize)
    std::unordered_map<uint64_t, CachedModuleInfo> _moduleCache;

    // Helper methods
    bool InitializeSymbolHandler();
    void CleanupSymbolHandler();
    CachedSymbolInfo CreateUnknownSymbol(uint64_t address, ddog_prof_ManagedStringStorage& stringStorage);

    // Module information extraction
    std::optional<CachedModuleInfo> GetOrCreateModuleInfo(uint64_t baseAddress, uint32_t moduleSize,
                                                           const char* imageName,
                                                           ddog_prof_ManagedStringStorage& stringStorage);
    std::optional<std::string> ExtractBuildIdFromPEHeader(uint64_t baseAddress);
    bool ExtractBuildIdFromPEHeaderRaw(uint64_t baseAddress, char* buildIdBuffer, size_t bufferSize);
    uint64_t ComputeModuleCacheKey(uint64_t baseAddress, uint32_t moduleSize) const;
};
