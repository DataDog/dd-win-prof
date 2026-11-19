// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <atomic>
#include <mutex>

#include "Configuration.h"
#include "CpuTimeProvider.h"
#include "ProfilingConstants.h"
#include "RumContext.h"
#include "ScopedHandle.h"
#include "StackFrameCollector.h"
#include "ThreadList.h"
#include "WalltimeProvider.h"
#include "pch.h"

typedef enum { WallTime, CpuTime } PROFILING_TYPE;

const ULONG WAIT_REASON_NONE = 0xFFFF;

class StackSamplerLoop {
 public:
  StackSamplerLoop(
      Configuration* pConfiguration,
      ThreadList* pThreadList,
      CpuTimeProvider* pCpuTimeProvider,
      WallTimeProvider* pWallTimeProvider,
      IRumViewContextProvider* pRumViewContextProvider = nullptr,
      IViewVitalsAccumulator* pViewVitalsAccumulator = nullptr
  );
  ~StackSamplerLoop();

  // Start() and Stop() may be called in any order and any number of times:
  // Start() is ignored while running, and a Start() after Stop() begins a new run
  // with a cleared stop request.
  void Start();
  void Stop();

 private:
  friend class StackSamplerLoopTest;

  bool IsStopRequested() const;
  void MainLoop();
  void MainLoopIteration();
  void CpuProfilingIteration();
  void WalltimeProfilingIteration();
  void CollectOneThreadSample(
      std::shared_ptr<ThreadInfo>& pThreadInfo,
      std::chrono::nanoseconds thisSampleTimestamp,
      std::chrono::nanoseconds duration,
      PROFILING_TYPE profilingType,
      ULONG waitingReason
  );  // waitingReason is only used for WallTime samples
  std::chrono::nanoseconds ComputeWallTime(
      std::chrono::nanoseconds currentTimestampNs,
      std::chrono::nanoseconds prevTimestampNs
  );

 private:
  static const int MaxFrameCount = dd_win_prof::kMaxStackDepth;

  // serializes Start() and Stop()
  std::mutex _lifecycleMutex;
  std::unique_ptr<std::thread> _pLoopThread;

  // Manual-reset: once Stop() sets it, every wait of the current run returns
  // immediately, so a stop cannot be missed whatever the loop is doing. Only
  // Start() resets it, when no loop thread is running.
  ScopedHandle _stopEvent;
  // lets an ongoing iteration give up without a system call
  std::atomic<bool> _stopRequested;

  // configuration
  std::chrono::nanoseconds _samplingPeriod;
  uint32_t _cpuThreadsThreshold;
  uint32_t _walltimeThreadsThreshold;

  uint32_t _nbCores;

  ThreadList* _pThreadList;
  uint32_t _iteratorCpuTime;
  uint32_t _iteratorWallTime;

  StackFrameCollector _stackFrameCollector;
  CpuTimeProvider* _pCpuTimeProvider;
  WallTimeProvider* _pWallTimeProvider;
  IRumViewContextProvider* _pRumViewContextProvider;
  IViewVitalsAccumulator* _pViewVitalsAccumulator;
};
