// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

// clang-format off: verify the SDK header is self-contained.
#include "internal/dd-win-rum-private.h"
// clang-format on

#include <iostream>
#include <utility>

int TestSdkCallback() {
  std::vector<ProfilingDataResult> results;
  const ProfilingDataCallback callback = [&](ProfilingDataResult result) {
    results.push_back(std::move(result));
  };
  if (auto error = SetupProfilingDataCallback(callback)) {
    std::cerr << "Failed to register the profiling callback: " << error->message
              << '\n';
    return 1;
  }

  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  config.uploadIntervalSeconds = 3600;
  if (!SetupProfiler(&config) || !StartProfiler()) {
    std::cerr << "Failed to configure or start the profiler\n";
    if (auto error = SetupProfilingDataCallback({})) {
      std::cerr << "Failed to unregister the profiling callback: " << error->message
                << '\n';
    }
    return 1;
  }

  StopProfiler();
  if (auto error = SetupProfilingDataCallback({})) {
    std::cerr << "Failed to unregister the profiling callback: " << error->message
              << '\n';
    return 1;
  }

  if (results.size() != 1) {
    std::cerr << "Expected one callback on stop, got " << results.size() << '\n';
    return 1;
  }

  const auto* profile = std::get_if<ProfilingData>(&results.front());
  if (profile == nullptr) {
    std::cerr << "Expected a profile, got an error: "
              << std::get<ProfilingError>(results.front()).message << '\n';
    return 1;
  }
  if (!profile->isLast) {
    std::cerr << "Profile delivered on stop was not marked final\n";
    return 1;
  }
  if (profile->pprof.empty()) {
    std::cerr << "Final profile has no pprof bytes\n";
    return 1;
  }
  if (profile->startUnixTimeMs <= 0) {
    std::cerr << "Final profile has an invalid start timestamp\n";
    return 1;
  }
  if (profile->endUnixTimeMs < profile->startUnixTimeMs) {
    std::cerr << "Final profile ends before it starts\n";
    return 1;
  }
  return 0;
}
