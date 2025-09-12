// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"
#include "Profiler.h"

#include "Log.h"
#include "SampleValueTypeProvider.h"
#include "SamplesCollector.h"

Profiler* Profiler::_this = nullptr;
std::unique_ptr<Configuration> Profiler::_pConfiguration = std::make_unique<Configuration>();

Profiler::Profiler()
    :
    _isStarted(false),
    _pThreadList(std::make_unique<ThreadList>()),
    _pStackSamplerLoop(nullptr)
{
    _this = this;
}

Profiler::~Profiler()
{
    _this = nullptr;
    _isStarted = false;
}

bool Profiler::StartProfiling()
{
    // no needed to look at env var to enable profiler
    // --> used only as kill switch to disable it
    if (!_pConfiguration->IsProfilerEnabled())
    {
        Log::Info("Profiler is explicitely disabled by environment variable.");
        return false;
    }

    Log::Info("Starting profiler...");

    auto valueTypeProvider = SampleValueTypeProvider();

    _pCpuTimeProvider = std::make_unique<CpuTimeProvider>(valueTypeProvider);
    _pCpuWallTimeProvider = std::make_unique<WallTimeProvider>(valueTypeProvider);

    // create the thread responsible for looping through the thread list
    _pStackSamplerLoop = std::make_unique<StackSamplerLoop>(
        _pConfiguration.get(),
        _pThreadList.get(),
        _pCpuTimeProvider.get(),
        _pCpuWallTimeProvider.get());

    // get the values definition from the different providers...
    auto const& sampleTypeDefinitions = valueTypeProvider.GetValueTypes();
    Sample::SetValuesCount(sampleTypeDefinitions.size());

    //... and pass them to the exporter
    _pProfileExporter = std::make_unique<ProfileExporter>(_pConfiguration.get(), sampleTypeDefinitions);

    // Initialize the ProfileExporter
    if (!_pProfileExporter->Initialize())
    {
        Log::Error("Failed to initialize profile exporter: ", _pProfileExporter->GetLastError());
        return false;
    }

    // create the samples collector and pass it the exporter
    _pSamplesCollector = std::make_unique<SamplesCollector>(_pConfiguration.get(), _pProfileExporter.get());

    // register the providers to the collector
    if (_pConfiguration->IsCpuProfilingEnabled())
    {
        _pSamplesCollector->Register(_pCpuTimeProvider.get());
    }

    if (_pConfiguration->IsWallTimeProfilingEnabled())
    {
        _pSamplesCollector->Register(_pCpuWallTimeProvider.get());
    }

    // start processing
    _pSamplesCollector->Start();
    _pStackSamplerLoop->Start();

    _isStarted = true;
    return true;
}

void Profiler::StopProfiling(bool shutdownOngoing)
{
    // avoid being stopped multiple times
    if (!_isStarted)
    {
        return;
    }

    Log::Info("Stopping profiler...");

    _isStarted = false;

    // Signal SamplesCollector if we're in shutdown mode to stop exports immediately
    if (shutdownOngoing)
    {
        SamplesCollector::SignalShutdown();
    }

    if (_pStackSamplerLoop != nullptr)
    {
        _pStackSamplerLoop->Stop();
    }

    if (_pSamplesCollector != nullptr)
    {
        _pSamplesCollector->Stop(shutdownOngoing);
    }

    // Explicitly clean up ProfileExporter with shutdown detection
    if (_pProfileExporter != nullptr)
    {
        // Use the parameter to decide cleanup approach
        _pProfileExporter->Cleanup(shutdownOngoing);
    }
    Log::Info("Profiler stopped...");
}

bool Profiler::AddCurrentThread()
{
    auto tid = ::GetCurrentThreadId();
    HANDLE hThread;
    auto success = ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &hThread, THREAD_ALL_ACCESS, false, 0);
    if (!success)
    {
        Log::Debug("DuplicateHandle() failed for thread ID: ", tid);
        return false;
    }

    _pThreadList->AddThread(tid, hThread);
    return true;
}

void Profiler::RemoveCurrentThread()
{
    auto tid = ::GetCurrentThreadId();
    _pThreadList->RemoveThread(tid);
}
