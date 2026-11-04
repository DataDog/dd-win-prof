// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

// clang-format off: verify the SDK header is self-contained.
#include "internal/dd-win-rum-private.h"
// clang-format on

#include <iostream>
#include <utility>

int TestSdkCallback() {
  std::vector<ProfilingData> results;
  const ProfilingDataCallback callback = [&](ProfilingData data) {
    if (data.result != 0) {
      std::cerr << "Profile delivery failed: " << GetProfilingErrorMessage(data.result)
                << '\n';
    }
    results.push_back(std::move(data));
  };
  if (!SetupProfilingDataCallback(callback)) {
    std::cerr << "Failed to register the profiling callback\n";
    return 1;
  }

  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  config.uploadIntervalSeconds = 3600;
  const bool configured = SetupProfiler(&config);
  if (!configured || !StartProfiler()) {
    std::cerr << "Failed to " << (configured ? "start" : "configure")
              << " the profiler\n";
    if (!SetupProfilingDataCallback({})) {
      std::cerr << "Failed to unregister the profiling callback\n";
    }
    return 1;
  }

  StopProfiler();
  const bool unregistered = SetupProfilingDataCallback({});
  if (!unregistered) {
    std::cerr << "Failed to unregister the profiling callback\n";
    return 1;
  }

  if (results.size() != 1) {
    std::cerr << "Expected one callback on stop, got " << results.size() << '\n';
    return 1;
  }

  const auto& profile = results.front();
  if (profile.result != 0) {
    std::cerr << "Expected a profile, got: " << GetProfilingErrorMessage(profile.result)
              << '\n';
    return 1;
  }
  if (!profile.isLast) {
    std::cerr << "Profile delivered on stop was not marked final\n";
    return 1;
  }
  if (profile.pprof.size() < 4 || profile.pprof[0] != 0x28 ||
      profile.pprof[1] != 0xb5 || profile.pprof[2] != 0x2f ||
      profile.pprof[3] != 0xfd) {
    std::cerr << "Final profile is not a Zstd frame\n";
    return 1;
  }
  if (profile.startUnixTimeMs <= 0) {
    std::cerr << "Final profile has an invalid start timestamp\n";
    return 1;
  }
  if (profile.endUnixTimeMs < profile.startUnixTimeMs) {
    std::cerr << "Final profile ends before it starts\n";
    return 1;
  }
  return 0;
}
