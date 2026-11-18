// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

// clang-format off: verify the SDK header is self-contained.
#include "internal/dd-win-rum-private.h"
// clang-format on

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

namespace {
static_assert(
    offsetof(ProfilingData, reserved) == offsetof(ProfilingData, is_last) + 1
);
static_assert(sizeof(ProfilingData{}.reserved) == 7);
static_assert(
    sizeof(ProfilingData) ==
    offsetof(ProfilingData, reserved) + sizeof(ProfilingData{}.reserved)
);

bool HasZstdMagic(const std::vector<std::uint8_t>& bytes) {
  return bytes.size() >= 4 && bytes[0] == 0x28 && bytes[1] == 0xb5 &&
         bytes[2] == 0x2f && bytes[3] == 0xfd;
}

struct ProfileSnapshot {
  std::uint32_t result;
  std::vector<std::uint8_t> bytes;
  std::int64_t startUnixTimeMs;
  std::int64_t endUnixTimeMs;
  bool isLast;
  bool reservedBytesAreZero;
};

struct CallbackContext {
  std::vector<ProfileSnapshot> results;
};

void DD_WIN_PROF_CALL CaptureProfile(const ProfilingData* data, void* userData) {
  auto& context = *static_cast<CallbackContext*>(userData);
  ProfileSnapshot snapshot{
      data->result,
      {},
      data->start_unix_time_ms,
      data->end_unix_time_ms,
      data->is_last != 0,
      std::all_of(
          std::begin(data->reserved), std::end(data->reserved), [](std::uint8_t byte) {
            return byte == 0;
          }
      )
  };
  if (data->result != 0) {
    std::cerr << "Profile delivery failed: " << GetProfilingErrorMessage(data->result)
              << '\n';
  } else if (data->pprof_size != 0) {
    snapshot.bytes.assign(data->pprof_bytes, data->pprof_bytes + data->pprof_size);
  }
  context.results.push_back(std::move(snapshot));
}
}  // namespace

int TestSdkCallback() {
  CallbackContext context;
  if (!SetupProfilingDataCallback(CaptureProfile, &context)) {
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
    if (!SetupProfilingDataCallback(nullptr, nullptr)) {
      std::cerr << "Failed to unregister the profiling callback\n";
    }
    return 1;
  }

  StopProfiler();
  const bool unregistered = SetupProfilingDataCallback(nullptr, nullptr);
  if (!unregistered) {
    std::cerr << "Failed to unregister the profiling callback\n";
    return 1;
  }

  if (context.results.size() != 1) {
    std::cerr << "Expected one callback on stop, got " << context.results.size()
              << '\n';
    return 1;
  }

  const auto& profile = context.results.front();
  if (profile.result != 0) {
    std::cerr << "Expected a profile, got: " << GetProfilingErrorMessage(profile.result)
              << '\n';
    return 1;
  }
  if (!profile.isLast) {
    std::cerr << "Profile delivered on stop was not marked final\n";
    return 1;
  }
  if (!profile.reservedBytesAreZero) {
    std::cerr << "Profile delivered nonzero reserved bytes\n";
    return 1;
  }
  if (!HasZstdMagic(profile.bytes)) {
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
