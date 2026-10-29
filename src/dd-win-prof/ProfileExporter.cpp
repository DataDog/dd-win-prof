// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "ProfileExporter.h"

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>

#include "LibDatadogHelper.h"
#include "Log.h"
#include "OsSpecificApi.h"
#include "Uuid.h"
#include "pch.h"
#include "version.h"

using namespace dd_win_prof;
using namespace OsSpecificApi;

namespace {
int64_t ToUnixTimeMs(std::chrono::system_clock::time_point time) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch())
      .count();
}
}  // namespace

ProfileExporter::ProfileExporter(
    Configuration* pConfiguration,
    std::span<const SampleValueType> sampleTypeDefinitions,
    IRumRecordProvider* pRumRecordProvider,
    ProfilingDataCallback callback,
    ProfileEncodingApi encodingApi
)
    : _pConfiguration(pConfiguration),
      _sampleTypeDefinitions{
          sampleTypeDefinitions.begin(), sampleTypeDefinitions.end()
      },
      _initialized(false),
      _processId{0},
      _currentExportId(0),
      _debugPprofFileWritingEnabled(false),
      _debugPprofPrefix(""),
      _exportEnabled(false),
      _profilingDataCallback(std::move(callback)),
      _exporter(nullptr),
      _agentMode(true),
      _encodingApi(encodingApi),
      _pRumRecordProvider(pRumRecordProvider) {
  _runtimeId = ComputeRuntimeId();

  _kProfilerVersion = PROFILER_VERSION_STRING;
  _kProfilerUserAgent = DLL_NAME;

  // Configure debug output from Configuration
  if (_pConfiguration != nullptr) {
    auto outputDir = _pConfiguration->GetProfilesOutputDirectory();
    if (!outputDir.empty()) {
      // ensure the output directory exists
      const auto log_path = fs::path(outputDir);
      if (!fs::exists(log_path)) {
        fs::create_directories(log_path);
      }

      _debugPprofFileWritingEnabled = true;
      _debugPprofPrefix = outputDir.string();
      // Ensure the prefix ends with a path separator
      if (!_debugPprofPrefix.empty() && _debugPprofPrefix.back() != '/' &&
          _debugPprofPrefix.back() != '\\') {
        _debugPprofPrefix += "/";
      }
      _debugPprofPrefix += "profile_";
    }

    const auto mode = _profilingDataCallback ? ProfileDeliveryMode::Callback
                                             : ProfileDeliveryMode::Http;
    _exportEnabled = _pConfiguration->IsExportEnabled(mode);
    if (_exportEnabled) {
      _apiKey = _pConfiguration->GetApiKey();
      _agentMode = !_pConfiguration->IsAgentless();
    }
  }
}

ProfileExporter::~ProfileExporter() {
  // Just call cleanup - by this point, explicit cleanup should have been called
  // If not, we'll do basic cleanup but may skip exporter cleanup to avoid deadlocks
  if (_initialized || _stringStorage.inner != nullptr || _exporter.inner != nullptr) {
    Cleanup(true);  // Skip exporter cleanup in destructor to avoid deadlocks
  }
}

void ProfileExporter::Cleanup(bool skipExporterCleanup) {
  if (!_initialized && _stringStorage.inner == nullptr && _exporter.inner == nullptr) {
    return;
  }

  Log::Debug("Starting exporter cleanup, skipExporterCleanup=", skipExporterCleanup);

  // Clean up exporter first if not skipping
  if (!skipExporterCleanup) {
    CleanupExporter();
  } else {
    Log::Debug("Skipping exporter cleanup to avoid potential deadlocks");
    // Just clear the pointer without calling the Rust destructor
    if (_exporter.inner) {
      _exporter.inner = nullptr;
    }
  }

  // Clean up libdatadog's managed string storage
  if (_stringStorage.inner != nullptr) {
    ddog_prof_ManagedStringStorage_drop(_stringStorage);
    _stringStorage = {};
  }

  _profilingDataCallback = {};
  _initialized = false;
  Log::Debug("Exporter cleanup completed");
}

bool ProfileExporter::Initialize() {
  try {
    _processId = GetCurrentProcessId();

    // Initialize libdatadog's managed string storage
    auto storageResult = ddog_prof_ManagedStringStorage_new();
    if (storageResult.tag != DDOG_PROF_MANAGED_STRING_STORAGE_NEW_RESULT_OK) {
      Log::Error("Failed to create libdatadog ManagedStringStorage");
      return false;
    }
    _stringStorage = storageResult.ok;

    // Initialize symbolication engine
    _symbolication = std::make_unique<Symbolication>();
    if (!_symbolication->Initialize(
            _stringStorage, _pConfiguration->AreCallstacksSymbolized()
        )) {
      Log::Error("Failed to initialize symbolication engine");
      return false;
    }

    // Initialize PprofAggregator directly with SampleValueType (no enum conversion
    // needed)
    _aggregator = std::make_unique<dd_win_prof::PprofAggregator>(
        _sampleTypeDefinitions, _stringStorage, 10, _encodingApi
    );

    if (!_aggregator->IsInitialized()) {
      return false;
    }

    // Intern sample labels that will be reused across samples
    if (!InternSampleLabels(_sampleLabels)) {
      return false;
    }

    // Set the profile start time
    _profileStartTime = std::chrono::system_clock::now();

    // Log debug output configuration
    if (_debugPprofFileWritingEnabled) {
      Log::Info("Debug pprof file writing enabled, prefix: ", _debugPprofPrefix);
    }

    // Initialize exporter if export is enabled
    if (_exportEnabled) {
      if (!InitializeExporter()) {
        return false;
      }
      Log::Info(
          "Profiles export enabled, mode: ",
          (_agentMode ? "agent" : "agentless"),
          ", URL: ",
          _exportUrl
      );
    } else {
      Log::Info("Profiles export disabled");
    }

    _initialized = true;
    return true;
  } catch (const std::exception& ex) {
    Log::Error("Exception during initialization: ", ex.what());
    return false;
  }
}

bool ProfileExporter::Add(std::shared_ptr<Sample> const& sample) {
  if (!_initialized) {
    LogOnce(Error, "Trying to add sample but exporter is not initialized");
    return false;
  }

  // Convert callstack addresses to LocationIds
  std::vector<ddog_prof_LocationId> locationIds;
  std::span<const uint64_t> callstack = sample->GetFrames();
  locationIds.reserve(callstack.size());

  for (size_t i = 0; i < callstack.size(); ++i) {
    uint64_t address = callstack[i];
    auto locationIdOpt = InternLocation(address);
    if (!locationIdOpt.has_value()) {
      LogOnce(
          Error,
          "Failed to intern location for address 0x",
          std::hex,
          address,
          std::dec,
          " when adding sample"
      );
      return false;  // Return false to indicate failure
    }
    locationIds.push_back(locationIdOpt.value());
  }

  // Get sample values directly
  std::span<const int64_t> sampleValues = sample->GetValues();

  // Get timestamp - convert nanoseconds to int64_t
  int64_t timestampNs = sample->GetTimestamp().count();

  // Get thread info for labeling
  std::shared_ptr<ThreadInfo> threadInfo = sample->GetThreadInfo();

  // Get RUM view context for labeling
  const auto& rumView = sample->GetRumViewContext();

  // Create labelset for this sample (includes thread name and RUM labels if available)
  ddog_prof_LabelSetId labelsetId = CreateLabelSet(_sampleLabels, threadInfo, rumView);

  // Add sample to aggregator with labels
  return _aggregator->AddSample(locationIds, sampleValues, timestampNs, labelsetId);
}

