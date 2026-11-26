// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "UiHangDetector.h"

#include "Log.h"
#include "OpSysTools.h"
#include "ProfilingConstants.h"
#include "StackFrameCollector.h"
#include "pch.h"

std::atomic<UiHangDetector*> UiHangDetector::_instance = nullptr;
std::mutex UiHangDetector::_instanceMutex;

constexpr const wchar_t* ThreadName = L"DD_UI_Hang";

namespace {
template <typename... Args>
void WarnNoexcept(const Args&... args) noexcept {
  try {
    Log::Warn(args...);
  } catch (...) {
  }
}

class Win32UiHangDetectorRuntime final : public IUiHangDetectorRuntime {
 public:
  HANDLE CreateStopEvent() override {
    return ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }

  BOOL SetEvent(HANDLE hEvent) override { return ::SetEvent(hEvent); }

  DWORD WaitForSingleObject(HANDLE hHandle, DWORD milliseconds) override {
    return ::WaitForSingleObject(hHandle, milliseconds);
  }

  BOOL CloseHandle(HANDLE hHandle) override { return ::CloseHandle(hHandle); }

  BOOL IsWindow(HWND hWnd) override { return ::IsWindow(hWnd); }

  DWORD GetWindowThreadProcessId(HWND hWnd, DWORD* pProcessId) override {
    return ::GetWindowThreadProcessId(hWnd, pProcessId);
  }

  bool InstallMessageHook(DWORD threadId, MessageCallback onMessage) override {
    _onMessage.store(onMessage, std::memory_order_release);
    // Thread-specific hook on the window's UI thread. A global hook
    // (dwThreadId == 0) would inject this DLL into every GUI process on the
    // desktop, locking the file so rebuilds of consumers (e.g. UIApp) fail.
    // hMod is NULL because the hook procedure lives in this process.
    _hHook = ::SetWindowsHookExW(WH_GETMESSAGE, GetMsgProc, nullptr, threadId);
    return _hHook != nullptr;
  }

  bool RemoveMessageHook() override {
    HHOOK hHook = std::exchange(_hHook, nullptr);
    return (hHook == nullptr) || (::UnhookWindowsHookEx(hHook) != FALSE);
  }

  BOOL PostMessageW(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) override {
    return ::PostMessageW(hWnd, message, wParam, lParam);
  }

  std::unique_ptr<std::thread> StartThread(std::function<void()> threadProc) override {
    return std::make_unique<std::thread>(std::move(threadProc));
  }

  std::chrono::nanoseconds GetHighPrecisionTimestamp() override {
    return OpSysTools::GetHighPrecisionTimestamp();
  }

  bool CaptureStack(
      const std::shared_ptr<ThreadInfo>& pThreadInfo, std::vector<uint64_t>& frames
  ) override {
    StackFrameCollector collector;
    CONTEXT seedContext;
    if (!collector.TrySuspendThread(pThreadInfo, seedContext)) {
      return false;
    }

    bool isTruncated = false;
    uint64_t buffer[dd_win_prof::kMaxStackDepth];
    uint16_t framesCount = dd_win_prof::kMaxStackDepth;
    bool isStackCaptured = collector.CaptureStack(
        pThreadInfo->GetOsThreadHandle(), seedContext, buffer, framesCount, isTruncated
    );
    // resume the thread before doing any allocation that could cause a deadlock
    ::ResumeThread(pThreadInfo->GetOsThreadHandle());

    if (!isStackCaptured) {
      return false;
    }

    // set a null address for the last frame in case of truncated stack
    if (isTruncated) {
      buffer[framesCount - 1] = 0;
    }
    frames.assign(buffer, buffer + framesCount);
    return true;
  }

 private:
  static LRESULT CALLBACK GetMsgProc(int code, WPARAM wParam, LPARAM lParam) {
    if ((code == HC_ACTION) && (wParam == PM_REMOVE) && (lParam != 0)) {
      _onMessage.load(std::memory_order_acquire)(*reinterpret_cast<const MSG*>(lParam));
    }
    // onMessage has returned, so no lock is held if the rest of the chain reenters
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
  }

  // static because a hook procedure receives no context
  inline static std::atomic<MessageCallback> _onMessage{nullptr};
  HHOOK _hHook = nullptr;
};
constinit Win32UiHangDetectorRuntime g_win32Runtime;
}  // namespace

