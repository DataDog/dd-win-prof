// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <atomic>
#include <condition_variable>
#include <forward_list>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "Configuration.h"
#include "ISamplesProvider.h"
#include "ProfileExporter.h"
#include "pch.h"

class SamplesCollector {
 public:
  // creates a worker thread running the given routine; tests inject failures here
  using ThreadFactory = std::function<std::thread(std::function<void()>)>;

  SamplesCollector(
      Configuration* pConfiguration,
      ProfileExporter* exporter,
      ThreadFactory threadFactory = nullptr
  );
  ~SamplesCollector();

  // One instance runs at most once: each profiling session creates its own collector.
  // Start() starts both workers or none: if the second one cannot be created, the
  // first one is stopped and Start() returns false. Start() is ignored after a first
  // call, Abort() or Stop(). Stop() may be called any number of times; only the first
  // Stop() after a successful Start() collects the remaining samples.
  //
  // underLoaderLock: Start() and Abort() are called from DllMain. Workers created in
  // this DllMain call cannot run before it returns, so they are detached instead of
  // joined; they exit on their first wait without touching this instance.
  bool Start(bool underLoaderLock = false);
  // stops the workers without collecting or exporting
  void Abort(bool underLoaderLock = false);
  // Returns the final export status, or nullopt when no final export is attempted.
  std::optional<FinalExportStatus> Stop(bool shutdownOngoing = false);
  void Register(ISamplesProvider* samplesProvider);
  void Export();
  FinalExportStatus ExportFinal();
  bool IsShutdownReceived() const;
  void SignalShutdown();

 private:
  friend class SamplesCollectorTests;

  // Owned by the workers too: a detached worker only uses this state until it sees
  // the stop request, so it may outlive the collector. Never reset.
  struct StopState {
    std::mutex mutex;
    std::condition_variable condition;
    bool isRequested = false;
  };

  static bool WaitForStop(StopState& stopState, std::chrono::nanoseconds timeout);
  bool IsStopRequested();
  void StopWorkers(bool underLoaderLock = false);
  static void ReleaseWorker(std::thread& thread, bool underLoaderLock);
  void CollectSamples(
      std::forward_list<std::pair<ISamplesProvider*, uint64_t>>& samplesProviders
  );

 private:
  // configuration
  inline static constexpr const WCHAR* WorkerThreadName = L"DD_worker";
  inline static constexpr const WCHAR* ExporterThreadName = L"DD_exporter";
  inline static constexpr std::chrono::nanoseconds CollectingPeriod = 60ms;

  std::chrono::seconds _uploadInterval;
  std::chrono::milliseconds _collectingPeriod;

  // processing management
  ThreadFactory _threadFactory;
  // serializes Start(), Abort() and Stop()
  std::mutex _lifecycleMutex;
  bool _hasStarted = false;
  bool _isRunning = false;
  std::thread _workerThread;
  std::thread _exporterThread;
  std::recursive_mutex _exportLock;

  std::shared_ptr<StopState> _stopState = std::make_shared<StopState>();
  std::atomic<bool> _shutdownReceived{false};

  std::forward_list<std::pair<ISamplesProvider*, uint64_t>> _samplesProviders;
  ProfileExporter* _exporter;
};
