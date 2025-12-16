// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"
#include "ProfileExporter.h"
#include "LibDatadogHelper.h"
#include "OsSpecificApi.h"
#include "Uuid.h"
#include <iostream>
#include <chrono>
#include <fstream>
#include <ctime>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <io.h>
#include "Log.h"
#include "version.h"


using namespace dd_win_prof;
using namespace OsSpecificApi;

ProfileExporter::ProfileExporter(Configuration* pConfiguration, std::span<const SampleValueType> sampleTypeDefinitions)
    :
    _pConfiguration(pConfiguration),
    _sampleTypeDefinitions{ sampleTypeDefinitions.begin(), sampleTypeDefinitions.end() },
    _initialized(false),
    _processId{0},
    _currentExportId(0),
    _debugPprofFileWritingEnabled(false),  // Will be set based on configuration
    _debugPprofPrefix(""),
    _exportEnabled(false),
    _exporter(nullptr),
    _agentMode(true),
    _consecutiveErrors(0)
{
    _runtimeId = ComputeRuntimeId();

    _kProfilerVersion = PROFILER_VERSION_STRING;
    _kProfilerUserAgent = DLL_NAME;

    // Configure debug output from Configuration
    if (_pConfiguration != nullptr) {
        auto outputDir = _pConfiguration->GetProfilesOutputDirectory();
        if (!outputDir.empty()) {
            // ensure the output directory exists
            const auto log_path = fs::path(outputDir);
            if (!fs::exists(log_path))
            {
                fs::create_directories(log_path);
            }

            _debugPprofFileWritingEnabled = true;
            _debugPprofPrefix = outputDir.string();
            // Ensure the prefix ends with a path separator
            if (!_debugPprofPrefix.empty() &&
                _debugPprofPrefix.back() != '/' &&
                _debugPprofPrefix.back() != '\\') {
                _debugPprofPrefix += "/";
            }
            _debugPprofPrefix += "profile_";
        }

        // Configure export settings from Configuration
        _exportEnabled = _pConfiguration->IsExportEnabled(); // Use dedicated export setting
        _apiKey = _pConfiguration->GetApiKey();
        _agentMode = !_pConfiguration->IsAgentless(); // Use agent mode unless agentless is specified
    }
}

ProfileExporter::~ProfileExporter()
{
    // Just call cleanup - by this point, explicit cleanup should have been called
    // If not, we'll do basic cleanup but may skip exporter cleanup to avoid deadlocks
    if (_initialized) {
        Cleanup(true);  // Skip exporter cleanup in destructor to avoid deadlocks
    }
}

void ProfileExporter::Cleanup(bool skipExporterCleanup)
{
    if (!_initialized) {
        return;  // Already cleaned up
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
    ddog_prof_ManagedStringStorage_drop(_stringStorage);

    _initialized = false;
    Log::Debug("Exporter cleanup completed");
}

bool ProfileExporter::Initialize()
{
    try {
        _processId = GetCurrentProcessId();

        // Initialize libdatadog's managed string storage
        auto storageResult = ddog_prof_ManagedStringStorage_new();
        if (storageResult.tag != DDOG_PROF_MANAGED_STRING_STORAGE_NEW_RESULT_OK) {
            _lastError = "Failed to create libdatadog ManagedStringStorage";
            return false;
        }
        _stringStorage = storageResult.ok;

        // Initialize symbolication engine
        _symbolication = std::make_unique<Symbolication>();
        if (!_symbolication->Initialize(_stringStorage, _pConfiguration->AreCallstacksSymbolized())) {
            _lastError = "Failed to initialize symbolication engine";
            return false;
        }

        // Initialize PprofAggregator directly with SampleValueType (no enum conversion needed)
        _aggregator = std::make_unique<dd_win_prof::PprofAggregator>(_sampleTypeDefinitions, _stringStorage, 10);

        if (!_aggregator->IsInitialized()) {
            _lastError = "Failed to initialize PprofAggregator: " + _aggregator->GetLastError();
            return false;
        }

        // Intern sample labels that will be reused across samples
        if (!InternSampleLabels(_sampleLabels)) {
            _lastError = "Failed to intern sample labels";
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
                _lastError = "Failed to initialize exporter: " + _lastError;
                return false;
            }
            Log::Info("Profiles export enabled, mode: ", (_agentMode ? "agent" : "agentless"), ", URL: ", _exportUrl);
        } else {
            Log::Info("Profiles export disabled");
        }

        _initialized = true;
        return true;
    }
    catch (const std::exception& ex) {
        _lastError = "Exception during initialization: " + std::string(ex.what());
        return false;
    }
}

