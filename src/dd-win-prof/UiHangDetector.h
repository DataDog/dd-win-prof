// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "ProfilingConstants.h"
#include "StackFrameCollector.h"
#include "ThreadList.h"
#include "UiHangProvider.h"
#include "pch.h"

class UiHangDetector {
 public:
  // needed by the global Windows Hook callback
  static UiHangDetector* _this;

  UiHangDetector(
      UINT hangProbeMessageId,
      UiHangProvider* pHangProvider,
      IRumViewContextProvider* _pRumViewContextProvider
  );
  ~UiHangDetector();

  bool MonitorWindowHangs(HWND hWnd, ThreadList* pThreadList);
  void Stop();
  void ProcessHook(int code, WPARAM wParam, LPARAM lParam);

 private:
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

  UINT _hangProbeMessageId;
  HWND _hWnd;
  UiHangProvider* _pHangProvider = nullptr;
  IRumViewContextProvider* _pRumViewContextProvider = nullptr;

  StackFrameCollector _stackFrameCollector;
  std::shared_ptr<ThreadInfo> _pThreadInfo = nullptr;
  HHOOK _hGetMessageHook;
  HANDLE _stopEvent;
  std::unique_ptr<std::thread> _pWatchdogThread = nullptr;

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
