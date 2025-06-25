#pragma once

#include "pch.h"
#include "Configuration.h"
#include "ProfileExporter.h"
#include "ISamplesProvider.h"

#include <forward_list>
#include <mutex>
#include <thread>
#include <future>

class SamplesCollector
{
public:
    SamplesCollector(
        Configuration* pConfiguration,
        ProfileExporter* exporter
        );
    ~SamplesCollector() = default;
    void Start();
    void Stop(bool shutdownOngoing = false);
    void Register(ISamplesProvider* samplesProvider);
    void Export(bool lastCall = false);
    static bool IsShutdownReceived();
    static void SignalShutdown();

private:
    void SamplesWork();
    void ExportWork();
    void CollectSamples(std::forward_list<std::pair<ISamplesProvider*, uint64_t>>& samplesProviders);

private:
    // configuration
    const WCHAR* WorkerThreadName = L"DD_worker";
    const WCHAR* ExporterThreadName = L"DD_exporter";
    inline static constexpr std::chrono::nanoseconds CollectingPeriod = 60ms;

    std::chrono::seconds _uploadInterval;
    std::chrono::milliseconds _collectingPeriod;

    // processing management
    std::thread _workerThread;
    std::thread _exporterThread;
    std::recursive_mutex _exportLock;
    std::promise<void> _exporterThreadPromise;
    std::promise<void> _workerThreadPromise;

    std::forward_list<std::pair<ISamplesProvider*, uint64_t>> _samplesProviders;
    ProfileExporter* _exporter;
};

