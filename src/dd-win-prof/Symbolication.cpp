// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "Symbolication.h"

#include <DbgHelp.h>

#include "Log.h"
#include "pch.h"

#pragma comment(lib, "dbghelp.lib")

// Symbolication implementation
Symbolication::Symbolication() : _isInitialized(false), _symbolizeFrames(false) {
  _emptyStringId = {};
}

Symbolication::~Symbolication() { Cleanup(); }

bool Symbolication::Initialize(bool symbolizeFrames) {
  if (_isInitialized) return true;

  _symbolizeFrames = symbolizeFrames;

  if (!InitializeSymbolHandler()) return false;

  _isInitialized = true;
  return true;
}

void Symbolication::Cleanup() {
  if (_isInitialized) {
    CleanupSymbolHandler();
    _isInitialized = false;
    ClearCaches();
  }
}

std::optional<CachedSymbolInfo> Symbolication::SymbolicateAndIntern(uint64_t address) {
  // Handle uninitialized state - return nullopt (major failure)
  if (!_isInitialized) {
    return std::nullopt;
  }

  // Buffer for symbol information
  const size_t maxNameLength = 256;
  char symbolBuffer[sizeof(SYMBOL_INFO) + maxNameLength] = {0};
  PSYMBOL_INFO pSymbol = reinterpret_cast<PSYMBOL_INFO>(symbolBuffer);
  pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  pSymbol->MaxNameLen = maxNameLength;

  CachedSymbolInfo result;

  // Initialize string IDs to zero (invalid)
  result.FunctionName = std::string_view{};
  result.FileName = std::string_view{};
  result.ModuleName = std::string_view{};
  result.BuildId = std::string_view{};
  result.ModuleBaseAddress = 0;
  result.ModuleSize = 0;

  // Get module information first - this works even if symbol lookup fails
  IMAGEHLP_MODULE64 moduleInfo = {0};
  moduleInfo.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);

  if (SymGetModuleInfo64(GetCurrentProcess(), address, &moduleInfo)) {
    // Get or create cached module information
    auto moduleInfoOpt = GetOrCreateModuleInfo(
        moduleInfo.BaseOfImage, moduleInfo.ImageSize, moduleInfo.ImageName
    );

    if (moduleInfoOpt.has_value()) {
      const CachedModuleInfo& cachedModule = moduleInfoOpt.value();
      result.ModuleName = cachedModule.ModuleName;
      result.BuildId = cachedModule.BuildId;
      result.ModuleBaseAddress = cachedModule.ModuleBaseAddress;
      result.ModuleSize = cachedModule.ModuleSize;

      if (result.ModuleBaseAddress != 0 && result.ModuleSize != 0) {
        if (address < result.ModuleBaseAddress ||
            (address - result.ModuleBaseAddress) >= result.ModuleSize) {
          LogOnce(
              Debug,
              "Address 0x",
              std::hex,
              address,
              " outside module range [0x",
              result.ModuleBaseAddress,
              ", 0x",
              (result.ModuleBaseAddress + result.ModuleSize),
              ")",
              std::dec
          );
        }
      }
    }
  }

  // get the symbol corresponding to the address if needed
  if (!_symbolizeFrames) {
    // Symbolization disabled - return module info only
    result.FunctionName = _emptyStringId;

    result.isValid = true;
    return result;
  }

  DWORD64 displacement64 = 0;
  if (SymFromAddr(GetCurrentProcess(), address, &displacement64, pSymbol)) {
    result.FunctionName = Store(pSymbol->Name);
    result.displacement = static_cast<uint64_t>(displacement64);

    // Try to get line information
    IMAGEHLP_LINE64 line = {0};
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
    DWORD displacement32 = 0;

    if (SymGetLineFromAddr64(GetCurrentProcess(), address, &displacement32, &line)) {
      result.FileName = Store(line.FileName);
      result.lineNumber = line.LineNumber;
      // Note: If file name interning fails, we still return the function symbol
    }
  } else {
    // SymFromAddr failed - address not found, return unknown symbol
    // Note: module info was already populated above if available
    result.FunctionName = _emptyStringId;
  }

  result.isValid = true;
  return result;
}

bool Symbolication::RefreshModules() {
  if (!_isInitialized) return false;

  // This will refresh the module list by re-enumerating all loaded modules
  return SymRefreshModuleList(GetCurrentProcess()) != FALSE;
}

bool Symbolication::InitializeSymbolHandler() {
  // Set symbol options for better debugging
  // todo: does order matter between init and set options ?
  DWORD options = SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS;
#ifdef _DEBUG
  // Enable debug output only in debug builds
  options |= SYMOPT_DEBUG;
#endif
  SymSetOptions(options);

  // Initialize symbol handler - TRUE means auto-enumerate all loaded modules
  if (!SymInitialize(GetCurrentProcess(), nullptr, TRUE)) {
    return false;
  }

  return true;
}

void Symbolication::CleanupSymbolHandler() { SymCleanup(GetCurrentProcess()); }

void Symbolication::ClearCaches() {
  _moduleCache.clear();
  // Release bucket and node high-water capacity after exceptional symbol churn.
  decltype(_strings)().swap(_strings);
}