bool ProfileExporter::Export(bool lastCall) {
  if (!_initialized) {
    Log::Error("ProfileExporter::Export() called but not initialized");
    return false;
  }

  OnExportStart();
  const auto end = std::chrono::system_clock::now();
  const bool exported = ExportCurrentProfile(
      lastCall, ToUnixTimeMs(_profileStartTime), ToUnixTimeMs(end)
  );
  // Reset even after failed delivery so the next export can recover.
  const bool prepared = FinishExport(end, lastCall);
  return exported && prepared;
}

void ProfileExporter::EncodedProfileDeleter::operator()(
    ddog_prof_EncodedProfile* profile
) const {
  if (profile == nullptr) {
    return;
  }
  if (profile->inner != nullptr) {
    ddog_prof_EncodedProfile_drop(profile);
  }
  delete profile;
}

bool ProfileExporter::ExportCurrentProfile(
    bool lastCall, int64_t startUnixTimeMs, int64_t endUnixTimeMs
) {
  if (_profilingDataCallback) {
    DiscardRumRecords();
  }
  EncodedProfilePtr profile(_aggregator->Serialize(startUnixTimeMs, endUnixTimeMs));
  if (!profile) {
    return ReportError(ProfilingErrorCode::SerializationFailed, lastCall);
  }

  auto rumRecordsJson =
      _profilingDataCallback ? std::string{} : CollectRumRecordsJson();
  WriteDebugFiles(profile.get(), rumRecordsJson, endUnixTimeMs);

  // Request_build consumes the profile, so read its bytes before HTTP export.
  auto bytes = _encodingApi.bytes(profile.get());
  if (bytes.tag != DDOG_PROF_RESULT_BYTE_SLICE_OK_BYTE_SLICE) {
    std::string message = "libdatadog failed to read encoded profile bytes: ";
    message.append(
        reinterpret_cast<const char*>(bytes.err.message.ptr), bytes.err.message.len
    );
    ddog_Error_drop(&bytes.err);
    Log::Error(message);
    return ReportError(ProfilingErrorCode::ProfileBytesUnavailable, lastCall);
  }

  LogExport(lastCall, endUnixTimeMs - startUnixTimeMs, bytes.ok.len);
  if (_profilingDataCallback) {
    return DeliverProfileToCallback(bytes.ok, startUnixTimeMs, endUnixTimeMs, lastCall);
  }
  if (_exportEnabled) {
    return ExportProfile(profile.get(), _currentExportId, rumRecordsJson);
  }
  return true;
}

std::string ProfileExporter::CollectRumRecordsJson() {
  if (_pRumRecordProvider == nullptr) {
    return {};
  }
  _pRumRecordProvider->ConsumeViewRecords(_viewRecordsBuffer);
  _pRumRecordProvider->ConsumeSessionRecords(_sessionRecordsBuffer);
  if (_viewRecordsBuffer.empty() && _sessionRecordsBuffer.empty()) {
    return {};
  }

  auto json = SerializeRumRecordsToJson(_viewRecordsBuffer, _sessionRecordsBuffer);
  _viewRecordsBuffer.clear();
  _sessionRecordsBuffer.clear();
  return json;
}

void ProfileExporter::DiscardRumRecords() {
  if (_pRumRecordProvider == nullptr) {
    return;
  }
  _pRumRecordProvider->ConsumeViewRecords(_viewRecordsBuffer);
  _pRumRecordProvider->ConsumeSessionRecords(_sessionRecordsBuffer);
  _viewRecordsBuffer.clear();
  _sessionRecordsBuffer.clear();
}

void ProfileExporter::WriteDebugFiles(
    const ddog_prof_EncodedProfile* profile,
    const std::string& rumRecordsJson,
    int64_t endUnixTimeMs
) {
  if (!_debugPprofFileWritingEnabled || _debugPprofPrefix.empty()) {
    return;
  }
  if (!WritePprofFile(profile)) {
    Log::Warn("Failed to write debug pprof file (continuing with export)");
  }
  if (!rumRecordsJson.empty()) {
    ddog_Timespec time = {endUnixTimeMs / 1000, 0};
    if (!WriteRumRecordsFile(rumRecordsJson, time)) {
      Log::Warn("Failed to write RUM records file (continuing with export)");
    }
  }
}

bool ProfileExporter::DeliverProfileToCallback(
    const ddog_ByteSlice& bytes,
    int64_t startUnixTimeMs,
    int64_t endUnixTimeMs,
    bool lastCall
) {
  ProfilingData data;
  if (bytes.len != 0) {
    data.pprof.assign(bytes.ptr, bytes.ptr + bytes.len);
  }
  data.startUnixTimeMs = startUnixTimeMs;
  data.endUnixTimeMs = endUnixTimeMs;
  data.isLast = lastCall;
  return DeliverProfilingData(std::move(data));
}

void ProfileExporter::LogExport(
    bool lastCall, int64_t durationMs, size_t profileSize
) const {
  Log::Info(
      lastCall ? "Export last profile #" : "Export profile #",
      _currentExportId,
      ", Process ID: ",
      _processId,
      ", Runtime ID: ",
      _runtimeId,
      ", Profile duration: ",
      durationMs,
      "ms",
      ", Profile buffer size: ",
      profileSize,
      " bytes",
      ", Persistent symbol cache size: ",
      _persistentSymbolCache.size()
  );
}

bool ProfileExporter::DeliverProfilingData(ProfilingData data) {
  try {
    _profilingDataCallback(std::move(data));
    return true;
  } catch (const std::exception& error) {
    Log::Error("Profiling data callback threw: ", error.what());
  } catch (...) {
    Log::Error("Profiling data callback threw a non-standard exception");
  }
  return false;
}

bool ProfileExporter::ReportError(ProfilingErrorCode code, bool lastCall) {
  if (_profilingDataCallback) {
    ProfilingData data;
    data.result = static_cast<uint32_t>(code);
    data.isLast = lastCall;
    DeliverProfilingData(std::move(data));
  }
  return false;
}

