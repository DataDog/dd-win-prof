// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "Profiler.h"
#include "internal/dd-win-rum-private.h"
#include "pch.h"

namespace {
thread_local std::optional<ProfilingError> lastError;
thread_local bool exceptionMessageUnavailable = false;

ProfilingSeverity UnexpectedException(const char* message) noexcept {
  try {
    lastError = ProfilingError{
        ProfilingSeverity::Fatal, ProfilingErrorCode::UnexpectedException, message
    };
  } catch (...) {
    exceptionMessageUnavailable = true;
  }
  return ProfilingSeverity::Fatal;
}

template <typename Action>
ProfilingSeverity RunLifecycle(Action action) noexcept {
  lastError.reset();
  exceptionMessageUnavailable = false;
  try {
    auto profiler = Profiler::GetInstance();
    if (profiler == nullptr) {
      lastError = ProfilingError{
          ProfilingSeverity::Error,
          ProfilingErrorCode::ProfilerUnavailable,
          "Profiler instance is unavailable"
      };
    } else {
      lastError = action(*profiler);
    }
    return lastError ? lastError->severity : ProfilingSeverity::Success;
  } catch (const std::exception& error) {
    return UnexpectedException(error.what());
  } catch (...) {
    return UnexpectedException(
        "Non-standard exception during profiler lifecycle operation"
    );
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

DD_WIN_PROF_API ProfilingSeverity SetupProfiler(const ProfilerConfig& settings
) noexcept {
  return RunLifecycle([&](Profiler& profiler) { return profiler.Configure(settings); });
}

DD_WIN_PROF_API ProfilingSeverity StartProfiler() noexcept {
  return RunLifecycle([](Profiler& profiler) { return profiler.StartProfiling(); });
}

DD_WIN_PROF_API ProfilingSeverity StopProfiler() noexcept {
  return RunLifecycle([](Profiler& profiler) { return profiler.StopProfiling(); });
}

DD_WIN_PROF_API ProfilingDiagnostic GetLastProfilerDiagnostic() noexcept {
  if (exceptionMessageUnavailable) {
    return {
        ProfilingSeverity::Fatal,
        ProfilingErrorCode::UnexpectedException,
        "Unexpected exception; diagnostic message unavailable"
    };
  }
  return lastError
      ? ProfilingDiagnostic{lastError->severity, lastError->code, lastError->message.c_str()}
      : ProfilingDiagnostic{};
}

DD_WIN_PROF_API bool EnterView(const char* viewName) {
  auto profiler = Profiler::GetInstance();
  return profiler != nullptr && profiler->EnterView(viewName);
}

DD_WIN_PROF_API bool LeaveCurrentView() {
  auto profiler = Profiler::GetInstance();
  return profiler != nullptr && profiler->LeaveCurrentView();
}

DD_WIN_PROF_API bool SetRumSession(const RumSessionContext* context) {
  auto profiler = Profiler::GetInstance();
  return profiler != nullptr && profiler->SetRumSession(context);
}

DD_WIN_PROF_API bool SetRumView(const RumViewValues* context) {
  auto profiler = Profiler::GetInstance();
  return profiler != nullptr && profiler->SetRumView(context);
}
