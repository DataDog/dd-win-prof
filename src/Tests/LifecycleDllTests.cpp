// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

// clang-format off: verify the private lifecycle header is self-contained.
#include "internal/dd-win-prof-private.h"
// clang-format on

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <thread>
#include <type_traits>

#include "internal/dd-win-rum-private.h"

static_assert(std::is_same_v<ProfilerConfig, _ProfilerConfig>);
static_assert(
    std::is_same_v<decltype(&::SetupProfiler), bool (*)(const ProfilerConfig*)>
);
static_assert(std::is_same_v<decltype(&::StartProfiler), bool (*)()>);
static_assert(std::is_same_v<decltype(&::StopProfiler), bool (*)()>);
static_assert(std::is_same_v<
              decltype(&dd_win_prof::SetupProfiler),
              bool (*)(const ProfilerConfig&) noexcept>);
static_assert(
    std::is_same_v<decltype(&dd_win_prof::StartProfiler), bool (*)() noexcept>
);
static_assert(
    std::is_same_v<decltype(&dd_win_prof::StopProfiler), bool (*)() noexcept>
);
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
  const auto module = ::GetModuleHandleW(L"dd-win-prof.dll");
  Check(module != nullptr, "profiler DLL is loaded");
  if (module != nullptr) {
    for (const char* name : {"SetupProfiler", "StartProfiler", "StopProfiler"}) {
      Check(::GetProcAddress(module, name) != nullptr, name);
    }
  }

  Check(!::SetupProfiler(nullptr), "null public setup returns false");
  ProfilerConfig invalid{};
  Check(!::SetupProfiler(&invalid), "invalid public setup returns false");
  Check(!dd_win_prof::SetupProfiler(invalid), "invalid private setup returns false");
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
  Check(::StopProfiler(), "public stop succeeds when already stopped");
  Check(dd_win_prof::StopProfiler(), "private stop succeeds when already stopped");

  ProfilerConfig config{};
  config.size = sizeof(config);
  config.uploadIntervalSeconds = 3600;
  for (int cycle = 0; cycle < 2; ++cycle) {
    Check(::SetupProfiler(&config), "public setup succeeds");
    Check(::StartProfiler(), "public start succeeds");
    Check(!::StartProfiler(), "repeated public start returns false");
    Check(::StopProfiler(), "public stop succeeds");
    Check(::StopProfiler(), "repeated public stop succeeds");
    Check(dd_win_prof::StartProfiler(), "private start succeeds after public stop");
    Check(dd_win_prof::StopProfiler(), "private stop succeeds");
  }
  for (int cycle = 0; cycle < 2; ++cycle) {
    Check(dd_win_prof::SetupProfiler(config), "private setup succeeds");
    Check(dd_win_prof::StartProfiler(), "private start succeeds");
    Check(!dd_win_prof::StartProfiler(), "repeated private start returns false");
    Check(dd_win_prof::StopProfiler(), "private stop succeeds");
    Check(dd_win_prof::StopProfiler(), "repeated private stop succeeds");
  }
  Check(
      message == GetProfilingErrorMessage(1),
      "message remains valid after lifecycle calls"
  );
  std::printf("Lifecycle DLL checks: %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
