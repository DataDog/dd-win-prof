// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <cstdio>
#include <string>
#include <thread>
#include <type_traits>

#include "internal/dd-win-rum-private.h"

static_assert(std::is_same_v<decltype(StartProfiler()), bool>);
static_assert(std::is_same_v<decltype(SetupProfiler(ProfilerConfig{})), bool>);
static_assert(std::is_same_v<decltype(StopProfiler()), void>);
static_assert(std::is_same_v<decltype(GetLastProfilerError()), const ProfilingError*>);

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
  const auto* error = GetLastProfilerError();
  Check(error != nullptr, "getter returns a rich error");
  if (!error) {
    return 1;
  }
  Check(error->severity == ProfilingSeverity::Error, "getter returns severity");
  Check(
      error->code == ProfilingErrorCode::UnsupportedConfigSize,
      "getter returns typed code"
  );
  const std::string saved = error->message;
  Check(!saved.empty(), "getter borrows error text");
  Check(GetLastProfilerError() == error, "getter does not consume or copy the error");

  bool initiallyEmpty = false;
  std::thread other([&] {
    initiallyEmpty = GetLastProfilerError() == nullptr;
    SetupProfiler(ProfilerConfig{});
    StopProfiler();
  });
  other.join();
  Check(initiallyEmpty, "new thread has no error");
  Check(GetLastProfilerError() == error, "other thread did not clear caller error");
  Check(error->message == saved, "other thread exit did not invalidate borrowed text");
  StopProfiler();
  Check(GetLastProfilerError() == nullptr, "successful stop clears the error");
  Check(
      saved == "Invalid profiler configuration structure size",
      "caller-owned copy survives next operation"
  );

  ProfilerConfig config{};
  config.size = sizeof(config);
  config.uploadIntervalSeconds = 3600;
  for (int cycle = 0; cycle < 2; ++cycle) {
    Check(SetupProfiler(config), "setup succeeds");
    Check(StartProfiler(), "start succeeds");
    Check(!StartProfiler(), "repeated start returns false");
    StopProfiler();
    Check(GetLastProfilerError() == nullptr, "stop succeeds");
    StopProfiler();
    Check(GetLastProfilerError() == nullptr, "repeated stop succeeds");
  }
  std::printf("Lifecycle DLL checks: %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
