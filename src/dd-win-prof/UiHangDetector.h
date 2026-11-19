// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <functional>
#include <thread>

#include "ProfilingConstants.h"
#include "StackFrameCollector.h"
#include "ThreadList.h"
#include "UiHangProvider.h"
#include "pch.h"

// Win32 services used by UiHangDetector. Tests provide their own implementation to
// inject failures and to control how hook callbacks, probes and teardown interleave.
class IUiHangDetectorPlatform {
 public:
  virtual ~IUiHangDetectorPlatform() = default;

  // manual-reset event, initially non-signaled; nullptr on failure
  virtual HANDLE CreateStopEvent() = 0;
  virtual BOOL SetEvent(HANDLE hEvent) = 0;
  virtual DWORD WaitForSingleObject(HANDLE hHandle, DWORD milliseconds) = 0;
  virtual BOOL CloseHandle(HANDLE hHandle) = 0;

  virtual BOOL IsWindow(HWND hWnd) = 0;
  virtual DWORD GetWindowThreadProcessId(HWND hWnd, DWORD* pProcessId) = 0;

  // WH_GETMESSAGE hook on the given thread; nullptr on failure
  virtual HHOOK SetGetMessageHook(HOOKPROC hookProc, DWORD threadId) = 0;
  virtual BOOL UnhookWindowsHookEx(HHOOK hHook) = 0;

  virtual BOOL PostMessageW(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) = 0;

  // throws like the std::thread constructor when the thread cannot be created
  virtual std::unique_ptr<std::thread> StartThread(
      std::function<void()> threadProc
  ) = 0;

  virtual std::chrono::nanoseconds GetHighPrecisionTimestamp() = 0;
};

// Detects hangs of a window by posting probe messages from a watchdog thread and
// observing them from a WH_GETMESSAGE hook on the window's thread.
//
// Hook callbacks reach a detector only through the static _this router. Activation
// creates the stop event, validates the window and its thread, installs the hook and
// starts the watchdog before publishing _this; a failed stage releases the earlier ones
// and leaves the detector ready for another attempt. Stop() withdraws _this under
// _instanceMutex, then unhooks, signals and joins the watchdog without holding it,
// before closing the event and dropping its references. Stop() is idempotent and a
// stopped detector cannot monitor again. MonitorWindowHangs() and Stop() must not run
// concurrently.
class UiHangDetector {
 public:
  UiHangDetector(
      UINT hangProbeMessageId,
      UiHangProvider* pHangProvider,
      IRumViewContextProvider* pRumViewContextProvider,
      IUiHangDetectorPlatform& platform = GetWin32Platform()
  );
  ~UiHangDetector();

  bool MonitorWindowHangs(HWND hWnd, ThreadList* pThreadList);
  void Stop();

  static IUiHangDetectorPlatform& GetWin32Platform();

 private:
  friend class UiHangDetectorLifetimeTests;

  static LRESULT CALLBACK GetMsgProc(int code, WPARAM wParam, LPARAM lParam);

  bool Activate(HWND hWnd, ThreadList* pThreadList);
  bool Publish();
  void Withdraw();
  void Deactivate();
  void ProcessHook(int code, WPARAM wParam, LPARAM lParam);
  void WatchdogLoop();
  bool PostProbeMessage();
  void AddHangSample(
      bool startHang,
      std::chrono::nanoseconds timestamp,
      std::chrono::nanoseconds duration
  );

 private:
  enum class WatchdogState : uint8_t {
    None,     // start
    Probing,  // a probe message has been posted
    Hang      // a hang is detected
  };

 private:
  static const int MaxFrameCount = dd_win_prof::kMaxStackDepth;

  // the detector receiving the Windows hook callbacks, if any
  static UiHangDetector* _this;
  // held by the hook callback while it uses _this, so that withdrawing _this also waits
  // for a callback already running on the detector
  static std::mutex _instanceMutex;

  IUiHangDetectorPlatform& _platform;
  UINT _hangProbeMessageId;
  HWND _hWnd;
  UiHangProvider* _pHangProvider = nullptr;
  IRumViewContextProvider* _pRumViewContextProvider = nullptr;

  StackFrameCollector _stackFrameCollector;
  std::shared_ptr<ThreadInfo> _pThreadInfo = nullptr;
  HHOOK _hGetMessageHook;
  HANDLE _stopEvent;
  std::unique_ptr<std::thread> _pWatchdogThread = nullptr;

  // mirrors (_this == this) for the watchdog, which never takes _instanceMutex: a probe
  // posted before publication would be processed without being observed
  std::atomic<bool> _isRouted;

  // read/write by the watchdog thread and write by the hook
  std::atomic<bool> _isProcessed;

  // read/write only by the watchdog thread (not touched by the hook)
  WatchdogState _state;

  // timestamp when the probe message was processed
  std::atomic<std::chrono::nanoseconds> _processedProbeTimestamp;

  // timestamp when the probe message was posted
  std::chrono::nanoseconds _postProbeTimestamp;

  // timestamp when a tick happened still probing but without hang
  std::chrono::nanoseconds _lastNoHangTimestamp;

  // timestamp when the hang was detected
  std::chrono::nanoseconds _hangDetectionTimestamp;

  // computed when the hang is detected and associated to the start hang sample
  // --> don't double count it in the hang stop sample duration
  std::chrono::nanoseconds _initialHangDuration;
};
