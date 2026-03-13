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
    if (_pConfiguration->IsProfilerExplicitlyDisabled())
    {
        Log::Info("Profiler is explicitly disabled: check following environment variable DD_PROFILING_ENABLED");
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
        _pCpuWallTimeProvider.get(),
        this);

    // get the values definition from the different providers...
    auto const& sampleTypeDefinitions = valueTypeProvider.GetValueTypes();
    Sample::SetValuesCount(sampleTypeDefinitions.size());

    //... and pass them to the exporter
    _pProfileExporter = std::make_unique<ProfileExporter>(_pConfiguration.get(), sampleTypeDefinitions, this);

    // Initialize the ProfileExporter
    if (!_pProfileExporter->Initialize())
    {
        Log::Error("Failed to initialize profile exporter: ", _pProfileExporter->GetLastError());
        return false;
    }

    // Flush buffered RUM application ID to the exporter
    {
        std::lock_guard lock(_rumAppMutex);
        if (_rumAppIdSet)
        {
            _pProfileExporter->SetRumApplicationId(_rumApplicationId);
        }
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

bool Profiler::UpdateRumContext(const RumContextValues* pContext)
{
    if (pContext == nullptr)
    {
        return false;
    }

    // Application ID: write-once, buffered until exporter exists
    {
        std::lock_guard lock(_rumAppMutex);
        bool hasAppId = pContext->application_id != nullptr && pContext->application_id[0] != '\0';

        if (hasAppId)
        {
            if (_rumAppIdSet && _rumApplicationId != pContext->application_id)
            {
                return false;
            }

            if (!_rumAppIdSet)
            {
                _rumApplicationId = pContext->application_id;
                _rumAppIdSet = true;

                if (_pProfileExporter != nullptr)
                {
                    _pProfileExporter->SetRumApplicationId(_rumApplicationId);
                }
            }
        }
    }

    // Session + view context
    {
        std::lock_guard lock(_rumContextMutex);

        // Session tracking: complete previous session on change, start new one
        bool hasSessionId = pContext->session_id != nullptr && pContext->session_id[0] != '\0';
        if (hasSessionId && _currentSessionId != pContext->session_id)
        {
            if (_hasPendingSession)
            {
                auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                _completedSessionRecords.push_back({
                    _sessionStartMs,
                    nowMs - _sessionStartMs,
                    std::move(_currentSessionId)
                });
            }

            _currentSessionId = pContext->session_id;
            _sessionStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            _hasPendingSession = true;
        }

        // View tracking
        if (pContext->view_id != nullptr && pContext->view_id[0] != '\0')
        {
            auto snapshot = std::make_shared<const RumViewContext>(RumViewContext{
                pContext->view_id,
                (pContext->view_name != nullptr) ? pContext->view_name : ""});
            std::atomic_store(&_currentView, snapshot);

            _pendingViewStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            _hasPendingView = true;
        }
        else
        {
            if (_hasPendingView)
            {
                auto prevView = std::atomic_load(&_currentView);
                auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                if (prevView)
                {
                    _completedViewRecords.push_back({
                        _pendingViewStartMs,
                        nowMs - _pendingViewStartMs,
                        prevView->view_id,
                        prevView->view_name
                    });
                }
                _hasPendingView = false;
            }

            std::atomic_store(&_currentView, std::shared_ptr<const RumViewContext>{nullptr});
        }
    }

    return true;
}

std::shared_ptr<const RumViewContext> Profiler::GetCurrentViewContext() const
{
    return std::atomic_load(&_currentView);
}

void Profiler::ConsumeViewRecords(std::vector<RumViewRecord>& records)
{
    std::lock_guard lock(_rumContextMutex);
    _completedViewRecords.swap(records);
}

void Profiler::ConsumeSessionRecords(std::vector<RumSessionRecord>& records)
{
    std::lock_guard lock(_rumContextMutex);
    _completedSessionRecords.swap(records);
}

std::string Profiler::GetCurrentSessionId() const
{
    std::lock_guard lock(_rumContextMutex);
    return _currentSessionId;
}
