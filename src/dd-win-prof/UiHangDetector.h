// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <atomic>
#include <functional>
#include <thread>

#include "ProfilingConstants.h"
#include "StackFrameCollector.h"
#include "ThreadList.h"
#include "UiHangProvider.h"
#include "pch.h"

// Runtime services used by UiHangDetector. Tests provide their own implementation to
// inject failures and to control how hook notifications, probes and teardown
// interleave.
class IUiHangDetectorRuntime {
 public:
  // receives each message that the hooked thread removes from its queue
  using MessageCallback = void (*)(const MSG& message);

  // manual-reset event, initially non-signaled; nullptr on failure
  virtual HANDLE CreateStopEvent() = 0;
  virtual BOOL SetEvent(HANDLE hEvent) = 0;
  virtual DWORD WaitForSingleObject(HANDLE hHandle, DWORD milliseconds) = 0;
  virtual BOOL CloseHandle(HANDLE hHandle) = 0;

  virtual BOOL IsWindow(HWND hWnd) = 0;
  virtual DWORD GetWindowThreadProcessId(HWND hWnd, DWORD* pProcessId) = 0;

  // One hook at a time, on the given thread; false on failure. onMessage runs on that
  // thread and can still run shortly after RemoveMessageHook() returns.
  virtual bool InstallMessageHook(DWORD threadId, MessageCallback onMessage) = 0;
  // false on failure
  virtual bool RemoveMessageHook() = 0;

  virtual BOOL PostMessageW(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) = 0;

  // throws like the std::thread constructor when the thread cannot be created
  virtual std::unique_ptr<std::thread> StartThread(
      std::function<void()> threadProc
  ) = 0;

  virtual std::chrono::nanoseconds GetHighPrecisionTimestamp() = 0;

 protected:
  ~IUiHangDetectorRuntime() = default;
};

// Detects hangs of a window by posting probe messages from a watchdog thread and
// observing them from a message hook on the window's thread.
//
// _instance is set once activation has fully succeeded, so a hook notification never
// reaches a partially initialized detector. Stop() stops the watchdog and removes the
// hook, then clears _instance under _instanceMutex, which waits for a notification
// being processed. Only one detector is active at a time, and MonitorWindowHangs() and
// Stop() calls must not overlap, on any detector. Stop() is idempotent and a stopped
// detector cannot monitor again.
class UiHangDetector {
 public:
  UiHangDetector(
      UINT hangProbeMessageId,
      UiHangProvider* pHangProvider,
      IRumViewContextProvider* pRumViewContextProvider
  );
  ~UiHangDetector() noexcept;

  bool MonitorWindowHangs(HWND hWnd, ThreadList* pThreadList) noexcept;
  void Stop() noexcept;

 private:
  friend class UiHangDetectorLifetimeTests;

  UiHangDetector(
      UINT hangProbeMessageId,
      UiHangProvider* pHangProvider,
      IRumViewContextProvider* pRumViewContextProvider,
      IUiHangDetectorRuntime& runtime
  );

  // Passed to the message hook, which can still call it after Stop() removed the
  // hook: only the active detector, if any, is notified.
  static void OnMessage(const MSG& message);

  bool Activate(HWND hWnd, ThreadList* pThreadList);
  void Deactivate() noexcept;
  void ProcessMessage(const MSG& message);
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

  // the fully activated detector receiving hook notifications, if any; atomic because
  // the watchdog reads it without _instanceMutex
  static std::atomic<UiHangDetector*> _instance;
  // held by OnMessage() while it uses _instance, so clearing _instance also waits for a
  // notification being processed
  static std::mutex _instanceMutex;

  IUiHangDetectorRuntime& _runtime;
  UINT _hangProbeMessageId;
  HWND _hWnd;
  UiHangProvider* _pHangProvider = nullptr;
  IRumViewContextProvider* _pRumViewContextProvider = nullptr;

  StackFrameCollector _stackFrameCollector;
  std::shared_ptr<ThreadInfo> _pThreadInfo = nullptr;
  bool _isHookInstalled;
  HANDLE _stopEvent;
  std::unique_ptr<std::thread> _pWatchdogThread = nullptr;

  // lets the watchdog exit on its next timeout if signaling the stop event fails
  std::atomic<bool> _stopRequested;
  bool _isStopped;

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
