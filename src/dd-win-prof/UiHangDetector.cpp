// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"

#include "UiHangDetector.h"
#include "ProfilingConstants.h"
#include "Log.h"

UiHangDetector* UiHangDetector::_this = nullptr;
constexpr const wchar_t* ThreadName = L"DD_UIHang";

UiHangDetector::UiHangDetector(
    HMODULE hModule,
    UINT hangProbeMessageId,
    ThreadList* pThreadList,
    UiHangProvider* pHangProvider
)
  :
  _hModule(hModule),
  _hangProbeMessageId(hangProbeMessageId),
  _pThreadList(pThreadList),
  _pHangProvider(pHangProvider),
  _hWnd(nullptr),
  _hGetMessageHook(nullptr),
  _pWatchdogThread(nullptr),
  _stopEvent(nullptr),
  _state(WatchdogState::None)
  {
  // manual reset event set to stop the watchdog thread
  _stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

UiHangDetector::~UiHangDetector() {
    Stop();
}

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

bool UiHangDetector::MonitorWindowHangs(HWND hWnd) {
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

  if (_pThreadList == nullptr) {
    Log::Debug("Null ThreadList passed to UiHangDetector.");
    return false;
  }

  DWORD pid = 0;
  DWORD tid = ::GetWindowThreadProcessId(_hWnd, &pid);
  if (tid == 0) {
    return false;
  }
  if (pid != ::GetCurrentProcessId()) {
    Log::Warn("The window to monitor does not belong to the current process.");
    return false;
  }

  _hWnd = hWnd;

  // TODO: get the ThreadInfo corresponding to the hWnd's thread
  // _pThreadList->GetThreadInfo(tid)

  // register the Windows hook
  HHOOK hGetMessageHook = ::SetWindowsHookExW(WH_GETMESSAGE, GetMsgProc, _hModule, 0);
  if (hGetMessageHook == NULL) {
    DWORD lastError = ::GetLastError();
    Log::Warn("Failed to set Windows hook for UI hang detection. Error code: %lu", lastError);
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
    }
    catch (const std::exception&) {
    }
  }
  ::CloseHandle(_stopEvent);
  _stopEvent = nullptr;

  // unregister the Windows hook
  if (_hGetMessageHook != nullptr) {
    ::UnhookWindowsHookEx(_hGetMessageHook);
    _hGetMessageHook = nullptr;
  }
}

// Run on the monitored window's thread, called from the Windows hook callback
void UiHangDetector::ProcessHook(int code, WPARAM wParam, LPARAM lParam) {
  // notify the Detector that the probe message has been processed (i.e. the UI should be responsive)
  if (code == HC_ACTION && wParam == PM_REMOVE) {
    const MSG* m = reinterpret_cast<const MSG*>(lParam);
    if (
        (m != nullptr)
        && (m->hwnd == _hWnd)
        && (m->message == _hangProbeMessageId)
       ) {
      auto now = OpSysTools::GetHighPrecisionTimestamp();
      _processedProbeTimestamp.store(now);

      _state.store(WatchdogState::Processed);
    }
  }
}

bool UiHangDetector::PostProbeMessage() {
   if (::PostMessageW(_hWnd, _hangProbeMessageId, 0, 0) == FALSE) {
     // TODO: should be map this to a hang (i.e. queue might be full)?
     DWORD lastError = ::GetLastError();
     Log::Debug("Failed to post probe message for UI hang detection. Error code: %lu", lastError);
     return false;
   }

   _postProbeTimestamp = OpSysTools::GetHighPrecisionTimestamp();
   _state.store(WatchdogState::Probing);
   return true;
}

// implement the watchdog loop to monitor the UI thread responsiveness
void UiHangDetector::WatchdogLoop() {

  for (;;) {
    // detect if the watchdog thread should stop/detect a hang every dd_win_prof::kWatchdogTickMs
    if (WaitForSingleObject(_stopEvent, dd_win_prof::kWatchdogTickMs) == WAIT_OBJECT_0) {
      break;
    }

    // post a probe message at startup
    if (_state == WatchdogState::None) {
      PostProbeMessage();
    }
    else if (_state == WatchdogState::Probing) {
      auto now = OpSysTools::GetHighPrecisionTimestamp();
      auto probingDuration = now - _postProbeTimestamp;
      if (probingDuration >= dd_win_prof::kHangThresholdMs) {
        _state.store(WatchdogState::Hang);
        // TODO: add a hang wait sample
        //       --> need to extract the stackwalk code from StackSamplerLoop::CollectOneThreadSample
        // _pHangProvider->Add(sample, probingDuration, true)
        _hangDetectionTimestamp = now;
        _initialHangDuration = probingDuration;
      }
      else {
        // TODO: optimization?
        // sleep for the remaining duration before the hang threshold would be reached
        // to avoid missing the first milliseconds of the hang.
        // with the current implementation, 50 + 50 (= 2x tick) = 100 (=threshold), so should not miss
        // but could be a problem if tick is no more a divider of threshold
      }
    }
    else if (_state == WatchdogState::Processed) {
      auto now = OpSysTools::GetHighPrecisionTimestamp();

      // we don't want to flood the queue if there was no hang but still keep the same tick
      if (_initialHangDuration == 0ns) {
        ::Sleep(dd_win_prof::kWatchdogTickMs - (now - _postProbeTimestamp).count() / 1000000);
        PostProbeMessage();
        continue;
      }

      // TODO: there was a hang already detected so emit a sample for its ending
    }
    else if (_state == WatchdogState::Hang) {
      // nothing to do before the end of the hang...
      // TODO: should we emit a hang sample on a regular basis to avoid missing a looong one in a profile?
    }
  }
}
