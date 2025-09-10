// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"
#include "StackSamplerLoop.h"

#include "CpuTimeProvider.h"
#include "Log.h"
#include "OpSysTools.h"
#include "OsSpecificApi.h"

constexpr const wchar_t* ThreadName = L"DD_StackSampler";

StackSamplerLoop::StackSamplerLoop(
    Configuration* pConfiguration,
    ThreadList* pThreadList,
    CpuTimeProvider* pCpuTimeProvider,
    WallTimeProvider* pWallTimeProvider
    )
    :
    _samplingPeriod(pConfiguration->CpuWallTimeSamplingRate()),
    _cpuThreadsThreshold(pConfiguration->CpuThreadsThreshold()),
    _walltimeThreadsThreshold(pConfiguration->WalltimeThreadsThreshold()),
    _shutdownRequested(false),
    _pThreadList(pThreadList),
    _pCpuTimeProvider(pCpuTimeProvider),
    _pWallTimeProvider(pWallTimeProvider),
    _iteratorCpuTime(0),
    _iteratorWallTime(0),
    _pLoopThread(nullptr)
{
    _iteratorCpuTime = _pThreadList->CreateIterator();
    _iteratorWallTime = _pThreadList->CreateIterator();
    _nbCores = OsSpecificApi::GetProcessorCount();

    // deal with configuration
    if (!pConfiguration->IsCpuProfilingEnabled())
    {
        _pCpuTimeProvider = nullptr;
    }
}

StackSamplerLoop::~StackSamplerLoop()
{
    Stop();
}

void StackSamplerLoop::Start()
{
    // avoid consuming resources if neither cpu nor walltime profiling is enabled
    if ((_pCpuTimeProvider == nullptr) && (_pWallTimeProvider == nullptr))
    {
        return;
    }

    _pLoopThread = std::make_unique<std::thread>([this]
    {
        OpSysTools::SetNativeThreadName(ThreadName);
        MainLoop();
    });
}

void StackSamplerLoop::Stop()
{
    _shutdownRequested = true;

    if (_pLoopThread != nullptr)
    {
        try
        {
            _pLoopThread->join();
            _pLoopThread.reset();
        }
        catch (const std::exception&)
        {
        }
    }
}

void StackSamplerLoop::MainLoop()
{
    DWORD samplingPeriodMs = static_cast<DWORD>(_samplingPeriod.count() / 1000000);
    while (!_shutdownRequested)
    {
        try
        {
            ::Sleep(samplingPeriodMs);
            MainLoopIteration();
        }
        catch (...)
        {
            Log::Error("Unknown Exception in StackSamplerLoop::MainLoop.");
        }
    }
}

void StackSamplerLoop::MainLoopIteration()
{
    if (_pCpuTimeProvider != nullptr)
    {
        CpuProfilingIteration();
    }

    if (_pWallTimeProvider != nullptr)
    {
        WalltimeProfilingIteration();
    }
}

void StackSamplerLoop::CpuProfilingIteration()
{
    uint32_t sampledThreads = 0;
    uint32_t managedThreadsCount = static_cast<uint32_t>(_pThreadList->Count());
    uint32_t sampledThreadsCount = (std::min)(managedThreadsCount, _cpuThreadsThreshold);
    std::shared_ptr<ThreadInfo> pThreadInfo = nullptr;

    for (uint32_t i = 0; i < sampledThreadsCount && !_shutdownRequested; i++)
    {
        pThreadInfo = _pThreadList->LoopNext(_iteratorCpuTime);
        if (pThreadInfo != nullptr)
        {
            // don't sample the sampling thread
            if (pThreadInfo->GetThreadId() == ::GetCurrentThreadId())
            {
                pThreadInfo.reset();
                continue;
            }

            // sample only if the thread is currently running on a core
            auto lastConsumption = pThreadInfo->GetCpuConsumption();
            auto [isRunning, currentConsumption, failure] = OsSpecificApi::IsRunning(pThreadInfo->GetOsThreadHandle());

            // Note: it is not possible to get this information on Windows 32-bit or in some cases in 64-bit
            //       so isRunning should be true if this thread consumed some CPU since the last iteration
            if (failure)
            {
                isRunning = (lastConsumption < currentConsumption);
            }

            if (isRunning)
            {
                auto cpuForSample = currentConsumption - lastConsumption;

                // we don't collect a sample for this thread is no CPU was consumed since the last check
                if (cpuForSample > 0ms)
                {
                    auto lastCpuTimestamp = pThreadInfo->GetCpuTimestamp();
                    auto thisSampleTimestamp = OpSysTools::GetHighPrecisionTimestamp();

                    // For the first computation, no need to deal with overlapping CPU usage
                    if (lastCpuTimestamp != 0ns)
                    {
                        // detect overlapping CPU usage
                        auto threshold = lastCpuTimestamp + cpuForSample;
                        if (threshold > thisSampleTimestamp)
                        {
                            // ensure that we don't overlap
                            // -> only the largest possibly CPU consumption is accounted = diff between the 2 timestamps
                            cpuForSample = std::chrono::duration_cast<std::chrono::milliseconds>(
                                thisSampleTimestamp - lastCpuTimestamp - 1us); // removing 1 microsecond to be sure;
                        }
                    }

                    pThreadInfo->SetCpuConsumption(currentConsumption, thisSampleTimestamp);

                    CollectOneThreadSample(pThreadInfo, thisSampleTimestamp, cpuForSample, PROFILING_TYPE::CpuTime);

                    // don't scan more threads than nb logical cores
                    sampledThreads++;
                    if (sampledThreads >= _nbCores)
                    {
                        break;
                    }
                }
            }
            pThreadInfo.reset();
        }
    }
}