std::string_view Symbolication::Store(std::string_view value) {
  if (value.empty()) return {};
  auto found = _strings.find(value);
  if (found == _strings.end()) found = _strings.emplace(value).first;
  return *found;
}

bool Symbolication::ExtractBuildIdFromPEHeaderRaw(
    uint64_t baseAddress, char* buildIdBuffer, size_t bufferSize
) {
  __try {
    // Read DOS header
    IMAGE_DOS_HEADER* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(baseAddress);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
      return false;
    }

    // Read NT headers
    IMAGE_NT_HEADERS* ntHeaders =
        reinterpret_cast<IMAGE_NT_HEADERS*>(baseAddress + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
      return false;
    }

    // Get debug directory
    IMAGE_DATA_DIRECTORY* debugDir =
        &ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (debugDir->Size == 0 || debugDir->VirtualAddress == 0) {
      return false;
    }

    // Calculate number of debug entries
    DWORD numEntries = debugDir->Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    IMAGE_DEBUG_DIRECTORY* debugEntries = reinterpret_cast<IMAGE_DEBUG_DIRECTORY*>(
        baseAddress + debugDir->VirtualAddress
    );

    // Search for IMAGE_DEBUG_TYPE_CODEVIEW entry
    for (DWORD i = 0; i < numEntries; i++) {
      if (debugEntries[i].Type == IMAGE_DEBUG_TYPE_CODEVIEW) {
        // Found CodeView debug info
        if (debugEntries[i].SizeOfData < sizeof(DWORD)) {
          continue;
        }

        // Get pointer to CodeView data
        void* cvData =
            reinterpret_cast<void*>(baseAddress + debugEntries[i].AddressOfRawData);
        DWORD signature = *reinterpret_cast<DWORD*>(cvData);

        // Check if it's PDB 7.0 format (RSDS signature)
        const DWORD CV_SIGNATURE_RSDS = 0x53445352;  // 'SDSR' (RSDS in little-endian)
        if (signature == CV_SIGNATURE_RSDS) {
          // Structure of CV_INFO_PDB70
          struct CV_INFO_PDB70 {
            DWORD Signature;      // 'RSDS'
            GUID Guid;            // GUID
            DWORD Age;            // Age
            char PdbFileName[1];  // PDB file name (variable length)
          };

          CV_INFO_PDB70* cvInfo = reinterpret_cast<CV_INFO_PDB70*>(cvData);

          // Format GUID and Age as contiguous hex string (no dashes), consistent with
          // PDB parsing tooling. Example: 1234567812341234ABCDEF12345678901
          snprintf(
              buildIdBuffer,
              bufferSize,
              "%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%X",
              cvInfo->Guid.Data1,
              cvInfo->Guid.Data2,
              cvInfo->Guid.Data3,
              cvInfo->Guid.Data4[0],
              cvInfo->Guid.Data4[1],
              cvInfo->Guid.Data4[2],
              cvInfo->Guid.Data4[3],
              cvInfo->Guid.Data4[4],
              cvInfo->Guid.Data4[5],
              cvInfo->Guid.Data4[6],
              cvInfo->Guid.Data4[7],
              cvInfo->Age
          );

          return true;
        }
      }
    }

    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Failed to read memory - module might be unloaded or invalid
    return false;
  }
}

std::optional<std::string> Symbolication::ExtractBuildIdFromPEHeader(
    uint64_t baseAddress
) {
  char buildIdBuffer[64];
  if (ExtractBuildIdFromPEHeaderRaw(
          baseAddress, buildIdBuffer, sizeof(buildIdBuffer)
      )) {
    return std::string(buildIdBuffer);
  }
  return std::nullopt;
}

std::optional<CachedModuleInfo> Symbolication::GetOrCreateModuleInfo(
    uint64_t baseAddress, uint32_t moduleSize, const char* imageName
) {
  // Compute cache key
  auto cacheKey = std::make_pair(baseAddress, moduleSize);

  // Check if already cached
  auto it = _moduleCache.find(cacheKey);
  if (it != _moduleCache.end()) {
    return it->second;
  }

  // Create new module info
  CachedModuleInfo moduleInfo;
  moduleInfo.ModuleBaseAddress = baseAddress;
  moduleInfo.ModuleSize = moduleSize;

  // Extract module name (just the filename without path from ImageName)
  if (imageName != nullptr && imageName[0] != '\0') {
    // Find the last occurrence of '\' or '/' to extract just the filename
    const char* fileName = imageName;
    const char* lastSlash = nullptr;

    for (const char* p = imageName; *p != '\0'; ++p) {
      if (*p == '\\' || *p == '/') {
        lastSlash = p;
      }
    }

    // If we found a slash, the filename starts after it
    if (lastSlash != nullptr) {
      fileName = lastSlash + 1;
    }

    // Only intern if we have a non-empty filename
    if (fileName[0] != '\0') {
      moduleInfo.ModuleName = Store(fileName);
    }
  }

  // Extract build ID from PE header
  auto buildIdOpt = ExtractBuildIdFromPEHeader(baseAddress);
  if (buildIdOpt.has_value()) {
    moduleInfo.BuildId = Store(buildIdOpt.value());
  }

  // Cache the result
  _moduleCache[cacheKey] = moduleInfo;

  return moduleInfo;
}