bool ProfileExporter::Add(std::shared_ptr<Sample> const& sample)
{
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
            LogOnce(Error, "Failed to intern location for address 0x", std::hex, address, std::dec, " when adding sample");
            return false; // Return false to indicate failure
        }
        locationIds.push_back(locationIdOpt.value());
    }

    // Get sample values directly
    std::span<const int64_t> sampleValues = sample->GetValues();

    // Get timestamp - convert nanoseconds to int64_t
    int64_t timestampNs = sample->GetTimestamp().count();

    // Get thread info for labeling
    std::shared_ptr<ThreadInfo> threadInfo = sample->GetThreadInfo();

    // Create labelset for this sample (includes thread name if available)
    ddog_prof_LabelSetId labelsetId = CreateLabelSet(_sampleLabels, threadInfo);

    // Add sample to aggregator with labels
    if (!_aggregator->AddSample(locationIds, sampleValues, timestampNs, labelsetId)) {
        LogOnce(Error, "Failed to add sample to aggregator: ", _aggregator->GetLastError());
        return false;
    }

    return true;
}

bool ProfileExporter::Export(bool lastCall)
{
    if (!_initialized) {
        Log::Error("ProfileExporter::Export() called but not initialized");
        return false;
    }

    // Clear per-export caches since location IDs become invalid after profile reset
    OnExportStart();

    // Serialize the profile using the aggregator
    auto currentTime = std::chrono::system_clock::now();
    auto startMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        _profileStartTime.time_since_epoch()).count();
    auto endMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        currentTime.time_since_epoch()).count();

    auto encodedProfile = _aggregator->Serialize(startMs, endMs);
    if (!encodedProfile) {
        Log::Error("Failed to serialize profile: ", _aggregator->GetLastError());
        return false;
    }

    // Write debug pprof file if enabled
    if (_debugPprofFileWritingEnabled && !_debugPprofPrefix.empty()) {
        if (!WritePprofFile(encodedProfile)) {
            Log::Warn("Failed to write debug pprof file (continuing with export)");
        }
    }

    // Export profile to backend if enabled
    bool exportSuccess = true;
    if (_exportEnabled && _exporter.inner) {
        exportSuccess = ExportProfile(encodedProfile, _currentExportId);
        if (!exportSuccess) {
            Log::Error("Failed to export profile to backend");
            // Continue with cleanup even if export failed
        }
    }

    // Get profile bytes for logging before export consumes it
    // TODO: if this call is expensive, get it from ExportProfile call
    auto bytesResult = ddog_prof_EncodedProfile_bytes(encodedProfile);
    size_t profileSize = 0;
    if (bytesResult.tag == DDOG_PROF_RESULT_BYTE_SLICE_OK_BYTE_SLICE) {
        profileSize = bytesResult.ok.len;
    }

    // Calculate profile duration
    auto profileDurationMs = endMs - startMs;

    // For now, just log with current export ID and system info
    if (lastCall) {
        Log::Info("Export last profile #", _currentExportId,
                  ", Process ID: ", _processId,
                  ", Runtime ID: ", _runtimeId,
                  ", Profile duration: ", profileDurationMs, "ms",
                  ", Profile buffer size: ", profileSize, " bytes",
                  ", Persistent symbol cache size: ", _persistentSymbolCache.size());
    }
    else {
        Log::Info("Export profile #", _currentExportId,
                  ", Process ID: ", _processId,
                  ", Runtime ID: ", _runtimeId,
                  ", Profile duration: ", profileDurationMs, "ms",
                  ", Profile buffer size: ", profileSize, " bytes",
                  ", Persistent symbol cache size: ", _persistentSymbolCache.size());
    }

    // Clean up encoded profile
    ddog_prof_EncodedProfile_drop(encodedProfile);

    // Reset the aggregator for next collection cycle
    _aggregator->Reset();

    // Re-intern sample labels since they become invalid after profile reset
    if (!InternSampleLabels(_sampleLabels)) {
        Log::Error("Failed to re-intern sample labels after reset");
        // Continue anyway - this will cause Add() calls to fail but won't crash
    }

    // Reset the profile start time for the next collection cycle
    _profileStartTime = currentTime;

    // Increment export ID for next export
    _currentExportId++;

    // Periodic cleanup for large persistent cache
    if (_currentExportId % CACHE_CLEANUP_THRESHOLD == 0) {
        CleanupUnusedCacheEntries(_currentExportId);
    }

    return true;
}

