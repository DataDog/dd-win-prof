// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <optional>
#include <stdexcept>

#include "EnvironmentVariables.h"
#include "Profiler.h"
#include "internal/dd-win-rum-private.h"
#include "pch.h"

namespace {
class ScopedEnvironment {
 public:
  ScopedEnvironment(const char* name, const char* value) : _name(name) {
    const auto size = ::GetEnvironmentVariableA(name, nullptr, 0);
    if (size != 0) {
      _previous.resize(size);
      ::GetEnvironmentVariableA(name, _previous.data(), size);
      _previous.resize(size - 1);
    }
    ::SetEnvironmentVariableA(name, value);
  }
  ~ScopedEnvironment() {
    ::SetEnvironmentVariableA(_name, _previous.empty() ? nullptr : _previous.c_str());
  }

 private:
  const char* _name;
  std::string _previous;
};
}  // namespace

class LifecycleDiagnosticTests : public ::testing::Test {
 protected:
  void SetUp() override {
    previous = *Profiler::GetConfiguration();
    Profiler::GetConfiguration()->ResetToDefaults();
    profiler = std::make_unique<Profiler>();
    ASSERT_TRUE(StopProfiler());
  }

  void TearDown() override {
    if (profiler->IsStarted()) {
      StopProfiler();
    }
    profiler.reset();
    *Profiler::GetConfiguration() = std::move(*previous);
  }

  void ConfigureCallback(ProfilingDataCallback callback = [](ProfilingData) {}) {
    ASSERT_TRUE(SetupProfilingDataCallback(callback));
    ProfilerConfig config{};
    config.size = sizeof(config);
    config.noEnvVars = true;
    config.uploadIntervalSeconds = 3600;
    ASSERT_TRUE(SetupProfiler(config));
  }

  std::optional<Configuration> previous;
  std::unique_ptr<Profiler> profiler;
};

TEST_F(LifecycleDiagnosticTests, InvalidConfigurationReturnsFalse) {
  ProfilerConfig config{};
  EXPECT_FALSE(SetupProfiler(config));
  config.size = sizeof(config);
  config.noEnvVars = true;
  EXPECT_FALSE(SetupProfiler(config));
  EXPECT_FALSE(profiler->IsStarted());
}

TEST_F(LifecycleDiagnosticTests, ExplicitDisableReturnsFalse) {
  ScopedEnvironment disabled(EnvironmentVariables::ProfilerEnabled, "0");
  *Profiler::GetConfiguration() = Configuration();
  EXPECT_FALSE(StartProfiler());
  EXPECT_FALSE(profiler->IsStarted());
}

TEST_F(LifecycleDiagnosticTests, ConflictingTransportReturnsFalse) {
  ScopedEnvironment key("DD_API_KEY", "test-key");
  ScopedEnvironment endpoint("DD_TRACE_AGENT_URL", "http://127.0.0.1:1");
  *Profiler::GetConfiguration() = Configuration();
  EXPECT_FALSE(StartProfiler());
  EXPECT_FALSE(profiler->IsStarted());
}

TEST_F(LifecycleDiagnosticTests, MissingProfilerRejectsAllOperations) {
  profiler.reset();
  EXPECT_FALSE(StartProfiler());
  EXPECT_FALSE(StopProfiler());
  ProfilerConfig config{};
  config.size = sizeof(config);
  EXPECT_FALSE(SetupProfiler(config));
  EXPECT_FALSE(SetupProfilingDataCallback({}));
  profiler = std::make_unique<Profiler>();
}

