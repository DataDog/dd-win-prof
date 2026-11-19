// Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

#include "Configuration.h"
#include "PprofAggregator.h"
#include "RumContext.h"
#include "Sample.h"
#include "Symbolication.h"
#include "TagsHelper.h"
#include "internal/dd-win-rum-private.h"
#include "profile_exporter.h"
class ProfileExporter {
 public:
  ProfileExporter(
      Configuration*,
      std::span<const SampleValueType>,
      IRumRecordProvider* = nullptr,
      ProfilingDataCallback = {},
      dd_win_prof::ProfileEncodingApi = {}
  );
  ~ProfileExporter();
  bool Initialize();
  bool Add(std::shared_ptr<Sample> const&);
  bool Export(bool lastCall = false);
  void Cleanup(bool skipExporterCleanup = false);
  bool IsInitialized() const { return _initialized; }
  void SetRumApplicationId(const std::string& id) { _rumApplicationId = id; }
  static std::string SerializeRumRecordsToJson(
      const std::vector<RumViewRecord>&, const std::vector<RumSessionRecord>&
  );
  static void EscapeJsonString(std::ostream&, const std::string&);
  static tags BuildRumTags(
      const std::string& applicationId,
      const std::vector<std::string>& sessionIds,
      const std::vector<std::string>& viewIds = {}
  );
  void SetDebugPprofFileWritingEnabled(bool v) { _debugPprofFileWritingEnabled = v; }
  bool IsDebugPprofFileWritingEnabled() const { return _debugPprofFileWritingEnabled; }
  void SetDebugPprofPrefix(const std::string& p) { _debugPprofPrefix = p; }
  const std::string& GetDebugPprofPrefix() const { return _debugPprofPrefix; }

 private:
  bool InitializeHttpExporter();
  bool SendHttp(
      const std::vector<uint8_t>&,
      const std::string&,
      int64_t,
      int64_t,
      const std::vector<std::string>&,
      const std::vector<std::string>&
  );
  bool InternSampleLabels();
  std::optional<ddc_profile_id> InternLocation(uint64_t);
  std::optional<ddc_profile_id> CreateLabelSet(
      const std::shared_ptr<ThreadInfo>&, const RumViewContext&, UiHangSampleKind
  );
  bool ReportError(ProfilingErrorCode, bool);
  bool DeliverProfilingData(ProfilingData);
  std::string CollectRumRecordsJson(
      bool discard,
      std::vector<std::string>& sessionIds,
      std::vector<std::string>& viewIds
  );
  void WriteDebugFiles(const std::vector<uint8_t>&, const std::string&, int64_t);
  std::unique_ptr<pe_exporter, decltype(&pe_exporter_destroy)> _httpExporter{
      nullptr, pe_exporter_destroy
  };
  tags _httpTags;
  Configuration* _pConfiguration;
  std::vector<SampleValueType> _sampleTypeDefinitions;
  ProfilingDataCallback _profilingDataCallback;
  dd_win_prof::ProfileEncodingApi _encodingApi;
  IRumRecordProvider* _pRumRecordProvider;
  bool _initialized = false;
  bool _exportEnabled = false;
  bool _debugPprofFileWritingEnabled = false;
  std::string _debugPprofPrefix, _rumApplicationId;
  std::unique_ptr<Symbolication> _symbolication;
  std::unique_ptr<dd_win_prof::PprofAggregator> _aggregator;
  // Owned symbol metadata survives resets; profile IDs are generation-local.
  std::unordered_map<uint64_t, CachedSymbolInfo> _persistentSymbolCache;
  std::unordered_map<uint64_t, ddc_profile_id> _currentExportLocationCache;
  std::vector<ddc_profile_id> _locationScratch;
  std::string _threadNameScratch;
  ddc_profile_label _processLabel{}, _uiHangTrueLabel{}, _uiHangFalseLabel{};
  ddc_profile_id _threadKey = 0, _nameKey = 0, _viewKey = 0, _endpointKey = 0;
  uint32_t _currentExportId = 0;
  unsigned _consecutiveErrors = 0;
  unsigned _resetsSinceStringClear = 0;
  std::chrono::system_clock::time_point _profileStartTime;
  std::vector<RumViewRecord> _viewRecordsBuffer;
  std::vector<RumSessionRecord> _sessionRecordsBuffer;
};
