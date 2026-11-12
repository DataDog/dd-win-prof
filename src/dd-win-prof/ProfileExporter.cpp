// Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0
#include "ProfileExporter.h"

#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "Log.h"
#include "OsSpecificApi.h"
#include "Uuid.h"
#include "version.h"
namespace {
int64_t ToUnixTimeMs(std::chrono::system_clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch())
      .count();
}
pa_status String(pa_profile* p, std::string_view s, pa_id& id) {
  return pa_intern_string(p, s.data(), s.size(), &id);
}
}  // namespace
ProfileExporter::ProfileExporter(
    Configuration* config,
    std::span<const SampleValueType> types,
    IRumRecordProvider* rum,
    ProfilingDataCallback callback,
    dd_win_prof::ProfileEncodingApi api
)
    : _pConfiguration(config),
      _sampleTypeDefinitions(types.begin(), types.end()),
      _profilingDataCallback(std::move(callback)),
      _encodingApi(api),
      _pRumRecordProvider(rum) {
  if (config) {
    auto dir = config->GetProfilesOutputDirectory();
    if (!dir.empty()) {
      fs::create_directories(dir);
      _debugPprofFileWritingEnabled = true;
      _debugPprofPrefix = (dir / "profile_").string();
    }
    _exportEnabled = config->IsExportEnabled(
        _profilingDataCallback ? ProfileDeliveryMode::Callback
                               : ProfileDeliveryMode::Http
    );
  }
}
ProfileExporter::~ProfileExporter() { Cleanup(); }
void ProfileExporter::Cleanup(bool) {
  _initialized = false;
  _httpExporter.reset();
  _httpTags.clear();
  _consecutiveErrors = 0;
  _aggregator.reset();
  _symbolication.reset();
  _persistentSymbolCache.clear();
  _currentExportLocationCache.clear();
  _profilingDataCallback = {};
}
bool ProfileExporter::Initialize() {
  if (_initialized) return true;
  try {
    if (_exportEnabled && !_profilingDataCallback && !InitializeHttpExporter())
      return false;
    _symbolication = std::make_unique<Symbolication>();
    if (!_symbolication->Initialize(
            _pConfiguration && _pConfiguration->AreCallstacksSymbolized()
        ))
      return false;
    _aggregator = std::make_unique<dd_win_prof::PprofAggregator>(
        _sampleTypeDefinitions, 10, _encodingApi
    );
    if (!_aggregator->IsInitialized() || !InternSampleLabels()) return false;
    _profileStartTime = std::chrono::system_clock::now();
    _initialized = true;
    return true;
  } catch (const std::exception& e) {
    Log::Error("Profile initialization failed: ", e.what());
    return false;
  }
}
namespace {
pe_slice Bytes(std::string_view s) { return {s.data(), s.size()}; }
}  // namespace
bool ProfileExporter::InitializeHttpExporter() {
  if (!_pConfiguration || !_pConfiguration->ValidateTransportConfig()) return false;
  const auto& c = *_pConfiguration;
  std::string endpoint = c.IsAgentless() ? c.GetSite() : c.GetAgentUrl();
  if (!c.IsAgentless() && endpoint.empty()) {
    if (c.GetAgentPort() < 1 || c.GetAgentPort() > 65535) return false;
    auto host = c.GetAgentHost();
    if (host.find(':') != std::string::npos && !host.starts_with("["))
      host = "[" + host + "]";
    endpoint = "http://" + host + ":" + std::to_string(c.GetAgentPort());
  }
  pe_config config{
      Bytes(endpoint),
      Bytes(c.GetApiKey()),
      Bytes(DLL_NAME),
      Bytes(PROFILER_VERSION_STRING),
      10000,
      c.IsAgentless()
  };
  pe_exporter* exporter = nullptr;
  pe_result result{};
  if (pe_exporter_create(&config, &exporter, &result) != PE_OK) {
    Log::Error(
        "Experimental HTTP exporter initialization failed: ",
        result.status,
        ", system error: ",
        result.system_error
    );
    return false;
  }
  _httpExporter.reset(exporter);
  _httpTags = {
      {"language", "native"},
      {"runtime-id", ddprof::Uuid().to_string()},
      {"profiler_version", PROFILER_VERSION_STRING},
      {"remote_symbols", "yes"},
      {"runtime_os", "windows"}
  };
  auto add = [&](std::string key, std::string value) {
    if (!value.empty()) _httpTags.emplace_back(std::move(key), std::move(value));
  };
  add("service", c.GetServiceName());
  add("env", c.GetEnvironment());
  add("version", c.GetVersion());
  add("host", c.GetHostname());
  using namespace OsSpecificApi;
  int physical = 0, logical = 0;
  if (GetCpuCores(physical, logical)) {
    add("number_of_cpu_cores", std::to_string(physical));
    add("number_of_logical_cores", std::to_string(logical));
  }
  add("cpu_vendor", GetCpuVendor());
  add("cpu_desc", GetCpuModel());
  add("cpu_arch", GetCpuArchitecture());
  std::string description, driverVersion, date, name, chip;
  uint64_t ram = 0;
  int device = 0;
  while (
      GetGpuFromRegistry(device, description, driverVersion, date, name, chip, ram)
  ) {
    const auto index = std::to_string(device++);
    if (description.empty() && name.empty()) continue;
    add("gpu_driver_desc_" + index, description);
    add("gpu_driver_version_" + index, driverVersion);
    add("gpu_driver_date_" + index, date);
    add("gpu_name_" + index, name);
    add("gpu_chip_" + index, chip);
    if (ram) add("gpu_ram_" + index, std::to_string(ram));
  }
  add("gpu_count", std::to_string(device));
  uint64_t total = 0, available = 0;
  uint32_t load = 0;
  if (GetMemoryInfo(total, available, load)) add("ram_size", std::to_string(total));
  _httpTags.insert(_httpTags.end(), c.GetUserTags().begin(), c.GetUserTags().end());
  return true;
}
bool ProfileExporter::SendHttp(
    const std::vector<uint8_t>& bytes,
    const std::string& rum,
    int64_t start,
    int64_t end
) {
  try {
    auto tags = _httpTags;
    tags.emplace_back("profile_seq", std::to_string(_currentExportId));
    tags.emplace_back("pid", std::to_string(::GetCurrentProcessId()));
    if (!_rumApplicationId.empty())
      tags.emplace_back("rum.application_id", _rumApplicationId);
    std::vector<pe_tag> borrowed;
    borrowed.reserve(tags.size());
    for (const auto& [key, value] : tags)
      borrowed.push_back({Bytes(key), Bytes(value)});
    pe_profile profile{
        {bytes.data(), bytes.size()},
        start,
        end,
        borrowed.data(),
        borrowed.size(),
        Bytes(rum),
        nullptr,
        0
    };
    pe_result result{};
    const auto status = pe_exporter_send(_httpExporter.get(), &profile, &result);
    if (status == PE_TRANSPORT) {
      ++_consecutiveErrors;
      Log::Warn("HTTP transport error: ", result.system_error);
      return _consecutiveErrors < 3;
    }
    if (status == PE_OK || status == PE_HTTP) {
      _consecutiveErrors = 0;
      if (result.http_status == 403 || result.http_status == 404) return false;
      if (status == PE_OK)
        Log::Info("Experimental HTTP export succeeded, HTTP: ", result.http_status);
      else
        Log::Warn("HTTP error ", result.http_status, " (continuing)");
      return true;
    }
    Log::Error("HTTP request preparation failed: ", result.status);
  } catch (const std::exception& e) {
    Log::Error("HTTP export failed: ", e.what());
  }
  return false;
}
bool ProfileExporter::InternSampleLabels() {
  auto* p = _aggregator->GetProfile();
  pa_id key = 0, value = 0;
  if (String(p, "process_id", key) != PA_OK ||
      String(p, std::to_string(::GetCurrentProcessId()), value) != PA_OK)
    return false;
  _processLabel = {};
  _processLabel.key = key;
  _processLabel.str = value;
  _uiHangTrueLabel = {};
  _uiHangFalseLabel = {};
  if (String(p, "UIHang", key) != PA_OK ||
      String(p, "true", _uiHangTrueLabel.str) != PA_OK ||
      String(p, "false", _uiHangFalseLabel.str) != PA_OK)
    return false;
  _uiHangTrueLabel.key = _uiHangFalseLabel.key = key;
  return String(p, "thread id", _threadKey) == PA_OK &&
         String(p, "thread_name", _nameKey) == PA_OK &&
         String(p, "rum.view_id", _viewKey) == PA_OK &&
         String(p, "trace endpoint", _endpointKey) == PA_OK;
}
std::optional<pa_id> ProfileExporter::InternLocation(uint64_t address) {
  auto found = _currentExportLocationCache.find(address);
  if (found != _currentExportLocationCache.end()) return found->second;
  auto symbol = _persistentSymbolCache.find(address);
  if (symbol == _persistentSymbolCache.end()) {
    auto result = _symbolication->SymbolicateAndIntern(address);
    if (!result) return std::nullopt;
    symbol = _persistentSymbolCache.emplace(address, std::move(*result)).first;
  }
  const auto& info = symbol->second;
  auto* p = _aggregator->GetProfile();
  pa_mapping m{};
  pa_function f{};
  pa_location l{};
  if (String(p, info.ModuleName, m.filename) != PA_OK ||
      String(p, info.BuildId, m.build_id) != PA_OK ||
      String(p, info.FunctionName, f.name) != PA_OK ||
      String(p, info.FileName, f.filename) != PA_OK)
    return std::nullopt;
  m.memory_start = info.ModuleBaseAddress;
  m.memory_limit = info.ModuleBaseAddress + info.ModuleSize;
  if ((m.filename || m.build_id) && pa_intern_mapping(p, &m, &l.mapping) != PA_OK)
    return std::nullopt;
  f.system_name = f.name;
  if (pa_intern_function(p, &f, &l.function) != PA_OK) return std::nullopt;
  // Preserve the existing Windows module-relative location-address convention.
  l.address = info.ModuleBaseAddress ? address - info.ModuleBaseAddress : address;
  l.line = info.lineNumber;
  pa_id id = 0;
  if (pa_intern_location(p, &l, &id) != PA_OK) return std::nullopt;
  _currentExportLocationCache.emplace(address, id);
  return id;
}
std::optional<pa_id> ProfileExporter::CreateLabelSet(
    const std::shared_ptr<ThreadInfo>& thread,
    const RumViewContext& view,
    UiHangSampleKind uiHangKind
) {
  auto* p = _aggregator->GetProfile();
  std::array<pa_label, 6> labels{};
  size_t count = 0;
  // Process label is recreated from stable interned strings, not concatenated blobs.
  auto str = [&](pa_id key, const std::string& value) {
    pa_id id = 0;
    if (String(p, value, id) != PA_OK) return false;
    labels[count].key = key;
    labels[count++].str = id;
    return true;
  };
  labels[count++] = _processLabel;
  if (uiHangKind == UiHangSampleKind::Detected)
    labels[count++] = _uiHangTrueLabel;
  else if (uiHangKind == UiHangSampleKind::Recovered)
    labels[count++] = _uiHangFalseLabel;
  if (thread) {
    labels[count].key = _threadKey;
    labels[count].is_numeric = 1;
    labels[count++].num = thread->GetThreadId();
    if (thread->GetThreadName(_threadNameScratch) && !str(_nameKey, _threadNameScratch))
      return std::nullopt;
  }
  if (!view.view_id.empty()) {
    if (!str(_viewKey, view.view_id)) return std::nullopt;
    if (!view.view_name.empty() && !str(_endpointKey, view.view_name))
      return std::nullopt;
  }
  pa_id id = 0;
  if (pa_intern_labels(p, labels.data(), count, &id) != PA_OK) return std::nullopt;
  return id;
}
bool ProfileExporter::Add(std::shared_ptr<Sample> const& sample) {
  if (!_initialized || !sample) return false;
  _locationScratch.clear();
  for (auto address : sample->GetFrames()) {
    auto id = InternLocation(address);
    if (!id) return false;
    _locationScratch.push_back(*id);
  }
  auto labels = CreateLabelSet(
      sample->GetThreadInfo(),
      sample->GetRumViewContext(),
      sample->GetUiHangSampleKind()
  );
  return labels && _aggregator->AddSample(
                       _locationScratch,
                       sample->GetValues(),
                       sample->GetTimestamp().count(),
                       *labels
                   );
}
bool ProfileExporter::DeliverProfilingData(ProfilingData data) {
  try {
    _profilingDataCallback(std::move(data));
    return true;
  } catch (const std::exception& e) {
    Log::Error("Profiling callback threw: ", e.what());
  } catch (...) {
    Log::Error("Profiling callback threw");
  }
  return false;
}
bool ProfileExporter::ReportError(ProfilingErrorCode code, bool last) {
  if (_profilingDataCallback) {
    ProfilingData data;
    data.result = static_cast<uint32_t>(code);
    data.isLast = last;
    DeliverProfilingData(std::move(data));
  }
  return false;
}
bool ProfileExporter::Export(bool last) {
  if (!_initialized) return false;
  auto end = std::chrono::system_clock::now();
  int64_t startMs = ToUnixTimeMs(_profileStartTime), endMs = ToUnixTimeMs(end);
  std::vector<uint8_t> bytes;
  auto rum = CollectRumRecordsJson(static_cast<bool>(_profilingDataCallback));
  bool success = _aggregator->Serialize(startMs, endMs, bytes);
  if (!success)
    ReportError(ProfilingErrorCode::SerializationFailed, last);
  else {
    WriteDebugFiles(bytes, rum, endMs);
    Log::Info(
        "C profile #",
        _currentExportId,
        last ? " final, compressed bytes: " : ", compressed bytes: ",
        bytes.size(),
        ", symbol cache entries: ",
        _persistentSymbolCache.size()
    );
    if (_profilingDataCallback) {
      ProfilingData data;
      data.pprof = std::move(bytes);
      data.startUnixTimeMs = startMs;
      data.endUnixTimeMs = endMs;
      data.isLast = last;
      success = DeliverProfilingData(std::move(data));
    } else if (_httpExporter) {
      success = SendHttp(bytes, rum, startMs, endMs);
    }
  }
  // Recovery remains explicit even after encoding, sink or callback failures.
  if (!_aggregator->Reset()) {
    success = ReportError(ProfilingErrorCode::ProfileResetFailed, last);
  }
  if (_currentExportLocationCache.size() > 10000) {
    decltype(_currentExportLocationCache)().swap(_currentExportLocationCache);
  } else {
    _currentExportLocationCache.clear();
  }
  if (!InternSampleLabels())
    success = ReportError(ProfilingErrorCode::SampleLabelsUnavailable, last);
  _profileStartTime = end;
  ++_currentExportId;
  // Reclaim exceptional symbol/address churn instead of retaining an unbounded
  // persistent metadata high-water mark. Active small workloads stay cached.
  if (_persistentSymbolCache.size() > 10000) {
    decltype(_persistentSymbolCache)().swap(_persistentSymbolCache);
    _symbolication->ClearCaches();
  }
  if (_locationScratch.capacity() > 4096) std::vector<pa_id>().swap(_locationScratch);
  return success;
}
std::string ProfileExporter::CollectRumRecordsJson(bool discard) {
  if (!_pRumRecordProvider) return {};
  _pRumRecordProvider->ConsumeViewRecords(_viewRecordsBuffer);
  _pRumRecordProvider->ConsumeSessionRecords(_sessionRecordsBuffer);
  auto result =
      discard ? std::string{}
              : SerializeRumRecordsToJson(_viewRecordsBuffer, _sessionRecordsBuffer);
  _viewRecordsBuffer.clear();
  _sessionRecordsBuffer.clear();
  return result;
}
void ProfileExporter::WriteDebugFiles(
    const std::vector<uint8_t>& bytes, const std::string& rum, int64_t endMs
) {
  if (!_debugPprofFileWritingEnabled || _debugPprofPrefix.empty()) return;
  const auto stem = _debugPprofPrefix + std::to_string(endMs) + "_" +
                    std::to_string(_currentExportId);
  std::ofstream profile(stem + ".zstd.pprof", std::ios::binary);
  profile.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (!profile) Log::Warn("Failed to write debug profile");
  if (!rum.empty()) {
    std::ofstream metadata(stem + ".rum-views.json", std::ios::binary);
    metadata << rum;
  }
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
