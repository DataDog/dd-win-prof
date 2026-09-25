// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "UiHangDetector.h"

#include "Log.h"
#include "ProfilingConstants.h"
#include "pch.h"

UiHangDetector* UiHangDetector::_this = nullptr;
std::mutex UiHangDetector::_instanceMutex;
std::atomic<WPARAM> UiHangDetector::_nextProbeId{1};
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
  // manual reset event set to stop the watchdog thread
  _stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

UiHangDetector::~UiHangDetector() { Stop(); }

LRESULT CALLBACK UiHangDetector::GetMsgProc(int code, WPARAM wParam, LPARAM lParam) {
  if (code >= 0) {
    // UnhookWindowsHookEx can return while a callback is still executing.
    std::lock_guard<std::mutex> lock(_instanceMutex);
    if (_this != nullptr) {
      _this->ProcessHook(code, wParam, lParam);
    }
  }
  return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

bool UiHangDetector::MonitorWindowHangs(HWND hWnd, ThreadList* pThreadList) {
  if (hWnd == nullptr || _stopEvent == nullptr) {
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

  // get the ThreadInfo corresponding to the hWnd's thread
  _pThreadInfo = pThreadList->GetThread(tid);
  if (_pThreadInfo == nullptr) {
    Log::Warn("The window to monitor does not belong to a monitored thread.");
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(_instanceMutex);
    if (_this != nullptr) {
      return false;
    }
    _hWnd = hWnd;
    _this = this;
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

    std::lock_guard<std::mutex> lock(_instanceMutex);
    _this = nullptr;
    _hWnd = nullptr;
    _pThreadInfo = nullptr;
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

  {
    std::lock_guard<std::mutex> lock(_instanceMutex);
    if (_this == this) {
      _this = nullptr;
    }
  }

  if (_state == WatchdogState::Hang) {
    auto timestamp = _processedProbeTimestamp.load();
    EndHang(timestamp == 0ns ? OpSysTools::GetHighPrecisionTimestamp() : timestamp);
  }
  _hWnd = nullptr;
}

std::shared_ptr<const ThreadInfo::HangCallstack> UiHangDetector::CaptureHangCallstack(
) {
  // get the callstack of the hang thread
  CONTEXT seedContext;
  if (!_stackFrameCollector.TrySuspendThread(_pThreadInfo, seedContext)) {
    return nullptr;
  }

  bool isTruncated = false;
  uint64_t frames[MaxFrameCount];
  uint16_t framesCount = MaxFrameCount;
  bool isStackCaptured = _stackFrameCollector.CaptureStack(
      _pThreadInfo->GetOsThreadHandle(), seedContext, frames, framesCount, isTruncated
  );
  // resume the thread before doing any allocation that could cause a deadlock
  ::ResumeThread(_pThreadInfo->GetOsThreadHandle());

  if (!isStackCaptured || framesCount == 0) {
    return nullptr;
  }

  if (isTruncated) {
    frames[framesCount - 1] = 0;
  }
  return std::make_shared<const ThreadInfo::HangCallstack>(
      frames, frames + framesCount
  );
}

void UiHangDetector::AddHangSample(
    bool startHang,
    std::chrono::nanoseconds timestamp,
    std::chrono::nanoseconds duration
) {
  if (startHang) {
    _hangCallstack = CaptureHangCallstack();
  }
  if (!_hangCallstack) {
    return;
  }

  // Snapshot the current RUM view context (shared-lock, fast copy)
  RumViewContext rumView;
  bool hasRumView = _pRumViewContextProvider->GetCurrentViewContext(rumView);

  // create the sample
  Sample sample(
      timestamp, _pThreadInfo, _hangCallstack->data(), _hangCallstack->size()
  );
  if (hasRumView) {
    sample.SetRumViewContext(std::move(rumView));
  }
  _pHangProvider->Add(std::move(sample), duration, startHang);
  if (startHang) {
    _pThreadInfo->SetHangCallstack(_hangCallstack);
  }

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
    if ((m != nullptr) && (m->hwnd == _hWnd) && (m->message == _hangProbeMessageId) &&
        (m->wParam == _probeId.load())) {
      std::lock_guard<std::mutex> lock(_probeMutex);
      _processedProbeTimestamp.store(OpSysTools::GetHighPrecisionTimestamp());
    }
  }
}

bool UiHangDetector::PostProbeMessage() {
  _state = WatchdogState::Probing;
  _postProbeTimestamp = OpSysTools::GetHighPrecisionTimestamp();
  _lastNoHangTimestamp = _postProbeTimestamp;

  // don't forget to reset the state to be ready to detect the next hang
  _initialHangDuration = 0ns;
  _processedProbeTimestamp.store(0ns);
  // Ignore probes left in the queue by an earlier monitoring registration.
  auto probeId = _nextProbeId.fetch_add(1);
  _probeId.store(probeId);

  if (!::PostMessageW(_hWnd, _hangProbeMessageId, probeId, 0)) {
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

bool UiHangDetector::TryDetectHang(std::chrono::nanoseconds now) {
  std::lock_guard<std::mutex> lock(_probeMutex);
  if (_processedProbeTimestamp.load() != 0ns) {
    return false;
  }
  if (now - _postProbeTimestamp < dd_win_prof::kHangThresholdMs) {
    _lastNoHangTimestamp = now;
    return false;
  }

  _state = WatchdogState::Hang;
  _hangDetectionTimestamp = now;
  _initialHangDuration = now - _lastNoHangTimestamp;
  return true;
}

void UiHangDetector::EndHang(std::chrono::nanoseconds timestamp) {
  AddHangSample(false, timestamp, timestamp - _hangDetectionTimestamp);
  if (_hangCallstack) {
    _pThreadInfo->EndHang(timestamp);
    _hangCallstack.reset();
  }
  _state = WatchdogState::None;
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
    if (_state == WatchdogState::None) {
      PostProbeMessage();
    } else if (_state == WatchdogState::Probing) {
      // previous posted probe message has been processed by the UI thread
      if (_processedProbeTimestamp.load() != 0ns) {
        PostProbeMessage();
      } else if (TryDetectHang(OpSysTools::GetHighPrecisionTimestamp())) {
        AddHangSample(true, _hangDetectionTimestamp, _initialHangDuration);
      }
    } else if (_state == WatchdogState::Hang) {
      // check if hang is over
      auto timestamp = _processedProbeTimestamp.load();
      if (timestamp != 0ns) {
        EndHang(timestamp);
        PostProbeMessage();
      } else {
        // TODO: should we emit a hang sample on a regular basis to avoid missing a
        // looong one in a profile?
      }
    }
  }
}
