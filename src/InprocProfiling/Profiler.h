#pragma once

#include "pch.h"

#include "Configuration.h"
#include "CpuTimeProvider.h"
#include "ProfileExporter.h"
#include "SamplesCollector.h"
#include "StackSamplerLoop.h"
#include "ThreadList.h"


class Profiler
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

public:
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

    bool _isStarted;

    std::unique_ptr<Configuration> _pConfiguration;

    std::unique_ptr<ThreadList> _pThreadList;
    std::unique_ptr<StackSamplerLoop> _pStackSamplerLoop;

    // providers
    std::unique_ptr<CpuTimeProvider> _pCpuTimeProvider = nullptr;
    // TODO: std::unique_ptr<WallTimeProvider> _pCpuWallTimeProvider = nullptr;

    // exporter
    std::unique_ptr<ProfileExporter> _pProfileExporter = nullptr;

    // samples collector
    std::unique_ptr<SamplesCollector> _pSamplesCollector = nullptr;
};