bool ProfileExporter::FinishExport(
    std::chrono::system_clock::time_point end, bool lastCall
) {
  bool success = true;
  if (!_aggregator->Reset()) {
    success = ReportError(ProfilingErrorCode::ProfileResetFailed, lastCall);
  } else if (!InternSampleLabels(_sampleLabels)) {
    success = ReportError(ProfilingErrorCode::SampleLabelsUnavailable, lastCall);
  }

  // Reset the profile start time for the next collection cycle
  _profileStartTime = end;

  // Increment export ID for next export
  _currentExportId++;

  // Periodic cleanup for large persistent cache
  if (_currentExportId % CACHE_CLEANUP_THRESHOLD == 0) {
    CleanupUnusedCacheEntries(_currentExportId);
  }
  return success;
}

std::string ProfileExporter::ComputeRuntimeId() {
  // Generate a unique runtime ID using UUID
  ddprof::Uuid uuid;
  return uuid.to_string();
}

std::optional<ddog_prof_LocationId> ProfileExporter::InternLocation(uint64_t address) {
  // Check current export location cache first
  auto it = _currentExportLocationCache.find(address);
  if (it != _currentExportLocationCache.end()) {
    return it->second;
  }

  // Get profile for interning operations
  ddog_prof_Profile* profile = _aggregator->GetProfile();
  if (profile == nullptr) {
    // this should never happen if aggregator is properly initialized
    //  i.e. we should avoid calling InternLocation before aggregator is ready
    return std::nullopt;
  }

  // Check persistent symbol cache first
  CachedSymbolInfo symbolInfo;
  auto symbolIt = _persistentSymbolCache.find(address);
  if (symbolIt != _persistentSymbolCache.end()) {
    // Use cached symbol info
    symbolInfo = symbolIt->second;
  } else {
    // Symbolicate and cache the result persistently
    auto symbolInfoOpt = _symbolication->SymbolicateAndIntern(address, _stringStorage);
    if (!symbolInfoOpt.has_value()) {
      LogOnce(Error, "Failed to symbolicate address 0x", std::hex, address, std::dec);
      return std::nullopt;
    }
    symbolInfo = symbolInfoOpt.value();
    _persistentSymbolCache[address] = symbolInfo;
  }

  uint64_t addressForProfile = address;

  if (symbolInfo.ModuleBaseAddress != 0) {
    addressForProfile = address - symbolInfo.ModuleBaseAddress;
  }

  // Intern the mapping using the cached symbol info (module name and build ID)
  std::optional<ddog_prof_MappingId> mappingIdOpt;
  if (symbolInfo.ModuleNameId.value != 0 || symbolInfo.BuildIdId.value != 0) {
    mappingIdOpt = InternMapping(symbolInfo, profile);
    // Note: We continue even if mapping creation fails - location can exist without
    // mapping
  }

  // Intern the function using the cached symbol info
  // todo: we could have a cache that has start / end addresses of the function (cf
  // ddprof)
  auto functionIdOpt = InternFunction(symbolInfo, profile);
  if (!functionIdOpt.has_value()) {
    LogOnce(
        Error, "Failed to intern function for address 0x", std::hex, address, std::dec
    );
    return std::nullopt;
  }

  // Create location using the function, address, and optionally the mapping
  ddog_prof_LocationId_Result locationResult;
  if (mappingIdOpt.has_value()) {
    // Create location with mapping ID
    locationResult = ddog_prof_Profile_intern_location_with_mapping_id(
        profile,
        mappingIdOpt.value(),
        functionIdOpt.value(),
        addressForProfile,
        static_cast<int64_t>(symbolInfo.lineNumber)
    );
  } else {
    // Create location without mapping (fallback)
    locationResult = ddog_prof_Profile_intern_location(
        profile,
        functionIdOpt.value(),
        addressForProfile,
        static_cast<int64_t>(symbolInfo.lineNumber)
    );
  }

  if (locationResult.tag ==
      DDOG_PROF_LOCATION_ID_RESULT_OK_GENERATIONAL_ID_LOCATION_ID) {
    // Cache the result for this export only
    _currentExportLocationCache[address] = locationResult.ok;
    return locationResult.ok;
  }

  LogOnce(
      Error,
      "Failed to intern location for address 0x",
      std::hex,
      address,
      std::dec,
      " (tag: ",
      locationResult.tag,
      ")"
  );
  return std::nullopt;
}

std::optional<ddog_prof_MappingId> ProfileExporter::InternMapping(
    const CachedSymbolInfo& symbolInfo, ddog_prof_Profile* profile
) {
  // Create a cache key based on module name and build ID
  // Use hash_combine for proper hash combination
  uint64_t mappingKey = static_cast<uint64_t>(symbolInfo.ModuleNameId.value);
  hash_combine(mappingKey, static_cast<uint64_t>(symbolInfo.BuildIdId.value));

  // Check if we've already interned this mapping
  auto it = _currentExportMappingCache.find(mappingKey);
  if (it != _currentExportMappingCache.end()) {
    return it->second;
  }

  // Intern module name and build ID as strings in the profile
  ddog_prof_StringId moduleNameStringId = ddog_prof_Profile_interned_empty_string();
  ddog_prof_StringId buildIdStringId = ddog_prof_Profile_interned_empty_string();

  // Intern module name if available
  if (symbolInfo.ModuleNameId.value != 0) {
    auto moduleNameResult =
        ddog_prof_Profile_intern_managed_string(profile, symbolInfo.ModuleNameId);
    if (moduleNameResult.tag ==
        DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
      moduleNameStringId = moduleNameResult.ok;
    } else {
      LogOnce(
          Error,
          "InternMapping: Failed to intern module name (tag: ",
          moduleNameResult.tag,
          ")"
      );
    }
  }

  // Intern build ID if available
  if (symbolInfo.BuildIdId.value != 0) {
    auto buildIdResult =
        ddog_prof_Profile_intern_managed_string(profile, symbolInfo.BuildIdId);
    if (buildIdResult.tag == DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
      buildIdStringId = buildIdResult.ok;
    } else {
      LogOnce(
          Error,
          "InternMapping: Failed to intern build ID (tag: ",
          buildIdResult.tag,
          ")"
      );
    }
  }

  // Process address space for Windows mappings: [module_base, module_base +
  // module_size). This is only for debug purposes
  uint64_t memoryStart = symbolInfo.ModuleBaseAddress;
  uint64_t memoryLimit = symbolInfo.ModuleBaseAddress + symbolInfo.ModuleSize;

  auto mappingResult = ddog_prof_Profile_intern_mapping(
      profile,
      memoryStart,         // memory_start (module base address)
      memoryLimit,         // memory_limit (module end address)
      0,                   // file_offset
      moduleNameStringId,  // filename (module name)
      buildIdStringId      // build_id
  );

  if (mappingResult.tag == DDOG_PROF_MAPPING_ID_RESULT_OK_GENERATIONAL_ID_MAPPING_ID) {
    // Cache the mapping for reuse
    _currentExportMappingCache[mappingKey] = mappingResult.ok;
    return mappingResult.ok;
  }

  LogOnce(
      Error, "InternMapping: Failed to intern mapping (tag: ", mappingResult.tag, ")"
  );
  return std::nullopt;
}

