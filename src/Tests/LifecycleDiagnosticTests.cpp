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
    StopProfiler();
    ASSERT_EQ(GetLastProfilerError(), nullptr);
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
    ASSERT_TRUE(SetupProfiler(config));
  }

  std::optional<Configuration> previous;
  std::unique_ptr<Profiler> profiler;
};

TEST_F(LifecycleDiagnosticTests, InvalidConfigurationHasRichError) {
  ProfilerConfig config{};
  EXPECT_FALSE(SetupProfiler(config));
  const auto* error = GetLastProfilerError();
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->severity, ProfilingSeverity::Error);
  EXPECT_EQ(error->code, ProfilingErrorCode::UnsupportedConfigSize);
  EXPECT_EQ(error->message, "Invalid profiler configuration structure size");

  config.size = sizeof(config);
  config.noEnvVars = true;
  EXPECT_FALSE(SetupProfiler(config));
  error = GetLastProfilerError();
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->code, ProfilingErrorCode::InvalidConfiguration);
  EXPECT_NE(error->message.find("Url"), std::string::npos);
}

TEST_F(LifecycleDiagnosticTests, ExplicitDisableReturnsInformationalError) {
  ScopedEnvironment disabled(EnvironmentVariables::ProfilerEnabled, "0");
  *Profiler::GetConfiguration() = Configuration();
  EXPECT_FALSE(StartProfiler());
  const auto* error = GetLastProfilerError();
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->severity, ProfilingSeverity::Info);
  EXPECT_EQ(error->code, ProfilingErrorCode::ExplicitlyDisabled);
  EXPECT_FALSE(profiler->IsStarted());
}

TEST_F(LifecycleDiagnosticTests, ConflictingTransportHasConfigurationError) {
  ScopedEnvironment key("DD_API_KEY", "test-key");
  ScopedEnvironment endpoint("DD_TRACE_AGENT_URL", "http://127.0.0.1:1");
  *Profiler::GetConfiguration() = Configuration();
  EXPECT_FALSE(StartProfiler());
  const auto* error = GetLastProfilerError();
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->code, ProfilingErrorCode::InvalidConfiguration);
  EXPECT_NE(error->message.find("Conflicting"), std::string::npos);
  EXPECT_FALSE(profiler->IsStarted());
}

TEST_F(LifecycleDiagnosticTests, MissingProfilerIsReportedByAllOperations) {
  profiler.reset();
  EXPECT_FALSE(StartProfiler());
  ASSERT_NE(GetLastProfilerError(), nullptr);
  EXPECT_EQ(GetLastProfilerError()->code, ProfilingErrorCode::ProfilerUnavailable);
  StopProfiler();
  ASSERT_NE(GetLastProfilerError(), nullptr);
  EXPECT_EQ(GetLastProfilerError()->code, ProfilingErrorCode::ProfilerUnavailable);
  ProfilerConfig config{};
  config.size = sizeof(config);
  EXPECT_FALSE(SetupProfiler(config));
  ASSERT_NE(GetLastProfilerError(), nullptr);
  EXPECT_EQ(GetLastProfilerError()->code, ProfilingErrorCode::ProfilerUnavailable);
  profiler = std::make_unique<Profiler>();
}

TEST_F(LifecycleDiagnosticTests, GetterBorrowsWithoutConsumingAndSuccessClears) {
  EXPECT_FALSE(SetupProfiler(ProfilerConfig{}));
  const auto* error = GetLastProfilerError();
  ASSERT_NE(error, nullptr);
  const ProfilingError saved = *error;
  EXPECT_EQ(GetLastProfilerError(), error);
  EXPECT_EQ(GetLastProfilerError()->message.data(), error->message.data());
  EXPECT_EQ(error->message, saved.message);
  StopProfiler();
  EXPECT_EQ(GetLastProfilerError(), nullptr);
  EXPECT_EQ(saved.code, ProfilingErrorCode::UnsupportedConfigSize);
  EXPECT_EQ(saved.message, "Invalid profiler configuration structure size");
}

