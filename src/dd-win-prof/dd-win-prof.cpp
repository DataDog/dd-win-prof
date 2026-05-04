// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "Log.h"
#include "Profiler.h"
#include "dd-win-prof-internal.h"
#include "dd-win-rum-private.h"
#include "pch.h"

extern "C" {

DD_WIN_PROF_API bool SetupProfiler(const ProfilerConfig* pSettings) {
  if (pSettings == nullptr) {
    Log::Warn("Null profiler configuration structure.");
    return false;
  }

  if (pSettings->size != sizeof(ProfilerConfig)) {
    Log::Warn("Invalid profiler configuration structure.");
    return false;
  }

  auto profiler = Profiler::GetInstance();
  if (profiler == nullptr) {
    Log::Warn(
        "Profiler instance is not created: missing Process Attach event in DllMain."
    );
    return false;
  }

  if (profiler->IsStarted()) {
    Log::Warn("SetupProfiler() must be called before StartProfiler().");
    return false;
  }

  return InitializeConfiguration(Profiler::GetConfiguration(), pSettings);
}

DD_WIN_PROF_API bool StartProfiler() {
  auto profiler = Profiler::GetInstance();
  if (profiler == nullptr) {
    Log::Warn(
        "Profiler instance is not created: missing Process Attach event in DllMain."
    );
    return false;
  }

  if (profiler->IsStarted()) {
    Log::Warn("Profiler is already running.");
    return false;
  }

  return profiler->StartProfiling();
}

DD_WIN_PROF_API void StopProfiler() {
  auto profiler = Profiler::GetInstance();
  if (profiler == nullptr) {
    Log::Warn(
        "Profiler instance is not created: missing Process Attach event in DllMain."
    );
    return;
  }

  if (!profiler->IsStarted()) {
    Log::Warn("Profiler is not running.");
    return;
  }

  profiler->StopProfiling();
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