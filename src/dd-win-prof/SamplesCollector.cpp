// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "SamplesCollector.h"

#include <exception>
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
SamplesCollector::~SamplesCollector() { Abort(); }

void SamplesCollector::Register(ISamplesProvider* samplesProvider) {
  _samplesProviders.push_front(std::make_pair(samplesProvider, 0));
}

bool SamplesCollector::Start(bool underLoaderLock) {
  std::lock_guard lock(_lifecycleMutex);

  if (_hasStarted || IsStopRequested()) {
    Log::Warn("SamplesCollector cannot be restarted: create a new instance.");
    return false;
  }
  _hasStarted = true;

  // workers block in their first wait until both are created
  std::unique_lock startupLock(_stopState->mutex);
  try {
    // a worker must not touch this instance before its first wait returns
    _workerThread = _threadFactory([this, stopState = _stopState] {
      OpSysTools::SetNativeThreadName(WorkerThreadName);
      while (!WaitForStop(*stopState, CollectingPeriod)) {
        CollectSamples(_samplesProviders);
      }
    });
    _exporterThread = _threadFactory(
        [this, stopState = _stopState, uploadInterval = _uploadInterval] {
          OpSysTools::SetNativeThreadName(ExporterThreadName);
          // no periodic export once shutdown is signaled
          while (!WaitForStop(*stopState, uploadInterval) && !IsShutdownReceived()) {
            Export();
          }
        }
    );
  } catch (const std::exception& error) {
    Log::Error("Failed to create a SamplesCollector thread: ", error.what());
    // roll back without collecting or exporting
    _stopState->isRequested = true;
    startupLock.unlock();
    StopWorkers(underLoaderLock);
    return false;
  }
  _isRunning = true;
  return true;
}

void SamplesCollector::Abort(bool underLoaderLock) {
  std::lock_guard lock(_lifecycleMutex);
  _isRunning = false;
  StopWorkers(underLoaderLock);
}

std::optional<FinalExportStatus> SamplesCollector::Stop(bool shutdownOngoing) {
  std::lock_guard lock(_lifecycleMutex);

  if (shutdownOngoing) {
    SignalShutdown();
  }

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

void SamplesCollector::StopWorkers(bool underLoaderLock) {
  {
    std::lock_guard lock(_stopState->mutex);
    _stopState->isRequested = true;
  }
  _stopState->condition.notify_all();

  // producer first, then consumer
  ReleaseWorker(_workerThread, underLoaderLock);
  ReleaseWorker(_exporterThread, underLoaderLock);
}

void SamplesCollector::ReleaseWorker(std::thread& thread, bool underLoaderLock) {
  if (!thread.joinable()) {
    return;
  }

  // Joining would never return: a thread created under the loader lock cannot run
  // before DllMain returns. Detaching is safe only because this thread was created in
  // the same DllMain call and has not run yet: once it can, it sees the stop request
  // on its first wait and exits without touching this instance.
  if (underLoaderLock) {
    thread.detach();
    return;
  }
  if (thread.get_id() == std::this_thread::get_id()) {
    Log::Error("A SamplesCollector thread cannot stop its own collector.");
    std::terminate();
  }
  try {
    thread.join();
  } catch (const std::exception& e) {
    Log::Error("Failed to join a SamplesCollector thread: ", e.what());
    std::terminate();
  }
}

bool SamplesCollector::WaitForStop(
    StopState& stopState, std::chrono::nanoseconds timeout
) {
  std::unique_lock lock(stopState.mutex);
  return stopState.condition.wait_for(lock, timeout, [&stopState] {
    return stopState.isRequested;
  });
}

bool SamplesCollector::IsStopRequested() {
  std::lock_guard lock(_stopState->mutex);
  return _stopState->isRequested;
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