TEST_F(LifecycleDiagnosticTests, ErrorIsLocalToCallingThread) {
  EXPECT_FALSE(SetupProfiler(ProfilerConfig{}));
  const auto* error = GetLastProfilerError();
  ASSERT_NE(error, nullptr);
  const std::string saved = error->message;
  const auto other = std::async(std::launch::async, [] {
                       EXPECT_EQ(GetLastProfilerError(), nullptr);
                       ProfilerConfig config{};
                       config.size = sizeof(config);
                       config.noEnvVars = true;
                       EXPECT_FALSE(SetupProfiler(config));
                       const auto* error = GetLastProfilerError();
                       EXPECT_NE(error, nullptr);
                       return error ? std::optional{error->code} : std::nullopt;
                     }).get();
  EXPECT_EQ(other, ProfilingErrorCode::InvalidConfiguration);
  EXPECT_EQ(GetLastProfilerError(), error);
  EXPECT_EQ(error->code, ProfilingErrorCode::UnsupportedConfigSize);
  EXPECT_EQ(error->message, saved);
}

TEST_F(LifecycleDiagnosticTests, LifecycleCanRestartAndRejectsSetupWhileRunning) {
  int calls = 0;
  ConfigureCallback([&](ProfilingDataResult result) {
    EXPECT_TRUE(std::get<ProfilingData>(result).isLast);
    ++calls;
  });
  for (int cycle = 0; cycle < 2; ++cycle) {
    ASSERT_TRUE(StartProfiler());
    EXPECT_FALSE(StartProfiler());
    ASSERT_NE(GetLastProfilerError(), nullptr);
    EXPECT_EQ(GetLastProfilerError()->code, ProfilingErrorCode::ProfilerAlreadyRunning);
    ProfilerConfig config{};
    config.size = sizeof(config);
    EXPECT_FALSE(SetupProfiler(config));
    ASSERT_NE(GetLastProfilerError(), nullptr);
    EXPECT_EQ(GetLastProfilerError()->code, ProfilingErrorCode::ProfilerAlreadyRunning);
    StopProfiler();
    EXPECT_EQ(GetLastProfilerError(), nullptr);
    EXPECT_FALSE(profiler->IsStarted());
    StopProfiler();
    EXPECT_EQ(GetLastProfilerError(), nullptr);
  }
  EXPECT_EQ(calls, 2);
}

TEST_F(LifecycleDiagnosticTests, PrivateCallsRetainRichResultsWithoutChangingSlot) {
  EXPECT_FALSE(SetupProfiler(ProfilerConfig{}));
  const auto* stored = GetLastProfilerError();
  ASSERT_NE(stored, nullptr);
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  const auto error = profiler->Configure(config);
  ASSERT_TRUE(error);
  EXPECT_EQ(error->severity, ProfilingSeverity::Error);
  EXPECT_EQ(error->code, ProfilingErrorCode::InvalidConfiguration);
  EXPECT_EQ(GetLastProfilerError(), stored);
  EXPECT_EQ(stored->code, ProfilingErrorCode::UnsupportedConfigSize);
  ASSERT_FALSE(SetupProfilingDataCallback({}));
  EXPECT_EQ(GetLastProfilerError(), stored);
}

TEST_F(LifecycleDiagnosticTests, SynchronousFailuresAreNotAlsoLogged) {
  const auto path = CurrentLogFile();
  if (path.empty()) {
    GTEST_SKIP() << "File logging is unavailable";
  }
  const auto offset = fs::file_size(path);
  EXPECT_FALSE(SetupProfiler(ProfilerConfig{}));
  ConfigureCallback([](ProfilingDataResult) {
    throw std::runtime_error("synchronous-callback-marker");
  });
  ASSERT_TRUE(StartProfiler());
  StopProfiler();
  ASSERT_NE(GetLastProfilerError(), nullptr);
  EXPECT_EQ(GetLastProfilerError()->severity, ProfilingSeverity::Fatal);
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
  ASSERT_TRUE(StartProfiler());
  fs::remove(output);
  const auto offset = fs::file_size(path);
  StopProfiler();
  EXPECT_NE(
      ReadLogSince(path, offset).find("Failed to write debug pprof file"),
      std::string::npos
  );
  EXPECT_EQ(GetLastProfilerError(), nullptr);
}

