// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "Log.h"
#include "Profiler.h"
#include "ProfilingErrorCode.h"
#include "dd-win-prof-internal.h"
#include "internal/dd-win-rum-private.h"
#include "pch.h"

namespace {
void LogApiException(const char* api, const char* message) noexcept {
  try {
    Log::Error(api, " failed: ", message);
  } catch (...) {
    // Logging must not let exceptions escape the public API.
  }
}

// Exception firewall of every C API entry that uses the Profiler: no exception crosses
// the C ABI, and a missing instance or an exception returns false.
template <typename Action>
bool CallProfiler(const char* api, Action action) noexcept {
  try {
    auto profiler = Profiler::GetInstance();
    if (profiler == nullptr) {
      Log::Warn(
          "Profiler instance is not created: missing Process Attach event in DllMain."
      );
      return false;
    }
    profiler->BeforeApiCall();
    return action(*profiler);
  } catch (const std::exception& error) {
    LogApiException(api, error.what());
  } catch (...) {
    LogApiException(api, "non-standard exception");
  }
  return false;
}
}  // namespace

extern "C" {

DD_WIN_PROF_API bool DD_WIN_PROF_CALL
SetupProfilingDataCallback(ProfilingDataCallback callback, void* userData) {
  return CallProfiler("SetupProfilingDataCallback()", [&](Profiler& profiler) {
    return profiler.SetProfilingDataCallback(callback, userData);
  });
}

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
    case ProfilingErrorCode::InternalError:
      return "Internal profiler error";
    default:
      return "Unknown profiling error";
  }
}

DD_WIN_PROF_API bool SetupProfiler(const ProfilerConfig* pSettings) {
  return CallProfiler("SetupProfiler()", [&](Profiler& profiler) {
    if (pSettings == nullptr) {
      Log::Warn("Null profiler configuration structure.");
      return false;
    }

    if (pSettings->size != sizeof(ProfilerConfig)) {
      Log::Warn("Invalid profiler configuration structure.");
      return false;
    }

    return profiler.Setup(pSettings);
  });
}

DD_WIN_PROF_API bool StartProfiler() {
  return CallProfiler("StartProfiler()", [](Profiler& profiler) {
    return profiler.StartProfiling();
  });
}

DD_WIN_PROF_API bool MonitorWindowHangs(HWND hWnd) {
  return CallProfiler("MonitorWindowHangs()", [&](Profiler& profiler) {
    return profiler.MonitorWindowHangs(hWnd);
  });
}

DD_WIN_PROF_API void StopProfiler() {
  CallProfiler("StopProfiler()", [](Profiler& profiler) {
    profiler.StopProfiling();
    return true;
  });
}

DD_WIN_PROF_API bool EnterView(const char* viewName) {
  return CallProfiler("EnterView()", [&](Profiler& profiler) {
    return profiler.EnterView(viewName);
  });
}

DD_WIN_PROF_API bool LeaveCurrentView() {
  return CallProfiler("LeaveCurrentView()", [](Profiler& profiler) {
    return profiler.LeaveCurrentView();
  });
}

DD_WIN_PROF_API bool SetRumSession(const RumSessionContext* pContext) {
  return CallProfiler("SetRumSession()", [&](Profiler& profiler) {
    return profiler.SetRumSession(pContext);
  });
}

DD_WIN_PROF_API bool SetRumView(const RumViewValues* pContext) {
  return CallProfiler("SetRumView()", [&](Profiler& profiler) {
    return profiler.SetRumView(pContext);
  });
}
}
