// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <fstream>
#include <future>
#include <stdexcept>

#include "EnvironmentVariables.h"
#include "Log.h"
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

fs::path CurrentLogFile() {
  const auto logger = spdlog::get("DD-InprocProfiler");
  if (!logger || logger->sinks().empty()) {
    return {};
  }
  const auto sink = std::dynamic_pointer_cast<spdlog::sinks::rotating_file_sink_mt>(
      logger->sinks().front()
  );
  return sink ? fs::path(sink->filename()) : fs::path{};
}

std::string ReadLogSince(const fs::path& path, uintmax_t offset) {
  std::ifstream stream(path, std::ios::binary);
  stream.seekg(static_cast<std::streamoff>(offset));
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
}  // namespace

class LifecycleDiagnosticTests : public ::testing::Test {
 protected:
  void SetUp() override {
    previous = *Profiler::GetConfiguration();
    Profiler::GetConfiguration()->ResetToDefaults();
    profiler = std::make_unique<Profiler>();
    ASSERT_EQ(StopProfiler(), ProfilingSeverity::Success);
  }

  void TearDown() override {
    if (profiler->IsStarted()) {
      StopProfiler();
    }
    profiler.reset();
    *Profiler::GetConfiguration() = std::move(*previous);
  }

  void ConfigureCallback(ProfilingDataCallback callback = [](ProfilingDataResult) {}) {
    ASSERT_FALSE(SetupProfilingDataCallback(callback));
    ProfilerConfig config{};
    config.size = sizeof(config);
    config.noEnvVars = true;
    config.uploadIntervalSeconds = 3600;
    ASSERT_EQ(SetupProfiler(config), ProfilingSeverity::Success);
  }

  std::optional<Configuration> previous;
  std::unique_ptr<Profiler> profiler;
};

TEST_F(LifecycleDiagnosticTests, InvalidConfigurationHasTypedDiagnostic) {
  ProfilerConfig config{};
  EXPECT_EQ(SetupProfiler(config), ProfilingSeverity::Error);
  const auto diagnostic = GetLastProfilerDiagnostic();
  EXPECT_EQ(diagnostic.severity, ProfilingSeverity::Error);
  EXPECT_EQ(diagnostic.code, ProfilingErrorCode::UnsupportedConfigSize);
  EXPECT_STREQ(diagnostic.message, "Invalid profiler configuration structure size");

  config.size = sizeof(config);
  config.noEnvVars = true;
  EXPECT_EQ(SetupProfiler(config), ProfilingSeverity::Error);
  EXPECT_EQ(GetLastProfilerDiagnostic().code, ProfilingErrorCode::InvalidConfiguration);
  EXPECT_NE(
      std::string(GetLastProfilerDiagnostic().message).find("Url"), std::string::npos
  );
}

TEST_F(LifecycleDiagnosticTests, ExplicitDisableReturnsInformationalDiagnostic) {
  ScopedEnvironment disabled(EnvironmentVariables::ProfilerEnabled, "0");
  *Profiler::GetConfiguration() = Configuration();
  EXPECT_EQ(StartProfiler(), ProfilingSeverity::Info);
  EXPECT_EQ(GetLastProfilerDiagnostic().severity, ProfilingSeverity::Info);
  EXPECT_EQ(GetLastProfilerDiagnostic().code, ProfilingErrorCode::ExplicitlyDisabled);
  EXPECT_FALSE(profiler->IsStarted());
}

TEST_F(LifecycleDiagnosticTests, ConflictingTransportHasConfigurationDiagnostic) {
  ScopedEnvironment key("DD_API_KEY", "test-key");
  ScopedEnvironment endpoint("DD_TRACE_AGENT_URL", "http://127.0.0.1:1");
  *Profiler::GetConfiguration() = Configuration();
  EXPECT_EQ(StartProfiler(), ProfilingSeverity::Error);
  EXPECT_EQ(GetLastProfilerDiagnostic().code, ProfilingErrorCode::InvalidConfiguration);
  EXPECT_NE(
      std::string(GetLastProfilerDiagnostic().message).find("Conflicting"),
      std::string::npos
  );
  EXPECT_FALSE(profiler->IsStarted());
}

