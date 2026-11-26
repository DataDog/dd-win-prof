// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "ProfilingConstants.h"
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

  // suspends the thread, captures its stack and resumes it; false on failure
  virtual bool CaptureStack(
      const std::shared_ptr<ThreadInfo>& pThreadInfo, std::vector<uint64_t>& frames
  ) = 0;

 protected:
  ~IUiHangDetectorRuntime() = default;
};

// Detects hangs of a window by posting probe messages from a watchdog thread and
// observing them from a message hook on the window's thread. Each probe carries the
// profiling session ID and a sequence: only the latest probe of this detector counts,
// so a probe left queued by a previous session on the same window is ignored.
//
// _instance is set once activation has fully succeeded, so a hook notification never
// reaches a partially initialized detector. Stop() records a cutoff, stops the watchdog
// and removes the hook, then clears _instance under _instanceMutex, which waits for a
// notification being processed. After the cutoff, the watchdog emits no hang sample.
// A hang still active then is closed once, from the frames and RUM view cached at its
// detection: at its acknowledgement if it was processed before the cutoff, else at the
// cutoff with reason profiler_stop.
// Only one detector is active at a time, and MonitorWindowHangs() and Stop() calls must
// not overlap, on any detector. Stop() is idempotent and a stopped detector cannot
// monitor again.
class UiHangDetector {
 public:
  UiHangDetector(
      uint64_t sessionId,
      UINT hangProbeMessageId,
      UiHangProvider* pHangProvider,
      IRumViewContextProvider* pRumViewContextProvider
  );
  ~UiHangDetector() noexcept;

  bool MonitorWindowHangs(HWND hWnd, ThreadList* pThreadList) noexcept;
  void Stop() noexcept;

 private:
  friend class UiHangDetectorLifetimeTests;
  friend class UiHangDetectorSessionTests;

  UiHangDetector(
      uint64_t sessionId,
      UINT hangProbeMessageId,
      UiHangProvider* pHangProvider,
      IRumViewContextProvider* pRumViewContextProvider,
      IUiHangDetectorRuntime& runtime
  );

  // what a hang sample needs from the hung thread
  struct HangCapture {
    std::vector<uint64_t> frames;
    RumViewContext rumView;
    bool hasRumView = false;
  };

  // Passed to the message hook, which can still call it after Stop() removed the
  // hook: only the active detector, if any, is notified.
  static void OnMessage(const MSG& message);

  bool Activate(HWND hWnd, ThreadList* pThreadList);
  void Deactivate() noexcept;
  void RecordCutoff() noexcept;
  void ProcessMessage(const MSG& message);
  void WatchdogLoop();
  bool PostProbeMessage();
  bool DetectHang(std::chrono::nanoseconds now);
  bool RecoverHang();
  void EndHang(std::chrono::nanoseconds timestamp);
  void CloseActiveHang() noexcept;
  std::chrono::nanoseconds HangDurationUntil(std::chrono::nanoseconds timestamp) const;
  bool CaptureHang(HangCapture& capture);
  void AddHangSample(
      HangCapture& capture,
      UiHangSampleKind kind,
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
  // the fully activated detector receiving hook notifications, if any; atomic because
  // the watchdog reads it without _instanceMutex
  static std::atomic<UiHangDetector*> _instance;
  // held by OnMessage() while it uses _instance, so clearing _instance also waits for a
  // notification being processed
  static std::mutex _instanceMutex;

  IUiHangDetectorRuntime& _runtime;
  const uint64_t _sessionId;
  UINT _hangProbeMessageId;
  HWND _hWnd;
  UiHangProvider* _pHangProvider = nullptr;
  IRumViewContextProvider* _pRumViewContextProvider = nullptr;

  std::shared_ptr<ThreadInfo> _pThreadInfo = nullptr;
  bool _isHookInstalled;
  HANDLE _stopEvent;
  std::unique_ptr<std::thread> _pWatchdogThread = nullptr;

  // Set at the cutoff; also lets the watchdog exit on its next timeout if signaling
  // the stop event fails.
  std::atomic<bool> _stopRequested;
  bool _isStopped;

  // held by the watchdog while it emits a hang sample and by Stop() while it records
  // the cutoff, so no hang sample is emitted after the cutoff
  std::mutex _stateMutex;
  // when Stop() started: acknowledgements processed later do not end the hang
  std::chrono::nanoseconds _cutoffTimestamp;

  // sequence of the latest posted probe: written by the watchdog, read by the hook
  std::atomic<uint64_t> _probeSequence;

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

  // captured when the hang is detected, so Stop() can close it without suspending the
  // UI thread; empty if that capture failed
  std::optional<HangCapture> _activeHang;
};
