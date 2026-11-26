// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

// clang-format off: verify the SDK header is self-contained.
#include "internal/dd-win-rum-private.h"
// clang-format on

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <stdexcept>
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

ProfilerConfig MakeConfig(std::int32_t uploadIntervalSeconds) {
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  config.uploadIntervalSeconds = uploadIntervalSeconds;
  return config;
}

// Calls every lifecycle and RUM API from one periodic or final callback.
struct ReentryContext {
  bool fromPeriodic = false;
  bool reentered = false;
  int finalCalls = 0;
  bool setupRejected = false;
  bool registrationRejected = false;
  bool startRejected = false;
  bool monitorRejected = false;
  bool rumAllowed = false;
  std::chrono::steady_clock::duration elapsed{};
  std::promise<void> done;
};

void DD_WIN_PROF_CALL ReenterApis(const ProfilingData* data, void* userData) {
  auto& context = *static_cast<ReentryContext*>(userData);
  const bool isLast = data->is_last != 0;
  context.finalCalls += isLast ? 1 : 0;
  if (isLast == context.fromPeriodic || context.reentered) {
    return;
  }
  context.reentered = true;
  const auto start = std::chrono::steady_clock::now();
  const auto config = MakeConfig(1);
  context.setupRejected = !SetupProfiler(&config);
  context.registrationRejected = !SetupProfilingDataCallback(nullptr, nullptr);
  context.startRejected = !StartProfiler();
  context.monitorRejected = !MonitorWindowHangs(nullptr);
  StopProfiler();
  RumSessionContext session{"app-1", "session-1"};
  context.rumAllowed = SetRumSession(&session) && EnterView("CallbackView") &&
                       LeaveCurrentView() && SetRumSession(nullptr);
  context.elapsed = std::chrono::steady_clock::now() - start;
  context.done.set_value();
}

const char* Rejected(bool rejected) { return rejected ? "rejected" : "ACCEPTED"; }

// The rejected calls must not change the registration or the run: the run keeps going
// until the caller stops it, and a new run delivers to the same callback.
int TestReentryFrom(bool fromPeriodic) {
  const char* name = fromPeriodic ? "periodic" : "final";
  ReentryContext context;
  context.fromPeriodic = fromPeriodic;
  auto done = context.done.get_future();
  const auto config = MakeConfig(fromPeriodic ? 1 : 3600);
  if (!SetupProfilingDataCallback(ReenterApis, &context) || !SetupProfiler(&config) ||
      !StartProfiler()) {
    std::cerr << "Failed to start the " << name << " reentry run\n";
    SetupProfilingDataCallback(nullptr, nullptr);
    return 1;
  }
  bool keptRunning = true;
  if (fromPeriodic) {
    if (done.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
      std::cerr << "No periodic callback within 10 s\n";
    }
    // a start is rejected only while the run is still active
    keptRunning = !StartProfiler();
  }
  StopProfiler();
  const bool restarted = StartProfiler();
  StopProfiler();
  const bool unregistered = SetupProfilingDataCallback(nullptr, nullptr);

  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(context.elapsed).count();
  std::cout << name << " callback reentry: setup " << Rejected(context.setupRejected)
            << ", registration " << Rejected(context.registrationRejected) << ", start "
            << Rejected(context.startRejected) << ", monitor "
            << Rejected(context.monitorRejected) << ", stop returned, RUM "
            << (context.rumAllowed ? "allowed" : "FAILED") << ", " << ms << " ms\n";
  if (!context.reentered || !context.setupRejected || !context.registrationRejected ||
      !context.startRejected || !context.monitorRejected || !context.rumAllowed ||
      !keptRunning || !restarted || !unregistered || context.finalCalls != 2) {
    std::cerr << "Calls from the " << name << " callback were not rejected cleanly\n";
    return 1;
  }
  return 0;
}

// Throws a standard exception, then a non-standard one, from each kind of callback.
struct ThrowingContext {
  int periodicCalls = 0;
  int finalCalls = 0;
  std::promise<void> twoPeriodicCalls;
};

void DD_WIN_PROF_CALL ThrowFromCallback(const ProfilingData* data, void* userData) {
  auto& context = *static_cast<ThrowingContext*>(userData);
  const int call = data->is_last != 0 ? ++context.finalCalls : ++context.periodicCalls;
  if (data->is_last == 0 && call == 2) {
    context.twoPeriodicCalls.set_value();
  }
  if (call == 1) {
    throw std::runtime_error("injected callback failure");
  }
  throw 42;
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

int TestCallbackReentry() {
  return TestReentryFrom(true) == 0 && TestReentryFrom(false) == 0 ? 0 : 1;
}

// The DLL contains the exceptions: periodic delivery goes on and stop returns.
int TestCallbackExceptions() {
  ThrowingContext context;
  auto twoPeriodicCalls = context.twoPeriodicCalls.get_future();
  const auto config = MakeConfig(1);
  if (!SetupProfilingDataCallback(ThrowFromCallback, &context) ||
      !SetupProfiler(&config)) {
    std::cerr << "Failed to set up the throwing callback run\n";
    SetupProfilingDataCallback(nullptr, nullptr);
    return 1;
  }
  bool started = StartProfiler();
  const bool periodicContinued =
      started &&
      twoPeriodicCalls.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
  StopProfiler();
  started = started && StartProfiler();
  StopProfiler();
  const bool unregistered = SetupProfilingDataCallback(nullptr, nullptr);

  std::cout << "callback exceptions: " << context.periodicCalls << " periodic and "
            << context.finalCalls
            << " final callbacks threw (standard first, then non-standard)\n";
  if (!started || !periodicContinued || context.finalCalls != 2 || !unregistered) {
    std::cerr << "Callback exceptions were not contained\n";
    return 1;
  }
  return 0;
}