static bool s_hasLoggedInternFunctionNameError = false;

std::optional<ddog_prof_FunctionId> ProfileExporter::InternFunction(
    const CachedSymbolInfo& symbolInfo, ddog_prof_Profile* profile
) {
  // Convert ManagedStringId to StringId (now using shared string storage)
  auto nameResult =
      ddog_prof_Profile_intern_managed_string(profile, symbolInfo.FunctionNameId);
  if (nameResult.tag != DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
    if (!s_hasLoggedInternFunctionNameError) {
      s_hasLoggedInternFunctionNameError = true;  // Log only once

      // Try to get the actual string to see what we're trying to intern
      std::string functionName;
      auto functionNameWrapper = ddog_prof_ManagedStringStorage_get_string(
          _stringStorage, symbolInfo.FunctionNameId
      );
      if (functionNameWrapper.tag == DDOG_STRING_WRAPPER_RESULT_OK) {
        functionName = std::string(
            reinterpret_cast<const char*>(functionNameWrapper.ok.message.ptr),
            functionNameWrapper.ok.message.len
        );
        ddog_StringWrapper_drop(&functionNameWrapper.ok);
      }

      if (functionName.empty()) {
        Log::Error("Failed to intern function name (tag: ", nameResult.tag, ")");
      } else {
        Log::Error(
            "Failed to intern function name '",
            functionName,
            "' (tag: ",
            nameResult.tag,
            ")"
        );
      }
    }
    return std::nullopt;
  }
  auto filenameResult =
      ddog_prof_Profile_intern_managed_string(profile, symbolInfo.FileNameId);
  if (filenameResult.tag != DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
    LogOnce(
        Error,
        "InternFunction: Failed to intern filename (tag: ",
        filenameResult.tag,
        ")"
    );
    return std::nullopt;
  }

  // Create function using StringId (use same string for name and system_name for now)
  // todo: this is not correct, we need to use the actual string ids
  auto functionResult = ddog_prof_Profile_intern_function(
      profile, nameResult.ok, nameResult.ok, filenameResult.ok
  );
  if (functionResult.tag ==
      DDOG_PROF_FUNCTION_ID_RESULT_OK_GENERATIONAL_ID_FUNCTION_ID) {
    return functionResult.ok;
  }

  LogOnce(
      Error, "InternFunction: Failed to intern function (tag: ", functionResult.tag, ")"
  );
  return std::nullopt;
}

void ProfileExporter::OnExportStart() {
  // Clear per-export caches since location and mapping IDs become invalid after profile
  // reset
  _currentExportLocationCache.clear();
  _currentExportMappingCache.clear();

  Log::Debug(
      "Cleared location and mapping caches, keeping ",
      _persistentSymbolCache.size(),
      " persistent symbol entries"
  );
}

void ProfileExporter::ClearCaches() {
  // This method can now be used for emergency cleanup or testing
  _currentExportLocationCache.clear();
  _currentExportMappingCache.clear();
  _persistentSymbolCache.clear();

  Log::Debug("Cleared all caches");
}

void ProfileExporter::CleanupUnusedCacheEntries(uint32_t currentExportId) {
  // Periodic cleanup for persistent symbol cache if it gets too large
  if (_persistentSymbolCache.size() > 10000) {  // Max 10k entries
    Log::Warn(
        "CleanupUnusedCacheEntries: Symbol cache size (",
        _persistentSymbolCache.size(),
        ") exceeds limit, consider cleanup"
    );
    // Note: We could implement LRU cleanup here if needed, but for now just log
    // The symbol cache should generally stay reasonable in size since it's bounded by
    // the number of unique addresses in the process
  }
}

bool ProfileExporter::AddSingleTag(
    ddog_Vec_Tag& tags, std::string_view key, std::string_view value
) {
  ddog_CharSlice keySlice = to_CharSlice(key);
  ddog_CharSlice valueSlice = to_CharSlice(value);

  ddog_Vec_Tag_PushResult pushResult = ddog_Vec_Tag_push(&tags, keySlice, valueSlice);
  if (pushResult.tag == DDOG_VEC_TAG_PUSH_RESULT_ERR) {
    std::string message = "Failed to add tag: " + std::string(key);
    ddog_Error_drop(&pushResult.err);
    Log::Error(message);
    return false;
  }

  return true;
}

bool ProfileExporter::PrepareStableTags(ddog_Vec_Tag& tags) {
  // Add runtime-id tag (stable across exports)
  if (!AddSingleTag(tags, TAG_RUNTIME_ID, _runtimeId)) {
    return false;
  }

  if (!AddSingleTag(tags, "profiler_version", _kProfilerVersion)) {
    return false;
  }

  // add CPU related tags
  int physicalCores;
  int logicalCores;
  if (GetCpuCores(logicalCores, physicalCores)) {
    if (!AddSingleTag(tags, TAG_CPU_CORES_COUNT, std::to_string(physicalCores))) {
      return false;
    }
    if (!AddSingleTag(
            tags, TAG_CPU_LOGICAL_CORES_COUNT, std::to_string(logicalCores)
        )) {
      return false;
    }
  }

  std::string cpuVendor = GetCpuVendor();
  if (!cpuVendor.empty()) {
    if (!AddSingleTag(tags, TAG_CPU_VENDOR, cpuVendor)) {
      return false;
    }
  }

  std::string cpuModel = GetCpuModel();
  if (!cpuModel.empty()) {
    if (!AddSingleTag(tags, TAG_CPU_DESC, cpuModel)) {
      return false;
    }
  }

  std::string cpuArch = GetCpuArchitecture();
  if (!cpuArch.empty()) {
    if (!AddSingleTag(tags, TAG_CPU_ARCH, cpuArch)) {
      return false;
    }
  }

  // GPU tags depend on the number of GPUs
  std::string driverDescTag;
  std::string driverVersionTag;
  std::string driverDateTag;
  std::string gpuNameTag;
  std::string gpuChipTag;
  std::string gpuRamTag;

  std::string driverDesc;
  std::string driverVersion;
  std::string driverDate;
  std::string gpuName;
  std::string gpuChip;
  uint64_t gpuRam;
  int device = 0;
  while (GetGpuFromRegistry(
      device, driverDesc, driverVersion, driverDate, gpuName, gpuChip, gpuRam
  )) {
    // no need to send GPU details if GPU cannot be identified
    if (driverDesc.empty() && gpuName.empty()) {
      continue;
    }

    driverDescTag = TAG_GPU_DRIVER_DESC_PREFIX + std::to_string(device);
    if (!driverDesc.empty()) {
      if (!AddSingleTag(tags, driverDescTag, driverDesc)) {
        return false;
      }
    }

    driverVersionTag = TAG_GPU_DRIVER_VERSION_PREFIX + std::to_string(device);
    if (!driverVersion.empty()) {
      if (!AddSingleTag(tags, driverVersionTag, driverVersion)) {
        return false;
      }
    }

    driverDateTag = TAG_GPU_DRIVER_DATE_PREFIX + std::to_string(device);
    if (!driverDate.empty()) {
      if (!AddSingleTag(tags, driverDateTag, driverDate)) {
        return false;
      }
    }

    gpuNameTag = TAG_GPU_NAME_PREFIX + std::to_string(device);
    if (!gpuName.empty()) {
      if (!AddSingleTag(tags, gpuNameTag, gpuName)) {
        return false;
      }
    }

    gpuChipTag = TAG_GPU_CHIP_PREFIX + std::to_string(device);
    if (!gpuChip.empty()) {
      if (!AddSingleTag(tags, gpuChipTag, gpuChip)) {
        return false;
      }
    }

    gpuRamTag = TAG_GPU_RAM_PREFIX + std::to_string(device);
    if (gpuRam > 0) {
      if (!AddSingleTag(tags, gpuRamTag, std::to_string(gpuRam))) {
        return false;
      }
    }

    device++;
  }

  if (!AddSingleTag(tags, TAG_GPU_COUNT, std::to_string(device))) {
    return false;
  }

  // add memory related tags
  uint64_t totalPhys;
  uint64_t availPhys;
  uint32_t memoryLoad;
  if (GetMemoryInfo(totalPhys, availPhys, memoryLoad)) {
    if (!AddSingleTag(tags, TAG_RAM_SIZE, std::to_string(totalPhys))) {
      return false;
    }
  }

  // Add remote_symbols tag to indicate symbolication support
  if (!AddSingleTag(tags, TAG_REMOTE_SYMBOLS, "yes")) {
    return false;
  }

  // Add runtime_os tag to indicate the operating system
  if (!AddSingleTag(tags, TAG_RUNTIME_OS, "windows")) {
    return false;
  }

  return true;
}

