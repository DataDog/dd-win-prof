// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "Log.h"
#include "Profiler.h"
#include "internal/dd-win-prof-private.h"
#include "internal/dd-win-rum-private.h"
#include "pch.h"

namespace {
void LogLifecycleException(const char* message) noexcept {
  try {
    Log::Error("Profiler lifecycle operation failed: ", message);
  } catch (...) {
    // Logging must not let exceptions escape the public API.
  }
}

template <typename Action>
bool RunLifecycle(Action action) noexcept {
  try {
    auto profiler = Profiler::GetInstance();
    if (profiler == nullptr) {
      Log::Error("Profiler instance is unavailable");
      return false;
    }
    return action(*profiler);
  } catch (const std::exception& error) {
    LogLifecycleException(error.what());
  } catch (...) {
    LogLifecycleException("Non-standard exception");
  }
  return false;
}
}  // namespace

DD_WIN_PROF_API bool SetupProfilingDataCallback(
    const ProfilingDataCallback& callback
) noexcept {
  return RunLifecycle([&](Profiler& profiler) {
    return profiler.SetProfilingDataCallback(callback);
  });
}

namespace dd_win_prof {
DD_WIN_PROF_API bool SetupProfiler(const ProfilerConfig& settings) noexcept {
  return RunLifecycle([&](Profiler& profiler) { return profiler.Configure(settings); });
}

DD_WIN_PROF_API bool StartProfiler() noexcept {
  return RunLifecycle([](Profiler& profiler) { return profiler.StartProfiling(); });
}

DD_WIN_PROF_API bool StopProfiler() noexcept {
  return RunLifecycle([](Profiler& profiler) { return profiler.StopProfiling(); });
}
}  // namespace dd_win_prof

DD_WIN_PROF_API bool SetupProfiler(const ProfilerConfig* pSettings) {
  if (pSettings == nullptr) {
    LogLifecycleException("Null profiler configuration structure");
    return false;
  }
  return dd_win_prof::SetupProfiler(*pSettings);
}

DD_WIN_PROF_API bool StartProfiler() { return dd_win_prof::StartProfiler(); }

DD_WIN_PROF_API bool StopProfiler() { return dd_win_prof::StopProfiler(); }

DD_WIN_PROF_API const char* GetProfilingErrorMessage(uint32_t result) noexcept {
  switch (static_cast<ProfilingErrorCode>(result)) {
    case ProfilingErrorCode::None:
      return "Success";
    case ProfilingErrorCode::SerializationFailed:
      return "Profile serialization failed";
    case ProfilingErrorCode::ProfileBytesUnavailable:
      return "Encoded profile bytes are unavailable";
    case ProfilingErrorCode::ProfileResetFailed:
      return "Profile reset failed";
    case ProfilingErrorCode::SampleLabelsUnavailable:
      return "Sample labels are unavailable";
    default:
      return "Unknown profiling error";
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
