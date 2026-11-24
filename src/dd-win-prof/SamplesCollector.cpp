// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "SamplesCollector.h"

#include <utility>

#include "Configuration.h"
#include "Log.h"
#include "OpSysTools.h"
#include "pch.h"

SamplesCollector::SamplesCollector(
    Configuration* pConfiguration,
    ProfileExporter* exporter,
    ThreadFactory threadFactory
)
    : _uploadInterval(pConfiguration->GetUploadInterval()),
      _threadFactory(std::move(threadFactory)),
      _exporter(exporter) {
  if (_threadFactory == nullptr) {
    _threadFactory = [](std::function<void()> routine) {
      return std::thread(std::move(routine));
    };
  }
}

// no final export: Stop() is responsible for it
SamplesCollector::~SamplesCollector() {
  std::lock_guard lock(_lifecycleMutex);
  StopWorkers();
}

void SamplesCollector::Register(ISamplesProvider* samplesProvider) {
  _samplesProviders.push_front(std::make_pair(samplesProvider, 0));
}

void SamplesCollector::Start() {
  std::lock_guard lock(_lifecycleMutex);

  if (_hasStarted || IsStopRequested()) {
    Log::Warn("SamplesCollector cannot be restarted: create a new instance.");
    return;
  }
  _hasStarted = true;

  // workers block in their first WaitForStop() until both are created
  std::unique_lock startupLock(_stopMutex);
  try {
    _workerThread = _threadFactory([this] {
      OpSysTools::SetNativeThreadName(WorkerThreadName);
      SamplesWork();
    });
    _exporterThread = _threadFactory([this] {
      OpSysTools::SetNativeThreadName(ExporterThreadName);
      ExportWork();
    });
  } catch (...) {
    // roll back without collecting or exporting
    _stopRequested = true;
    startupLock.unlock();
    StopWorkers();
    throw;
  }
  _isRunning = true;
}

std::optional<FinalExportStatus> SamplesCollector::Stop(bool shutdownOngoing) {
  std::lock_guard lock(_lifecycleMutex);

  if (shutdownOngoing) {
    SignalShutdown();
  }

  // producer first, then consumer
  StopWorkers();

  if (!std::exchange(_isRunning, false)) {
    return std::nullopt;
  }

  if (IsShutdownReceived()) {
    Log::Info("SamplesCollector::Stop() - Skipping final export due to shutdown");
    // Still collect samples for potential debug output, but don't export via HTTP
    CollectSamples(_samplesProviders);
    // exporting at this point will PANIC.
    // libdatadog asks for a thread to be created, and the OS says NO.
    return std::nullopt;
  }
  // Normal stop - collect samples and export
  CollectSamples(_samplesProviders);
  return ExportFinal();
}

void SamplesCollector::StopWorkers() {
  {
    std::lock_guard lock(_stopMutex);
    _stopRequested = true;
  }
  _stopCondition.notify_all();

  for (auto* pThread : {&_workerThread, &_exporterThread}) {
    if (pThread->joinable()) {
      try {
        pThread->join();
      } catch (const std::exception& e) {
        Log::Error("Failed to join a SamplesCollector thread: ", e.what());
      }
    }
  }
}

bool SamplesCollector::WaitForStop(std::chrono::nanoseconds timeout) {
  std::unique_lock lock(_stopMutex);
  return _stopCondition.wait_for(lock, timeout, [this] { return _stopRequested; });
}

bool SamplesCollector::IsStopRequested() {
  std::lock_guard lock(_stopMutex);
  return _stopRequested;
}

void SamplesCollector::SamplesWork() {
  while (!WaitForStop(CollectingPeriod)) {
    CollectSamples(_samplesProviders);
  }
}

void SamplesCollector::ExportWork() {
  // Althouth export is not called explicitly on shutdown,
  // we check it in case the periodic thread calls us at the same time as the shutdown.
  while (!WaitForStop(_uploadInterval) && !IsShutdownReceived()) {
    Export();
  }
}

void SamplesCollector::Export() {
  bool success = false;

  try {
    std::lock_guard lock(_exportLock);

    Log::Debug("Collected samples per provider:");
    for (auto& samplesProvider : _samplesProviders) {
      auto name = samplesProvider.first->GetName();
      Log::Debug("  ", name, " : ", samplesProvider.second);
      samplesProvider.second = 0;
    }

    success = _exporter->Export();
  } catch (std::exception const& ex) {
    Log::Error("An exception occurred during export: ", ex.what());
  }
}

FinalExportStatus SamplesCollector::ExportFinal() {
  std::lock_guard lock(_exportLock);
  return _exporter->ExportFinal();
}

bool SamplesCollector::IsShutdownReceived() const {
  return _shutdownReceived.load(std::memory_order_acquire);
}

void SamplesCollector::CollectSamples(
    std::forward_list<std::pair<ISamplesProvider*, uint64_t>>& samplesProviders
) {
  for (auto& samplesProvider : samplesProviders) {
    try {
      std::lock_guard lock(_exportLock);

      std::vector<Sample> samples;
      auto count = samplesProvider.first->MoveSamples(samples);
      samplesProvider.second += count;

      // iterate on each sample and add it to the exporter
      for (auto& sample : samples) {
        auto samplePtr = std::make_shared<Sample>(std::move(sample));
        _exporter->Add(samplePtr);
      }
    } catch (std::exception const& ex) {
      Log::Error("An exception occurred while collecting samples: ", ex.what());
    }
  }
}

void SamplesCollector::SignalShutdown() {
  _shutdownReceived.store(true, std::memory_order_release);
}
