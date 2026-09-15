// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "Profiler.h"

#include <random>
#include <string_view>

#include "Log.h"
#include "SampleValueTypeProvider.h"
#include "SamplesCollector.h"
#include "pch.h"

Profiler* Profiler::_this = nullptr;
std::unique_ptr<Configuration> Profiler::_pConfiguration =
    std::make_unique<Configuration>();

namespace {
bool IsValidUtf8(std::string_view value) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
  size_t index = 0;

  while (index < value.size()) {
    uint32_t codePoint = 0;
    size_t continuationCount = 0;
    uint32_t minimumCodePoint = 0;
    const unsigned char first = bytes[index++];

    if (first <= 0x7f) {
      continue;
    }
    if ((first & 0xe0) == 0xc0) {
      codePoint = first & 0x1f;
      continuationCount = 1;
      minimumCodePoint = 0x80;
    } else if ((first & 0xf0) == 0xe0) {
      codePoint = first & 0x0f;
      continuationCount = 2;
      minimumCodePoint = 0x800;
    } else if ((first & 0xf8) == 0xf0) {
      codePoint = first & 0x07;
      continuationCount = 3;
      minimumCodePoint = 0x10000;
    } else {
      return false;
    }

    if (index + continuationCount > value.size()) {
      return false;
    }

    for (size_t i = 0; i < continuationCount; i++) {
      const unsigned char continuation = bytes[index++];
      if ((continuation & 0xc0) != 0x80) {
        return false;
      }
      codePoint = (codePoint << 6) | (continuation & 0x3f);
    }

    if (codePoint < minimumCodePoint || codePoint > 0x10ffff ||
        (codePoint >= 0xd800 && codePoint <= 0xdfff)) {
      return false;
    }
  }

  return true;
}

int64_t CurrentTimeMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch()
  )
      .count();
}
}  // namespace

Profiler::Profiler()
    : _isStarted(false),
      _pThreadList(std::make_unique<ThreadList>()),
      _pStackSamplerLoop(nullptr) {
  _this = this;
}

Profiler::~Profiler() {
  _this = nullptr;
  _isStarted = false;
}

bool Profiler::StartProfiling() {
  // no needed to look at env var to enable profiler
  // --> used only as kill switch to disable it
  if (_pConfiguration->IsProfilerExplicitlyDisabled()) {
    Log::Info(
        "Profiler is explicitly disabled: check following environment variable "
        "DD_PROFILING_ENABLED"
    );
    return false;
  }

  Log::Info("Starting profiler...");

  auto valueTypeProvider = SampleValueTypeProvider();

  _pCpuTimeProvider = std::make_unique<CpuTimeProvider>(valueTypeProvider);
  _pCpuWallTimeProvider = std::make_unique<WallTimeProvider>(valueTypeProvider);

  // create the thread responsible for looping through the thread list
  _pStackSamplerLoop = std::make_unique<StackSamplerLoop>(
      _pConfiguration.get(),
      _pThreadList.get(),
      _pCpuTimeProvider.get(),
      _pCpuWallTimeProvider.get(),
      this,
      this
  );

  // get the values definition from the different providers...
  auto const& sampleTypeDefinitions = valueTypeProvider.GetValueTypes();
  Sample::SetValuesCount(sampleTypeDefinitions.size());

  //... and pass them to the exporter
  auto profileExporter = std::make_unique<ProfileExporter>(
      _pConfiguration.get(), sampleTypeDefinitions, this
  );

  // Initialize the ProfileExporter
  if (!profileExporter->Initialize()) {
    Log::Error(
        "Failed to initialize profile exporter: ", profileExporter->GetLastError()
    );
    return false;
  }

  {
    std::unique_lock lock(_rumContextMutex);
    if (!_rumApplicationId.empty()) {
      profileExporter->SetRumApplicationId(_rumApplicationId);
    }
    _pProfileExporter = std::move(profileExporter);
  }

  // create the samples collector and pass it the exporter
  _pSamplesCollector = std::make_unique<SamplesCollector>(
      _pConfiguration.get(), _pProfileExporter.get()
  );

  // register the providers to the collector
  if (_pConfiguration->IsCpuProfilingEnabled()) {
    _pSamplesCollector->Register(_pCpuTimeProvider.get());
  }

  if (_pConfiguration->IsWallTimeProfilingEnabled()) {
    _pSamplesCollector->Register(_pCpuWallTimeProvider.get());
  }

  // start processing
  _pSamplesCollector->Start();
  _pStackSamplerLoop->Start();

  _isStarted = true;
  return true;
}