bool ProfileExporter::InternSampleLabels(SampleLabels& labels) {
  const auto fail = [](ddog_Error& error, const char* message) {
    ddog_Error_drop(&error);
    Log::Error(message);
    return false;
  };
  ddog_prof_Profile* profile = _aggregator->GetProfile();
  if (profile == nullptr) {
    Log::Error("Profile unavailable while interning sample labels");
    return false;
  }

  // Intern the process_id label key
  auto processIdKeyResult =
      ddog_prof_Profile_intern_string(profile, to_CharSlice(LABEL_PROCESS_ID));
  if (processIdKeyResult.tag !=
      DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
    return fail(processIdKeyResult.err, "Failed to intern process_id label key");
  }

  // Intern the process ID value
  std::string processIdStr = std::to_string(GetCurrentProcessId());
  auto processIdValueResult =
      ddog_prof_Profile_intern_string(profile, to_CharSlice(processIdStr));
  if (processIdValueResult.tag !=
      DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
    return fail(processIdValueResult.err, "Failed to intern process_id value");
  }

  // Create process_id label ID using the proper API function
  auto processIdLabelResult = ddog_prof_Profile_intern_label_str(
      profile, processIdKeyResult.ok, processIdValueResult.ok
  );
  if (processIdLabelResult.tag !=
      DDOG_PROF_LABEL_ID_RESULT_OK_GENERATIONAL_ID_LABEL_ID) {
    return fail(processIdLabelResult.err, "Failed to intern process_id label");
  }

  labels.processIdLabelId = processIdLabelResult.ok;
  labels.processIdValueId = processIdValueResult.ok;

  // Intern the thread_id label key (we'll create specific labels per thread later)
  auto threadIdKeyResult =
      ddog_prof_Profile_intern_string(profile, to_CharSlice(LABEL_THREAD_ID));
  if (threadIdKeyResult.tag !=
      DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
    return fail(threadIdKeyResult.err, "Failed to intern thread_id label key");
  }
  // Store the thread ID label key ID (we'll use this to create per-thread labels)
  labels.threadIdKeyId = threadIdKeyResult.ok;

  // Intern the thread name value
  auto threadNameKeyResult =
      ddog_prof_Profile_intern_string(profile, to_CharSlice(LABEL_THREAD_NAME));
  if (threadNameKeyResult.tag !=
      DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
    return fail(threadNameKeyResult.err, "Failed to intern thread_name label key");
  }
  // Store the thread name label key ID (we'll use this to create per-thread labels)
  labels.threadNameKeyId = threadNameKeyResult.ok;

  // Intern RUM label keys (values are interned per-sample since they vary across views)
  auto rumViewIdKeyResult =
      ddog_prof_Profile_intern_string(profile, to_CharSlice(LABEL_RUM_VIEW_ID));
  if (rumViewIdKeyResult.tag !=
      DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
    return fail(rumViewIdKeyResult.err, "Failed to intern rum.view_id label key");
  }
  labels.rumViewIdKeyId = rumViewIdKeyResult.ok;

  auto traceEndpointKeyResult =
      ddog_prof_Profile_intern_string(profile, to_CharSlice(LABEL_TRACE_ENDPOINT));
  if (traceEndpointKeyResult.tag !=
      DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
    return fail(
        traceEndpointKeyResult.err, "Failed to intern trace endpoint label key"
    );
  }
  labels.traceEndpointKeyId = traceEndpointKeyResult.ok;

  return true;
}

