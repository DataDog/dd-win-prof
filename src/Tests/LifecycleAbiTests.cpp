// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "dd-win-prof.h"

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
  Check(SetupProfiler(nullptr) == DD_WIN_PROF_ERROR, "setup returns error severity");
  int32_t severity = 0;
  uint32_t code = 0;
  const auto required = GetLastProfilerDiagnostic(&severity, &code, nullptr, 0);
  Check(severity == DD_WIN_PROF_ERROR, "getter returns same severity");
  Check(code == DD_WIN_PROF_CODE_INVALID_ARGUMENT, "getter returns stable code");
  Check(required > 1, "getter reports required message size");
  if (required <= 1) {
    return 1;
  }
  std::vector<char> message(required);
  Check(
      GetLastProfilerDiagnostic(nullptr, nullptr, message.data(), required) == required,
      "size query is non-consuming"
  );
  Check(std::strlen(message.data()) + 1 == required, "message is NUL terminated");
  char one = 'x';
  GetLastProfilerDiagnostic(nullptr, nullptr, &one, 1);
  Check(one == '\0', "one-byte buffer receives only NUL");

  uint32_t otherCode = 0;
  uint32_t initiallyEmpty = 1;
  std::thread other([&] {
    initiallyEmpty = GetLastProfilerDiagnostic(nullptr, nullptr, nullptr, 0);
    ProfilerConfig invalid{};
    SetupProfiler(&invalid);
    GetLastProfilerDiagnostic(nullptr, &otherCode, nullptr, 0);
  });
  other.join();
  Check(initiallyEmpty == 0, "new thread has no diagnostic");
  Check(
      otherCode == DD_WIN_PROF_CODE_UNSUPPORTED_CONFIG_SIZE,
      "other thread has its own diagnostic"
  );
  GetLastProfilerDiagnostic(nullptr, &code, nullptr, 0);
  Check(
      code == DD_WIN_PROF_CODE_INVALID_ARGUMENT,
      "other thread did not overwrite caller diagnostic"
  );

  ProfilerConfig config{};
  config.size = sizeof(config);
  config.uploadIntervalSeconds = 3600;
  for (int cycle = 0; cycle < 2; ++cycle) {
    Check(SetupProfiler(&config) == DD_WIN_PROF_SUCCESS, "setup succeeds");
    Check(
        GetLastProfilerDiagnostic(&severity, &code, nullptr, 0) == 0,
        "success clears last diagnostic"
    );
    Check(StartProfiler() == DD_WIN_PROF_SUCCESS, "start succeeds");
    Check(StartProfiler() == DD_WIN_PROF_ERROR, "repeated start returns severity");
    Check(StopProfiler() == DD_WIN_PROF_SUCCESS, "stop succeeds");
    Check(StopProfiler() == DD_WIN_PROF_SUCCESS, "repeated stop succeeds");
  }
  std::printf("Lifecycle DLL ABI checks: %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
