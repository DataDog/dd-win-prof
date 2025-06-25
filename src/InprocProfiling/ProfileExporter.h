// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "pch.h"
#include "Configuration.h"
#include "Sample.h"
#include "PprofAggregator.h"
#include "Symbolication.h"
#include <unordered_map>
#include <memory>
#include <string>
#include <string_view>
#include <optional>
#include <span>

#include "datadog/profiling.h"


class ProfileExporter
{
public:
    ProfileExporter(Configuration* pConfiguration, std::span<const SampleValueType> sampleTypeDefinitions);
    ~ProfileExporter();
    bool Initialize();
    bool Add(std::shared_ptr<Sample> const& sample);
    bool Export(bool lastCall = false);
    void Cleanup(bool skipExporterCleanup = false);  // Explicit cleanup with option to skip exporter cleanup

    // Check if properly initialized
    bool IsInitialized() const { return _initialized; }
    const std::string& GetLastError() const { return _lastError; }

    // Configuration methods for debug file writing
    // These methods allow enabling/disabling writing of pprof files to disk for debugging purposes.
    // When enabled, each exported profile will be written to a timestamped .pprof.lz4 file.
    // This feature is OFF by default and should only be enabled for debugging or development.
    //
    // Example usage:
    //   exporter.SetDebugPprofFileWritingEnabled(true);
    //   exporter.SetDebugPprofPrefix("C:\\temp\\debug_profiles\\myapp_");
    // This would create files like: C:\temp\debug_profiles\myapp_20241201T143022Z.pprof.lz4
    void SetDebugPprofFileWritingEnabled(bool enabled) { _debugPprofFileWritingEnabled = enabled; }
    bool IsDebugPprofFileWritingEnabled() const { return _debugPprofFileWritingEnabled; }
    void SetDebugPprofPrefix(const std::string& prefix) { _debugPprofPrefix = prefix; }
    const std::string& GetDebugPprofPrefix() const { return _debugPprofPrefix; }

    // Export tags (stable metadata set at export time)
    bool PrepareStableTags(ddog_Vec_Tag& tags, uint32_t profileSeq);
    bool AddSingleTag(ddog_Vec_Tag& tags, std::string_view key, std::string_view value);

    // Export functionality
    bool InitializeExporter();
    bool CreateExporterEndpoint(ddog_prof_Endpoint& endpoint);
    bool BuildExportUrl();
    bool ExportProfile(const ddog_prof_EncodedProfile* encodedProfile, uint32_t profileSeq);
    bool PrepareAdditionalTags(ddog_Vec_Tag& tags, uint32_t profileSeq);
    bool CheckExportResponse(uint16_t responseCode);
    void CleanupExporter();

private:
    // Helper methods for location/function management
    std::optional<ddog_prof_LocationId> InternLocation(uint64_t address);
    std::optional<ddog_prof_FunctionId> InternFunction(const CachedSymbolInfo& symbolInfo, ddog_prof_Profile* profile);

    // Sample labels (per-sample metadata that gets interned)
    struct SampleLabels {
        ddog_prof_LabelId processIdLabelId;
        ddog_prof_StringId processIdValueId;
        ddog_prof_StringId threadIdKeyId;  // String ID for thread_id key (used to create numeric labels per thread)
        ddog_prof_StringId threadNameKeyId;  // String ID for thread_name key
        // Add more label IDs as needed
    };

    bool InternSampleLabels(SampleLabels& labels);
    ddog_prof_LabelSetId CreateLabelSet(const SampleLabels& labels, std::shared_ptr<ThreadInfo> threadInfo);  // Updated to take threadInfo

    // Debug file writing methods
    bool WritePprofFile(const ddog_prof_EncodedProfile* encodedProfile);
    bool CreatePprofFile(const ddog_Timespec& startTime, int* fd);
    bool WriteProfileToFile(const ddog_prof_EncodedProfile* encodedProfile, int fd);

    // System information helpers
    uint32_t GetCurrentProcessId();

    // Tag key constants (for stable export tags)
    static constexpr const char* TAG_RUNTIME_ID = "runtime-id";
    static constexpr const char* TAG_PROFILE_SEQ = "profile_seq";

    // Label key constants (for per-sample labels)
    static constexpr const char* LABEL_PROCESS_ID = "process_id";
    static constexpr const char* LABEL_THREAD_ID = "thread id";  // New label constant for thread ID
    static constexpr const char* LABEL_THREAD_NAME = "thread_name";  // New label constant for thread name if any

    // Cache management
    void ClearCaches();
    void CleanupUnusedCacheEntries(uint32_t currentExportId);
    void OnExportStart(); // New method to manage cache lifecycle

    // Utility methods
    std::string ComputeRuntimeId();
    std::string BuildAgentEndpoint();

    // Configuration and state
    Configuration* _pConfiguration;
    std::vector<SampleValueType> _sampleTypeDefinitions;
    std::string _runtimeId;
    uint32_t _processId;
    bool _initialized;
    std::string _lastError;

    // Debug pprof file writing configuration
    bool _debugPprofFileWritingEnabled;
    std::string _debugPprofPrefix;

    // Export configuration and state
    bool _exportEnabled;
    ddog_prof_ProfileExporter _exporter;  // Value type, not pointer
    std::string _exportUrl;
    std::string _apiKey;
    bool _agentMode;  // true for agent, false for agentless
    uint32_t _consecutiveErrors;
    static constexpr uint32_t MAX_CONSECUTIVE_ERRORS = 3;
    static constexpr int EXPORT_TIMEOUT_MS = 10000;

    // libdatadog components
    ddog_prof_ManagedStringStorage _stringStorage;
    std::unique_ptr<Symbolication> _symbolication;
    std::unique_ptr<InprocProfiling::PprofAggregator> _aggregator;

    // Cache structures
    struct LocationCacheEntry {
        ddog_prof_LocationId locationId;
        uint32_t lastUsedExportId;
    };

    // Persistent cache - keeps expensive symbolication results across exports
    std::unordered_map<uint64_t, CachedSymbolInfo> _persistentSymbolCache;

    // Per-export cache - cleared on each export since location IDs become invalid after profile reset
    std::unordered_map<uint64_t, ddog_prof_LocationId> _currentExportLocationCache;

    // Export tracking
    uint32_t _currentExportId;
    std::chrono::time_point<std::chrono::system_clock> _profileStartTime;

    // number of sent profiles before cleaning up caches
    static constexpr uint32_t CACHE_CLEANUP_THRESHOLD = 100;

    // Interned sample labels (reused across samples)
    SampleLabels _sampleLabels;
};

