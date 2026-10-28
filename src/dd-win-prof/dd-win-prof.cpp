// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "DiagnosticLogging.h"
#include "LifecycleDiagnostic.h"
#include "Log.h"
#include "Profiler.h"
#include "dd-win-prof-internal.h"
#include "internal/dd-win-rum-private.h"
#include "pch.h"

namespace {
thread_local dd_win_prof::LifecycleDiagnostic lastDiagnostic;

ProfilingError Unavailable() {
  return {
      ProfilingSeverity::Error,
      ProfilingErrorCode::ProfilerUnavailable,
      "Profiler instance is unavailable"
  };
}

template <typename Action>
std::optional<ProfilingError> RunPrivateLifecycle(Action action) {
  dd_win_prof::ScopedDiagnosticLogging logging(false);
  try {
    return action();
  } catch (const std::exception& error) {
    return ProfilingError{
        ProfilingSeverity::Fatal, ProfilingErrorCode::UnexpectedException, error.what()
    };
  } catch (...) {
    return ProfilingError{
        ProfilingSeverity::Fatal,
        ProfilingErrorCode::UnexpectedException,
        "Non-standard exception during profiler lifecycle operation"
    };
  }
}

template <typename Action>
int32_t RunPublicLifecycle(Action action) noexcept {
  lastDiagnostic.Clear();
  try {
    const auto error = action();
    if (error) {
      return lastDiagnostic.Set(
          static_cast<int32_t>(error->severity),
          static_cast<uint32_t>(error->code),
          error->message
      );
    }
    return DD_WIN_PROF_SUCCESS;
  } catch (const std::exception& error) {
    return lastDiagnostic.Set(
        DD_WIN_PROF_FATAL, DD_WIN_PROF_CODE_UNEXPECTED_EXCEPTION, error.what()
    );
  } catch (...) {
    return lastDiagnostic.Set(
        DD_WIN_PROF_FATAL,
        DD_WIN_PROF_CODE_UNEXPECTED_EXCEPTION,
        "Non-standard exception during profiler lifecycle operation"
    );
  }
}

void LogLastDiagnostic(int32_t severity) {
  char message[1024];
  lastDiagnostic.Copy(nullptr, nullptr, message, sizeof(message));
  if (severity == DD_WIN_PROF_INFO) {
    Log::Info(message);
  } else if (severity == DD_WIN_PROF_WARNING) {
    Log::Warn(message);
  } else if (severity != DD_WIN_PROF_SUCCESS) {
    Log::Error(message);
  }
}
}  // namespace

DD_WIN_PROF_API std::optional<ProfilingError> SetupProfilingDataCallback(
    const ProfilingDataCallback& callback
) {
  auto profiler = Profiler::GetInstance();
  if (profiler == nullptr) {
    return ProfilingError{
        ProfilingSeverity::Error,
        ProfilingErrorCode::ProfilerUnavailable,
        "Profiler instance is unavailable; callback was not registered"
    };
  }
  return profiler->SetProfilingDataCallback(callback);
}

DD_WIN_PROF_API std::optional<ProfilingError> SetupProfilerWithDiagnostics(
    const ProfilerConfig* settings
) {
  return RunPrivateLifecycle([&]() -> std::optional<ProfilingError> {
    if (settings == nullptr) {
      return ProfilingError{
          ProfilingSeverity::Error,
          ProfilingErrorCode::InvalidArgument,
          "Null profiler configuration structure"
      };
    }
    if (settings->size != sizeof(ProfilerConfig)) {
      return ProfilingError{
          ProfilingSeverity::Error,
          ProfilingErrorCode::UnsupportedConfigSize,
          "Invalid profiler configuration structure size"
      };
    }
    auto profiler = Profiler::GetInstance();
    if (profiler == nullptr) {
      return Unavailable();
    }
    if (profiler->IsStarted()) {
      return ProfilingError{
          ProfilingSeverity::Error,
          ProfilingErrorCode::ProfilerAlreadyRunning,
          "SetupProfiler must be called while stopped"
      };
    }
    std::string message;
    if (!InitializeConfiguration(
            Profiler::GetConfiguration(),
            settings,
            profiler->GetDeliveryMode(),
            &message
        )) {
      return ProfilingError{
          ProfilingSeverity::Error,
          ProfilingErrorCode::InvalidConfiguration,
          std::move(message)
      };
    }
    return std::nullopt;
  });
}