TEST_F(LifecycleDiagnosticTests, MissingProfilerIsReportedByAllOperations) {
  profiler.reset();
  EXPECT_EQ(StartProfiler(), ProfilingSeverity::Error);
  EXPECT_EQ(GetLastProfilerDiagnostic().code, ProfilingErrorCode::ProfilerUnavailable);
  EXPECT_EQ(StopProfiler(), ProfilingSeverity::Error);
  EXPECT_EQ(GetLastProfilerDiagnostic().code, ProfilingErrorCode::ProfilerUnavailable);
  ProfilerConfig config{};
  config.size = sizeof(config);
  EXPECT_EQ(SetupProfiler(config), ProfilingSeverity::Error);
  EXPECT_EQ(GetLastProfilerDiagnostic().code, ProfilingErrorCode::ProfilerUnavailable);
  profiler = std::make_unique<Profiler>();
}

TEST_F(LifecycleDiagnosticTests, GetterBorrowsWithoutConsumingAndSuccessClears) {
  EXPECT_EQ(SetupProfiler(ProfilerConfig{}), ProfilingSeverity::Error);
  const auto diagnostic = GetLastProfilerDiagnostic();
  const std::string saved = diagnostic.message;
  EXPECT_EQ(GetLastProfilerDiagnostic().message, diagnostic.message);
  EXPECT_STREQ(GetLastProfilerDiagnostic().message, saved.c_str());
  EXPECT_EQ(StopProfiler(), ProfilingSeverity::Success);
  const auto cleared = GetLastProfilerDiagnostic();
  EXPECT_EQ(cleared.severity, ProfilingSeverity::Success);
  EXPECT_EQ(cleared.code, ProfilingErrorCode::None);
  EXPECT_STREQ(cleared.message, "");
  EXPECT_EQ(saved, "Invalid profiler configuration structure size");
}

TEST_F(LifecycleDiagnosticTests, DiagnosticIsLocalToCallingThread) {
  EXPECT_EQ(SetupProfiler(ProfilerConfig{}), ProfilingSeverity::Error);
  const auto diagnostic = GetLastProfilerDiagnostic();
  const std::string saved = diagnostic.message;
  const auto other =
      std::async(std::launch::async, [] {
        EXPECT_EQ(GetLastProfilerDiagnostic().severity, ProfilingSeverity::Success);
        ProfilerConfig config{};
        config.size = sizeof(config);
        config.noEnvVars = true;
        EXPECT_EQ(SetupProfiler(config), ProfilingSeverity::Error);
        return GetLastProfilerDiagnostic().code;
      }).get();
  EXPECT_EQ(other, ProfilingErrorCode::InvalidConfiguration);
  EXPECT_EQ(
      GetLastProfilerDiagnostic().code, ProfilingErrorCode::UnsupportedConfigSize
  );
  EXPECT_EQ(GetLastProfilerDiagnostic().message, diagnostic.message);
  EXPECT_STREQ(diagnostic.message, saved.c_str());
}

TEST_F(LifecycleDiagnosticTests, LifecycleCanRestartAndRejectsSetupWhileRunning) {
  int calls = 0;
  ConfigureCallback([&](ProfilingDataResult result) {
    EXPECT_TRUE(std::get<ProfilingData>(result).isLast);
    ++calls;
  });
  for (int cycle = 0; cycle < 2; ++cycle) {
    ASSERT_EQ(StartProfiler(), ProfilingSeverity::Success);
    EXPECT_EQ(StartProfiler(), ProfilingSeverity::Error);
    EXPECT_EQ(
        GetLastProfilerDiagnostic().code, ProfilingErrorCode::ProfilerAlreadyRunning
    );
    ProfilerConfig config{};
    config.size = sizeof(config);
    EXPECT_EQ(SetupProfiler(config), ProfilingSeverity::Error);
    EXPECT_EQ(
        GetLastProfilerDiagnostic().code, ProfilingErrorCode::ProfilerAlreadyRunning
    );
    EXPECT_EQ(StopProfiler(), ProfilingSeverity::Success);
    EXPECT_EQ(GetLastProfilerDiagnostic().severity, ProfilingSeverity::Success);
    EXPECT_FALSE(profiler->IsStarted());
    EXPECT_EQ(StopProfiler(), ProfilingSeverity::Success);
  }
  EXPECT_EQ(calls, 2);
}

