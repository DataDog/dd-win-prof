// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "pch.h"

#include "Configuration.h"
#include "CpuTimeProvider.h"
#include "dd-win-prof.h"
#include "dd-win-rum-private.h"
#include "ProfileExporter.h"
#include "RumContext.h"
#include "SamplesCollector.h"
#include "StackSamplerLoop.h"
#include "ThreadList.h"


class Profiler : public IRumViewContextProvider, public IRumRecordProvider, public IViewVitalsAccumulator
{
public :
    Profiler();
    virtual ~Profiler();

    bool StartProfiling();
    void StopProfiling(bool shutdownOngoing = false);

    bool AddCurrentThread();
    void RemoveCurrentThread();

    bool IsStarted() const { return _isStarted.load(std::memory_order_relaxed); }
    size_t GetThreadCount() const { return _pThreadList ? _pThreadList->Count() : 0; }
    bool IsAutoStartEnabled() const { return _pConfiguration->IsProfilerAutoStartEnabled(); }

    // RUM context management (called from the C API, thread-safe)
    bool EnterView(const char* viewName);
    bool LeaveCurrentView();
    bool SetRumSession(const RumSessionContext* pContext);
    bool SetRumView(const RumViewValues* pContext);
    void ClearRumContext();

    // IRumViewContextProvider implementation
    bool GetCurrentViewContext(RumViewContext& context) const override;

    // IRumRecordProvider implementation
    void ConsumeViewRecords(std::vector<RumViewRecord>& records) override;
    void ConsumeSessionRecords(std::vector<RumSessionRecord>& records) override;
    std::string GetCurrentSessionId() const override;

    // IViewVitalsAccumulator implementation (lock-free, called from sampler hot path)
    bool AccumulateViewVitals(ViewVitalKind kind, int64_t valueNs) override;

public:
    static Configuration* GetConfiguration()
    {
        return _pConfiguration.get();
    }

    static Profiler* GetInstance()
    {
        return _this;
    }

    static Profiler* GetStartedInstance()
    {
        if (_this && _this->IsStarted())
        {
            return _this;
        }

        return nullptr;
    }

private:
    // configuration
    inline static constexpr std::chrono::seconds UploadInterval = std::chrono::seconds(10);

    static Profiler* _this;
    static std::unique_ptr<Configuration> _pConfiguration;

    std::atomic<bool> _isStarted;

    std::unique_ptr<ThreadList> _pThreadList;
    std::unique_ptr<StackSamplerLoop> _pStackSamplerLoop;

    // providers
    std::unique_ptr<CpuTimeProvider> _pCpuTimeProvider = nullptr;
    std::unique_ptr<WallTimeProvider> _pCpuWallTimeProvider = nullptr;

    // exporter
    std::unique_ptr<ProfileExporter> _pProfileExporter = nullptr;

    // samples collector
    std::unique_ptr<SamplesCollector> _pSamplesCollector = nullptr;

    // RUM view + session context (dynamic, protected by reader/writer lock)
    mutable std::shared_mutex _rumContextMutex;
    RumViewContext _currentRumView;

    void CompleteCurrentView();     // caller must hold _rumContextMutex exclusive
    void CompleteCurrentSession();  // caller must hold _rumContextMutex exclusive

    // RUM view timeline recording (protected by _rumContextMutex)
    std::vector<RumViewRecord> _completedViewRecords;
    int64_t _pendingViewStartMs{0};

    // Per-view vitals accumulators indexed by ViewVitalKind.
    // Written lock-free from the sampler thread (fetch_add with relaxed ordering),
    // read and reset under _rumContextMutex exclusive lock on view completion.
    std::array<std::atomic<int64_t>, MaxViewVitalKind> _pendingVitalsNs{};

    // RUM session tracking (protected by _rumContextMutex)
    std::string _currentSessionId;
    int64_t _sessionStartMs{0};
    std::vector<RumSessionRecord> _completedSessionRecords;

    // RUM app-level ID (write-once, buffered until exporter exists; protected by _rumContextMutex)
    std::string _rumApplicationId;
};