DD_WIN_PROF_API std::optional<ProfilingError> StartProfilerWithDiagnostics() {
  return RunPrivateLifecycle([]() -> std::optional<ProfilingError> {
    auto profiler = Profiler::GetInstance();
    if (profiler == nullptr) {
      return Unavailable();
    }
    if (profiler->IsStarted()) {
      return ProfilingError{
          ProfilingSeverity::Error,
          ProfilingErrorCode::ProfilerAlreadyRunning,
          "Profiler is already running"
      };
    }
    return profiler->StartProfilingWithDiagnostics();
  });
}

DD_WIN_PROF_API std::optional<ProfilingError> StopProfilerWithDiagnostics() {
  return RunPrivateLifecycle([]() -> std::optional<ProfilingError> {
    auto profiler = Profiler::GetInstance();
    if (profiler == nullptr) {
      return Unavailable();
    }
    return profiler->StopProfilingWithDiagnostics();
  });
}

extern "C" {
DD_WIN_PROF_API int32_t __cdecl SetupProfilerV1(const ProfilerConfig* settings) {
  return RunPublicLifecycle([&] { return SetupProfilerWithDiagnostics(settings); });
}

DD_WIN_PROF_API int32_t __cdecl StartProfilerV1(void) {
  return RunPublicLifecycle([] { return StartProfilerWithDiagnostics(); });
}

DD_WIN_PROF_API int32_t __cdecl StopProfilerV1(void) {
  return RunPublicLifecycle([] { return StopProfilerWithDiagnostics(); });
}

DD_WIN_PROF_API uint32_t __cdecl GetLastProfilerDiagnosticV1(
    int32_t* severity, uint32_t* code, char* message, uint32_t capacity
) {
  return lastDiagnostic.Copy(severity, code, message, capacity);
}

DD_WIN_PROF_API bool SetupProfiler(const ProfilerConfig* settings) {
  const auto severity = SetupProfilerV1(settings);
  LogLastDiagnostic(severity);
  return severity == DD_WIN_PROF_SUCCESS;
}

DD_WIN_PROF_API bool StartProfiler() {
  const auto severity = StartProfilerV1();
  LogLastDiagnostic(severity);
  return severity == DD_WIN_PROF_SUCCESS;
}

DD_WIN_PROF_API void StopProfiler() {
  const auto* profiler = Profiler::GetInstance();
  if (profiler != nullptr && !profiler->IsStarted()) {
    Log::Warn("Profiler is not running.");
  }
  bool reported = false;
  const auto severity = RunPublicLifecycle([&] {
    auto error = StopProfilerWithDiagnostics();
    reported = error && error->reportedToCallback;
    return error;
  });
  if (!reported) {
    LogLastDiagnostic(severity);
  }
}

DD_WIN_PROF_API bool EnterView(const char* viewName) {
  auto profiler = Profiler::GetInstance();
  if (profiler == nullptr) {
    return false;
  }
  return profiler->EnterView(viewName);
}

DD_WIN_PROF_API bool LeaveCurrentView() {
  auto profiler = Profiler::GetInstance();
  if (profiler == nullptr) {
    return false;
  }
  return profiler->LeaveCurrentView();
}

DD_WIN_PROF_API bool SetRumSession(const RumSessionContext* pContext) {
  auto profiler = Profiler::GetInstance();
  if (profiler == nullptr) {
    return false;
  }
  return profiler->SetRumSession(pContext);
}

DD_WIN_PROF_API bool SetRumView(const RumViewValues* pContext) {
  auto profiler = Profiler::GetInstance();
  if (profiler == nullptr) {
    return false;
  }
  return profiler->SetRumView(pContext);
}
}