void Profiler::StopProfiling(bool shutdownOngoing) {
  // avoid being stopped multiple times
  if (!_isStarted) {
    return;
  }

  Log::Info("Stopping profiler...");

  _isStarted = false;

  // Signal SamplesCollector if we're in shutdown mode to stop exports immediately
  if (shutdownOngoing) {
    SamplesCollector::SignalShutdown();
  }

  if (_pStackSamplerLoop != nullptr) {
    _pStackSamplerLoop->Stop();
  }

  if (_pSamplesCollector != nullptr) {
    _pSamplesCollector->Stop(shutdownOngoing);
  }

  // Explicitly clean up ProfileExporter with shutdown detection
  if (_pProfileExporter != nullptr) {
    // Use the parameter to decide cleanup approach
    _pProfileExporter->Cleanup(shutdownOngoing);
  }
  Log::Info("Profiler stopped...");
}

bool Profiler::AddCurrentThread() {
  auto tid = ::GetCurrentThreadId();
  HANDLE hThread;
  auto success = ::DuplicateHandle(
      ::GetCurrentProcess(),
      ::GetCurrentThread(),
      ::GetCurrentProcess(),
      &hThread,
      THREAD_ALL_ACCESS,
      false,
      0
  );
  if (!success) {
    Log::Debug("DuplicateHandle() failed for thread ID: ", tid);
    return false;
  }

  _pThreadList->AddThread(tid, hThread);
  return true;
}

void Profiler::RemoveCurrentThread() {
  auto tid = ::GetCurrentThreadId();
  _pThreadList->RemoveThread(tid);
}

