// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <cstdio>
#include <cstring>
#include <thread>
#include <type_traits>

#include "internal/dd-win-rum-private.h"

static_assert(std::is_same_v<decltype(StartProfiler()), bool>);
static_assert(std::is_same_v<decltype(SetupProfiler(ProfilerConfig{})), bool>);
static_assert(std::is_same_v<decltype(StopProfiler()), bool>);
static_assert(std::is_same_v<decltype(GetProfilingErrorMessage(0)), const char*>);

namespace {
int failures = 0;
void Check(bool condition, const char* description) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", description);
    ++failures;
  }
}
}  // namespace

int main() {
  Check(!SetupProfiler(ProfilerConfig{}), "invalid setup returns false");
  const char* message = GetProfilingErrorMessage(1);
  Check(
      std::strcmp(message, "Profile serialization failed") == 0,
      "known callback code has a static message"
  );
  Check(
      std::strcmp(GetProfilingErrorMessage(UINT32_MAX), "Unknown profiling error") == 0,
      "unknown callback code has a fallback message"
  );

  const char* otherMessage = nullptr;
  std::thread other([&] { otherMessage = GetProfilingErrorMessage(3); });
  other.join();
  Check(
      otherMessage != nullptr && std::strcmp(otherMessage, "Profile reset failed") == 0,
      "message can be read after the requesting thread exits"
  );
  Check(
      std::strcmp(message, "Profile serialization failed") == 0,
      "another lookup does not invalidate earlier messages"
  );
  Check(StopProfiler(), "stop succeeds when already stopped");

  ProfilerConfig config{};
  config.size = sizeof(config);
  config.uploadIntervalSeconds = 3600;
  for (int cycle = 0; cycle < 2; ++cycle) {
    Check(SetupProfiler(config), "setup succeeds");
    Check(StartProfiler(), "start succeeds");
    Check(!StartProfiler(), "repeated start returns false");
    Check(StopProfiler(), "stop succeeds");
    Check(StopProfiler(), "repeated stop succeeds");
  }
  Check(
      message == GetProfilingErrorMessage(1),
      "message remains valid after lifecycle calls"
  );
  std::printf("Lifecycle DLL checks: %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
