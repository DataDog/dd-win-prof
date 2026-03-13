// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "pch.h"

#include "Configuration.h"
#include "CpuTimeProvider.h"
#include "dd-win-prof.h"
#include "ProfileExporter.h"
#include "RumContext.h"
#include "SamplesCollector.h"
#include "StackSamplerLoop.h"
#include "ThreadList.h"


class Profiler : public IRumViewContextProvider, public IRumRecordProvider
{
public :
    Profiler();
    virtual ~Profiler();

    bool StartProfiling();
    void StopProfiling(bool shutdownOngoing = false);

    bool AddCurrentThread();
    void RemoveCurrentThread();

    bool IsStarted() const { return _isStarted; }
    size_t GetThreadCount() const { return _pThreadList ? _pThreadList->Count() : 0; }
    bool IsAutoStartEnabled() const { return _pConfiguration->IsProfilerAutoStartEnabled(); }

    // RUM context management (called from the C API, thread-safe)
    bool UpdateRumContext(const RumContextValues* pContext);

    // IRumViewContextProvider implementation
    std::shared_ptr<const RumViewContext> GetCurrentViewContext() const override;

    // IRumRecordProvider implementation
    void ConsumeViewRecords(std::vector<RumViewRecord>& records) override;
    void ConsumeSessionRecords(std::vector<RumSessionRecord>& records) override;
    std::string GetCurrentSessionId() const override;

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

    bool _isStarted;

    std::unique_ptr<ThreadList> _pThreadList;
    std::unique_ptr<StackSamplerLoop> _pStackSamplerLoop;

    // providers
    std::unique_ptr<CpuTimeProvider> _pCpuTimeProvider = nullptr;
    std::unique_ptr<WallTimeProvider> _pCpuWallTimeProvider = nullptr;

    // exporter
    std::unique_ptr<ProfileExporter> _pProfileExporter = nullptr;

    // samples collector
    std::unique_ptr<SamplesCollector> _pSamplesCollector = nullptr;

    // Current view snapshot: atomically swapped, readers get a ref-count bump (no lock, no string copy).
    // nullptr means no active view.
    std::shared_ptr<const RumViewContext> _currentView;

    // Mutex serializing writers for view records, session tracking, and the atomic view swap.
    // NOT acquired on the per-sample read path (GetCurrentViewContext uses atomic_load only).
    mutable std::mutex _rumContextMutex;

    // RUM view timeline recording (protected by _rumContextMutex)
    std::vector<RumViewRecord> _completedViewRecords;
    int64_t _pendingViewStartMs{0};
    bool _hasPendingView{false};

    // RUM session tracking (protected by _rumContextMutex)
    std::string _currentSessionId;
    int64_t _sessionStartMs{0};
    bool _hasPendingSession{false};
    std::vector<RumSessionRecord> _completedSessionRecords;

    // RUM app-level ID (write-once, buffered until exporter exists)
    std::mutex _rumAppMutex;
    bool _rumAppIdSet{false};
    std::string _rumApplicationId;
};