TEST_F(LifecycleDiagnosticTests, PrivateCallsRetainRichResultsWithoutChangingSlot) {
  EXPECT_EQ(SetupProfiler(ProfilerConfig{}), ProfilingSeverity::Error);
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  const auto error = profiler->Configure(config);
  ASSERT_TRUE(error);
  EXPECT_EQ(error->severity, ProfilingSeverity::Error);
  EXPECT_EQ(error->code, ProfilingErrorCode::InvalidConfiguration);
  EXPECT_EQ(
      GetLastProfilerDiagnostic().code, ProfilingErrorCode::UnsupportedConfigSize
  );
}

TEST_F(LifecycleDiagnosticTests, SynchronousFailuresAreNotAlsoLogged) {
  const auto path = CurrentLogFile();
  if (path.empty()) {
    GTEST_SKIP() << "File logging is unavailable";
  }
  const auto offset = fs::file_size(path);
  EXPECT_EQ(SetupProfiler(ProfilerConfig{}), ProfilingSeverity::Error);
  ConfigureCallback([](ProfilingDataResult) {
    throw std::runtime_error("synchronous-callback-marker");
  });
  ASSERT_EQ(StartProfiler(), ProfilingSeverity::Success);
  EXPECT_EQ(StopProfiler(), ProfilingSeverity::Fatal);
  const auto log = ReadLogSince(path, offset);
  EXPECT_EQ(
      log.find("Invalid profiler configuration structure size"), std::string::npos
  );
  EXPECT_EQ(log.find("synchronous-callback-marker"), std::string::npos);
}

