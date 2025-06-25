#pragma once

#include "pch.h"

#include "Configuration.h"
#include "CpuTimeProvider.h"
#include "StackFrameCollector.h"
#include "ThreadList.h"
#include "ProfilingConstants.h"

class StackSamplerLoop
{
public:
    StackSamplerLoop(
        Configuration* pConfiguration,
        ThreadList* pThreadList,
        CpuTimeProvider* pCpuTimeProvider//,
        //TODO: WallTimeProvider* pWallTimeProvider
        );
    ~StackSamplerLoop();

    void Start();
    void Stop();

private:
    void MainLoop();
    void MainLoopIteration();
    void CpuProfilingIteration();
    void WalltimeProfilingIteration();

private:
    static const int MaxFrameCount = InprocProfiling::kMaxStackDepth;

    volatile bool _shutdownRequested;
    std::unique_ptr<std::thread> _pLoopThread;

    // configuration
    std::chrono::nanoseconds _samplingPeriod;
    uint32_t _cpuThreadsThreshold;
    uint32_t _walltimeThreadsThreshold;

    uint32_t _nbCores;

    ThreadList* _pThreadList;
    uint32_t _iteratorCpuTime;

    StackFrameCollector _stackFrameCollector;
    CpuTimeProvider* _pCpuTimeProvider;
    // TODO: WallTimeProvider* _pWallTimeProvider;
};