UiHangDetector::UiHangDetector(
    uint64_t sessionId,
    UINT hangProbeMessageId,
    UiHangProvider* pHangProvider,
    IRumViewContextProvider* pRumViewContextProvider
)
    : UiHangDetector(
          sessionId,
          hangProbeMessageId,
          pHangProvider,
          pRumViewContextProvider,
          g_win32Runtime
      ) {}

UiHangDetector::UiHangDetector(
    uint64_t sessionId,
    UINT hangProbeMessageId,
    UiHangProvider* pHangProvider,
    IRumViewContextProvider* pRumViewContextProvider,
    IUiHangDetectorRuntime& runtime
)
    : _runtime(runtime),
      _sessionId(sessionId),
      _hangProbeMessageId(hangProbeMessageId),
      _hWnd(nullptr),
      _pHangProvider(pHangProvider),
      _pRumViewContextProvider(pRumViewContextProvider),
      _isHookInstalled(false),
      _stopEvent(nullptr),
      _pWatchdogThread(nullptr),
      _stopRequested(false),
      _isStopped(false),
      _cutoffTimestamp(0ns),
      _probeSequence(0),
      _isProcessed(false),
      _state(WatchdogState::None),
      _processedProbeTimestamp(0ns),
      _postProbeTimestamp(0ns),
      _lastNoHangTimestamp(0ns),
      _hangDetectionTimestamp(0ns),
      _initialHangDuration(0ns) {}

UiHangDetector::~UiHangDetector() noexcept { Stop(); }

void UiHangDetector::OnMessage(const MSG& message) {
  std::lock_guard<std::mutex> lock(_instanceMutex);
  UiHangDetector* pDetector = _instance.load(std::memory_order_acquire);
  if (pDetector != nullptr) {
    pDetector->ProcessMessage(message);
  }
}

bool UiHangDetector::MonitorWindowHangs(HWND hWnd, ThreadList* pThreadList) noexcept
    try {
  if (_isStopped) {
    Log::Warn("UI hang detector has already been stopped.");
    return false;
  }

  if (hWnd == nullptr) {
    return false;
  }

  // we can't monitor more than one window at a time
  if (_instance.load(std::memory_order_acquire) != nullptr) {
    Log::Warn("Impossible to monitor more than one window for UI hang detection.");
    return false;
  }

  if (_pHangProvider == nullptr) {
    Log::Debug("Null UiHangProvider passed to UiHangDetector.");
    return false;
  }

  if (_pRumViewContextProvider == nullptr) {
    Log::Debug("Null IRumViewContextProvider passed to UiHangDetector.");
    return false;
  }

  if (pThreadList == nullptr) {
    Log::Debug("Null ThreadList passed to UiHangDetector.");
    return false;
  }

  if (!Activate(hWnd, pThreadList)) {
    // release what the successful stages acquired so that a later call can succeed
    Deactivate();
    return false;
  }

  return true;
} catch (const std::exception& error) {
  WarnNoexcept("Failed to activate UI hang detection: ", error.what());
  Deactivate();
  return false;
} catch (...) {
  WarnNoexcept("Failed to activate UI hang detection: non-standard exception.");
  Deactivate();
  return false;
}

// Returns false at the first stage that fails; _instance is set last, once everything
// the hook notifications and the watchdog rely on exists.
bool UiHangDetector::Activate(HWND hWnd, ThreadList* pThreadList) {
  // manual reset event set to stop the watchdog thread
  _stopEvent = _runtime.CreateStopEvent();
  if (_stopEvent == nullptr) {
    Log::Warn("Failed to create the stop event.");
    return false;
  }

  if (_runtime.IsWindow(hWnd) == FALSE) {
    Log::Warn("The window handle to monitor is invalid.");
    return false;
  }

  DWORD pid = 0;
  DWORD tid = _runtime.GetWindowThreadProcessId(hWnd, &pid);
  if (tid == 0) {
    return false;
  }
  if (pid != ::GetCurrentProcessId()) {
    Log::Warn("The window to monitor does not belong to the current process.");
    return false;
  }

  // get the ThreadInfo corresponding to the hWnd's thread
  _pThreadInfo = pThreadList->GetThread(tid);
  if (_pThreadInfo == nullptr) {
    Log::Warn("The window to monitor does not belong to a monitored thread.");
    return false;
  }
  _hWnd = hWnd;

  if (!_runtime.InstallMessageHook(tid, OnMessage)) {
    DWORD lastError = ::GetLastError();
    Log::Warn(
        "Failed to set Windows hook for UI hang detection. Error code: ", lastError
    );
    return false;
  }
  _isHookInstalled = true;

  // create the watchdog thread
  try {
    auto pWatchdogThread = _runtime.StartThread([this] {
      OpSysTools::SetNativeThreadName(ThreadName);
      WatchdogLoop();
    });
    if (pWatchdogThread == nullptr || !pWatchdogThread->joinable()) {
      Log::Warn("Failed to create a joinable UI hang watchdog thread.");
      return false;
    }
    _pWatchdogThread = std::move(pWatchdogThread);
  } catch (const std::exception& error) {
    Log::Warn("Failed to create the UI hang watchdog thread: ", error.what());
    return false;
  }

  _instance.store(this, std::memory_order_release);
  return true;
}

