// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "ProfilingSession.h"

#include <utility>

#include "Log.h"
#include "SampleValueTypeProvider.h"
#include "pch.h"

namespace {
void LogFinalStatus(std::optional<FinalExportStatus> finalStatus) {
  if (!finalStatus) {
    return;
  }

  switch (*finalStatus) {
    case FinalExportStatus::Delivered:
      break;
    case FinalExportStatus::LocalWriteFailed:
      Log::Warn(
          "Final profile delivered, but the debug pprof file could not be written"
      );
      break;
    case FinalExportStatus::NotExported:
      Log::Info("Final profile not exported: ", ToString(*finalStatus));
      break;
    default:
      Log::Warn("Final profile was not delivered: ", ToString(*finalStatus));
      break;
  }
}
}  // namespace

ProfilingSession::ProfilingSession(
    uint64_t id,
    const Configuration& configuration,
    ProfilingDataCallback callback,
    void* callbackUserData,
    ProcessServices services,
    Hooks hooks
)
    : _id(id),
      _configuration(configuration),
      _callback(callback),
      _callbackUserData(callbackUserData),
      _services(services),
      _hooks(std::move(hooks)) {}

// no final export: only Stop() exports
ProfilingSession::~ProfilingSession() { TearDown(false, false); }

bool ProfilingSession::Start(bool underLoaderLock) {
  if (_hasStarted) {
    Log::Warn("A profiling session cannot be restarted: create a new one.");
    return false;
  }
  _hasStarted = true;
  _underLoaderLock = underLoaderLock;

  bool success = false;
  try {
    success = Prepare() && Activate();
  } catch (const std::exception& error) {
    Log::Error("Failed to start the profiling session: ", error.what());
  }
  if (!success) {
    TearDown(false, false);
    return false;
  }

  _isRunning = true;
  return true;
}

// Creates the components; no thread runs yet.
bool ProfilingSession::Prepare() {
  if (IsFailureInjected(Step::CreateProviders)) {
    return false;
  }
  // ThreadInfo objects live for the lifetime of the process. Clear the previous
  // session's timestamps, hang marker, CPU baseline, and sampler iterators before
  // creating a new sampling loop.
  _services.threadList->ResetProfilingState();
  SampleValueTypeProvider valueTypeProvider;
  _pCpuTimeProvider = std::make_unique<CpuTimeProvider>(valueTypeProvider);
  _pWallTimeProvider = std::make_unique<WallTimeProvider>(valueTypeProvider);
  _pUiHangProvider = std::make_unique<UiHangProvider>(valueTypeProvider);
  auto const& sampleTypeDefinitions = valueTypeProvider.GetValueTypes();
  Sample::SetValuesCount(sampleTypeDefinitions.size());
  OnStep(Step::CreateProviders);

  if (IsFailureInjected(Step::InitializeExporter)) {
    return false;
  }
  _pProfileExporter = std::make_unique<ProfileExporter>(
      &_configuration,
      sampleTypeDefinitions,
      _services.rumRecordProvider,
      _callback,
      _callbackUserData
  );
  if (!_pProfileExporter->Initialize()) {
    return false;
  }
  OnStep(Step::InitializeExporter);

  if (IsFailureInjected(Step::CreateCollector)) {
    return false;
  }
  _pSamplesCollector = std::make_unique<SamplesCollector>(
      &_configuration, _pProfileExporter.get(), _hooks.collectorThreadFactory
  );
  if (_configuration.IsCpuProfilingEnabled()) {
    _pSamplesCollector->Register(_pCpuTimeProvider.get());
  }
  if (_configuration.IsWallTimeProfilingEnabled()) {
    _pSamplesCollector->Register(_pWallTimeProvider.get());
  }
  // no configuration needed because enabled only when starting to monitor hang
  _pSamplesCollector->Register(_pUiHangProvider.get());
  OnStep(Step::CreateCollector);

  if (IsFailureInjected(Step::CreateSampler)) {
    return false;
  }
  _pStackSamplerLoop = std::make_unique<StackSamplerLoop>(
      &_configuration,
      _services.threadList,
      _pCpuTimeProvider.get(),
      _pWallTimeProvider.get(),
      _services.rumViewContextProvider,
      _services.viewVitalsAccumulator
  );
  OnStep(Step::CreateSampler);
  return true;
}

// Starts the consumers before the producer.
bool ProfilingSession::Activate() {
  if (IsFailureInjected(Step::StartCollector) ||
      !_pSamplesCollector->Start(_underLoaderLock)) {
    return false;
  }
  OnStep(Step::StartCollector);

  if (IsFailureInjected(Step::StartSampler)) {
    return false;
  }
  _pStackSamplerLoop->Start();
  OnStep(Step::StartSampler);
  return true;
}

bool ProfilingSession::MonitorWindowHangs(HWND hWnd, UINT hangProbeMessageId) {
  if (!_isRunning) {
    Log::Warn("The profiling session must be running to monitor window hangs.");
    return false;
  }

  if (_pUiHangDetector != nullptr) {
    Log::Warn("Impossible to monitor more than one window for UI hang detection.");
    return false;
  }

  // the hook is thread-specific, so no DLL hMod
  auto pUiHangDetector = std::make_unique<UiHangDetector>(
      hangProbeMessageId, _pUiHangProvider.get(), _services.rumViewContextProvider
  );
  if (!pUiHangDetector->MonitorWindowHangs(hWnd, _services.threadList)) {
    return false;
  }
  _pUiHangDetector = std::move(pUiHangDetector);
  return true;
}

void ProfilingSession::Stop(bool shutdownOngoing) {
  if (!std::exchange(_isRunning, false)) {
    return;
  }

  // no periodic export once the process is shutting down
  if (shutdownOngoing) {
    _pSamplesCollector->SignalShutdown();
  }
  TearDown(true, shutdownOngoing);
}

// Producers stop before the collector, the collector before the exporter it feeds,
// and the exporter before the providers it reads value types from.
void ProfilingSession::TearDown(bool finalExport, bool shutdownOngoing) {
  if (_pUiHangDetector != nullptr) {
    _pUiHangDetector->Stop();
    _pUiHangDetector.reset();
    OnStep(Step::StopDetector);
  }

  // After a failed start, the sampler thread does not exist: resetting the loop does
  // not join it, which would hang under the loader lock.
  if (_pStackSamplerLoop != nullptr) {
    _pStackSamplerLoop->Stop();
    _pStackSamplerLoop.reset();
    OnStep(Step::StopSampler);
  }

  if (_pSamplesCollector != nullptr) {
    if (finalExport) {
      LogFinalStatus(_pSamplesCollector->Stop(shutdownOngoing));
    } else {
      _pSamplesCollector->Abort(_underLoaderLock);
    }
    _pSamplesCollector.reset();
    OnStep(Step::StopCollector);
  }

  // the exporter destructor never exports
  if (_pProfileExporter != nullptr) {
    if (finalExport) {
      _pProfileExporter->Cleanup(shutdownOngoing);
    }
    _pProfileExporter.reset();
    OnStep(Step::ReleaseExporter);
  }

  if (_pCpuTimeProvider != nullptr) {
    _pUiHangProvider.reset();
    _pWallTimeProvider.reset();
    _pCpuTimeProvider.reset();
    OnStep(Step::ReleaseProviders);
  }
}

bool ProfilingSession::IsFailureInjected(Step step) const {
  return _hooks.failStep && _hooks.failStep(step);
}

void ProfilingSession::OnStep(Step step) const {
  if (_hooks.onStep) {
    _hooks.onStep(step);
  }
}
