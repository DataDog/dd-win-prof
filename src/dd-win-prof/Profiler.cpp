// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "Profiler.h"

#include <random>

#include "Log.h"
#include "SampleValueTypeProvider.h"
#include "SamplesCollector.h"
#include "pch.h"

Profiler* Profiler::_this = nullptr;
std::unique_ptr<Configuration> Profiler::_pConfiguration =
    std::make_unique<Configuration>();

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

bool Profiler::SetProfilingDataCallback(
    ProfilingDataCallback callback, void* userData
) {
  if (callback == nullptr && userData != nullptr) {
    Log::Error("A profiling callback context requires a callback");
    return false;
  }
  if (_callbackRegistrationFrozen.load(std::memory_order_acquire) || IsStarted()) {
    Log::Error("Cannot change the profiling callback while the profiler is running");
    return false;
  }
  _profilingDataCallback = callback;
  _profilingDataCallbackUserData = userData;
  return true;
}

bool Profiler::StartProfiling(bool underLoaderLock) {
  std::lock_guard<std::mutex> lock(_lifecycleMutex);

  if (_isStarted.load(std::memory_order_relaxed)) {
    Log::Warn("Profiler is already running.");
    return false;
  }

  // no needed to look at env var to enable profiler
  // --> used only as kill switch to disable it
  if (_pConfiguration->IsProfilerExplicitlyDisabled()) {
    Log::Info(
        "Profiler is explicitly disabled: check following environment variable "
        "DD_PROFILING_ENABLED"
    );
    return false;
  }

  if (!_pConfiguration->ValidateTransportConfig(GetDeliveryMode())) {
    return false;
  }

  Log::Info("Starting profiler...");

  // ThreadInfo objects live for the lifetime of the process. Clear the
  // previous session's timestamps, hang marker, CPU baseline, and sampler
  // iterators before creating a new sampling loop.
  _pThreadList->ResetProfilingState();

  auto valueTypeProvider = SampleValueTypeProvider();

  _pCpuTimeProvider = std::make_unique<CpuTimeProvider>(valueTypeProvider);
  _pCpuWallTimeProvider = std::make_unique<WallTimeProvider>(valueTypeProvider);
  _pUiHangProvider = std::make_unique<UiHangProvider>(valueTypeProvider);

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
  _pProfileExporter = std::make_unique<ProfileExporter>(
      _pConfiguration.get(),
      sampleTypeDefinitions,
      this,
      _profilingDataCallback,
      _profilingDataCallbackUserData
  );
  _callbackRegistrationFrozen.store(true, std::memory_order_release);

  // Initialize the ProfileExporter
  if (!_pProfileExporter->Initialize()) {
    RollBackStart(underLoaderLock);
    return false;
  }

  // a failure to register the hang probe message is not fatal, but it will prevent UI
  // hang detection from working (i.e. MonitorWindowHangs() will fail)
  InitializeWindowHangs();

  // Flush buffered RUM application ID to the exporter
  {
    std::shared_lock lock(_rumContextMutex);
    if (!_rumApplicationId.empty()) {
      _pProfileExporter->SetRumApplicationId(_rumApplicationId);
    }
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

  // no configuration needed because enabled only when starting to monitor hang
  _pSamplesCollector->Register(_pUiHangProvider.get());

  // start processing
  if (!_pSamplesCollector->Start(underLoaderLock)) {
    RollBackStart(underLoaderLock);
    return false;
  }
  try {
    _pStackSamplerLoop->Start();
  } catch (const std::exception& error) {
    Log::Error("Failed to start the stack sampler loop: ", error.what());
    RollBackStart(underLoaderLock);
    return false;
  }

  _isStarted = true;
  return true;
}

// Releases what a failed StartProfiling() created, producer first, without exporting.
// The sampler loop thread never exists here, so resetting the loop does not join it,
// which would hang under the loader lock.
void Profiler::RollBackStart(bool underLoaderLock) {
  _pStackSamplerLoop.reset();
  if (_pSamplesCollector != nullptr) {
    _pSamplesCollector->Abort(underLoaderLock);
    _pSamplesCollector.reset();
  }
  _pProfileExporter.reset();
  _pUiHangProvider.reset();
  _pCpuWallTimeProvider.reset();
  _pCpuTimeProvider.reset();
  _callbackRegistrationFrozen.store(false, std::memory_order_release);
}

bool Profiler::MonitorWindowHangs(HWND hWnd) {
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

  // create the UiHangDetector (hook is thread-specific, so no DLL hMod)
  if (_pUiHangDetector != nullptr) {
    Log::Warn("Impossible to monitor more than one window for UI hang detection.");
    return false;
  }

  auto pUiHangDetector = std::make_unique<UiHangDetector>(
      _hangProbeMessageId, _pUiHangProvider.get(), this
  );
  bool success = pUiHangDetector->MonitorWindowHangs(hWnd, _pThreadList.get());
  if (success) {
    _pUiHangDetector = std::move(pUiHangDetector);
  }
  return success;
}

void Profiler::StopProfiling(bool shutdownOngoing) {
  std::lock_guard<std::mutex> lock(_lifecycleMutex);

  // avoid being stopped multiple times
  if (!_isStarted.load(std::memory_order_relaxed)) {
    Log::Warn("Profiler is not running.");
    return;
  }

  Log::Info("Stopping profiler...");

  _isStarted = false;

  // Signal SamplesCollector if we're in shutdown mode to stop exports immediately
  if (shutdownOngoing && _pSamplesCollector != nullptr) {
    _pSamplesCollector->SignalShutdown();
  }

  // Stop the UI hang detector first
  if (_pUiHangDetector != nullptr) {
    _pUiHangDetector->Stop();
    _pUiHangDetector.reset();
  }

  if (_pStackSamplerLoop != nullptr) {
    _pStackSamplerLoop->Stop();
    _pStackSamplerLoop.reset();
  }

  if (_pSamplesCollector != nullptr) {
    const auto finalStatus = _pSamplesCollector->Stop(shutdownOngoing);
    if (finalStatus) {
      switch (*finalStatus) {
        case FinalExportStatus::Delivered:
          break;
        case FinalExportStatus::LocalWriteFailed:
          Log::Warn(
              "Final profile delivered, but the debug pprof file could not be written"
          );
          break;
        case FinalExportStatus::NotExported:
          Log::Info("Final profile not exported: ", ToString(*finalStatus));
          break;
        default:
          Log::Warn("Final profile was not delivered: ", ToString(*finalStatus));
          break;
      }
    }
    _pSamplesCollector.reset();
  }

  // Explicitly clean up ProfileExporter with shutdown detection
  if (_pProfileExporter != nullptr) {
    // Use the parameter to decide cleanup approach
    _pProfileExporter->Cleanup(shutdownOngoing);
    _pProfileExporter.reset();
  }
  _callbackRegistrationFrozen.store(false, std::memory_order_release);

  _pUiHangProvider.reset();
  _pCpuWallTimeProvider.reset();
  _pCpuTimeProvider.reset();

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
