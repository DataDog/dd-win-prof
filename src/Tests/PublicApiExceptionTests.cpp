// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <optional>
#include <stdexcept>

#include "Configuration.h"
#include "Profiler.h"
#include "internal/dd-win-rum-private.h"
#include "pch.h"

namespace {
void DD_WIN_PROF_CALL IgnoreProfile(const ProfilingData*, void*) {}

static_assert(noexcept(GetProfilingErrorMessage(0)));
}  // namespace

class PublicApiExceptionTests : public ::testing::Test {
 protected:
  void SetUp() override {
    previous = *Profiler::GetConfiguration();
    Profiler::GetConfiguration()->ResetToDefaults();
    profiler = std::make_unique<Profiler>();
  }

  void TearDown() override {
    if (profiler != nullptr) {
      profiler->_beforeApiCall = nullptr;
      if (profiler->IsStarted()) {
        StopProfiler();
      }
      EXPECT_TRUE(SetupProfilingDataCallback(nullptr, nullptr));
      profiler.reset();
    }
    *Profiler::GetConfiguration() = std::move(*previous);
  }

  void StartWithCallback() {
    ASSERT_TRUE(SetupProfilingDataCallback(IgnoreProfile, nullptr));
    ProfilerConfig config{};
    config.size = sizeof(config);
    config.noEnvVars = true;
    config.uploadIntervalSeconds = 3600;
    ASSERT_TRUE(SetupProfiler(&config));
    ASSERT_TRUE(StartProfiler());
  }

  // Every C API entry throws before running, and counts the calls.
  template <typename Exception>
  void ThrowOnApiCall(Exception exception) {
    profiler->_beforeApiCall = [this, exception] {
      ++apiCalls;
      throw exception;
    };
  }

  // Each entry must return its failure value: the call counter proves that the
  // failure comes from the injected exception.
  void ExpectEveryEntryFails(int expectedCallsPerEntry) {
    ProfilerConfig config{};
    config.size = sizeof(config);
    config.noEnvVars = true;
    RumSessionContext session{"app-1", "session-1"};
    RumViewValues view{"view-1", "HomePage"};
    const auto expectFails = [&](const char* api, auto call) {
      SCOPED_TRACE(api);
      const int before = apiCalls;
      EXPECT_FALSE(call());
      EXPECT_EQ(apiCalls, before + expectedCallsPerEntry);
    };
    expectFails("SetupProfilingDataCallback", [] {
      return SetupProfilingDataCallback(nullptr, nullptr);
    });
    expectFails("SetupProfiler", [&] { return SetupProfiler(&config); });
    expectFails("StartProfiler", [] { return StartProfiler(); });
    expectFails("MonitorWindowHangs", [] { return MonitorWindowHangs(nullptr); });
    expectFails("StopProfiler", [] {
      StopProfiler();
      return false;
    });
    expectFails("SetRumSession", [&] { return SetRumSession(&session); });
    expectFails("SetRumView", [&] { return SetRumView(&view); });
    expectFails("EnterView", [] { return EnterView("HomePage"); });
    expectFails("LeaveCurrentView", [] { return LeaveCurrentView(); });
  }

  void ExpectRunningWithCallback() {
    EXPECT_TRUE(profiler->IsStarted());
    EXPECT_EQ(profiler->GetProfilingDataCallback(), IgnoreProfile);
    EXPECT_TRUE(profiler->GetCurrentSessionId().empty());
  }

  std::optional<Configuration> previous;
  std::unique_ptr<Profiler> profiler;
  int apiCalls = 0;
};

TEST_F(PublicApiExceptionTests, StandardExceptionsAreContained) {
  StartWithCallback();
  ThrowOnApiCall(std::runtime_error("injected failure"));
  ExpectEveryEntryFails(1);
  ExpectRunningWithCallback();
}

TEST_F(PublicApiExceptionTests, NonStandardExceptionsAreContained) {
  StartWithCallback();
  ThrowOnApiCall(42);
  ExpectEveryEntryFails(1);
  ExpectRunningWithCallback();
}

TEST_F(PublicApiExceptionTests, MissingProfilerFailsEveryEntry) {
  profiler.reset();
  ExpectEveryEntryFails(0);
}