// Releases whatever Activate() acquired; each step is skipped when already done.
void UiHangDetector::Deactivate() noexcept {
  // Stop the watchdog first: hook notifications keep reaching this detector until it
  // has exited, so it can't mistake the teardown for a hang.
  if (_pWatchdogThread != nullptr) {
    _stopRequested.store(true, std::memory_order_release);
    try {
      if (_runtime.SetEvent(_stopEvent) == FALSE) {
        WarnNoexcept(
            "Failed to signal the UI hang watchdog. Error code: ", ::GetLastError()
        );
      }
    } catch (...) {
      WarnNoexcept("Failed to signal the UI hang watchdog.");
    }

    try {
      if (_pWatchdogThread->joinable()) {
        if (_pWatchdogThread->get_id() == std::this_thread::get_id()) {
          WarnNoexcept("UI hang watchdog cannot stop itself.");
          std::terminate();
        }
        _pWatchdogThread->join();
      }
    } catch (const std::exception& error) {
      WarnNoexcept("Failed to join the UI hang watchdog: ", error.what());
      std::terminate();
    } catch (...) {
      WarnNoexcept("Failed to join the UI hang watchdog.");
      std::terminate();
    }
    _pWatchdogThread.reset();
  }

  if (_isHookInstalled) {
    try {
      if (!_runtime.RemoveMessageHook()) {
        WarnNoexcept(
            "Failed to remove the UI hang hook. Error code: ", ::GetLastError()
        );
      }
    } catch (...) {
      WarnNoexcept("Failed to remove the UI hang hook.");
    }
    _isHookInstalled = false;
  }

  // Taken last, so the mutex is never held while joining or unhooking. It waits for a
  // hook notification being processed; later ones, which can arrive even after the
  // hook was removed, find no detector.
  try {
    std::lock_guard<std::mutex> lock(_instanceMutex);
    if (_instance.load(std::memory_order_relaxed) == this) {
      _instance.store(nullptr, std::memory_order_release);
    }
  } catch (...) {
    WarnNoexcept("Failed to clear the active UI hang detector.");
  }

  CloseActiveHang();

  if (_stopEvent != nullptr) {
    try {
      if (_runtime.CloseHandle(_stopEvent) == FALSE) {
        WarnNoexcept(
            "Failed to close the UI hang stop event. Error code: ", ::GetLastError()
        );
      }
    } catch (...) {
      WarnNoexcept("Failed to close the UI hang stop event.");
    }
    _stopEvent = nullptr;
  }

  _hWnd = nullptr;
  _pThreadInfo.reset();
}

void UiHangDetector::Stop() noexcept {
  _isStopped = true;
  RecordCutoff();
  Deactivate();

  // the providers belong to the profiling session that is ending
  _pHangProvider = nullptr;
  _pRumViewContextProvider = nullptr;
}

// Waits for a hang sample being emitted by the watchdog, which emits no other one.
void UiHangDetector::RecordCutoff() noexcept {
  if (_pWatchdogThread == nullptr) {
    return;
  }

  try {
    std::lock_guard<std::mutex> lock(_stateMutex);
    _cutoffTimestamp = _runtime.GetHighPrecisionTimestamp();
    _stopRequested.store(true, std::memory_order_release);
  } catch (...) {
    WarnNoexcept("Failed to record the UI hang cutoff.");
  }
}

