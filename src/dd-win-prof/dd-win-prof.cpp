// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "LifecycleDiagnostic.h"
#include "Profiler.h"
#include "internal/dd-win-rum-private.h"
#include "pch.h"

namespace {
thread_local dd_win_prof::LifecycleDiagnostic lastDiagnostic;

template <typename Action>
int32_t RunLifecycle(Action action) noexcept {
  lastDiagnostic.Clear();
  try {
    auto profiler = Profiler::GetInstance();
    if (profiler == nullptr) {
      return lastDiagnostic.Set(
          DD_WIN_PROF_ERROR,
          DD_WIN_PROF_CODE_PROFILER_UNAVAILABLE,
          "Profiler instance is unavailable"
      );
    }
    const auto error = action(*profiler);
    if (error) {
      return lastDiagnostic.Set(error->severity, error->code, error->message);
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
}  // namespace

DD_WIN_PROF_API std::optional<ProfilingError> SetupProfilingDataCallback(
    const ProfilingDataCallback& callback
) {
  auto profiler = Profiler::GetInstance();
  if (profiler == nullptr) {
    return ProfilingError{
        DD_WIN_PROF_ERROR,
        DD_WIN_PROF_CODE_PROFILER_UNAVAILABLE,
        "Profiler instance is unavailable; callback was not registered"
    };
  }
  return profiler->SetProfilingDataCallback(callback);
}

extern "C" {
DD_WIN_PROF_API int32_t __cdecl SetupProfiler(const ProfilerConfig* settings) {
  return RunLifecycle([&](Profiler& profiler) { return profiler.Configure(settings); });
}

DD_WIN_PROF_API int32_t __cdecl StartProfiler(void) {
  return RunLifecycle([](Profiler& profiler) { return profiler.StartProfiling(); });
}

DD_WIN_PROF_API int32_t __cdecl StopProfiler(void) {
  return RunLifecycle([](Profiler& profiler) { return profiler.StopProfiling(); });
}

DD_WIN_PROF_API uint32_t __cdecl GetLastProfilerDiagnostic(
    int32_t* severity, uint32_t* code, char* message, uint32_t capacity
) {
  return lastDiagnostic.Copy(severity, code, message, capacity);
}

DD_WIN_PROF_API bool EnterView(const char* viewName) {
  auto profiler = Profiler::GetInstance();
  return profiler != nullptr && profiler->EnterView(viewName);
}

DD_WIN_PROF_API bool LeaveCurrentView(void) {
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
}
