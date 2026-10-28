// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <type_traits>

#include "dd-win-prof.h"

static_assert(std::is_same_v<decltype(StartProfiler()), ProfilingSeverity>);
static_assert(std::is_trivially_copyable_v<ProfilingDiagnostic>);

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
  Check(
      SetupProfiler(ProfilerConfig{}) == ProfilingSeverity::Error,
      "setup returns error severity"
  );
  const auto diagnostic = GetLastProfilerDiagnostic();
  Check(
      diagnostic.severity == ProfilingSeverity::Error, "getter returns same severity"
  );
  Check(
      diagnostic.code == ProfilingErrorCode::UnsupportedConfigSize,
      "getter returns typed code"
  );
  const std::string saved = diagnostic.message;
  Check(!saved.empty(), "getter borrows message text");
  Check(
      GetLastProfilerDiagnostic().message == diagnostic.message,
      "getter does not consume or copy text"
  );

  ProfilingSeverity initialSeverity = ProfilingSeverity::Fatal;
  std::thread other([&] {
    initialSeverity = GetLastProfilerDiagnostic().severity;
    SetupProfiler(ProfilerConfig{});
    StopProfiler();
  });
  other.join();
  Check(initialSeverity == ProfilingSeverity::Success, "new thread has no diagnostic");
  Check(
      GetLastProfilerDiagnostic().code == diagnostic.code,
      "other thread did not clear caller diagnostic"
  );
  Check(
      std::strcmp(diagnostic.message, saved.c_str()) == 0,
      "other thread exit did not invalidate borrowed text"
  );
  Check(StopProfiler() == ProfilingSeverity::Success, "already stopped succeeds");
  Check(
      GetLastProfilerDiagnostic().severity == ProfilingSeverity::Success,
      "success clears diagnostic"
  );
  Check(
      saved == "Invalid profiler configuration structure size",
      "caller-owned copy survives next operation"
  );

  ProfilerConfig config{};
  config.size = sizeof(config);
  config.uploadIntervalSeconds = 3600;
  for (int cycle = 0; cycle < 2; ++cycle) {
    Check(SetupProfiler(config) == ProfilingSeverity::Success, "setup succeeds");
    Check(StartProfiler() == ProfilingSeverity::Success, "start succeeds");
    Check(
        StartProfiler() == ProfilingSeverity::Error, "repeated start returns severity"
    );
    Check(StopProfiler() == ProfilingSeverity::Success, "stop succeeds");
    Check(StopProfiler() == ProfilingSeverity::Success, "repeated stop succeeds");
  }
  std::printf("Lifecycle DLL checks: %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
