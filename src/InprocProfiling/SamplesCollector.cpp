#include "pch.h"
#include "SamplesCollector.h"

#include "Configuration.h"
#include "Log.h"
#include "OpSysTools.h"
#include <atomic>

// Global flag to track shutdown signal for this collector
static std::atomic<bool> g_shutdownReceived{false};

SamplesCollector::SamplesCollector(Configuration* pConfiguration, ProfileExporter* exporter)
    :
    _uploadInterval(pConfiguration->GetUploadInterval()),
    _exporter(exporter)
{
}

void SamplesCollector::Register(ISamplesProvider* samplesProvider)
{
    _samplesProviders.push_front(std::make_pair(samplesProvider, 0));
}

void SamplesCollector::Start()
{
    _workerThread = std::thread([this]
        {
            OpSysTools::SetNativeThreadName(WorkerThreadName);
            SamplesWork();
        });
    _exporterThread = std::thread([this]
        {
            OpSysTools::SetNativeThreadName(ExporterThreadName);
            ExportWork();
        });
}

void SamplesCollector::Stop(bool shutdownOngoing)
{
    _workerThreadPromise.set_value();
    _workerThread.join();

    _exporterThreadPromise.set_value();
    _exporterThread.join();

    if (shutdownOngoing) {
        Log::Info("SamplesCollector::Stop() - Skipping final export due to shutdown");
        // Still collect samples for potential debug output, but don't export via HTTP
        CollectSamples(_samplesProviders);
        // exporting at this point will PANIC.
        // libdatadog asks for a thread to be created, and the OS says NO.
    } else {
        // Normal shutdown - collect samples and export
        CollectSamples(_samplesProviders);
        Export(true);
    }
}

void SamplesCollector::SamplesWork()
{
    const auto future = _workerThreadPromise.get_future();

    while (future.wait_for(CollectingPeriod) == std::future_status::timeout)
    {
        CollectSamples(_samplesProviders);
    }
}

void SamplesCollector::ExportWork()
{
    const auto future = _exporterThreadPromise.get_future();

    // Althouth export is not called explicitly on shutdown,
    // we check it in case the periodic thread calls us at the same time as the shutdown.
    while (!IsShutdownReceived() && future.wait_for(_uploadInterval) == std::future_status::timeout)
    {
        Export();
    }
}

void SamplesCollector::Export(bool lastCall)
{
    bool success = false;

    try
    {
        std::lock_guard lock(_exportLock);

        Log::Debug("Collected samples per provider:");
        for (auto& samplesProvider : _samplesProviders)
        {
            auto name = samplesProvider.first->GetName();
            Log::Debug("  ", name, " : ", samplesProvider.second);
            samplesProvider.second = 0;
        }

        success = _exporter->Export(lastCall);
    }
    catch (std::exception const& ex)
    {
        Log::Error("An exception occurred during export: ", ex.what());
    }
}

bool SamplesCollector::IsShutdownReceived()
{
    return g_shutdownReceived.load(std::memory_order_acquire);
}

void SamplesCollector::CollectSamples(std::forward_list<std::pair<ISamplesProvider*, uint64_t>>& samplesProviders)
{
    for (auto& samplesProvider : samplesProviders)
    {
        try
        {
            std::lock_guard lock(_exportLock);

            std::vector<Sample> samples;
            auto count = samplesProvider.first->MoveSamples(samples);
            samplesProvider.second += count;

            // iterate on each sample and add it to the exporter
            for (auto& sample : samples)
            {
                auto samplePtr = std::make_shared<Sample>(std::move(sample));
                _exporter->Add(samplePtr);
            }
        }
        catch (std::exception const& ex)
        {
            Log::Error("An exception occurred while collecting samples: ", ex.what());
        }
    }
}

void SamplesCollector::SignalShutdown()
{
    g_shutdownReceived.store(true, std::memory_order_release);
}