ProfilerRumContextResult Profiler::SetRumCorrelationContext(
    const ProfilerRumCorrelationContext* pContext
) {
  if (pContext->application_id == nullptr || pContext->session_id == nullptr ||
      pContext->view_id == nullptr || pContext->view_name == nullptr) {
    return PROFILER_RUM_CONTEXT_NULL_ARGUMENT;
  }

  const std::string_view applicationIdValue(pContext->application_id);
  const std::string_view sessionIdValue(pContext->session_id);
  const std::string_view viewIdValue(pContext->view_id);
  const std::string_view viewNameValue(pContext->view_name);

  if (!IsValidUtf8(applicationIdValue) || !IsValidUtf8(sessionIdValue) ||
      !IsValidUtf8(viewIdValue) || !IsValidUtf8(viewNameValue)) {
    return PROFILER_RUM_CONTEXT_INVALID_UTF8;
  }

  if (applicationIdValue.empty() ||
      (sessionIdValue.empty() && (!viewIdValue.empty() || !viewNameValue.empty())) ||
      (viewIdValue.empty() && !viewNameValue.empty())) {
    return PROFILER_RUM_CONTEXT_INVALID_CONTEXT;
  }

  std::string applicationId(applicationIdValue);
  std::string sessionId(sessionIdValue);
  std::string viewId(viewIdValue);
  std::string viewName(viewNameValue);

  std::unique_lock lock(_rumContextMutex);

  if (!_rumApplicationId.empty() && _rumApplicationId != applicationId) {
    return PROFILER_RUM_CONTEXT_APPLICATION_ID_MISMATCH;
  }

  const bool applicationChanged = _rumApplicationId.empty();
  const bool sessionChanged = _currentSessionId != sessionId;
  const bool hasCurrentView = !_currentRumView.view_id.empty();
  const bool hasIncomingView = !viewId.empty();
  const bool viewIdChanged = hasCurrentView != hasIncomingView ||
                             (hasCurrentView && _currentRumView.view_id != viewId);
  const bool viewNameChanged =
      hasCurrentView && hasIncomingView && _currentRumView.view_name != viewName;

  if (!applicationChanged && !sessionChanged && !viewIdChanged && !viewNameChanged) {
    return PROFILER_RUM_CONTEXT_SUCCESS;
  }

  const bool completeView = hasCurrentView && (sessionChanged || viewIdChanged);
  const bool completeSession = !_currentSessionId.empty() && sessionChanged;
  if (completeView) {
    _completedViewRecords.reserve(_completedViewRecords.size() + 1);
  }
  if (completeSession) {
    _completedSessionRecords.reserve(_completedSessionRecords.size() + 1);
  }

  if (applicationChanged && _pProfileExporter != nullptr) {
    _pProfileExporter->SetRumApplicationId(applicationId);
  }

  const int64_t nowMs = CurrentTimeMilliseconds();

  if (completeView) {
    RumViewRecord record;
    record.timestamp_ms = _pendingViewStartMs;
    record.duration_ms = nowMs - _pendingViewStartMs;
    record.view_id = std::move(_currentRumView.view_id);
    record.view_name = std::move(_currentRumView.view_name);
    for (size_t i = 0; i < MaxViewVitalKind; ++i) {
      record.vitals_ns[i] = _pendingVitalsNs[i].exchange(0, std::memory_order_relaxed);
    }
    _completedViewRecords.push_back(std::move(record));
  }

  if (completeSession) {
    _completedSessionRecords.push_back(
        {_sessionStartMs, nowMs - _sessionStartMs, std::move(_currentSessionId)}
    );
  }

  if (applicationChanged) {
    _rumApplicationId = std::move(applicationId);
  }

  if (sessionChanged) {
    _currentSessionId = std::move(sessionId);
    _sessionStartMs = _currentSessionId.empty() ? 0 : nowMs;
  }

  if (sessionChanged || viewIdChanged) {
    _currentRumView.view_id = std::move(viewId);
    _currentRumView.view_name = std::move(viewName);
    _pendingViewStartMs = hasIncomingView ? nowMs : 0;
    if (hasIncomingView && !completeView) {
      for (auto& vital : _pendingVitalsNs) {
        vital.store(0, std::memory_order_relaxed);
      }
    }
  } else if (viewNameChanged) {
    _currentRumView.view_name = std::move(viewName);
  }

  return PROFILER_RUM_CONTEXT_SUCCESS;
}

static std::string GenerateUuidV4() {
  static thread_local std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

  auto r = [&]() { return dist(rng); };
  char buf[37];
  std::snprintf(
      buf,
      sizeof(buf),
      "%08x-%04x-%04x-%04x-%04x%08x",
      r(),
      r() & 0xFFFF,
      (r() & 0x0FFF) | 0x4000,
      (r() & 0x3FFF) | 0x8000,
      r() & 0xFFFF,
      r()
  );
  return buf;
}

void Profiler::CompleteCurrentSession() {
  if (_currentSessionId.empty()) {
    return;
  }

  auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()
  )
                   .count();
  _completedSessionRecords.push_back(
      {_sessionStartMs, nowMs - _sessionStartMs, std::move(_currentSessionId)}
  );
  _currentSessionId.clear();
}

