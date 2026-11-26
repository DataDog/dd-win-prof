// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "Profiler.h"

#include <random>
#include <type_traits>

#include "Log.h"
#include "pch.h"

Profiler* Profiler::_this = nullptr;
std::unique_ptr<Configuration> Profiler::_pConfiguration =
    std::make_unique<Configuration>();

Profiler::Profiler() : _isStarted(false), _pThreadList(std::make_unique<ThreadList>()) {
  _this = this;
}

Profiler::~Profiler() {
  _this = nullptr;
  _isStarted = false;
}

namespace {
// StopProfiling holds _lifecycleMutex while it runs the final export on its own thread
// and while it waits for the exporter thread to finish a periodic export. A call from
// the callback must therefore fail before taking the lock instead of deadlocking.
bool IsCalledFromProfilingCallback(const char* api) {
  if (!ProfileExporter::IsInsideProfilingCallback()) {
    return false;
  }
  Log::Error(api, " cannot be called from the profiling callback.");
  return true;
}
}  // namespace

bool Profiler::Setup(const ProfilerConfig* pSettings) {
  if (IsCalledFromProfilingCallback("SetupProfiler()")) {
    return false;
  }

  std::lock_guard<std::mutex> lock(_lifecycleMutex);
  if (_isStarted.load(std::memory_order_relaxed)) {
    Log::Warn("SetupProfiler() cannot be called while the profiler is running.");
    return false;
  }

  auto candidate =
      Configuration::Create(pSettings, _callbackRegistration.DeliveryMode());
  if (!candidate.has_value()) {
    return false;
  }

  // The commit cannot fail halfway, so a failed setup keeps every committed value.
  static_assert(std::is_nothrow_move_assignable_v<Configuration>);
  *_pConfiguration = std::move(*candidate);
  return true;
}

bool Profiler::SetProfilingDataCallback(
    ProfilingDataCallback callback, void* userData
) {
  if (callback == nullptr && userData != nullptr) {
    Log::Error("A profiling callback context requires a callback");
    return false;
  }

  if (IsCalledFromProfilingCallback("SetupProfilingDataCallback()")) {
    return false;
  }

  std::lock_guard<std::mutex> lock(_lifecycleMutex);
  if (_isStarted.load(std::memory_order_relaxed)) {
    Log::Error("Cannot change the profiling callback while the profiler is running");
    return false;
  }

  _callbackRegistration = {callback, userData};
  return true;
}

ProfilingDataCallback Profiler::GetProfilingDataCallback() const {
  std::lock_guard<std::mutex> lock(_lifecycleMutex);
  return _callbackRegistration.callback;
}

void* Profiler::GetProfilingDataCallbackUserData() const {
  std::lock_guard<std::mutex> lock(_lifecycleMutex);
  return _callbackRegistration.userData;
}

ProfileDeliveryMode Profiler::GetDeliveryMode() const {
  std::lock_guard<std::mutex> lock(_lifecycleMutex);
  return _callbackRegistration.DeliveryMode();
}

bool Profiler::StartProfiling(bool underLoaderLock) {
  if (IsCalledFromProfilingCallback("StartProfiler()")) {
    return false;
  }

  std::lock_guard<std::mutex> lock(_lifecycleMutex);

  if (_isStarted.load(std::memory_order_relaxed)) {
    Log::Warn("Profiler is already running.");
    return false;
  }

  // Setup and registration wait for this lock and are rejected while the run is
  // started, so this run keeps the configuration and registration it sees here.
  Configuration* const configuration = _pConfiguration.get();
  const auto callbackRegistration = _callbackRegistration;

  // no needed to look at env var to enable profiler
  // --> used only as kill switch to disable it
  if (configuration->IsProfilerExplicitlyDisabled()) {
    Log::Info(
        "Profiler is explicitly disabled: check following environment variable "
        "DD_PROFILING_ENABLED"
    );
    return false;
  }

  if (!configuration->ValidateTransportConfig(callbackRegistration.DeliveryMode())) {
    return false;
  }

  Log::Info("Starting profiler...");

  // a failure to register the hang probe message is not fatal, but it will prevent UI
  // hang detection from working (i.e. MonitorWindowHangs() will fail)
  InitializeWindowHangs();

  // The candidate stays private until it is fully started: a failed start has already
  // released everything it created.
  auto session = std::make_unique<ProfilingSession>(
      ++_lastSessionId,
      *configuration,
      callbackRegistration.callback,
      callbackRegistration.userData,
      ProfilingSession::ProcessServices{_pThreadList.get(), this, this, this}
  );
  if (!session->Start(underLoaderLock)) {
    return false;
  }

  _pSession = std::move(session);
  _isStarted = true;
  return true;
}

bool Profiler::MonitorWindowHangs(HWND hWnd) {
  if (IsCalledFromProfilingCallback("MonitorWindowHangs()")) {
    return false;
  }

  std::lock_guard<std::mutex> lock(_lifecycleMutex);

  if (!_isStarted.load(std::memory_order_relaxed)) {
    Log::Warn("Profiler must be started before monitoring window hangs.");
    return false;
  }

  if (_hangProbeMessageId == 0) {
    Log::Warn(
        "Probe message could not be registered: impossible to monitor window hangs."
    );
    return false;
  }

  return _pSession->MonitorWindowHangs(hWnd, _hangProbeMessageId);
}

void Profiler::StopProfiling(bool shutdownOngoing) {
  if (IsCalledFromProfilingCallback("StopProfiler()")) {
    return;
  }

  std::lock_guard<std::mutex> lock(_lifecycleMutex);

  // avoid being stopped multiple times
  if (!_isStarted.load(std::memory_order_relaxed)) {
    Log::Warn("Profiler is not running.");
    return;
  }

  Log::Info("Stopping profiler...");

  _isStarted = false;

  _pSession->Stop(shutdownOngoing);
  _pSession.reset();

  Log::Info("Profiler stopped...");
}

bool Profiler::InitializeWindowHangs() {
  _hangProbeMessageId = ::RegisterWindowMessageW(dd_win_prof::kHangProbeMessageName);
  if (_hangProbeMessageId == 0) {
    Log::Warn("Failed to register hang probe message.");
    return false;
  }

  return true;
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

  // Application ID: write-once, read by each exporter at export time
  if (!_rumApplicationId.empty() && _rumApplicationId != pContext->application_id) {
    return false;
  }

  if (_rumApplicationId.empty()) {
    _rumApplicationId = pContext->application_id;
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

std::string Profiler::GetApplicationId() const {
  std::shared_lock lock(_rumContextMutex);
  return _rumApplicationId;
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