// Run by Stop() once the watchdog has exited and no hook notification can run anymore.
void UiHangDetector::CloseActiveHang() noexcept {
  if (_state != WatchdogState::Hang) {
    return;
  }
  _state = WatchdogState::None;

  std::chrono::nanoseconds processedTimestamp = _processedProbeTimestamp.load();
  bool isAcknowledged = _isProcessed.load() && (processedTimestamp <= _cutoffTimestamp);
  std::chrono::nanoseconds timestamp =
      isAcknowledged ? processedTimestamp : _cutoffTimestamp;
  std::optional<HangCapture> activeHang = std::move(_activeHang);
  EndHang(timestamp);

  // no start sample was emitted if its capture failed
  if (!activeHang) {
    return;
  }
  try {
    AddHangSample(
        *activeHang,
        isAcknowledged ? UiHangSampleKind::Recovered : UiHangSampleKind::ProfilerStop,
        timestamp,
        HangDurationUntil(timestamp)
    );
  } catch (...) {
    WarnNoexcept("Failed to close the active UI hang.");
  }
}

std::chrono::nanoseconds UiHangDetector::HangDurationUntil(
    std::chrono::nanoseconds timestamp
) const {
  std::chrono::nanoseconds duration = timestamp - _hangDetectionTimestamp;
  // handle race condition where the probe message was processed JUST before the hang
  // detection timestamp was set
  return (duration < 0ns) ? 1ms : duration;
}

bool UiHangDetector::CaptureHang(HangCapture& capture) {
  if (!_runtime.CaptureStack(_pThreadInfo, capture.frames)) {
    return false;
  }

  // Snapshot the current RUM view context (shared-lock, fast copy)
  capture.hasRumView = _pRumViewContextProvider->GetCurrentViewContext(capture.rumView);
  return true;
}

void UiHangDetector::AddHangSample(
    HangCapture& capture,
    UiHangSampleKind kind,
    std::chrono::nanoseconds timestamp,
    std::chrono::nanoseconds duration
) {
  Sample sample =
      Sample(timestamp, _pThreadInfo, capture.frames.data(), capture.frames.size());
  if (capture.hasRumView) {
    sample.SetRumViewContext(RumViewContext(capture.rumView));
  }
  _pHangProvider->Add(std::move(sample), duration, kind);

  Log::Debug(
      "UI hang ",
      kind == UiHangSampleKind::Detected       ? "start"
      : kind == UiHangSampleKind::ProfilerStop ? "end at profiler stop"
                                               : "end",
      " sample added, duration: ",
      duration.count() / 1000000,
      " ms"
  );

  // TODO: figure out if we want to add a Hang count RUM vital
}

// Run on the monitored window's thread, called from the message hook
void UiHangDetector::ProcessMessage(const MSG& message) {
  // notify the Detector that its latest probe message has been processed (i.e. the UI
  // should be responsive); a probe posted by another session or an older probe is
  // ignored
  if ((message.hwnd == _hWnd) && (message.message == _hangProbeMessageId) &&
      (message.wParam == static_cast<WPARAM>(_sessionId)) &&
      (static_cast<uint64_t>(message.lParam) == _probeSequence.load())) {
    auto now = _runtime.GetHighPrecisionTimestamp();
    _processedProbeTimestamp.store(now);

    _isProcessed.store(true);
  }
}

bool UiHangDetector::PostProbeMessage() {
  _state = WatchdogState::Probing;
  _postProbeTimestamp = _runtime.GetHighPrecisionTimestamp();
  _lastNoHangTimestamp = _postProbeTimestamp;

  // don't forget to reset the state to be ready to detect the next hang
  _initialHangDuration = 0ns;
  uint64_t sequence = _probeSequence.fetch_add(1) + 1;
  _processedProbeTimestamp.store(0ns);
  _isProcessed.store(false);

  if (!_runtime.PostMessageW(
          _hWnd,
          _hangProbeMessageId,
          static_cast<WPARAM>(_sessionId),
          static_cast<LPARAM>(sequence)
      )) {
    // TODO: should be map this to a hang (i.e. queue might be full)?
    DWORD lastError = ::GetLastError();
    Log::Debug(
        "Failed to post probe message for UI hang detection. Error code: ", lastError
    );

    _state = WatchdogState::None;
    _postProbeTimestamp = 0ns;
    _lastNoHangTimestamp = 0ns;
    return false;
  }

  return true;
}

