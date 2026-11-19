// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "UiHangDetector.h"

#include "Log.h"
#include "OpSysTools.h"
#include "ProfilingConstants.h"
#include "pch.h"

UiHangDetector* UiHangDetector::_this = nullptr;
std::mutex UiHangDetector::_instanceMutex;

constexpr const wchar_t* ThreadName = L"DD_UI_Hang";

namespace {
class Win32UiHangDetectorPlatform final : public IUiHangDetectorPlatform {
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

  HHOOK SetGetMessageHook(HOOKPROC hookProc, DWORD threadId) override {
    // Thread-specific hook on the window's UI thread. A global hook
    // (dwThreadId == 0) would inject this DLL into every GUI process on the
    // desktop, locking the file so rebuilds of consumers (e.g. UIApp) fail.
    // hMod is NULL because the hook procedure lives in this process.
    return ::SetWindowsHookExW(WH_GETMESSAGE, hookProc, nullptr, threadId);
  }

  BOOL UnhookWindowsHookEx(HHOOK hHook) override {
    return ::UnhookWindowsHookEx(hHook);
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
};
}  // namespace

IUiHangDetectorPlatform& UiHangDetector::GetWin32Platform() {
  static Win32UiHangDetectorPlatform platform;
  return platform;
}

UiHangDetector::UiHangDetector(
    UINT hangProbeMessageId,
    UiHangProvider* pHangProvider,
    IRumViewContextProvider* pRumViewContextProvider,
    IUiHangDetectorPlatform& platform
)
    : _platform(platform),
      _hangProbeMessageId(hangProbeMessageId),
      _hWnd(nullptr),
      _pHangProvider(pHangProvider),
      _pRumViewContextProvider(pRumViewContextProvider),
      _hGetMessageHook(nullptr),
      _stopEvent(nullptr),
      _pWatchdogThread(nullptr),
      _isRouted(false),
      _isProcessed(false),
      _state(WatchdogState::None),
      _processedProbeTimestamp(0ns),
      _postProbeTimestamp(0ns),
      _lastNoHangTimestamp(0ns),
      _hangDetectionTimestamp(0ns),
      _initialHangDuration(0ns) {}

UiHangDetector::~UiHangDetector() { Stop(); }

LRESULT CALLBACK UiHangDetector::GetMsgProc(int code, WPARAM wParam, LPARAM lParam) {
  if (code < 0) {
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
  }

  // don't keep the lock when calling CallNextHookEx that could trigger a reentrency
  // issue
  {
    std::lock_guard<std::mutex> lock(_instanceMutex);

    // forward to the current instance of UiHangDetector
    if (_this != nullptr) {
      _this->ProcessHook(code, wParam, lParam);
    }
  }

  return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

bool UiHangDetector::MonitorWindowHangs(HWND hWnd, ThreadList* pThreadList) {
  if (hWnd == nullptr) {
    return false;
  }

  // we can't monitor more than one window at a time
  if (_hWnd != nullptr) {
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

  if (!Activate(hWnd, pThreadList)) {
    // release what the successful stages acquired so that a later call can succeed
    Deactivate();
    return false;
  }

  return true;
}

// Returns false at the first stage that fails; this detector is published last, once
// everything the hook callback and the watchdog rely on exists.
bool UiHangDetector::Activate(HWND hWnd, ThreadList* pThreadList) {
  // manual reset event set to stop the watchdog thread
  _stopEvent = _platform.CreateStopEvent();
  if (_stopEvent == nullptr) {
    Log::Warn("Failed to create the stop event.");
    return false;
  }

  if (_platform.IsWindow(hWnd) == FALSE) {
    Log::Warn("The window handle to monitor is invalid.");
    return false;
  }

  DWORD pid = 0;
  DWORD tid = _platform.GetWindowThreadProcessId(hWnd, &pid);
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

  _hGetMessageHook = _platform.SetGetMessageHook(GetMsgProc, tid);
  if (_hGetMessageHook == nullptr) {
    DWORD lastError = ::GetLastError();
    Log::Warn(
        "Failed to set Windows hook for UI hang detection. Error code: ", lastError
    );
    return false;
  }

  // create the watchdog thread
  try {
    _pWatchdogThread = _platform.StartThread([this] {
      OpSysTools::SetNativeThreadName(ThreadName);
      WatchdogLoop();
    });
  } catch (const std::exception& error) {
    Log::Warn("Failed to create the UI hang watchdog thread: ", error.what());
    return false;
  }

  return Publish();
}

bool UiHangDetector::Publish() {
  {
    std::lock_guard<std::mutex> lock(_instanceMutex);

    // there is a single router: never take over the callbacks of another detector
    if (_this == nullptr) {
      _this = this;
      _isRouted.store(true, std::memory_order_release);
      return true;
    }
  }

  Log::Warn("Impossible to monitor more than one window for UI hang detection.");
  return false;
}

void UiHangDetector::Withdraw() {
  // waits for a hook callback that is already running on this detector
  std::lock_guard<std::mutex> lock(_instanceMutex);

  if (_this == this) {
    _this = nullptr;
  }
  _isRouted.store(false, std::memory_order_release);
}

// Releases whatever Activate() acquired; each step is skipped when already done.
void UiHangDetector::Deactivate() {
  // no hook callback can reach this detector once this returns
  Withdraw();

  // the router lock is not held from here: a callback entering the hook meanwhile
  // finds no detector and returns
  if (_hGetMessageHook != nullptr) {
    _platform.UnhookWindowsHookEx(_hGetMessageHook);
    _hGetMessageHook = nullptr;
  }

  // the watchdog uses the stop event, the window and the thread until it exits
  if (_pWatchdogThread != nullptr) {
    _platform.SetEvent(_stopEvent);
    _pWatchdogThread->join();
    _pWatchdogThread.reset();
  }

  if (_stopEvent != nullptr) {
    _platform.CloseHandle(_stopEvent);
    _stopEvent = nullptr;
  }

  _hWnd = nullptr;
  _pThreadInfo.reset();
}

void UiHangDetector::Stop() {
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

// Run on the monitored window's thread, called from the Windows hook callback
void UiHangDetector::ProcessHook(int code, WPARAM wParam, LPARAM lParam) {
  // notify the Detector that the probe message has been processed (i.e. the UI should
  // be responsive)
  if (code == HC_ACTION && wParam == PM_REMOVE) {
    const MSG* m = reinterpret_cast<const MSG*>(lParam);
    if ((m != nullptr) && (m->hwnd == _hWnd) && (m->message == _hangProbeMessageId)) {
      auto now = _platform.GetHighPrecisionTimestamp();
      _processedProbeTimestamp.store(now);

      _isProcessed.store(true);
    }
  }
}

bool UiHangDetector::PostProbeMessage() {
  _state = WatchdogState::Probing;
  _postProbeTimestamp = _platform.GetHighPrecisionTimestamp();
  _lastNoHangTimestamp = _postProbeTimestamp;

  // don't forget to reset the state to be ready to detect the next hang
  _initialHangDuration = 0ns;
  _processedProbeTimestamp.store(0ns);
  _isProcessed.store(false);

  if (!_platform.PostMessageW(_hWnd, _hangProbeMessageId, 0, 0)) {
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
    if (_platform.WaitForSingleObject(_stopEvent, dd_win_prof::kWatchdogTickMs) ==
        WAIT_OBJECT_0) {
      break;
    }

    // post a probe message at startup, once the hook callbacks reach this detector
    if (_state == WatchdogState::None) {
      if (_isRouted.load(std::memory_order_acquire)) {
        PostProbeMessage();
      }
    } else if (_state == WatchdogState::Probing) {
      // previous posted probe message has been processed by the UI thread
      auto isProcessed = _isProcessed.load();
      if (isProcessed) {
        // post a new probe message to continue monitoring the UI thread responsiveness
        PostProbeMessage();
      } else {
        // check for hang
        auto now = _platform.GetHighPrecisionTimestamp();
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
