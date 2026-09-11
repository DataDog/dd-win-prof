// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "UiHangDetector.h"

#include "Log.h"
#include "ProfilingConstants.h"
#include "pch.h"

UiHangDetector* UiHangDetector::_this = nullptr;
constexpr const wchar_t* ThreadName = L"DD_UI_Hang";

UiHangDetector::UiHangDetector(
    UINT hangProbeMessageId,
    UiHangProvider* pHangProvider,
    IRumViewContextProvider* pRumViewContextProvider
)
    : _hangProbeMessageId(hangProbeMessageId),
      _pHangProvider(pHangProvider),
      _pRumViewContextProvider(pRumViewContextProvider),
      _hWnd(nullptr),
      _hGetMessageHook(nullptr),
      _pWatchdogThread(nullptr),
      _stopEvent(nullptr),
      _state(WatchdogState::None),
      _hangDetectionTimestamp(0ns),
      _initialHangDuration(0ns),
      _postProbeTimestamp(0ns),
      _processedProbeTimestamp(0ns) {
  UiHangDetector::_this = this;

  // manual reset event set to stop the watchdog thread
  _stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

UiHangDetector::~UiHangDetector() { Stop(); }

LRESULT CALLBACK GetMsgProc(int code, WPARAM wParam, LPARAM lParam) {
  if (UiHangDetector::_this == nullptr) {
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
  }

  if (code < 0) {
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
  }

  // forward to the current instance of UiHangDetector
  UiHangDetector::_this->ProcessHook(code, wParam, lParam);

  return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

bool UiHangDetector::MonitorWindowHangs(HWND hWnd, ThreadList* pThreadList) {
  if (hWnd == nullptr) {
    return false;
  }
  if (IsWindow(hWnd) == FALSE) {
    Log::Warn("The window handle to monitor is invalid.");
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

  DWORD pid = 0;
  DWORD tid = ::GetWindowThreadProcessId(hWnd, &pid);
  if (tid == 0) {
    return false;
  }
  if (pid != ::GetCurrentProcessId()) {
    Log::Warn("The window to monitor does not belong to the current process.");
    return false;
  }

  _hWnd = hWnd;

  // get the ThreadInfo corresponding to the hWnd's thread
  _pThreadInfo = pThreadList->GetThread(tid);
  if (_pThreadInfo == nullptr) {
    Log::Warn("The window to monitor does not belong to a monitored thread.");
    return false;
  }

  // Thread-specific hook on the window's UI thread. A global hook
  // (dwThreadId == 0) would inject this DLL into every GUI process on the
  // desktop, locking the file so rebuilds of consumers (e.g. UIApp) fail.
  // hMod is NULL because the hook procedure lives in this process.
  _hGetMessageHook = ::SetWindowsHookExW(WH_GETMESSAGE, GetMsgProc, nullptr, tid);
  if (_hGetMessageHook == NULL) {
    DWORD lastError = ::GetLastError();
    Log::Warn(
        "Failed to set Windows hook for UI hang detection. Error code: ", lastError
    );
    return false;
  }

  // create the watchdog thread
  _pWatchdogThread = std::make_unique<std::thread>([this] {
    OpSysTools::SetNativeThreadName(ThreadName);
    WatchdogLoop();
  });

  return true;
}

void UiHangDetector::Stop() {
  // Should be called only once
  if (_stopEvent == nullptr) {
    return;
  }

  // wait for the watchdog thread to exit
  ::SetEvent(_stopEvent);
  if (_pWatchdogThread != nullptr) {
    try {
      _pWatchdogThread->join();
      _pWatchdogThread.reset();
    } catch (const std::exception&) {
    }
  }
  ::CloseHandle(_stopEvent);
  _stopEvent = nullptr;

  // unregister the Windows hook
  if (_hGetMessageHook != nullptr) {
    ::UnhookWindowsHookEx(_hGetMessageHook);
    _hGetMessageHook = nullptr;
  }

  UiHangDetector::_this = nullptr;
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
      auto now = OpSysTools::GetHighPrecisionTimestamp();
      _processedProbeTimestamp.store(now);

      _state.store(WatchdogState::Processed);
    }
  }
}

bool UiHangDetector::PostProbeMessage() {
  _state.store(WatchdogState::Probing);
  _postProbeTimestamp = OpSysTools::GetHighPrecisionTimestamp();

  if (::PostMessageW(_hWnd, _hangProbeMessageId, 0, 0) == FALSE) {
    // TODO: should be map this to a hang (i.e. queue might be full)?
    DWORD lastError = ::GetLastError();
    Log::Debug(
        "Failed to post probe message for UI hang detection. Error code: ", lastError
    );
    return false;
  }

  return true;
}

// implement the watchdog loop to monitor the UI thread responsiveness
void UiHangDetector::WatchdogLoop() {
  for (;;) {
    // detect if the watchdog thread should stop/detect a hang every
    // dd_win_prof::kWatchdogTickMs
    if (WaitForSingleObject(_stopEvent, dd_win_prof::kWatchdogTickMs) ==
        WAIT_OBJECT_0) {
      break;
    }

    // post a probe message at startup
    if (_state.load() == WatchdogState::None) {
      PostProbeMessage();
    } else if (_state.load() == WatchdogState::Probing) {
      auto now = OpSysTools::GetHighPrecisionTimestamp();
      auto probingDuration = now - _postProbeTimestamp;
      if (probingDuration >= dd_win_prof::kHangThresholdMs) {
        // there could be a race condition here if the probe message was just processed
        if (_state.load() == WatchdogState::Processed) {
          // TODO: no hang or generate start/stop samples for a short hang?

          // post a new probe message to continue monitoring the UI thread
          // responsiveness
          PostProbeMessage();
        } else {
          _state.store(WatchdogState::Hang);
          _hangDetectionTimestamp = now;

          // we assume that the hang started when the probe message is sent: it is
          // overcounting at most of 1 tick
          _initialHangDuration = probingDuration;
          AddHangSample(true, now, probingDuration);
        }
      } else {
        // no hang detected yet, but we are still probing the UI thread responsiveness
      }
    } else if (_state.load() == WatchdogState::Processed) {
      // we are here AFTER a tick and the probe message was processed
      // since the last tick, so we can assume that the UI thread is responsive again

      // a hang was detected since the last tick...
      if (_initialHangDuration > 0ns) {
        // ...so emit a sample for its ending
        std::chrono::nanoseconds timestamp = _processedProbeTimestamp.load();
        AddHangSample(false, timestamp, timestamp - _hangDetectionTimestamp);

        // don't forget to reset the state to be ready to detect the next hang
        _initialHangDuration = 0ns;
        _processedProbeTimestamp.store(0ns);
      }

      // post a new probe message to continue monitoring the UI thread responsiveness
      PostProbeMessage();
    } else if (_state.load() == WatchdogState::Hang) {
      // nothing to do before the end of the hang...
      // TODO: should we emit a hang sample on a regular basis to avoid missing a looong
      // one in a profile?
    }
  }
}
