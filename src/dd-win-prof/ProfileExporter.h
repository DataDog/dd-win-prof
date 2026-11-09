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
#include "internal/dd-win-rum-private.h"
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
  void SetDebugPprofFileWritingEnabled(bool v) { _debugPprofFileWritingEnabled = v; }
  bool IsDebugPprofFileWritingEnabled() const { return _debugPprofFileWritingEnabled; }
  void SetDebugPprofPrefix(const std::string& p) { _debugPprofPrefix = p; }
  const std::string& GetDebugPprofPrefix() const { return _debugPprofPrefix; }

 private:
  bool InternSampleLabels();
  std::optional<pa_id> InternLocation(uint64_t);
  std::optional<pa_id> CreateLabelSet(
      const std::shared_ptr<ThreadInfo>&, const RumViewContext&, UiHangSampleKind
  );
  bool ReportError(ProfilingErrorCode, bool);
  bool DeliverProfilingData(ProfilingData);
  std::string CollectRumRecordsJson(bool discard);
  void WriteDebugFiles(const std::vector<uint8_t>&, const std::string&, int64_t);
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
  // Owned symbol metadata survives profile resets, not Rust-managed strings/IDs.
  std::unordered_map<uint64_t, CachedSymbolInfo> _persistentSymbolCache;
  std::unordered_map<uint64_t, pa_id> _currentExportLocationCache;
  std::vector<pa_id> _locationScratch;
  std::string _threadNameScratch;
  pa_label _processLabel{}, _uiHangTrueLabel{}, _uiHangFalseLabel{};
  pa_id _threadKey = 0, _nameKey = 0, _viewKey = 0, _endpointKey = 0;
  uint32_t _currentExportId = 0;
  std::chrono::system_clock::time_point _profileStartTime;
  std::vector<RumViewRecord> _viewRecordsBuffer;
  std::vector<RumSessionRecord> _sessionRecordsBuffer;
};