std::string ProfileExporter::ComputeRuntimeId()
{
    // Generate a unique runtime ID using UUID
    ddprof::Uuid uuid;
    return uuid.to_string();
}

std::optional<ddog_prof_LocationId> ProfileExporter::InternLocation(uint64_t address)
{
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

    // Intern the mapping using the cached symbol info (module name and build ID)
    std::optional<ddog_prof_MappingId> mappingIdOpt;
    if (symbolInfo.ModuleNameId.value != 0 || symbolInfo.BuildIdId.value != 0) {
        mappingIdOpt = InternMapping(symbolInfo, profile);
        // Note: We continue even if mapping creation fails - location can exist without mapping
    }

    // Intern the function using the cached symbol info
    // todo: we could have a cache that has start / end addresses of the function (cf ddprof)
    auto functionIdOpt = InternFunction(symbolInfo, profile);
    if (!functionIdOpt.has_value()) {
        LogOnce(Error, "Failed to intern function for address 0x", std::hex, address, std::dec);
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
            address,
            static_cast<int64_t>(symbolInfo.lineNumber)
        );
    } else {
        // Create location without mapping (fallback)
        locationResult = ddog_prof_Profile_intern_location(
            profile,
            functionIdOpt.value(),
            address,
            static_cast<int64_t>(symbolInfo.lineNumber)
        );
    }

    if (locationResult.tag == DDOG_PROF_LOCATION_ID_RESULT_OK_GENERATIONAL_ID_LOCATION_ID)
    {
        // Cache the result for this export only
        _currentExportLocationCache[address] = locationResult.ok;
        return locationResult.ok;
    }

    LogOnce(Error, "Failed to intern location for address 0x", std::hex, address, std::dec, " (tag: ", locationResult.tag, ")");
    return std::nullopt;
}


std::optional<ddog_prof_MappingId> ProfileExporter::InternMapping(const CachedSymbolInfo& symbolInfo, ddog_prof_Profile* profile)
{
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
        auto moduleNameResult = ddog_prof_Profile_intern_managed_string(profile, symbolInfo.ModuleNameId);
        if (moduleNameResult.tag == DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
            moduleNameStringId = moduleNameResult.ok;
        } else {
            LogOnce(Error, "InternMapping: Failed to intern module name (tag: ", moduleNameResult.tag, ")");
        }
    }

    // Intern build ID if available
    if (symbolInfo.BuildIdId.value != 0) {
        auto buildIdResult = ddog_prof_Profile_intern_managed_string(profile, symbolInfo.BuildIdId);
        if (buildIdResult.tag == DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
            buildIdStringId = buildIdResult.ok;
        } else {
            LogOnce(Error, "InternMapping: Failed to intern build ID (tag: ", buildIdResult.tag, ")");
        }
    }

    // Create mapping with module information including memory range
    // The memory range enables automatic association of locations with this mapping
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

    LogOnce(Error, "InternMapping: Failed to intern mapping (tag: ", mappingResult.tag, ")");
    return std::nullopt;
}

static bool s_hasLoggedInternFunctionNameError = false;

std::optional<ddog_prof_FunctionId> ProfileExporter::InternFunction(const CachedSymbolInfo& symbolInfo, ddog_prof_Profile* profile)
{
    // Convert ManagedStringId to StringId (now using shared string storage)
    auto nameResult = ddog_prof_Profile_intern_managed_string(profile, symbolInfo.FunctionNameId);
    if (nameResult.tag != DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID)
    {
        if (!s_hasLoggedInternFunctionNameError) {
            s_hasLoggedInternFunctionNameError = true; // Log only once

            // Try to get the actual string to see what we're trying to intern
            std::string functionName;
            auto functionNameWrapper = ddog_prof_ManagedStringStorage_get_string(_stringStorage, symbolInfo.FunctionNameId);
            if (functionNameWrapper.tag == DDOG_STRING_WRAPPER_RESULT_OK) {
                functionName = std::string(reinterpret_cast<const char*>(functionNameWrapper.ok.message.ptr), functionNameWrapper.ok.message.len);
                ddog_StringWrapper_drop(&functionNameWrapper.ok);
            }

            if (functionName.empty()) {
                Log::Error("Failed to intern function name (tag: ", nameResult.tag, ")");
            }
            else {
                Log::Error("Failed to intern function name '", functionName ,"' (tag: ", nameResult.tag, ")");
            }
        }
        return std::nullopt;
    }
    auto filenameResult = ddog_prof_Profile_intern_managed_string(profile, symbolInfo.FileNameId);
    if (filenameResult.tag != DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID)
    {
        LogOnce(Error, "InternFunction: Failed to intern filename (tag: ", filenameResult.tag, ")");
        return std::nullopt;
    }

    // Create function using StringId (use same string for name and system_name for now)
    // todo: this is not correct, we need to use the actual string ids
    auto functionResult = ddog_prof_Profile_intern_function(profile, nameResult.ok, nameResult.ok, filenameResult.ok);
    if (functionResult.tag == DDOG_PROF_FUNCTION_ID_RESULT_OK_GENERATIONAL_ID_FUNCTION_ID)
    {
        return functionResult.ok;
    }

    LogOnce(Error, "InternFunction: Failed to intern function (tag: ", functionResult.tag, ")");
    return std::nullopt;
}

