// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "internal/dd-win-rum-private.h"

bool TestSdkCallback() {
  int calls = 0;
  bool finalProfile = false;
  std::vector<uint8_t> bytes;
  ProfilingDataCallback callback = [&](ProfilingDataResult result) {
    ++calls;
    if (auto* profile = std::get_if<ProfilingData>(&result)) {
      finalProfile = profile->isLast && profile->startUnixTimeMs > 0 &&
                     profile->endUnixTimeMs >= profile->startUnixTimeMs;
      bytes.swap(profile->pprof);
    }
  };
  if (!SetupProfilingDataCallback(callback)) {
    return false;
  }
  callback = {};

  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  config.uploadIntervalSeconds = 3600;
  if (!SetupProfiler(&config) || !StartProfiler()) {
    SetupProfilingDataCallback({});
    return false;
  }
  StopProfiler();
  const bool unregistered = SetupProfilingDataCallback({});
  return unregistered && calls == 1 && finalProfile && !bytes.empty();
}
