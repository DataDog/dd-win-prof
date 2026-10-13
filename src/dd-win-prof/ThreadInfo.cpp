// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "ThreadInfo.h"

#include <algorithm>

#include "pch.h"

ThreadInfo::ThreadInfo(uint32_t tid, HANDLE hThread)
    : _tid(tid),
      _hThread(ScopedHandle(hThread)),
      _lastWalltimeSampleTimestamp{0ns},
      _cpuConsumption{0ms},
      _timestamp{0ns} {}

void ThreadInfo::ResetWaitSampleTimestamp() {
  std::lock_guard lock(_waitAccountingMutex);
  _lastWaitSampleTimestamp = 0ns;
}

std::chrono::nanoseconds ThreadInfo::RecordWaitSample(
    std::chrono::nanoseconds timestamp, std::chrono::nanoseconds samplingPeriod
) {
  std::lock_guard lock(_waitAccountingMutex);
  if (_hangDetected || timestamp <= _waitAccountingTimestamp) {
    return 0ns;
  }

  auto start = _lastWaitSampleTimestamp == 0ns ? timestamp - samplingPeriod
                                               : _lastWaitSampleTimestamp;
  start = (std::max)(start, _waitAccountingTimestamp);
  _lastWaitSampleTimestamp = timestamp;
  _waitAccountingTimestamp = timestamp;
  return timestamp - start;
}

ThreadInfo::HangSampleInterval ThreadInfo::StartUiHang(
    std::chrono::nanoseconds timestamp, std::chrono::nanoseconds earliestStart
) {
  std::lock_guard lock(_waitAccountingMutex);
  auto end = (std::max)(timestamp, _waitAccountingTimestamp);
  auto start = (std::max)(earliestStart, _waitAccountingTimestamp);
  _hangDetected = true;
  _waitAccountingTimestamp = end;
  return {end, (std::max)(0ns, end - start)};
}

ThreadInfo::HangSampleInterval ThreadInfo::EndUiHang(std::chrono::nanoseconds timestamp
) {
  std::lock_guard lock(_waitAccountingMutex);
  auto end = (std::max)(timestamp, _waitAccountingTimestamp);
  auto duration = end - _waitAccountingTimestamp;
  _lastWaitSampleTimestamp = end;
  _waitAccountingTimestamp = end;
  _hangDetected = false;
  return {end, duration};
}