void ProfileExporter::OnExportStart()
{
    // Clear per-export caches since location and mapping IDs become invalid after profile reset
    _currentExportLocationCache.clear();
    _currentExportMappingCache.clear();

    Log::Debug("Cleared location and mapping caches, keeping ", _persistentSymbolCache.size(), " persistent symbol entries");
}

void ProfileExporter::ClearCaches()
{
    // This method can now be used for emergency cleanup or testing
    _currentExportLocationCache.clear();
    _currentExportMappingCache.clear();
    _persistentSymbolCache.clear();

    Log::Debug("Cleared all caches");
}

void ProfileExporter::CleanupUnusedCacheEntries(uint32_t currentExportId)
{
    // Periodic cleanup for persistent symbol cache if it gets too large
    if (_persistentSymbolCache.size() > 10000) { // Max 10k entries
        Log::Warn("CleanupUnusedCacheEntries: Symbol cache size (", _persistentSymbolCache.size(), ") exceeds limit, consider cleanup");
        // Note: We could implement LRU cleanup here if needed, but for now just log
        // The symbol cache should generally stay reasonable in size since it's bounded by
        // the number of unique addresses in the process
    }
}

bool ProfileExporter::AddSingleTag(ddog_Vec_Tag& tags, std::string_view key, std::string_view value)
{
    ddog_CharSlice keySlice = to_CharSlice(key);
    ddog_CharSlice valueSlice = to_CharSlice(value);

    ddog_Vec_Tag_PushResult pushResult = ddog_Vec_Tag_push(&tags, keySlice, valueSlice);
    if (pushResult.tag == DDOG_VEC_TAG_PUSH_RESULT_ERR) {
        LogOnce(Error, "Failed to add tag: ", key, "=", value);
        return false;
    }

    return true;
}