TEST_F(LifecycleDiagnosticTests, LifecycleCanRestartAndRejectsSetupWhileRunning) {
  int calls = 0;
  ConfigureCallback([&](ProfilingData data) {
    EXPECT_EQ(data.result, 0u);
    EXPECT_TRUE(data.isLast);
    ++calls;
  });
  for (int cycle = 0; cycle < 2; ++cycle) {
    ASSERT_TRUE(StartProfiler());
    EXPECT_FALSE(StartProfiler());
    ProfilerConfig config{};
    config.size = sizeof(config);
    EXPECT_FALSE(SetupProfiler(config));
    EXPECT_TRUE(StopProfiler());
    EXPECT_FALSE(profiler->IsStarted());
    EXPECT_TRUE(StopProfiler());
  }
  EXPECT_EQ(calls, 2);
}

TEST_F(LifecycleDiagnosticTests, PrivateConfigurationUsesBooleanResults) {
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  EXPECT_FALSE(profiler->Configure(config));
  ASSERT_TRUE(SetupProfilingDataCallback([](ProfilingData) {}));
  EXPECT_TRUE(profiler->Configure(config));
}

TEST_F(LifecycleDiagnosticTests, DebugFileFailureDoesNotFailStop) {
  ConfigureCallback();
  const auto output =
      fs::temp_directory_path() /
      ("dd-win-prof-warning-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Profiler::GetConfiguration()->SetProfilesOutputDirectory(output);
  ASSERT_TRUE(StartProfiler());
  fs::remove(output);
  EXPECT_TRUE(StopProfiler());
  EXPECT_FALSE(profiler->IsStarted());
}

TEST_F(LifecycleDiagnosticTests, CallbackExceptionMakesStopFailButCleanupCompletes) {
  ConfigureCallback([](ProfilingData) { throw std::runtime_error("callback failed"); });
  ASSERT_TRUE(StartProfiler());
  EXPECT_FALSE(StopProfiler());
  EXPECT_FALSE(profiler->IsStarted());
  EXPECT_TRUE(StopProfiler());
}

TEST_F(LifecycleDiagnosticTests, NonStandardCallbackExceptionMakesStopFail) {
  ConfigureCallback([](ProfilingData) { throw 42; });
  ASSERT_TRUE(StartProfiler());
  EXPECT_FALSE(StopProfiler());
  EXPECT_FALSE(profiler->IsStarted());
  EXPECT_TRUE(StopProfiler());
}

TEST_F(LifecycleDiagnosticTests, ConstructorCopyFailureLeavesStoppedAndCanRetry) {
  struct ThrowOnCopy {
    bool* fail;
    explicit ThrowOnCopy(bool* flag) : fail(flag) {}
    ThrowOnCopy(const ThrowOnCopy& other) : fail(other.fail) {
      if (*fail) {
        throw std::runtime_error("exporter callback copy failed");
      }
    }
    void operator()(ProfilingData) const {}
  };
  bool fail = false;
  ConfigureCallback(ThrowOnCopy(&fail));
  fail = true;
  EXPECT_FALSE(StartProfiler());
  EXPECT_FALSE(profiler->IsStarted());
  fail = false;
  EXPECT_TRUE(StartProfiler());
  EXPECT_TRUE(StopProfiler());
}

TEST_F(LifecycleDiagnosticTests, FirstHttpSendFailureMakesStopReturnFalse) {
  ScopedEnvironment key("DD_API_KEY", nullptr);
  ScopedEnvironment endpoint("DD_TRACE_AGENT_URL", "http://127.0.0.1:1");
  ScopedEnvironment enabled("DD_INTERNAL_PROFILING_EXPORT_ENABLED", "1");
  *Profiler::GetConfiguration() = Configuration();
  ASSERT_TRUE(StartProfiler());
  EXPECT_FALSE(StopProfiler());
  EXPECT_FALSE(profiler->IsStarted());
}

TEST(LifecycleFinalExportTests, HttpStatusAlwaysDescribesDeliveryOutcome) {
  Configuration config;
  std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
  ProfileExporter exporter(&config, types);
  for (const uint16_t status :
       {200, 202, 204, 301, 400, 403, 404, 429, 500, 503, 504}) {
    EXPECT_EQ(exporter.CheckExportResponse(status), status >= 200 && status < 300);
  }
}
