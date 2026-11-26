// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <functional>
#include <memory>

#include "Configuration.h"
#include "CpuTimeProvider.h"
#include "ProfileExporter.h"
#include "RumContext.h"
#include "SamplesCollector.h"
#include "StackSamplerLoop.h"
#include "ThreadList.h"
#include "UiHangDetector.h"
#include "UiHangProvider.h"
#include "WalltimeProvider.h"
#include "dd-win-prof.h"
#include "pch.h"

// The mutable resources of one profiling run: one session per successful Start, stopped
// and destroyed by Stop, never reused.
//
// Owned by the session: the configuration and callback snapshots, the providers, the
// exporter, the collector and its workers, the sampler loop and its thread, and the
// optional UI hang detector with its hook and watchdog.
// Not owned (process state on Profiler, outliving every session): ThreadList and its
// ThreadInfo objects, the RUM state, the UI probe message ID and the session ID
// generation. UiHangDetector::_instance is the process-wide active detector; the
// session owns only its own detector.
//
// Start order: providers -> exporter -> collector -> sampler, then the collector
// workers and the sampler thread. A failed Start releases what was created in reverse
// order, without final export or callback.
// Stop order: detector -> sampler -> collector (final export) -> exporter -> providers.
// Every worker is joined before its owner is destroyed.
//
// Not thread-safe: Profiler serializes Start, MonitorWindowHangs and Stop.
class ProfilingSession {
 public:
  // Process state used by the session; it must outlive the session.
  struct ProcessServices {
    ThreadList* threadList = nullptr;
    IRumViewContextProvider* rumViewContextProvider = nullptr;
    IRumRecordProvider* rumRecordProvider = nullptr;
    IViewVitalsAccumulator* viewVitalsAccumulator = nullptr;
  };

  // Start steps, then teardown steps, in their order.
  enum class Step {
    CreateProviders,
    InitializeExporter,
    CreateCollector,
    CreateSampler,
    StartCollector,
    StartSampler,
    StopDetector,
    StopSampler,
    StopCollector,
    ReleaseExporter,
    ReleaseProviders,
  };

  // Test seams for failure and ordering tests.
  struct Hooks {
    // called before each start step: returning true fails that step
    std::function<bool(Step)> failStep;
    // called after each completed step
    std::function<void(Step)> onStep;
    // creates the collector workers
    SamplesCollector::ThreadFactory collectorThreadFactory;
  };

  ProfilingSession(
      uint64_t id,
      const Configuration& configuration,
      ProfilingDataCallback callback,
      void* callbackUserData,
      ProcessServices services,
      Hooks hooks = {}
  );
  ~ProfilingSession();
  ProfilingSession(const ProfilingSession&) = delete;
  ProfilingSession& operator=(const ProfilingSession&) = delete;

  // Creates and starts every component, or rolls back and returns false. Called once.
  // underLoaderLock: called from DllMain, where a rollback must not join threads.
  bool Start(bool underLoaderLock = false);
  // At most one successful call per session.
  bool MonitorWindowHangs(HWND hWnd, UINT hangProbeMessageId);
  // Stops every component and runs the final export. Ignored if not running.
  void Stop(bool shutdownOngoing = false);

  uint64_t Id() const { return _id; }
  bool IsRunning() const { return _isRunning; }

 private:
  bool Prepare();
  bool Activate();
  bool IsFailureInjected(Step step) const;
  void OnStep(Step step) const;
  // Releases the components in reverse start order. finalExport: collect the remaining
  // samples and run the final export; otherwise they are discarded (rollback).
  void TearDown(bool finalExport, bool shutdownOngoing);

  const uint64_t _id;
  // never changed: not const only because the components take a mutable pointer
  Configuration _configuration;
  const ProfilingDataCallback _callback;
  void* const _callbackUserData;
  const ProcessServices _services;
  const Hooks _hooks;
  bool _hasStarted = false;
  bool _isRunning = false;
  bool _underLoaderLock = false;

  // declared in start order, so that even the implicit destruction is reversed
  std::unique_ptr<CpuTimeProvider> _pCpuTimeProvider;
  std::unique_ptr<WallTimeProvider> _pWallTimeProvider;
  std::unique_ptr<UiHangProvider> _pUiHangProvider;
  std::unique_ptr<ProfileExporter> _pProfileExporter;
  std::unique_ptr<SamplesCollector> _pSamplesCollector;
  std::unique_ptr<StackSamplerLoop> _pStackSamplerLoop;
  std::unique_ptr<UiHangDetector> _pUiHangDetector;
};