bool Profiler::SetRumSession(const RumSessionContext* pContext) {
  if (pContext == nullptr) {
    std::unique_lock lock(_rumContextMutex);
    CompleteCurrentSession();
    CompleteCurrentView();
    return true;
  }

  bool hasAppId =
      pContext->application_id != nullptr && pContext->application_id[0] != '\0';
  if (!hasAppId) {
    return false;
  }

  bool hasSessionId =
      pContext->session_id != nullptr && pContext->session_id[0] != '\0';
  if (!hasSessionId) {
    return false;
  }

  std::unique_lock lock(_rumContextMutex);

  // Application ID: write-once, buffered until exporter exists
  if (!_rumApplicationId.empty() && _rumApplicationId != pContext->application_id) {
    return false;
  }

  if (_rumApplicationId.empty()) {
    _rumApplicationId = pContext->application_id;

    if (_pProfileExporter != nullptr) {
      _pProfileExporter->SetRumApplicationId(_rumApplicationId);
    }
  }

  // Session tracking: complete previous session on change, start new one
  if (_currentSessionId != pContext->session_id) {
    CompleteCurrentSession();

    _currentSessionId = pContext->session_id;
    _sessionStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch()
    )
                          .count();
  }

  return true;
}

void Profiler::CompleteCurrentView() {
  if (_currentRumView.view_id.empty()) {
    return;
  }

  auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()
  )
                   .count();

  RumViewRecord rec;
  rec.timestamp_ms = _pendingViewStartMs;
  rec.duration_ms = nowMs - _pendingViewStartMs;
  rec.view_id = std::move(_currentRumView.view_id);
  rec.view_name = std::move(_currentRumView.view_name);
  for (size_t i = 0; i < MaxViewVitalKind; ++i)
    rec.vitals_ns[i] = _pendingVitalsNs[i].exchange(0, std::memory_order_relaxed);
  _completedViewRecords.push_back(std::move(rec));

  _currentRumView.view_id.clear();
  _currentRumView.view_name.clear();
}

bool Profiler::SetRumView(const RumViewValues* pContext) {
  std::unique_lock lock(_rumContextMutex);

  if (_currentSessionId.empty()) {
    return false;
  }

  CompleteCurrentView();

  bool hasViewId = pContext != nullptr && pContext->view_id != nullptr &&
                   pContext->view_id[0] != '\0';

  if (!hasViewId) {
    return true;
  }

  _currentRumView.view_id = pContext->view_id;
  _currentRumView.view_name =
      (pContext->view_name != nullptr) ? pContext->view_name : "";

  _pendingViewStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()
  )
                            .count();

  for (auto& a : _pendingVitalsNs) a.store(0, std::memory_order_relaxed);

  return true;
}

bool Profiler::EnterView(const char* viewName) {
  std::string viewId = GenerateUuidV4();
  RumViewValues vals = {};
  vals.view_id = viewId.c_str();
  vals.view_name = viewName;
  return SetRumView(&vals);
}

bool Profiler::LeaveCurrentView() {
  std::unique_lock lock(_rumContextMutex);

  if (_currentRumView.view_id.empty()) {
    return false;
  }

  CompleteCurrentView();
  return true;
}

bool Profiler::GetCurrentViewContext(RumViewContext& context) const {
  std::shared_lock lock(_rumContextMutex);
  if (_currentRumView.view_id.empty()) {
    return false;
  }
  context = _currentRumView;
  return true;
}

void Profiler::ConsumeViewRecords(std::vector<RumViewRecord>& records) {
  std::unique_lock lock(_rumContextMutex);
  _completedViewRecords.swap(records);
}

void Profiler::ConsumeSessionRecords(std::vector<RumSessionRecord>& records) {
  std::unique_lock lock(_rumContextMutex);
  _completedSessionRecords.swap(records);
}

std::string Profiler::GetCurrentSessionId() const {
  std::shared_lock lock(_rumContextMutex);
  return _currentSessionId;
}

bool Profiler::AccumulateViewVitals(ViewVitalKind kind, int64_t valueNs) {
  auto idx = static_cast<size_t>(kind);
  if (idx >= MaxViewVitalKind) return false;

  _pendingVitalsNs[idx].fetch_add(valueNs, std::memory_order_relaxed);
  return true;
}
