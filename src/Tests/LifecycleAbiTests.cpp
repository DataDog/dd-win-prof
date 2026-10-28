// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

// The legacy declarations are frozen at PR #85 (a540f30).
#include "dd-win-prof-diagnostics.h"
#include "fixtures/profiler-85/dd-win-prof.h"

static_assert(sizeof(ProfilerConfig) == 96);
static_assert(offsetof(ProfilerConfig, tags) == 72);
static_assert(offsetof(ProfilerConfig, pprofOutputDirectory) == 88);

namespace {
int failures = 0;
void Check(bool condition, const char* description) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", description);
    ++failures;
  }
}

template <typename Function>
Function Resolve(HMODULE module, const char* name) {
  return reinterpret_cast<Function>(::GetProcAddress(module, name));
}
}  // namespace

int main(int argc, char** argv) {
  const bool expectLegacy = argc > 1 && std::strcmp(argv[1], "--expect-legacy") == 0;
  // These calls use the original import-library ABI, not the new declarations.
  Check(!SetupProfiler(nullptr), "legacy null setup returns false");
  StopProfiler();
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.uploadIntervalSeconds = 3600;
  Check(SetupProfiler(&config), "legacy config layout is accepted");
  Check(StartProfiler(), "legacy start succeeds");
  Check(!StartProfiler(), "legacy repeated start returns false");
  StopProfiler();
  StopProfiler();

  const auto module = ::GetModuleHandleW(L"dd-win-prof.dll");
  Check(module != nullptr, "profiler DLL was imported");
  const auto setup = Resolve<decltype(&SetupProfilerV1)>(module, "SetupProfilerV1");
  const auto start = Resolve<decltype(&StartProfilerV1)>(module, "StartProfilerV1");
  const auto stop = Resolve<decltype(&StopProfilerV1)>(module, "StopProfilerV1");
  const auto details = Resolve<decltype(&GetLastProfilerDiagnosticV1)>(
      module, "GetLastProfilerDiagnosticV1"
  );
  if (expectLegacy) {
    Check(!setup && !start && !stop && !details, "old DLL has no V1 exports");
    return failures == 0 ? 0 : 1;
  }
  Check(setup && start && stop && details, "all additive C exports resolve");
  if (!setup || !start || !stop || !details) {
    return 1;
  }

  Check(setup(nullptr) == DD_WIN_PROF_ERROR, "V1 setup returns error severity");
  int32_t severity = 0;
  uint32_t code = 0;
  const auto required = details(&severity, &code, nullptr, 0);
  Check(severity == DD_WIN_PROF_ERROR, "getter returns same severity");
  Check(code == DD_WIN_PROF_CODE_INVALID_ARGUMENT, "getter returns stable code");
  Check(required > 1, "getter reports required message size");
  std::vector<char> message(required);
  Check(
      details(nullptr, nullptr, message.data(), required) == required,
      "size query is non-consuming"
  );
  Check(std::strlen(message.data()) + 1 == required, "message is NUL terminated");
  char one = 'x';
  details(nullptr, nullptr, &one, 1);
  Check(one == '\0', "one-byte buffer receives only NUL");

  uint32_t otherCode = 0;
  uint32_t initiallyEmpty = 1;
  std::thread other([&] {
    initiallyEmpty = details(nullptr, nullptr, nullptr, 0);
    ProfilerConfig invalid{};
    setup(&invalid);
    details(nullptr, &otherCode, nullptr, 0);
  });
  other.join();
  Check(initiallyEmpty == 0, "new thread has no diagnostic");
  Check(
      otherCode == DD_WIN_PROF_CODE_UNSUPPORTED_CONFIG_SIZE,
      "other thread has its own diagnostic"
  );
  details(nullptr, &code, nullptr, 0);
  Check(
      code == DD_WIN_PROF_CODE_INVALID_ARGUMENT,
      "other thread did not overwrite caller diagnostic"
  );

  for (int cycle = 0; cycle < 2; ++cycle) {
    Check(setup(&config) == DD_WIN_PROF_SUCCESS, "V1 setup succeeds");
    Check(details(&severity, &code, nullptr, 0) == 0, "success clears last diagnostic");
    Check(start() == DD_WIN_PROF_SUCCESS, "V1 start succeeds");
    Check(start() == DD_WIN_PROF_ERROR, "V1 repeated start returns severity");
    Check(stop() == DD_WIN_PROF_SUCCESS, "V1 stop succeeds");
    Check(stop() == DD_WIN_PROF_SUCCESS, "V1 repeated stop succeeds");
  }
  std::printf("Lifecycle DLL ABI checks: %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