ddog_prof_LabelSetId ProfileExporter::CreateLabelSet(
    const SampleLabels& labels,
    std::shared_ptr<ThreadInfo> threadInfo,
    const RumViewContext& rumView
) {
  // Get profile for interning operations
  ddog_prof_Profile* profile = _aggregator->GetProfile();
  if (!profile) {
    return ddog_prof_LabelSetId{};
  }

  std::vector<ddog_prof_LabelId> labelIdArray;

  // Always add process_id label
  labelIdArray.push_back(labels.processIdLabelId);

  // Add thread_id label if thread info is available
  if (threadInfo) {
    // Use numeric label for thread ID (more efficient than string conversion)
    int64_t threadId = static_cast<int64_t>(threadInfo->GetThreadId());
    auto threadIdLabelResult =
        ddog_prof_Profile_intern_label_num(profile, labels.threadIdKeyId, threadId);
    if (threadIdLabelResult.tag ==
        DDOG_PROF_LABEL_ID_RESULT_OK_GENERATIONAL_ID_LABEL_ID) {
      labelIdArray.push_back(threadIdLabelResult.ok);
    } else {
      Log::Error(
          "CreateLabelSet: Failed to intern thread_id numeric label (tag: ",
          threadIdLabelResult.tag,
          ")"
      );
    }

    std::string name;
    if (threadInfo->GetThreadName(name)) {
      // Intern the thread name value
      auto threadNameValueResult =
          ddog_prof_Profile_intern_string(profile, to_CharSlice(name));
      if (threadNameValueResult.tag !=
          DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
        LogOnce(
            Error,
            "Failed to intern thread_name value (tag: ",
            threadNameValueResult.tag,
            ")"
        );
      } else {
        auto threadNameLabelResult = ddog_prof_Profile_intern_label_str(
            profile, labels.threadNameKeyId, threadNameValueResult.ok
        );
        if (threadNameLabelResult.tag !=
            DDOG_PROF_LABEL_ID_RESULT_OK_GENERATIONAL_ID_LABEL_ID) {
          LogOnce(
              Error,
              "Failed to intern thread_name label (tag: ",
              threadNameLabelResult.tag,
              ")"
          );
        } else {
          labelIdArray.push_back(threadNameLabelResult.ok);
        }
      }
    }
  }

  // Add RUM view labels if an active view was captured for this sample
  if (!rumView.view_id.empty()) {
    auto viewIdValueResult =
        ddog_prof_Profile_intern_string(profile, to_CharSlice(rumView.view_id));
    if (viewIdValueResult.tag ==
        DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
      auto viewIdLabelResult = ddog_prof_Profile_intern_label_str(
          profile, labels.rumViewIdKeyId, viewIdValueResult.ok
      );
      if (viewIdLabelResult.tag ==
          DDOG_PROF_LABEL_ID_RESULT_OK_GENERATIONAL_ID_LABEL_ID) {
        labelIdArray.push_back(viewIdLabelResult.ok);
      }
    }

    if (!rumView.view_name.empty()) {
      auto viewNameValueResult =
          ddog_prof_Profile_intern_string(profile, to_CharSlice(rumView.view_name));
      if (viewNameValueResult.tag ==
          DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
        auto endpointLabelResult = ddog_prof_Profile_intern_label_str(
            profile, labels.traceEndpointKeyId, viewNameValueResult.ok
        );
        if (endpointLabelResult.tag ==
            DDOG_PROF_LABEL_ID_RESULT_OK_GENERATIONAL_ID_LABEL_ID) {
          labelIdArray.push_back(endpointLabelResult.ok);
        }
      }
    }
  }

  ddog_prof_Slice_LabelId labelSlice = {
      .ptr = labelIdArray.data(), .len = labelIdArray.size()
  };

  auto labelsetResult = ddog_prof_Profile_intern_labelset(profile, labelSlice);
  if (labelsetResult.tag !=
      DDOG_PROF_LABEL_SET_ID_RESULT_OK_GENERATIONAL_ID_LABEL_SET_ID) {
    LogOnce(Error, "Failed to intern labelset (tag: ", labelsetResult.tag, ")");
    return ddog_prof_LabelSetId{};
  }

  return labelsetResult.ok;
}

void ProfileExporter::SetRumApplicationId(const std::string& applicationId) {
  _rumApplicationId = applicationId;
}

uint32_t ProfileExporter::GetCurrentProcessId() { return ::GetCurrentProcessId(); }

bool ProfileExporter::WritePprofFile(const ddog_prof_EncodedProfile* encodedProfile) {
  if (!encodedProfile || _debugPprofPrefix.empty()) {
    return false;
  }

  // Use current time since we can't get the start time directly from encoded profile
  auto now = std::chrono::system_clock::now();
  auto time_t_now = std::chrono::system_clock::to_time_t(now);
  ddog_Timespec startTime = {static_cast<int64_t>(time_t_now), 0};

  int fd = -1;
  if (!CreatePprofFile(startTime, &fd)) {
    return false;
  }

  // RAII-style cleanup for file descriptor
  auto cleanup = [&fd]() {
    if (fd != -1) {
      _close(fd);
    }
  };

  bool success = WriteProfileToFile(encodedProfile, fd);
  cleanup();
  return success;
}

bool ProfileExporter::WriteRumRecordsFile(
    const std::string& json, const ddog_Timespec& startTime
) {
  if (json.empty() || _debugPprofPrefix.empty()) {
    return false;
  }

  constexpr size_t k_max_time_length = 128;
  char time_start[k_max_time_length] = {};

  std::tm tm_storage;
  if (gmtime_s(&tm_storage, &startTime.seconds) != 0) {
    Log::Debug("Failed to convert timestamp: cannot write RUM records to disk");
    return false;
  }

  std::strftime(time_start, std::size(time_start), "%Y%m%dT%H%M%SZ", &tm_storage);

  char filename[MAX_PATH];
  std::snprintf(
      filename,
      std::size(filename),
      "%s%s.rum-views.json",
      _debugPprofPrefix.c_str(),
      time_start
  );

  Log::Debug("Writing RUM records to file ", filename);

  std::ofstream ofs(filename, std::ios::binary);
  if (!ofs) {
    Log::Error("Failed to create RUM records file: '", filename, "'");
    return false;
  }

  ofs.write(json.data(), json.size());
  if (!ofs) {
    Log::Error("Failed to write RUM records to file: '", filename, "'");
    return false;
  }

  Log::Info("Successfully wrote RUM records to ", filename);
  return true;
}

bool ProfileExporter::CreatePprofFile(const ddog_Timespec& startTime, int* fd) {
  constexpr size_t k_max_time_length = 128;
  char time_start[k_max_time_length] = {};

  // Convert timestamp to formatted string
  std::tm tm_storage;
  if (gmtime_s(&tm_storage, &startTime.seconds) != 0) {
    Log::Debug("Failed to convert timestamp: cannot write profile to disk");
    return false;
  }

  std::strftime(time_start, std::size(time_start), "%Y%m%dT%H%M%SZ", &tm_storage);

  // Create filename with timestamp
  // Note: .lz4 extension is not added so the tools could easily open the files on
  // Windows thanks to file association
  char filename[MAX_PATH];
  std::snprintf(
      filename,
      std::size(filename),
      "%s%s.lz4.pprof",
      _debugPprofPrefix.c_str(),
      time_start
  );

  Log::Debug("Writing pprof to file ", filename);

  // Create file with read/write permissions for user only
  constexpr int read_write_user_only = _S_IREAD | _S_IWRITE;
  if (_sopen_s(
          fd, filename, _O_CREAT | _O_RDWR | _O_BINARY, _SH_DENYNO, read_write_user_only
      ) != 0) {
    char errorBuffer[256];
    strerror_s(errorBuffer, sizeof(errorBuffer), errno);
    Log::Error("Failed to create pprof file: '", filename, "'  - Error: ", errorBuffer);
    return false;
  }

  return true;
}

