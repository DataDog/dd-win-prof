// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <mutex>

#include "OpSysTools.h"
#include "ScopedHandle.h"
#include "pch.h"

// NOTE:
class ThreadInfo {
 public:
  ThreadInfo(uint32_t tid, HANDLE hThread);

  uint32_t GetThreadId() const { return _tid; }
  inline HANDLE GetOsThreadHandle() const { return _hThread; }

  inline std::chrono::nanoseconds SetLastWalltimeSampleTimestamp(
      std::chrono::nanoseconds value
  ) {
    auto prevValue = _lastWalltimeSampleTimestamp;
    _lastWalltimeSampleTimestamp = value;
    return prevValue;
  }

  inline std::chrono::milliseconds GetCpuConsumption() const { return _cpuConsumption; }

  inline std::chrono::nanoseconds GetCpuTimestamp() const { return _timestamp; }

  inline std::chrono::milliseconds SetCpuConsumption(
      std::chrono::milliseconds value, std::chrono::nanoseconds timestamp
  ) {
    _timestamp = timestamp;

    auto prevValue = _cpuConsumption;
    _cpuConsumption = value;
    return prevValue;
  }

  struct HangSampleInterval {
    std::chrono::nanoseconds timestamp;
    std::chrono::nanoseconds duration;
  };

  void ResetWaitSampleTimestamp();
  std::chrono::nanoseconds RecordWaitSample(
      std::chrono::nanoseconds timestamp, std::chrono::nanoseconds samplingPeriod
  );
  HangSampleInterval StartUiHang(
      std::chrono::nanoseconds timestamp, std::chrono::nanoseconds earliestStart
  );
  HangSampleInterval EndUiHang(std::chrono::nanoseconds timestamp);

  inline bool IsHangDetected() const {
    std::lock_guard lock(_waitAccountingMutex);
    return _hangDetected;
  }

  // Called between profiling sessions, after all sampler/watchdog threads have
  // stopped. ThreadInfo itself is process-scoped, so none of these values may
  // carry over into the next session.
  inline void ResetProfilingState(std::chrono::milliseconds cpuConsumption) {
    _lastWalltimeSampleTimestamp = 0ns;
    _cpuConsumption = cpuConsumption;
    _timestamp = 0ns;
    _lastWaitSampleTimestamp = 0ns;
    _waitAccountingTimestamp = 0ns;
    _hangDetected = false;
  }

  inline bool GetThreadName(std::string& name) {
    if (_hasThreadName) {
      name = _threadName;
      return true;
    }

    if (OpSysTools::GetNativeThreadName(_hThread, _threadName)) {
      _hasThreadName = true;
      name = _threadName;
      return true;
    }

    return false;
  }

 private:
  // we don't handle the case where a thread ID is reused by the OS after a thread has
  // exited so only keep track of the OS thread ID
  uint32_t _tid;

  ScopedHandle _hThread;

  // will be used for walltime
  std::chrono::nanoseconds _lastWalltimeSampleTimestamp;

  // last CPU consumption in milliseconds
  std::chrono::milliseconds _cpuConsumption;

  // timestamp of the last CPU consumption sample
  std::chrono::nanoseconds _timestamp;

  // Never hold this mutex while suspending or unwinding a thread.
  mutable std::mutex _waitAccountingMutex;
  std::chrono::nanoseconds _lastWaitSampleTimestamp{0ns};
  // End of the last accounted wait/hang interval; CPU activity must not reset it.
  std::chrono::nanoseconds _waitAccountingTimestamp{0ns};
  bool _hangDetected = false;

  // thread name, if available
  bool _hasThreadName = false;
  std::string _threadName;
};
