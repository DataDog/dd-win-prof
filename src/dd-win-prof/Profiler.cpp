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

bool Profiler::SetRumApplicationId(const char* applicationId)
{
    bool hasAppId = applicationId != nullptr && applicationId[0] != '\0';
    if (!hasAppId)
    {
        return true;
    }

    std::lock_guard lock(_rumAppMutex);

    if (_rumAppIdSet && _rumApplicationId != applicationId)
    {
        return false;
    }

    if (!_rumAppIdSet)
    {
        _rumApplicationId = applicationId;
        _rumAppIdSet = true;

        if (_pProfileExporter != nullptr)
        {
            _pProfileExporter->SetRumApplicationId(_rumApplicationId);
        }
    }

    return true;
}

bool Profiler::SetRumSession(const char* sessionId)
{
    bool hasSessionId = sessionId != nullptr && sessionId[0] != '\0';
    if (!hasSessionId)
    {
        return true;
    }

    std::unique_lock lock(_rumContextMutex);

    if (_currentSessionId != sessionId)
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

        _currentSessionId = sessionId;
        _sessionStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        _hasPendingSession = true;
    }

    return true;
}

bool Profiler::SetRumView(const char* viewId, const char* viewName)
{
    std::unique_lock lock(_rumContextMutex);

    if (viewId != nullptr && viewId[0] != '\0')
    {
        _currentRumView.view_id = viewId;
        _currentRumView.view_name = (viewName != nullptr) ? viewName : "";
        _hasActiveView = true;

        _pendingViewStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        _hasPendingView = true;
    }
    else
    {
        if (_hasPendingView)
        {
            auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            _completedViewRecords.push_back({
                _pendingViewStartMs,
                nowMs - _pendingViewStartMs,
                std::move(_currentRumView.view_id),
                std::move(_currentRumView.view_name)
            });
            _hasPendingView = false;
        }

        _currentRumView.view_id.clear();
        _currentRumView.view_name.clear();
        _hasActiveView = false;
    }

    return true;
}

bool Profiler::UpdateRumContext(const RumContextValues* pContext)
{
    if (pContext == nullptr)
    {
        return false;
    }

    if (!SetRumApplicationId(pContext->application_id))
    {
        return false;
    }
    SetRumSession(pContext->session_id);
    SetRumView(pContext->view_id, pContext->view_name);
    return true;
}

bool Profiler::GetCurrentViewContext(RumViewContext& context) const
{
    std::shared_lock lock(_rumContextMutex);
    if (!_hasActiveView)
    {
        return false;
    }
    context = _currentRumView;
    return true;
}

void Profiler::ConsumeViewRecords(std::vector<RumViewRecord>& records)
{
    std::unique_lock lock(_rumContextMutex);
    _completedViewRecords.swap(records);
}

void Profiler::ConsumeSessionRecords(std::vector<RumSessionRecord>& records)
{
    std::unique_lock lock(_rumContextMutex);
    _completedSessionRecords.swap(records);
}

std::string Profiler::GetCurrentSessionId() const
{
    std::shared_lock lock(_rumContextMutex);
    return _currentSessionId;
}