bool ProfileExporter::WriteProfileToFile(
    const ddog_prof_EncodedProfile* encodedProfile, int fd
) {
  // Get the profile bytes using the new API
  auto bytesResult = ddog_prof_EncodedProfile_bytes(
      const_cast<ddog_prof_EncodedProfile*>(encodedProfile)
  );
  if (bytesResult.tag != DDOG_PROF_RESULT_BYTE_SLICE_OK_BYTE_SLICE) {
    Log::Error("Failed to get profile bytes from encoded profile");
    return false;
  }

  const ddog_Slice_U8& buffer = bytesResult.ok;
  int bytes_written = _write(fd, buffer.ptr, static_cast<unsigned int>(buffer.len));

  if (bytes_written <= 0) {
    char errorBuffer[256];
    strerror_s(errorBuffer, sizeof(errorBuffer), errno);
    Log::Error("Failed to write byte buffer to file: ", errorBuffer);
    return false;
  }

  if (static_cast<size_t>(bytes_written) != buffer.len) {
    Log::Error(
        "Partial write to file: wrote ", bytes_written, " bytes out of ", buffer.len
    );
    return false;
  }

  Log::Info("Successfully wrote ", bytes_written, " bytes to pprof file");
  return true;
}

void ProfileExporter::EscapeJsonString(std::ostream& out, const std::string& s) {
  for (char c : s) {
    switch (c) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\b':
        out << "\\b";
        break;
      case '\f':
        out << "\\f";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          out << buf;
        } else {
          out << c;
        }
        break;
    }
  }
}

std::string ProfileExporter::SerializeRumRecordsToJson(
    const std::vector<RumViewRecord>& viewRecords,
    const std::vector<RumSessionRecord>& sessionRecords
) {
  std::ostringstream ss;
  ss << "{\"views\":[";
  for (size_t i = 0; i < viewRecords.size(); ++i) {
    if (i > 0) ss << ',';
    ss << "{\"startClocks\":{\"relative\":0,\"timeStamp\":"
       << viewRecords[i].timestamp_ms << "},\"duration\":" << viewRecords[i].duration_ms
       << ",\"viewId\":\"";
    EscapeJsonString(ss, viewRecords[i].view_id);
    ss << "\",\"viewName\":\"";
    EscapeJsonString(ss, viewRecords[i].view_name);
    ss << "\",\"vitals\":{\"cpuTimeNs\":"
       << viewRecords[i].vitals_ns[static_cast<size_t>(ViewVitalKind::CpuTime)]
       << ",\"waitTimeNs\":"
       << viewRecords[i].vitals_ns[static_cast<size_t>(ViewVitalKind::WaitTime)]
       << "}}";
  }
  ss << "],\"sessions\":[";
  for (size_t i = 0; i < sessionRecords.size(); ++i) {
    if (i > 0) ss << ',';
    ss << "{\"startClocks\":{\"relative\":0,\"timeStamp\":"
       << sessionRecords[i].timestamp_ms
       << "},\"duration\":" << sessionRecords[i].duration_ms << ",\"sessionId\":\"";
    EscapeJsonString(ss, sessionRecords[i].session_id);
    ss << "\"}";
  }
  ss << "]}";
  return ss.str();
}

// Export functionality implementation

bool ProfileExporter::InitializeExporter() {
  BuildExportUrl();

  // Create endpoint
  ddog_prof_Endpoint endpoint;
  if (!CreateExporterEndpoint(endpoint)) {
    return false;
  }

  // Prepare stable tags for exporter creation
  ddog_Vec_Tag stableTags = ddog_Vec_Tag_new();

  // Add language tag (required)
  if (!AddSingleTag(stableTags, "language", "native")) {
    ddog_Vec_Tag_drop(stableTags);
    return false;
  }

  // Add runtime and hardware details
  if (!PrepareStableTags(stableTags)) {
    ddog_Vec_Tag_drop(stableTags);
    return false;
  }

  // Add service information from configuration if available
  if (_pConfiguration) {
    auto service = _pConfiguration->GetServiceName();
    auto environment = _pConfiguration->GetEnvironment();
    auto version = _pConfiguration->GetVersion();
    auto hostname = _pConfiguration->GetHostname();

    if (!service.empty()) {
      if (!AddSingleTag(stableTags, "service", service)) {
        ddog_Vec_Tag_drop(stableTags);
        return false;
      }
    }
    if (!environment.empty()) {
      if (!AddSingleTag(stableTags, "env", environment)) {
        ddog_Vec_Tag_drop(stableTags);
        return false;
      }
    }
    if (!version.empty()) {
      if (!AddSingleTag(stableTags, "version", version)) {
        ddog_Vec_Tag_drop(stableTags);
        return false;
      }
    }
    if (!hostname.empty()) {
      if (!AddSingleTag(stableTags, "host", hostname)) {
        ddog_Vec_Tag_drop(stableTags);
        return false;
      }
    }

    // Add user-defined tags from configuration
    const auto& userTags = _pConfiguration->GetUserTags();
    for (const auto& tag : userTags) {
      if (!AddSingleTag(stableTags, tag.first, tag.second)) {
        ddog_Vec_Tag_drop(stableTags);
        return false;
      }
    }
  }

  // Create exporter
  auto exporterResult = ddog_prof_Exporter_new(
      to_CharSlice(_kProfilerUserAgent),  // user_agent
      to_CharSlice(_kProfilerVersion),    // profiler_version
      to_CharSlice("windows"),            // family
      &stableTags,
      endpoint
  );

  ddog_Vec_Tag_drop(stableTags);

  if (exporterResult.tag !=
      DDOG_PROF_PROFILE_EXPORTER_RESULT_OK_HANDLE_PROFILE_EXPORTER) {
    std::string message = "Failed to create exporter";
    if (exporterResult.tag ==
        DDOG_PROF_PROFILE_EXPORTER_RESULT_ERR_HANDLE_PROFILE_EXPORTER) {
      // Extract error message
      auto& error = exporterResult.err;
      std::string errorMessage = std::string(
          reinterpret_cast<const char*>(error.message.ptr), error.message.len
      );
      message += ": " + errorMessage;
      ddog_Error_drop(&exporterResult.err);
    } else {
      message += " with unknown error tag: " + std::to_string(exporterResult.tag);
    }
    Log::Error(message);
    return false;
  }

  _exporter = exporterResult.ok;

  // Set timeout
  auto timeoutResult = ddog_prof_Exporter_set_timeout(&_exporter, EXPORT_TIMEOUT_MS);
  if (timeoutResult.tag == DDOG_VOID_RESULT_ERR) {
    std::string message = "Failed to set exporter timeout";
    auto& error = timeoutResult.err;
    std::string errorMessage = std::string(
        reinterpret_cast<const char*>(error.message.ptr), error.message.len
    );
    message += ": " + errorMessage;
    ddog_Error_drop(&timeoutResult.err);
    CleanupExporter();
    Log::Error(message);
    return false;
  }

  return true;
}