// false once the cutoff is recorded: then Stop() wins and no hang is emitted
bool UiHangDetector::DetectHang(std::chrono::nanoseconds now) {
  std::lock_guard<std::mutex> lock(_stateMutex);
  if (_stopRequested.load(std::memory_order_acquire)) {
    return false;
  }

  _state = WatchdogState::Hang;
  _hangDetectionTimestamp = now;

  // from now on, we are in a hang state, so we don't want to generate Wait samples for
  // the hang duration, but we want to generate a sample for the hang start
  _pThreadInfo->SetHangDetected(true);

  // we assume that the hang started AFTER the last non-hang check
  // --> it is overcounting at most of 1/2 tick
  _initialHangDuration = now - _lastNoHangTimestamp;
  HangCapture capture;
  if (CaptureHang(capture)) {
    AddHangSample(capture, UiHangSampleKind::Detected, now, _initialHangDuration);
    _activeHang = std::move(capture);
  }
  return true;
}

// false once the cutoff is recorded: then Stop() closes the hang
bool UiHangDetector::RecoverHang() {
  std::lock_guard<std::mutex> lock(_stateMutex);
  if (_stopRequested.load(std::memory_order_acquire)) {
    return false;
  }

  // so, emit a sample for its ending
  std::chrono::nanoseconds timestamp = _processedProbeTimestamp.load();
  HangCapture capture;
  if (CaptureHang(capture)) {
    AddHangSample(
        capture, UiHangSampleKind::Recovered, timestamp, HangDurationUntil(timestamp)
    );
  }
  EndHang(timestamp);
  return true;
}

void UiHangDetector::EndHang(std::chrono::nanoseconds timestamp) {
  // reset the last timestamp to avoid overcounting the next wait sample duration
  _pThreadInfo->SetLastWaitSampleTimestamp(timestamp);

  // Wait samples are allowed again after a hang
  _pThreadInfo->SetHangDetected(false);
  _activeHang.reset();
}

// implement the watchdog loop to monitor the UI thread responsiveness
void UiHangDetector::WatchdogLoop() {
  for (;;) {
    // detect if the watchdog thread should stop/detect a hang every
    // dd_win_prof::kWatchdogTickMs
    DWORD waitResult =
        _runtime.WaitForSingleObject(_stopEvent, dd_win_prof::kWatchdogTickMs);
    if (waitResult == WAIT_OBJECT_0) {
      break;
    }
    if (waitResult != WAIT_TIMEOUT) {
      WarnNoexcept(
          "Failed to wait for the UI hang watchdog stop event. Result: ",
          waitResult,
          ", error code: ",
          ::GetLastError()
      );
      break;
    }
    // stop even if signaling the stop event failed
    if (_stopRequested.load(std::memory_order_acquire)) {
      break;
    }
    // a probe processed before activation completes would never be observed
    if (_instance.load(std::memory_order_acquire) != this) {
      continue;
    }

    // post a probe message at startup
    if (_state == WatchdogState::None) {
      PostProbeMessage();
    } else if (_state == WatchdogState::Probing) {
      // previous posted probe message has been processed by the UI thread
      auto isProcessed = _isProcessed.load();
      if (isProcessed) {
        // post a new probe message to continue monitoring the UI thread responsiveness
        PostProbeMessage();
      } else {
        // check for hang
        auto now = _runtime.GetHighPrecisionTimestamp();
        auto probingDuration = now - _postProbeTimestamp;
        if (probingDuration >= dd_win_prof::kHangThresholdMs) {
          if (!DetectHang(now)) {
            break;
          }
        } else {
          // no hang detected yet, but we are still probing the UI thread responsiveness
          _lastNoHangTimestamp = now;
        }
      }
    } else if (_state == WatchdogState::Hang) {
      // check if hang is over
      auto isProcessed = _isProcessed.load();
      if (isProcessed) {
        if (!RecoverHang()) {
          break;
        }

        // post a new probe message to continue monitoring the UI thread responsiveness
        PostProbeMessage();
      } else {
        // TODO: should we emit a hang sample on a regular basis to avoid missing a
        // looong one in a profile?
      }
    }
  }
}