void StackSamplerLoop::WalltimeProfilingIteration()
{
    uint32_t managedThreadsCount = _pThreadList->Count();
    uint32_t sampledThreadsCount = (std::min)(managedThreadsCount, _walltimeThreadsThreshold);

    uint32_t i = 0;

    ThreadInfo* firstThread = nullptr;
    std::shared_ptr<ThreadInfo> threadInfo = nullptr;

    do
    {
        threadInfo = _pThreadList->LoopNext(_iteratorWallTime);

        // either the list is empty or iterator is not in the array range
        // so prefer bailing out
        if (threadInfo == nullptr)
        {
            break;
        }

        if (firstThread == threadInfo.get())
        {
            threadInfo.reset();
            break;
        }

        if (firstThread == nullptr)
        {
            firstThread = threadInfo.get();
        }

        auto thisSampleTimestamp = OpSysTools::GetHighPrecisionTimestamp();
        auto prevSampleTimestamp = threadInfo->SetLastSampleTimestamp(thisSampleTimestamp);
        auto duration = ComputeWallTime(thisSampleTimestamp, prevSampleTimestamp);

        CollectOneThreadSample(threadInfo, thisSampleTimestamp, duration, PROFILING_TYPE::WallTime);

        threadInfo.reset();
        i++;

    } while (i < sampledThreadsCount && !_shutdownRequested);
}

void StackSamplerLoop::CollectOneThreadSample(std::shared_ptr<ThreadInfo>& pThreadInfo,
    std::chrono::nanoseconds thisSampleTimestamp,
    std::chrono::nanoseconds duration,
    PROFILING_TYPE profilingType)
{
    // the thread needs to be suspended before capturing the stack
    if (!_stackFrameCollector.TrySuspendThread(pThreadInfo))
    {
        return;
    }

    bool isTruncated = false;
    uint64_t frames[MaxFrameCount];
    uint16_t framesCount = MaxFrameCount;
    bool isStackCaptured = (_stackFrameCollector.CaptureStack(pThreadInfo->GetOsThreadHandle(), frames, framesCount, isTruncated));
    // resume the thread before doing any allocation that could cause a deadlock
    ::ResumeThread(pThreadInfo->GetOsThreadHandle());

    if (isStackCaptured)
    {
        // set a null address for the last frame in case of truncated stack
        if (isTruncated)
        {
            frames[framesCount - 1] = 0;
        }

        // create a sample
        if (profilingType == PROFILING_TYPE::CpuTime)
        {
            Sample sample = Sample(
                thisSampleTimestamp,
                pThreadInfo,
                frames,
                framesCount);
            _pCpuTimeProvider->Add(std::move(sample), duration);
        }
        else
            if (profilingType == PROFILING_TYPE::WallTime)
            {
                Sample sample = Sample(
                    thisSampleTimestamp,
                    pThreadInfo,
                    frames,
                    framesCount);
                _pWallTimeProvider->Add(std::move(sample), duration);
            }
            else
            {
                // should neven happen
            }
    }
}

std::chrono::nanoseconds StackSamplerLoop::ComputeWallTime(std::chrono::nanoseconds currentTimestampNs, std::chrono::nanoseconds prevTimestampNs)
{
    if (prevTimestampNs == 0ns)
    {
        // prevTimestampNs = 0 means that it is the first time the wall time is computed for a given thread
        // --> at least one sampling period has elapsed
        return _samplingPeriod;
    }

    if (prevTimestampNs > 0ns)
    {
        auto durationNs = currentTimestampNs - prevTimestampNs;
        return (std::max)(0ns, durationNs);
    }
    else
    {
        // this should never happen
        // count at least one sampling period
        return _samplingPeriod;
    }
}


