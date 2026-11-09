// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

#include "pch.h"

/// <summary>
/// Symbolication result borrowing immutable strings owned by the engine
/// This version stores strings that can be reused across function creation
/// </summary>
struct CachedSymbolInfo {
  std::string_view FunctionName;  // Borrowed from Symbolication string pool
  std::string_view FileName;      // Borrowed from Symbolication string pool
  std::string_view ModuleName;    // Borrowed from Symbolication string pool
  std::string_view BuildId;       // Opaque PDB GUID + Age, borrowed from pool
  uint64_t ModuleBaseAddress;     // Module base address (for mapping)
  uint32_t ModuleSize;            // Module size in bytes (for mapping)
  uint64_t displacement;
  uint32_t lineNumber;
  bool isValid;

  CachedSymbolInfo()
      : FunctionName{},
        FileName{},
        ModuleName{},
        BuildId{},
        ModuleBaseAddress(0),
        ModuleSize(0),
        displacement(0),
        lineNumber(0),
        isValid(false) {}
};

/// <summary>
/// Cached module information to avoid repeated PE header parsing
/// </summary>
struct CachedModuleInfo {
  std::string_view ModuleName;  // Borrowed from Symbolication string pool
  std::string_view BuildId;     // Borrowed build ID from PE header
  uint64_t ModuleBaseAddress;   // Module base address
  uint32_t ModuleSize;          // Module size in bytes

  CachedModuleInfo() : ModuleName{}, BuildId{}, ModuleBaseAddress(0), ModuleSize(0) {}
};

class Symbolication {
 public:
  Symbolication();
  virtual ~Symbolication();

  // Initialize the symbolication engine
  bool Initialize(bool symbolizeFrames);

  // Cleanup resources
  void Cleanup();

  // Views in results are valid until ClearCaches/Cleanup/destruction; caller must not
  // retain them beyond the engine lifetime. Only distinct strings allocate. Returns
  // cached symbol info with strings that can be reused, or nullopt if symbolication
  // fails
  std::optional<CachedSymbolInfo> SymbolicateAndIntern(uint64_t address);

  // Check if symbolication is initialized
  bool IsInitialized() const { return _isInitialized; }

  // Refresh the module list to pick up dynamically loaded modules
  bool RefreshModules();

  // Invalidates borrowed cached strings; caller must first discard symbol results.
  void ClearCaches();

 private:
  bool _isInitialized;
  bool _symbolizeFrames;

  // Views remain valid until ClearCaches/Cleanup/destruction. Rehash never invalidates
  // them.
  struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const {
      return std::hash<std::string_view>{}(s);
    }
  };
  std::unordered_set<std::string, StringHash, std::equal_to<>> _strings;
  std::string_view Store(std::string_view value);
  std::string_view _emptyStringId;

  // Structural key: module base and size, not a hash treated as identity.
  std::map<std::pair<uint64_t, uint32_t>, CachedModuleInfo> _moduleCache;

  // Helper methods
  bool InitializeSymbolHandler();
  void CleanupSymbolHandler();
  // Module information extraction
  std::optional<CachedModuleInfo> GetOrCreateModuleInfo(
      uint64_t baseAddress, uint32_t moduleSize, const char* imageName
  );
  std::optional<std::string> ExtractBuildIdFromPEHeader(uint64_t baseAddress);
  bool ExtractBuildIdFromPEHeaderRaw(
      uint64_t baseAddress, char* buildIdBuffer, size_t bufferSize
  );
};
