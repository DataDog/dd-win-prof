// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <atomic>

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

  inline std::chrono::nanoseconds SetLastWaitSampleTimestamp(
      std::chrono::nanoseconds timestamp
  ) {
    auto prevValue = _lastWaitSampleTimestamp;
    _lastWaitSampleTimestamp = timestamp;
    return prevValue;
  }

  using HangCallstack = std::vector<uint64_t>;

  std::shared_ptr<const HangCallstack> GetHangCallstack() const {
    return _hangCallstack.load();
  }

  void SetHangCallstack(std::shared_ptr<const HangCallstack> callstack) {
    _hangCallstack.store(std::move(callstack));
  }

  void EndHang(std::chrono::nanoseconds timestamp) {
    _lastHangEndTimestamp.store(timestamp);
    _hangCallstack.store(nullptr);
  }

  std::chrono::nanoseconds ComputeWaitDuration(
      std::chrono::nanoseconds timestamp, std::chrono::nanoseconds samplingPeriod
  ) {
    auto previous = SetLastWaitSampleTimestamp(timestamp);
    auto duration = previous == 0ns ? samplingPeriod : timestamp - previous;
    auto hangEnd = _lastHangEndTimestamp.load();
    if (hangEnd != 0ns) {
      duration = (std::min)(duration, timestamp - hangEnd);
    }
    return (std::max)(0ns, duration);
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

  // keep track of the last time this thread was seen as waiting
  // --> should be reset to 0 when the thread is no more waiting
  //     (i.e. CPU profiler and lock detection part of walltime profiler)
  // since we don't have the start/ end time of the wait, we "jump" from wait to wait
  std::chrono::nanoseconds _lastWaitSampleTimestamp;

  // Published after the detection capture; reused without suspending the thread.
  std::atomic<std::shared_ptr<const HangCallstack>> _hangCallstack;
  // Only the sampler modifies _lastWaitSampleTimestamp. The watchdog publishes
  // recovery separately to exclude hang intervals from subsequent wait samples.
  std::atomic<std::chrono::nanoseconds> _lastHangEndTimestamp{0ns};

  // thread name, if available
  bool _hasThreadName = false;
  std::string _threadName;
};