void ProfileExporter::BuildExportUrl() {
  if (_agentMode) {
    // Agent mode - use agent URL/host/port
    if (_pConfiguration) {
      std::string agentUrl = _pConfiguration->GetAgentUrl();

      if (!agentUrl.empty()) {
        _exportUrl = agentUrl;
      } else {
        std::string agentHost = _pConfiguration->GetAgentHost();
        int32_t agentPort = _pConfiguration->GetAgentPort();
        _exportUrl = "http://" + agentHost + ":" + std::to_string(agentPort);
      }
    } else {
      _exportUrl = "http://localhost:8126";
    }
  } else {
    // Agentless mode - libdatadog expects just the site, it constructs the intake URL
    _exportUrl = _pConfiguration ? _pConfiguration->GetSite() : "datadoghq.com";

    Log::Info(
        "Agentless mode, using site: ",
        _exportUrl,
        ", API key length: ",
        _apiKey.length()
    );
  }

  Log::Info(
      "Using URL: ", _exportUrl, " (mode: ", (_agentMode ? "agent" : "agentless"), ")"
  );
}

bool ProfileExporter::CreateExporterEndpoint(ddog_prof_Endpoint& endpoint) {
  ddog_CharSlice urlSlice = to_CharSlice(_exportUrl);

  if (_agentMode) {
    endpoint = ddog_prof_Endpoint_agent(urlSlice);
  } else {
    if (_apiKey.empty()) {
      Log::Error("Missing API key for agentless mode");
      return false;
    }
    ddog_CharSlice apiKeySlice = to_CharSlice(_apiKey);
    endpoint = ddog_prof_Endpoint_agentless(urlSlice, apiKeySlice);
  }

  return true;
}

bool ProfileExporter::ExportProfile(
    const ddog_prof_EncodedProfile* encodedProfile,
    uint32_t profileSeq,
    const std::string& rumRecordsJson
) {
  if (!_exporter.inner || !encodedProfile) {
    Log::Error("Exporter not initialized or invalid profile");
    return false;
  }

  // Prepare additional tags (per-export metadata)
  ddog_Vec_Tag additionalTags = ddog_Vec_Tag_new();
  if (!PrepareAdditionalTags(additionalTags, profileSeq)) {
    ddog_Vec_Tag_drop(additionalTags);
    return false;
  }

  // Get profile bytes for file creation
  auto bytesResult = ddog_prof_EncodedProfile_bytes(
      const_cast<ddog_prof_EncodedProfile*>(encodedProfile)
  );
  if (bytesResult.tag != DDOG_PROF_RESULT_BYTE_SLICE_OK_BYTE_SLICE) {
    ddog_Error_drop(&bytesResult.err);
    ddog_Vec_Tag_drop(additionalTags);
    Log::Error("Failed to get profile bytes");
    return false;
  }

  // Pass the RUM records JSON through optional_internal_metadata_json.
  // libdatadog embeds it in the `internal` field of the profile's event.json,
  // so no separate file attachment is needed. The backing std::string must
  // outlive the Request_build call; it's a function-scoped local.
  ddog_CharSlice internalMetadataSlice{};
  const ddog_CharSlice* internalMetadataPtr = nullptr;
  if (!rumRecordsJson.empty()) {
    internalMetadataSlice = to_CharSlice(rumRecordsJson);
    internalMetadataPtr = &internalMetadataSlice;
  }

  // Build request - time information is now embedded in the EncodedProfile
  auto requestResult = ddog_prof_Exporter_Request_build(
      &_exporter,
      const_cast<ddog_prof_EncodedProfile*>(encodedProfile),  // profile
      ddog_prof_Exporter_Slice_File_empty(),  // files_to_compress_and_export
      ddog_prof_Exporter_Slice_File_empty(),  // files_to_export_unmodified
      &additionalTags,                        // optional_additional_tags
      internalMetadataPtr,                    // optional_internal_metadata_json
      nullptr                                 // optional_info_json
  );

  ddog_Vec_Tag_drop(additionalTags);

  if (requestResult.tag != DDOG_PROF_REQUEST_RESULT_OK_HANDLE_REQUEST) {
    std::string message = "Failed to build export request";
    if (requestResult.tag == DDOG_PROF_REQUEST_RESULT_ERR_HANDLE_REQUEST) {
      auto& error = requestResult.err;
      std::string errorMessage = std::string(
          reinterpret_cast<const char*>(error.message.ptr), error.message.len
      );
      message += ": " + errorMessage;
      ddog_Error_drop(&requestResult.err);
    } else {
      message += " with unknown error tag: " + std::to_string(requestResult.tag);
    }
    Log::Error(message);
    return false;
  }

  ddog_prof_Request request = requestResult.ok;

  // Send request
  auto sendResult = ddog_prof_Exporter_send(&_exporter, &request, nullptr);

  // Request is consumed by send, so we don't need to drop it

  if (sendResult.tag == DDOG_PROF_RESULT_HTTP_STATUS_ERR_HTTP_STATUS) {
    std::string message = "Failed to send profile";
    auto& error = sendResult.err;
    std::string errorMessage = std::string(
        reinterpret_cast<const char*>(error.message.ptr), error.message.len
    );
    message += ": " + errorMessage;
    ddog_Error_drop(&sendResult.err);
    Log::Error(message);
    return false;
  }

  // Check HTTP response code
  uint16_t responseCode = sendResult.ok.code;
  if (!CheckExportResponse(responseCode)) {
    return false;
  }

  if (responseCode >= 200 && responseCode < 300) {
    Log::Info(
        "Successfully sent profile, HTTP ",
        responseCode,
        " (size: ",
        bytesResult.ok.len,
        " bytes)"
    );
  }

  return true;
}

bool ProfileExporter::PrepareAdditionalTags(ddog_Vec_Tag& tags, uint32_t profileSeq) {
  // Add profile sequence number
  if (!AddSingleTag(tags, TAG_PROFILE_SEQ, std::to_string(profileSeq))) {
    return false;
  }

  // Add process ID as additional tag
  if (!AddSingleTag(tags, "pid", std::to_string(_processId))) {
    return false;
  }

  // Add RUM application ID tag (set once via SetRumApplicationId)
  if (!_rumApplicationId.empty()) {
    if (!AddSingleTag(tags, TAG_RUM_APPLICATION_ID, _rumApplicationId)) {
      return false;
    }
  }

  // Note: RUM session IDs are no longer emitted as a tag. They are now
  // embedded in the optional_internal_metadata_json payload under
  // "rum_session_ids", alongside the per-view vitals.

  return true;
}

bool ProfileExporter::CheckExportResponse(uint16_t responseCode) {
  if (responseCode >= 200 && responseCode < 300) {
    return true;
  }
  Log::Error("Profile upload rejected with HTTP ", responseCode);
  return false;
}

void ProfileExporter::CleanupExporter() {
  if (_exporter.inner) {
    ddog_prof_Exporter_drop(&_exporter);
    _exporter.inner = nullptr;
  }
}