bool ProfileExporter::PrepareStableTags(ddog_Vec_Tag& tags)
{
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
    if (GetCpuCores(logicalCores, physicalCores))
    {
        if (!AddSingleTag(tags, TAG_CPU_CORES_COUNT, std::to_string(physicalCores))) {
            return false;
        }
        if (!AddSingleTag(tags, TAG_CPU_LOGICAL_CORES_COUNT, std::to_string(logicalCores))) {
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
    while (GetGpuFromRegistry(device, driverDesc, driverVersion, driverDate, gpuName, gpuChip, gpuRam))
    {
        // no need to send GPU details if GPU cannot be identified
        if (driverDesc.empty() && gpuNameTag.empty())
        {
            continue;
        }

        driverDescTag = TAG_GPU_DRIVER_DESC_PREFIX + std::to_string(device);
        if (!AddSingleTag(tags, driverDescTag, driverDesc)) {
            return false;
        }

        driverVersionTag = TAG_GPU_DRIVER_VERSION_PREFIX + std::to_string(device);
        if (!AddSingleTag(tags, driverVersionTag, driverVersion)) {
            return false;
        }

        driverDateTag = TAG_GPU_DRIVER_DATE_PREFIX + std::to_string(device);
        if (!AddSingleTag(tags, driverDateTag, driverDate)) {
            return false;
        }

        gpuNameTag = TAG_GPU_NAME_PREFIX + std::to_string(device);
        if (!AddSingleTag(tags, gpuNameTag, gpuName)) {
            return false;
        }

        gpuChipTag = TAG_GPU_CHIP_PREFIX + std::to_string(device);
        if (!AddSingleTag(tags, gpuChipTag, gpuChip)) {
            return false;
        }

        gpuRamTag = TAG_GPU_RAM_PREFIX + std::to_string(device);
        if (!AddSingleTag(tags, gpuRamTag, std::to_string(gpuRam))) {
            return false;
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
    if (GetMemoryInfo(totalPhys, availPhys, memoryLoad))
    {
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

bool ProfileExporter::InternSampleLabels(SampleLabels& labels)
{
    // Get profile for interning operations
    ddog_prof_Profile* profile = _aggregator->GetProfile();
    if (profile == nullptr)
    {
        return false;
    }

    // Intern the process_id label key
    auto processIdKeyResult = ddog_prof_Profile_intern_string(profile, to_CharSlice(LABEL_PROCESS_ID));
    if (processIdKeyResult.tag != DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
        LogOnce(Error, "Failed to intern process_id label key (tag: ", processIdKeyResult.tag, ")");
        return false;
    }

    // Intern the process ID value
    std::string processIdStr = std::to_string(GetCurrentProcessId());
    auto processIdValueResult = ddog_prof_Profile_intern_string(profile, to_CharSlice(processIdStr));
    if (processIdValueResult.tag != DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
        LogOnce(Error, "Failed to intern process_id value (tag: ", processIdValueResult.tag, ")");
        return false;
    }

    // Create process_id label ID using the proper API function
    auto processIdLabelResult = ddog_prof_Profile_intern_label_str(profile, processIdKeyResult.ok, processIdValueResult.ok);
    if (processIdLabelResult.tag != DDOG_PROF_LABEL_ID_RESULT_OK_GENERATIONAL_ID_LABEL_ID) {
        LogOnce(Error, "Failed to intern process_id label (tag: ", processIdLabelResult.tag, ")");

        return false;
    }

    labels.processIdLabelId = processIdLabelResult.ok;
    labels.processIdValueId = processIdValueResult.ok;

    // Intern the thread_id label key (we'll create specific labels per thread later)
    auto threadIdKeyResult = ddog_prof_Profile_intern_string(profile, to_CharSlice(LABEL_THREAD_ID));
    if (threadIdKeyResult.tag != DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
        LogOnce(Error, "InternSampleLabels: Failed to intern thread_id label key (tag: ", threadIdKeyResult.tag, ")");
        return false;
    }
    // Store the thread ID label key ID (we'll use this to create per-thread labels)
    labels.threadIdKeyId = threadIdKeyResult.ok;

    // Intern the thread name value
    auto threadNameKeyResult = ddog_prof_Profile_intern_string(profile, to_CharSlice(LABEL_THREAD_NAME));
    if (threadNameKeyResult.tag != DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
        LogOnce(Error, "InternSampleLabels: Failed to intern thread_name label key (tag: ", threadNameKeyResult.tag, ")");
        return false;
    }
    // Store the thread name label key ID (we'll use this to create per-thread labels)
    labels.threadNameKeyId = threadNameKeyResult.ok;

    return true;
}

ddog_prof_LabelSetId ProfileExporter::CreateLabelSet(const SampleLabels& labels, std::shared_ptr<ThreadInfo> threadInfo)
{
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
        auto threadIdLabelResult = ddog_prof_Profile_intern_label_num(profile, labels.threadIdKeyId, threadId);
        if (threadIdLabelResult.tag == DDOG_PROF_LABEL_ID_RESULT_OK_GENERATIONAL_ID_LABEL_ID) {
            labelIdArray.push_back(threadIdLabelResult.ok);
        } else {
            Log::Error("CreateLabelSet: Failed to intern thread_id numeric label (tag: ", threadIdLabelResult.tag, ")");
        }

        std::string name;
        if (threadInfo->GetThreadName(name))
        {
            // Intern the thread name value
            auto threadNameValueResult = ddog_prof_Profile_intern_string(profile, to_CharSlice(name));
            if (threadNameValueResult.tag != DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
                LogOnce(Error, "Failed to intern thread_name value (tag: ", threadNameValueResult.tag, ")");
            }
            else
            {
                auto threadNameLabelResult = ddog_prof_Profile_intern_label_str(profile, labels.threadNameKeyId, threadNameValueResult.ok);
                if (threadNameLabelResult.tag != DDOG_PROF_LABEL_ID_RESULT_OK_GENERATIONAL_ID_LABEL_ID) {
                    LogOnce(Error, "Failed to intern thread_name label (tag: ", threadNameLabelResult.tag, ")");
                }
                else
                {
                    labelIdArray.push_back(threadNameLabelResult.ok);
                }
            }
        }
    }

    ddog_prof_Slice_LabelId labelSlice = {
        .ptr = labelIdArray.data(),
        .len = labelIdArray.size()
    };

    auto labelsetResult = ddog_prof_Profile_intern_labelset(profile, labelSlice);
    if (labelsetResult.tag != DDOG_PROF_LABEL_SET_ID_RESULT_OK_GENERATIONAL_ID_LABEL_SET_ID) {
        LogOnce(Error, "Failed to intern labelset (tag: ", labelsetResult.tag, ")");
        return ddog_prof_LabelSetId{};
    }

    return labelsetResult.ok;
}

uint32_t ProfileExporter::GetCurrentProcessId()
{
    return ::GetCurrentProcessId();
}

bool ProfileExporter::WritePprofFile(const ddog_prof_EncodedProfile* encodedProfile)
{
    if (!encodedProfile || _debugPprofPrefix.empty()) {
        return false;
    }

    // Use current time since we can't get the start time directly from encoded profile
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    ddog_Timespec startTime = { static_cast<int64_t>(time_t_now), 0 };

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

bool ProfileExporter::CreatePprofFile(const ddog_Timespec& startTime, int* fd)
{
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
    // Note: .lz4 extension is not added so the tools could easily open the files on Windows thanks to file association
    char filename[MAX_PATH];
    std::snprintf(filename, std::size(filename), "%s%s.lz4.pprof", _debugPprofPrefix.c_str(), time_start);

    Log::Debug("Writing pprof to file ", filename);

    // Create file with read/write permissions for user only
    constexpr int read_write_user_only = _S_IREAD | _S_IWRITE;
    if (_sopen_s(fd, filename, _O_CREAT | _O_RDWR | _O_BINARY, _SH_DENYNO, read_write_user_only) != 0) {
        char errorBuffer[256];
        strerror_s(errorBuffer, sizeof(errorBuffer), errno);
        Log::Error("Failed to create pprof file: '", filename, "'  - Error: ", errorBuffer);
        return false;
    }

    return true;
}

bool ProfileExporter::WriteProfileToFile(const ddog_prof_EncodedProfile* encodedProfile, int fd)
{
    // Get the profile bytes using the new API
    auto bytesResult = ddog_prof_EncodedProfile_bytes(const_cast<ddog_prof_EncodedProfile*>(encodedProfile));
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
        Log::Error("Partial write to file: wrote ", bytes_written, " bytes out of ", buffer.len);
        return false;
    }

    Log::Info("Successfully wrote ", bytes_written, " bytes to pprof file");
    return true;
}

std::string ProfileExporter::BuildAgentEndpoint()
{
    // handle "with agent" case
    auto url = _pConfiguration->GetAgentUrl();

    if (url.empty())
    {
        // Agent mode
        const std::string& namePipeName = _pConfiguration->GetNamedPipeName();
        if (!namePipeName.empty())
        {
            url = R"(windows:\\.\pipe\)" + namePipeName;
        }

        if (url.empty())
        {
            // Use default HTTP endpoint
            std::stringstream oss;
            oss << "http://" << _pConfiguration->GetAgentHost() << ":" << _pConfiguration->GetAgentPort();
            url = oss.str();
        }
    }

    return url;
}

// Export functionality implementation

bool ProfileExporter::InitializeExporter()
{
    // Build export URL
    if (!BuildExportUrl()) {
        return false;
    }

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
    if (!PrepareStableTags(stableTags))
    {
        ddog_Vec_Tag_drop(stableTags);
        return false;
    }

    // Add service information from configuration if available
    if (_pConfiguration) {
        auto service = _pConfiguration->GetServiceName();
        auto environment = _pConfiguration->GetEnvironment();
        auto version = _pConfiguration->GetVersion();
        auto hostname = _pConfiguration->GetHostname();

        if (!service.empty() && !AddSingleTag(stableTags, "service", service)) {
            ddog_Vec_Tag_drop(stableTags);
            return false;
        }
        if (!environment.empty() && !AddSingleTag(stableTags, "env", environment)) {
            ddog_Vec_Tag_drop(stableTags);
            return false;
        }
        if (!version.empty() && !AddSingleTag(stableTags, "version", version)) {
            ddog_Vec_Tag_drop(stableTags);
            return false;
        }
        if (!hostname.empty() && !AddSingleTag(stableTags, "host", hostname)) {
            ddog_Vec_Tag_drop(stableTags);
            return false;
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
        to_CharSlice(_kProfilerUserAgent), // user_agent
        to_CharSlice(_kProfilerVersion),   // profiler_version
        to_CharSlice("native"),           // family
        &stableTags,
        endpoint
    );

    ddog_Vec_Tag_drop(stableTags);

    if (exporterResult.tag != DDOG_PROF_PROFILE_EXPORTER_RESULT_OK_HANDLE_PROFILE_EXPORTER) {
        _lastError = "Failed to create exporter";
        if (exporterResult.tag == DDOG_PROF_PROFILE_EXPORTER_RESULT_ERR_HANDLE_PROFILE_EXPORTER) {
            // Extract error message
            auto& error = exporterResult.err;
            std::string errorMessage = std::string(reinterpret_cast<const char*>(error.message.ptr), error.message.len);
            _lastError += ": " + errorMessage;
            Log::Error("Failed to create exporter: ", errorMessage, " (URL: ", _exportUrl, ", mode: ", (_agentMode ? "agent" : "agentless"), ")");
            ddog_Error_drop(&exporterResult.err);
        } else {
            Log::Error("Failed to create exporter with unknown error (tag: ", exporterResult.tag, ")");
        }
        return false;
    }

    _exporter = exporterResult.ok;

    // Set timeout
    auto timeoutResult = ddog_prof_Exporter_set_timeout(&_exporter, EXPORT_TIMEOUT_MS);
    if (timeoutResult.tag == DDOG_VOID_RESULT_ERR) {
        _lastError = "Failed to set exporter timeout";
        auto& error = timeoutResult.err;
        std::string errorMessage = std::string(reinterpret_cast<const char*>(error.message.ptr), error.message.len);
        _lastError += ": " + errorMessage;
        Log::Error("Failed to set timeout: ", errorMessage);
        ddog_Error_drop(&timeoutResult.err);
        CleanupExporter();
        return false;
    }

    return true;
}

bool ProfileExporter::BuildExportUrl()
{
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

        Log::Info("Agentless mode, using site: ", _exportUrl, ", API key length: ", _apiKey.length());
    }

    Log::Info("Using URL: ", _exportUrl, " (mode: ", (_agentMode ? "agent" : "agentless"), ")");

    return true;
}

bool ProfileExporter::CreateExporterEndpoint(ddog_prof_Endpoint& endpoint)
{
    ddog_CharSlice urlSlice = to_CharSlice(_exportUrl);

    if (_agentMode) {
        endpoint = ddog_prof_Endpoint_agent(urlSlice);
    } else {
        if (_apiKey.empty()) {
            _lastError = "MIssing API key for agentless mode";
            return false;
        }
        ddog_CharSlice apiKeySlice = to_CharSlice(_apiKey);
        endpoint = ddog_prof_Endpoint_agentless(urlSlice, apiKeySlice);
    }

    return true;
}

bool ProfileExporter::ExportProfile(const ddog_prof_EncodedProfile* encodedProfile, uint32_t profileSeq)
{
    if (!_exporter.inner || !encodedProfile) {
        _lastError = "Exporter not initialized or invalid profile";
        return false;
    }

    // Prepare additional tags (per-export metadata)
    ddog_Vec_Tag additionalTags = ddog_Vec_Tag_new();
    if (!PrepareAdditionalTags(additionalTags, profileSeq)) {
        ddog_Vec_Tag_drop(additionalTags);
        return false;
    }

    // Get profile bytes for file creation
    auto bytesResult = ddog_prof_EncodedProfile_bytes(const_cast<ddog_prof_EncodedProfile*>(encodedProfile));
    if (bytesResult.tag != DDOG_PROF_RESULT_BYTE_SLICE_OK_BYTE_SLICE) {
        _lastError = "Failed to get profile bytes";
        ddog_Vec_Tag_drop(additionalTags);
        return false;
    }

    // Build request - time information is now embedded in the EncodedProfile
    auto requestResult = ddog_prof_Exporter_Request_build(
        &_exporter,
        const_cast<ddog_prof_EncodedProfile*>(encodedProfile), // profile
        ddog_prof_Exporter_Slice_File_empty(), // files_to_compress_and_export
        ddog_prof_Exporter_Slice_File_empty(), // files_to_compress_and_export
        &additionalTags,                        // optional_additional_tags
        nullptr,                                // optional_internal_metadata_json
        nullptr                                 // optional_info_json
    );

    ddog_Vec_Tag_drop(additionalTags);

    if (requestResult.tag != DDOG_PROF_REQUEST_RESULT_OK_HANDLE_REQUEST) {
        _lastError = "Failed to build export request";
        if (requestResult.tag == DDOG_PROF_REQUEST_RESULT_ERR_HANDLE_REQUEST) {
            auto& error = requestResult.err;
            std::string errorMessage = std::string(reinterpret_cast<const char*>(error.message.ptr), error.message.len);
            _lastError += ": " + errorMessage;
            Log::Error("Request build failed: ", errorMessage);

            // TODO: check if we need to drop it all the time
            //       i.e. call it before returning false
            ddog_Error_drop(&requestResult.err);
        } else {
            Log::Error("Request build failed with unknown error (tag: ", requestResult.tag, ")");
        }
        return false;
    }

    ddog_prof_Request request = requestResult.ok;

    // Send request
    auto sendResult = ddog_prof_Exporter_send(&_exporter, &request, nullptr);

    // Request is consumed by send, so we don't need to drop it

    if (sendResult.tag == DDOG_PROF_RESULT_HTTP_STATUS_ERR_HTTP_STATUS) {
        _consecutiveErrors++;
        _lastError = "Failed to send profile";
        auto& error = sendResult.err;
        std::string errorMessage = std::string(reinterpret_cast<const char*>(error.message.ptr), error.message.len);
        _lastError += ": " + errorMessage;
        ddog_Error_drop(&sendResult.err);

        Log::Error("Send profile failed: ", errorMessage, " (consecutive errors: ", _consecutiveErrors, "/", MAX_CONSECUTIVE_ERRORS, ", URL: ", _exportUrl, ")");

        // Don't fail completely unless we have too many consecutive errors
        return _consecutiveErrors < MAX_CONSECUTIVE_ERRORS;
    }

    // Success - reset error counter
    _consecutiveErrors = 0;

    // Check HTTP response code
    uint16_t responseCode = sendResult.ok.code;
    bool responseOk = CheckExportResponse(responseCode);

    Log::Info("Successfully sent profile, HTTP ", responseCode, " (size: ", bytesResult.ok.len, " bytes)");

    return responseOk;
}

bool ProfileExporter::PrepareAdditionalTags(ddog_Vec_Tag& tags, uint32_t profileSeq)
{
    // Add profile sequence number
    if (!AddSingleTag(tags, TAG_PROFILE_SEQ, std::to_string(profileSeq))) {
        return false;
    }

    // Add process ID as additional tag
    if (!AddSingleTag(tags, "pid", std::to_string(_processId))) {
        return false;
    }

    return true;
}

bool ProfileExporter::CheckExportResponse(uint16_t responseCode)
{
    constexpr uint16_t HTTP_OK = 200;
    constexpr uint16_t HTTP_ACCEPTED = 202;
    constexpr uint16_t HTTP_MULTIPLE_CHOICES = 300;
    constexpr uint16_t HTTP_FORBIDDEN = 403;
    constexpr uint16_t HTTP_NOT_FOUND = 404;
    constexpr uint16_t HTTP_GATEWAY_TIMEOUT = 504;

    if (responseCode >= HTTP_OK && responseCode < HTTP_MULTIPLE_CHOICES) {
        // Success range
        if (responseCode == HTTP_ACCEPTED) {
            // HTTP 202 Accepted is normal for profile uploads
            return true;
        } else if (responseCode != HTTP_OK) {
            Log::Warn("Unexpected success code: ", responseCode);
        }
        return true;
    }

    // Handle specific error codes
    switch (responseCode) {
        case HTTP_GATEWAY_TIMEOUT:
            Log::Warn("Timeout (504), dropping profile");
            return true; // Don't treat as failure, just drop this profile

        case HTTP_FORBIDDEN:
            Log::Error("Forbidden (403), check API key");
            return false; // This is a configuration error

        case HTTP_NOT_FOUND:
            Log::Error("Not found (404), profiles not accepted");
            return false; // This is a configuration error

        default:
            Log::Warn("HTTP error ", responseCode, " (continuing)");
            return true; // Continue profiling despite HTTP error
    }
}

void ProfileExporter::CleanupExporter()
{
    if (_exporter.inner) {
        ddog_prof_Exporter_drop(&_exporter);
        _exporter.inner = nullptr;
    }
}
