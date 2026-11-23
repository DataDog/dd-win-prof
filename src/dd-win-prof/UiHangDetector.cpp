// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "UiHangDetector.h"

#include "Log.h"
#include "OpSysTools.h"
#include "ProfilingConstants.h"
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
    UINT hangProbeMessageId,
    UiHangProvider* pHangProvider,
    IRumViewContextProvider* pRumViewContextProvider
)
    : UiHangDetector(
          hangProbeMessageId, pHangProvider, pRumViewContextProvider, g_win32Runtime
      ) {}

UiHangDetector::UiHangDetector(
    UINT hangProbeMessageId,
    UiHangProvider* pHangProvider,
    IRumViewContextProvider* pRumViewContextProvider,
    IUiHangDetectorRuntime& runtime
)
    : _runtime(runtime),
      _hangProbeMessageId(hangProbeMessageId),
      _hWnd(nullptr),
      _pHangProvider(pHangProvider),
      _pRumViewContextProvider(pRumViewContextProvider),
      _isHookInstalled(false),
      _stopEvent(nullptr),
      _pWatchdogThread(nullptr),
      _stopRequested(false),
      _isStopped(false),
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
  Deactivate();

  // the providers belong to the profiling session that is ending
  _pHangProvider = nullptr;
  _pRumViewContextProvider = nullptr;
}

void UiHangDetector::AddHangSample(
    bool startHang,
    std::chrono::nanoseconds timestamp,
    std::chrono::nanoseconds duration
) {
  // get the callstack of the hang thread
  CONTEXT seedContext;
  if (!_stackFrameCollector.TrySuspendThread(_pThreadInfo, seedContext)) {
    return;
  }

  bool isTruncated = false;
  uint64_t frames[MaxFrameCount];
  uint16_t framesCount = MaxFrameCount;
  bool isStackCaptured = _stackFrameCollector.CaptureStack(
      _pThreadInfo->GetOsThreadHandle(), seedContext, frames, framesCount, isTruncated
  );
  // resume the thread before doing any allocation that could cause a deadlock
  ::ResumeThread(_pThreadInfo->GetOsThreadHandle());

  if (!isStackCaptured) {
    return;
  }

  // set a null address for the last frame in case of truncated stack
  if (isTruncated) {
    frames[framesCount - 1] = 0;
  }

  // Snapshot the current RUM view context (shared-lock, fast copy)
  RumViewContext rumView;
  bool hasRumView = _pRumViewContextProvider->GetCurrentViewContext(rumView);

  // create the sample
  Sample sample = Sample(timestamp, _pThreadInfo, frames, framesCount);
  if (hasRumView) {
    sample.SetRumViewContext(std::move(rumView));
  }
  _pHangProvider->Add(std::move(sample), duration, startHang);

  Log::Debug(
      "UI hang ",
      startHang ? "start" : "end",
      " sample added, duration: ",
      duration.count() / 1000000,
      " ms"
  );

  // TODO: figure out if we want to add a Hang count RUM vital
}

// Run on the monitored window's thread, called from the message hook
void UiHangDetector::ProcessMessage(const MSG& message) {
  // notify the Detector that the probe message has been processed (i.e. the UI should
  // be responsive)
  if ((message.hwnd == _hWnd) && (message.message == _hangProbeMessageId)) {
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
  _processedProbeTimestamp.store(0ns);
  _isProcessed.store(false);

  if (!_runtime.PostMessageW(_hWnd, _hangProbeMessageId, 0, 0)) {
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
          _state = WatchdogState::Hang;
          _hangDetectionTimestamp = now;

          // from now on, we are in a hang state, so we don't want to generate Wait
          // samples for the hang duration, but we want to generate a sample for the
          // hang start
          _pThreadInfo->SetHangDetected(true);

          // we assume that the hang started AFTER the last non-hang check
          // --> it is overcounting at most of 1/2 tick
          _initialHangDuration = now - _lastNoHangTimestamp;
          AddHangSample(true, now, _initialHangDuration);
        } else {
          // no hang detected yet, but we are still probing the UI thread responsiveness
          _lastNoHangTimestamp = now;
        }
      }
    } else if (_state == WatchdogState::Hang) {
      // check if hang is over
      auto isProcessed = _isProcessed.load();
      if (isProcessed) {
        // so, emit a sample for its ending
        std::chrono::nanoseconds timestamp = _processedProbeTimestamp.load();
        std::chrono::nanoseconds hangDuration = timestamp - _hangDetectionTimestamp;
        if (hangDuration < 0ns) {
          // handle race condition where the probe message was processed JUST before the
          // hang detection timestamp was set
          hangDuration = 1ms;
        }
        AddHangSample(false, timestamp, hangDuration);

        // reset the last timestamp to avoid overcounting the next wait sample duration
        _pThreadInfo->SetLastWaitSampleTimestamp(timestamp);

        // Wait samples are allowed again after a hang
        _pThreadInfo->SetHangDetected(false);

        // post a new probe message to continue monitoring the UI thread responsiveness
        PostProbeMessage();
      } else {
        // TODO: should we emit a hang sample on a regular basis to avoid missing a
        // looong one in a profile?
      }
    }
  }
}
