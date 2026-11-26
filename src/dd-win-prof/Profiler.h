// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "Configuration.h"
#include "ProfilingSession.h"
#include "RumContext.h"
#include "ThreadList.h"
#include "dd-win-prof.h"
#include "internal/dd-win-rum-private.h"
#include "pch.h"

// Lives from DLL process attach to unload. Owns the process state: the lifecycle mutex
// and started flag, the committed configuration and callback registration, ThreadList
// and its ThreadInfo objects, the RUM state, the UI probe message ID, the session ID
// generation, and at most one running ProfilingSession, which owns every run component.
class Profiler : public IRumViewContextProvider,
                 public IRumRecordProvider,
                 public IViewVitalsAccumulator {
 public:
  Profiler();
  virtual ~Profiler();

  bool Setup(const ProfilerConfig* pSettings);
  // underLoaderLock: called from DllMain, where a failed start must not join threads
  bool StartProfiling(bool underLoaderLock = false);
  bool MonitorWindowHangs(HWND hWnd);
  void StopProfiling(bool shutdownOngoing = false);

  [[nodiscard]] bool SetProfilingDataCallback(
      ProfilingDataCallback callback, void* userData
  );
  ProfilingDataCallback GetProfilingDataCallback() const;
  void* GetProfilingDataCallbackUserData() const;
  ProfileDeliveryMode GetDeliveryMode() const;

  bool AddCurrentThread();
  void RemoveCurrentThread();

  bool IsStarted() const { return _isStarted.load(std::memory_order_relaxed); }
  size_t GetThreadCount() const { return _pThreadList ? _pThreadList->Count() : 0; }
  bool IsAutoStartEnabled() const {
    return _pConfiguration->IsProfilerAutoStartEnabled();
  }

  // RUM context management (called from the C API, thread-safe)
  bool EnterView(const char* viewName);
  bool LeaveCurrentView();
  bool SetRumSession(const RumSessionContext* pContext);
  bool SetRumView(const RumViewValues* pContext);

  // IRumViewContextProvider implementation
  bool GetCurrentViewContext(RumViewContext& context) const override;

  // IRumRecordProvider implementation
  std::string GetApplicationId() const override;
  void ConsumeViewRecords(std::vector<RumViewRecord>& records) override;
  void ConsumeSessionRecords(std::vector<RumSessionRecord>& records) override;
  std::string GetCurrentSessionId() const override;

  // IViewVitalsAccumulator implementation (lock-free, called from sampler hot path)
  bool AccumulateViewVitals(ViewVitalKind kind, int64_t valueNs) override;

  // Called by every C API entry inside its exception firewall, before the call.
  void BeforeApiCall() const {
    if (_beforeApiCall) {
      _beforeApiCall();
    }
  }

 public:
  static Configuration* GetConfiguration() { return _pConfiguration.get(); }

  static Profiler* GetInstance() { return _this; }

  static Profiler* GetStartedInstance() {
    if (_this && _this->IsStarted()) {
      return _this;
    }

    return nullptr;
  }

 private:
  friend class ProfilerShutdownTest;
  friend class ProfilingDataCallbackTests;
  friend class PublicApiExceptionTests;

  // Test seam: lets tests make every C API entry throw.
  std::function<void()> _beforeApiCall;

  // configuration
  inline static constexpr std::chrono::seconds UploadInterval =
      std::chrono::seconds(10);

  static Profiler* _this;

  // configuration
  static std::unique_ptr<Configuration> _pConfiguration;

  // Process-scoped RUM state. Declared before the session so that it is destroyed
  // after it: exporters and the sampler read it until they are gone.
  // _rumContextMutex is a leaf lock: while holding it, code touches only the RUM
  // state, never a run-owned object or another lock. Exporters take it through
  // IRumRecordProvider under the collector's export lock, never around the profiling
  // callback, so RUM calls are allowed from the callback.
  mutable std::shared_mutex _rumContextMutex;
  RumViewContext _currentRumView;

  void CompleteCurrentView();     // caller must hold _rumContextMutex exclusive
  void CompleteCurrentSession();  // caller must hold _rumContextMutex exclusive

  // RUM view timeline recording (protected by _rumContextMutex)
  std::vector<RumViewRecord> _completedViewRecords;
  int64_t _pendingViewStartMs{0};

  // Per-view vitals accumulators indexed by ViewVitalKind.
  // Written lock-free from the sampler thread (fetch_add with relaxed ordering),
  // read and reset under _rumContextMutex exclusive lock on view completion.
  std::array<std::atomic<int64_t>, MaxViewVitalKind> _pendingVitalsNs{};

  // RUM session tracking (protected by _rumContextMutex)
  std::string _currentSessionId;
  int64_t _sessionStartMs{0};
  std::vector<RumSessionRecord> _completedSessionRecords;

  // RUM app-level ID (write-once, read by exporters at export time; protected by
  // _rumContextMutex)
  std::string _rumApplicationId;

  // protect the lifecycle of the profiler (setup/registration/start/stop/monitor) and
  // the resources below
  mutable std::mutex _lifecycleMutex;
  std::atomic<bool> _isStarted;

  std::unique_ptr<ThreadList> _pThreadList;

  // Protected by _lifecycleMutex. StartProfiling copies it into the run's session.
  struct CallbackRegistration {
    ProfilingDataCallback callback = nullptr;
    void* userData = nullptr;

    ProfileDeliveryMode DeliveryMode() const {
      return callback ? ProfileDeliveryMode::Callback : ProfileDeliveryMode::Http;
    }
  };
  CallbackRegistration _callbackRegistration;

  // UI Hang detection
  bool InitializeWindowHangs();
  UINT _hangProbeMessageId = 0;

  uint64_t _lastSessionId = 0;
  // Set only once fully started, reset by StopProfiling. Declared last so that it is
  // destroyed before the process state it uses.
  std::unique_ptr<ProfilingSession> _pSession;
};