TEST_F(LifecycleDiagnosticTests, DebugFileWarningRemainsVisibleDuringStop) {
  const auto path = CurrentLogFile();
  if (path.empty()) {
    GTEST_SKIP() << "File logging is unavailable";
  }
  ConfigureCallback();
  const auto output =
      fs::temp_directory_path() /
      ("dd-win-prof-warning-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Profiler::GetConfiguration()->SetProfilesOutputDirectory(output);
  ASSERT_EQ(StartProfiler(), ProfilingSeverity::Success);
  fs::remove(output);
  const auto offset = fs::file_size(path);
  EXPECT_EQ(StopProfiler(), ProfilingSeverity::Success);
  EXPECT_NE(
      ReadLogSince(path, offset).find("Failed to write debug pprof file"),
      std::string::npos
  );
  EXPECT_EQ(GetLastProfilerDiagnostic().severity, ProfilingSeverity::Success);
}

TEST_F(LifecycleDiagnosticTests, CallbackExceptionIsFatalButStopCompletes) {
  ConfigureCallback([](ProfilingDataResult) {
    throw std::runtime_error("final callback failed");
  });
  ASSERT_EQ(StartProfiler(), ProfilingSeverity::Success);
  EXPECT_EQ(StopProfiler(), ProfilingSeverity::Fatal);
  const auto diagnostic = GetLastProfilerDiagnostic();
  EXPECT_EQ(diagnostic.code, ProfilingErrorCode::CallbackFailed);
  EXPECT_NE(
      std::string(diagnostic.message).find("final callback failed"), std::string::npos
  );
  EXPECT_FALSE(profiler->IsStarted());
  EXPECT_EQ(StopProfiler(), ProfilingSeverity::Success);
}

TEST_F(LifecycleDiagnosticTests, NativeExceptionTextIsNotEncodingNormalized) {
  ConfigureCallback([](ProfilingDataResult) {
    throw std::runtime_error("native byte: \xe9");
  });
  ASSERT_EQ(StartProfiler(), ProfilingSeverity::Success);
  EXPECT_EQ(StopProfiler(), ProfilingSeverity::Fatal);
  EXPECT_EQ(GetLastProfilerDiagnostic().code, ProfilingErrorCode::CallbackFailed);
  EXPECT_NE(
      std::string(GetLastProfilerDiagnostic().message).find("native byte: \xe9"),
      std::string::npos
  );
}

TEST_F(LifecycleDiagnosticTests, LongDiagnosticIsNotTruncated) {
  const std::string message(8192, 'x');
  ConfigureCallback([&](ProfilingDataResult) { throw std::runtime_error(message); });
  ASSERT_EQ(StartProfiler(), ProfilingSeverity::Success);
  EXPECT_EQ(StopProfiler(), ProfilingSeverity::Fatal);
  const std::string saved = GetLastProfilerDiagnostic().message;
  EXPECT_EQ(saved, "Profiling data callback threw: " + message);
  EXPECT_EQ(StopProfiler(), ProfilingSeverity::Success);
  EXPECT_EQ(saved, "Profiling data callback threw: " + message);
}

TEST_F(LifecycleDiagnosticTests, NonStandardCallbackExceptionIsReported) {
  ConfigureCallback([](ProfilingDataResult) { throw 42; });
  ASSERT_EQ(StartProfiler(), ProfilingSeverity::Success);
  EXPECT_EQ(StopProfiler(), ProfilingSeverity::Fatal);
  EXPECT_EQ(GetLastProfilerDiagnostic().code, ProfilingErrorCode::CallbackFailed);
  EXPECT_FALSE(profiler->IsStarted());
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
    void operator()(ProfilingDataResult) const {}
  };
  bool fail = false;
  ConfigureCallback(ThrowOnCopy(&fail));
  fail = true;
  EXPECT_EQ(StartProfiler(), ProfilingSeverity::Fatal);
  EXPECT_EQ(GetLastProfilerDiagnostic().code, ProfilingErrorCode::UnexpectedException);
  EXPECT_NE(
      std::string(GetLastProfilerDiagnostic().message)
          .find("exporter callback copy failed"),
      std::string::npos
  );
  EXPECT_FALSE(profiler->IsStarted());
  fail = false;
  EXPECT_EQ(StartProfiler(), ProfilingSeverity::Success);
  EXPECT_EQ(StopProfiler(), ProfilingSeverity::Success);
}

TEST_F(LifecycleDiagnosticTests, FirstHttpSendFailureIsReportedAtStop) {
  ScopedEnvironment key("DD_API_KEY", nullptr);
  ScopedEnvironment endpoint("DD_TRACE_AGENT_URL", "http://127.0.0.1:1");
  ScopedEnvironment enabled("DD_INTERNAL_PROFILING_EXPORT_ENABLED", "1");
  *Profiler::GetConfiguration() = Configuration();
  ASSERT_EQ(StartProfiler(), ProfilingSeverity::Success);
  EXPECT_EQ(StopProfiler(), ProfilingSeverity::Error);
  EXPECT_EQ(GetLastProfilerDiagnostic().code, ProfilingErrorCode::ExportFailed);
  EXPECT_NE(
      std::string(GetLastProfilerDiagnostic().message).find("Failed to send profile"),
      std::string::npos
  );
  EXPECT_FALSE(profiler->IsStarted());
}

TEST(LifecycleFinalExportTests, HttpStatusAlwaysDescribesDeliveryOutcome) {
  Configuration config;
  std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
  for (const uint16_t status :
       {200, 202, 204, 301, 400, 403, 404, 429, 500, 503, 504}) {
    ProfileExporter exporter(&config, types);
    const bool success = exporter.CheckExportResponse(status);
    EXPECT_EQ(success, status >= 200 && status < 300);
    EXPECT_EQ(exporter.GetLastDiagnostic().has_value(), !success);
    if (!success) {
      EXPECT_NE(
          exporter.GetLastErrorMessage().find(std::to_string(status)), std::string::npos
      );
    }
  }
}