TEST_F(LifecycleDiagnosticTests, CallbackExceptionIsFatalButStopCompletes) {
  ConfigureCallback([](ProfilingDataResult) {
    throw std::runtime_error("final callback failed");
  });
  ASSERT_TRUE(StartProfiler());
  StopProfiler();
  const auto* error = GetLastProfilerError();
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->severity, ProfilingSeverity::Fatal);
  EXPECT_EQ(error->code, ProfilingErrorCode::CallbackFailed);
  EXPECT_NE(error->message.find("final callback failed"), std::string::npos);
  EXPECT_FALSE(profiler->IsStarted());
  StopProfiler();
  EXPECT_EQ(GetLastProfilerError(), nullptr);
}

TEST_F(LifecycleDiagnosticTests, NativeExceptionTextIsNotEncodingNormalized) {
  ConfigureCallback([](ProfilingDataResult) {
    throw std::runtime_error("native byte: \xe9");
  });
  ASSERT_TRUE(StartProfiler());
  StopProfiler();
  const auto* error = GetLastProfilerError();
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->code, ProfilingErrorCode::CallbackFailed);
  EXPECT_NE(error->message.find("native byte: \xe9"), std::string::npos);
}

TEST_F(LifecycleDiagnosticTests, LongErrorIsNotTruncated) {
  const std::string message(8192, 'x');
  ConfigureCallback([&](ProfilingDataResult) { throw std::runtime_error(message); });
  ASSERT_TRUE(StartProfiler());
  StopProfiler();
  const auto* error = GetLastProfilerError();
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->severity, ProfilingSeverity::Fatal);
  const std::string saved = error->message;
  EXPECT_EQ(saved, "Profiling data callback threw: " + message);
  StopProfiler();
  EXPECT_EQ(saved, "Profiling data callback threw: " + message);
}

TEST_F(LifecycleDiagnosticTests, NonStandardCallbackExceptionIsReported) {
  ConfigureCallback([](ProfilingDataResult) { throw 42; });
  ASSERT_TRUE(StartProfiler());
  StopProfiler();
  const auto* error = GetLastProfilerError();
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->code, ProfilingErrorCode::CallbackFailed);
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
  EXPECT_FALSE(StartProfiler());
  const auto* error = GetLastProfilerError();
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->severity, ProfilingSeverity::Fatal);
  EXPECT_EQ(error->code, ProfilingErrorCode::UnexpectedException);
  EXPECT_NE(error->message.find("exporter callback copy failed"), std::string::npos);
  EXPECT_FALSE(profiler->IsStarted());
  fail = false;
  EXPECT_TRUE(StartProfiler());
  StopProfiler();
  EXPECT_EQ(GetLastProfilerError(), nullptr);
}

TEST_F(LifecycleDiagnosticTests, FirstHttpSendFailureIsReportedAtStop) {
  ScopedEnvironment key("DD_API_KEY", nullptr);
  ScopedEnvironment endpoint("DD_TRACE_AGENT_URL", "http://127.0.0.1:1");
  ScopedEnvironment enabled("DD_INTERNAL_PROFILING_EXPORT_ENABLED", "1");
  *Profiler::GetConfiguration() = Configuration();
  ASSERT_TRUE(StartProfiler());
  StopProfiler();
  const auto* error = GetLastProfilerError();
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->code, ProfilingErrorCode::ExportFailed);
  EXPECT_NE(error->message.find("Failed to send profile"), std::string::npos);
  EXPECT_FALSE(profiler->IsStarted());
}

TEST(LifecycleFinalExportTests, HttpStatusAlwaysDescribesDeliveryOutcome) {
  Configuration config;
  std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
  ProfileExporter exporter(&config, types);
  for (const uint16_t status :
       {200, 202, 204, 301, 400, 403, 404, 429, 500, 503, 504}) {
    const auto error = exporter.CheckExportResponse(status);
    EXPECT_EQ(error.has_value(), status < 200 || status >= 300);
    if (error) {
      EXPECT_EQ(error->code, ProfilingErrorCode::ExportFailed);
      EXPECT_NE(error->message.find(std::to_string(status)), std::string::npos);
    }
  }
}
