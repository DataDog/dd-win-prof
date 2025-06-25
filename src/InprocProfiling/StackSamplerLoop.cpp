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
    CpuTimeProvider* pCpuTimeProvider//,
    //WallTimeProvider* pWallTimeProvider
    )
    :
    _samplingPeriod(pConfiguration->CpuWallTimeSamplingRate()),
    _cpuThreadsThreshold(pConfiguration->CpuThreadsThreshold()),
    _walltimeThreadsThreshold(pConfiguration->WalltimeThreadsThreshold()),
    _shutdownRequested(false),
    _pThreadList(pThreadList),
    _pCpuTimeProvider(pCpuTimeProvider),
    //_pWallTimeProvider(pWallTimeProvider),
    _iteratorCpuTime(0),
    _pLoopThread(nullptr)
{
    _iteratorCpuTime = _pThreadList->CreateIterator();
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
    if (_pCpuTimeProvider == nullptr /*&& _pWallTimeProvider == nullptr*/)
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

    WalltimeProfilingIteration();
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
                        Sample sample = Sample(
                            thisSampleTimestamp,
                            pThreadInfo,
                            frames,
                            framesCount);
                        std::chrono::nanoseconds cpuTimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(cpuForSample);
                        _pCpuTimeProvider->Add(std::move(sample), cpuTimeNs);
                    }

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
}
