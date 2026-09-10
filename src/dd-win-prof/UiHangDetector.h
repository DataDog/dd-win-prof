// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "pch.h"

#include "ThreadList.h"
#include "UiHangProvider.h"

class UiHangDetector {
public:
  // needed by the global Windows Hook callback
  static UiHangDetector* _this;

  UiHangDetector(
    HMODULE hModule,
    UINT hangProbeMessageId,
    ThreadList* pThreadList,
    UiHangProvider* pHangProvider);
  ~UiHangDetector();

  bool MonitorWindowHangs(HWND hWnd);
  void Stop();
  void ProcessHook(int code, WPARAM wParam, LPARAM lParam);

private:
  void WatchdogLoop();
  bool PostProbeMessage();

private:
  enum class WatchdogState : uint8_t {
    None,       // start
    Probing,    // a probe message has been posted
    Hang,       // a hang is detected
    Processed   // a probe message as been processed
  };

private:
  HMODULE _hModule;
  UINT _hangProbeMessageId;
  HWND _hWnd;
  UiHangProvider* _pHangProvider;
  ThreadList* _pThreadList = nullptr;
  HHOOK _hGetMessageHook;
  HANDLE _stopEvent;
  std::unique_ptr<std::thread> _pWatchdogThread = nullptr;

  std::atomic<WatchdogState> _state;

  // timestamp when the probe message was processed
  std::atomic<std::chrono::nanoseconds> _processedProbeTimestamp;

  // timestamp when the probe message was posted
  std::chrono::nanoseconds _postProbeTimestamp;

  // timestamp when the hang was detected
  std::chrono::nanoseconds _hangDetectionTimestamp;

  // computed when the hang is detected and associated to the start hang sample
  // --> it will have to be deduced from _processingDuration to compute the duration
  //     of the stop hang sample
  std::chrono::nanoseconds _initialHangDuration;
};
