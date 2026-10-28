// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <future>
#include <stdexcept>

#include "DiagnosticLogging.h"
#include "EnvironmentVariables.h"
#include "LifecycleDiagnostic.h"
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

struct Diagnostic {
  int32_t severity = -1;
  uint32_t code = UINT32_MAX;
  char message[1024]{};
  uint32_t required;

  Diagnostic() {
    required = GetLastProfilerDiagnosticV1(&severity, &code, message, sizeof(message));
  }
};
}  // namespace

class LifecycleDiagnosticTests : public ::testing::Test {
 protected:
  void SetUp() override {
    previous = *Profiler::GetConfiguration();
    Profiler::GetConfiguration()->ResetToDefaults();
    profiler = std::make_unique<Profiler>();
    ASSERT_EQ(StopProfilerV1(), DD_WIN_PROF_SUCCESS);
  }

  void TearDown() override {
    if (profiler->IsStarted()) {
      StopProfilerV1();
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
    ASSERT_EQ(SetupProfilerV1(&config), DD_WIN_PROF_SUCCESS);
  }

  std::optional<Configuration> previous;
  std::unique_ptr<Profiler> profiler;
};

TEST_F(LifecycleDiagnosticTests, InvalidArgumentsHaveStableCodes) {
  EXPECT_EQ(SetupProfilerV1(nullptr), DD_WIN_PROF_ERROR);
  Diagnostic diagnostic;
  EXPECT_EQ(diagnostic.severity, DD_WIN_PROF_ERROR);
  EXPECT_EQ(diagnostic.code, DD_WIN_PROF_CODE_INVALID_ARGUMENT);
  EXPECT_STREQ(diagnostic.message, "Null profiler configuration structure");
  EXPECT_EQ(diagnostic.required, strlen(diagnostic.message) + 1);

  ProfilerConfig config{};
  EXPECT_EQ(SetupProfilerV1(&config), DD_WIN_PROF_ERROR);
  EXPECT_EQ(Diagnostic().code, DD_WIN_PROF_CODE_UNSUPPORTED_CONFIG_SIZE);
  config.size = sizeof(config);
  config.noEnvVars = true;
  EXPECT_EQ(SetupProfilerV1(&config), DD_WIN_PROF_ERROR);
  EXPECT_EQ(Diagnostic().code, DD_WIN_PROF_CODE_INVALID_CONFIGURATION);
  EXPECT_NE(std::string(Diagnostic().message).find("Url"), std::string::npos);
}

TEST_F(LifecycleDiagnosticTests, ExplicitDisableReturnsInformationalDiagnostic) {
  ScopedEnvironment disabled(EnvironmentVariables::ProfilerEnabled, "0");
  *Profiler::GetConfiguration() = Configuration();
  EXPECT_EQ(StartProfilerV1(), DD_WIN_PROF_INFO);
  EXPECT_EQ(Diagnostic().severity, DD_WIN_PROF_INFO);
  EXPECT_EQ(Diagnostic().code, DD_WIN_PROF_CODE_EXPLICITLY_DISABLED);
  EXPECT_FALSE(profiler->IsStarted());
  EXPECT_FALSE(StartProfiler());
  EXPECT_EQ(Diagnostic().code, DD_WIN_PROF_CODE_EXPLICITLY_DISABLED);
}

TEST_F(LifecycleDiagnosticTests, ConflictingTransportHasConfigurationDiagnostic) {
  ScopedEnvironment key("DD_API_KEY", "test-key");
  ScopedEnvironment endpoint("DD_TRACE_AGENT_URL", "http://127.0.0.1:1");
  *Profiler::GetConfiguration() = Configuration();
  EXPECT_EQ(StartProfilerV1(), DD_WIN_PROF_ERROR);
  EXPECT_EQ(Diagnostic().code, DD_WIN_PROF_CODE_INVALID_CONFIGURATION);
  EXPECT_NE(std::string(Diagnostic().message).find("Conflicting"), std::string::npos);
  EXPECT_FALSE(profiler->IsStarted());
}

TEST_F(LifecycleDiagnosticTests, MissingProfilerIsReportedByAllOperations) {
  profiler.reset();
  EXPECT_EQ(StartProfilerV1(), DD_WIN_PROF_ERROR);
  EXPECT_EQ(Diagnostic().code, DD_WIN_PROF_CODE_PROFILER_UNAVAILABLE);
  EXPECT_EQ(StopProfilerV1(), DD_WIN_PROF_ERROR);
  EXPECT_EQ(Diagnostic().code, DD_WIN_PROF_CODE_PROFILER_UNAVAILABLE);
  ProfilerConfig config{};
  config.size = sizeof(config);
  EXPECT_EQ(SetupProfilerV1(&config), DD_WIN_PROF_ERROR);
  EXPECT_EQ(Diagnostic().code, DD_WIN_PROF_CODE_PROFILER_UNAVAILABLE);
  profiler = std::make_unique<Profiler>();
}

TEST_F(LifecycleDiagnosticTests, GettersDoNotConsumeAndSuccessClears) {
  EXPECT_EQ(SetupProfilerV1(nullptr), DD_WIN_PROF_ERROR);
  const auto required = GetLastProfilerDiagnosticV1(nullptr, nullptr, nullptr, 0);
  EXPECT_GT(required, 1u);
  char one[1] = {'x'};
  EXPECT_EQ(GetLastProfilerDiagnosticV1(nullptr, nullptr, one, 1), required);
  EXPECT_EQ(one[0], '\0');
  char untouched = 'x';
  EXPECT_EQ(GetLastProfilerDiagnosticV1(nullptr, nullptr, &untouched, 0), required);
  EXPECT_EQ(untouched, 'x');
  std::string exact(required, '\0');
  EXPECT_EQ(
      GetLastProfilerDiagnosticV1(nullptr, nullptr, exact.data(), required), required
  );
  EXPECT_STREQ(exact.c_str(), Diagnostic().message);
  EXPECT_EQ(StopProfilerV1(), DD_WIN_PROF_SUCCESS);
  Diagnostic cleared;
  EXPECT_EQ(cleared.required, 0u);
  EXPECT_EQ(cleared.severity, DD_WIN_PROF_SUCCESS);
  EXPECT_EQ(cleared.code, DD_WIN_PROF_CODE_NONE);
  EXPECT_STREQ(cleared.message, "");
}

TEST_F(LifecycleDiagnosticTests, DiagnosticIsLocalToCallingThread) {
  EXPECT_EQ(SetupProfilerV1(nullptr), DD_WIN_PROF_ERROR);
  const auto other = std::async(std::launch::async, [] {
                       EXPECT_EQ(Diagnostic().required, 0u);
                       ProfilerConfig config{};
                       EXPECT_EQ(SetupProfilerV1(&config), DD_WIN_PROF_ERROR);
                       return Diagnostic().code;
                     }).get();
  EXPECT_EQ(other, DD_WIN_PROF_CODE_UNSUPPORTED_CONFIG_SIZE);
  EXPECT_EQ(Diagnostic().code, DD_WIN_PROF_CODE_INVALID_ARGUMENT);
}

TEST_F(LifecycleDiagnosticTests, LegacyCallsKeepMappingsAndUpdateDiagnostic) {
  EXPECT_FALSE(SetupProfiler(nullptr));
  EXPECT_EQ(Diagnostic().code, DD_WIN_PROF_CODE_INVALID_ARGUMENT);
  StopProfiler();
  EXPECT_EQ(Diagnostic().required, 0u);
  ConfigureCallback();
  ASSERT_TRUE(StartProfiler());
  EXPECT_FALSE(StartProfiler());
  EXPECT_EQ(Diagnostic().code, DD_WIN_PROF_CODE_ALREADY_RUNNING);
  StopProfiler();
  EXPECT_EQ(Diagnostic().required, 0u);
}

TEST_F(LifecycleDiagnosticTests, LifecycleCanRestartAndRejectsSetupWhileRunning) {
  int calls = 0;
  ConfigureCallback([&](ProfilingDataResult result) {
    EXPECT_TRUE(std::get<ProfilingData>(result).isLast);
    ++calls;
  });
  for (int cycle = 0; cycle < 2; ++cycle) {
    ASSERT_EQ(StartProfilerV1(), DD_WIN_PROF_SUCCESS);
    EXPECT_EQ(StartProfilerV1(), DD_WIN_PROF_ERROR);
    EXPECT_EQ(Diagnostic().code, DD_WIN_PROF_CODE_ALREADY_RUNNING);
    ProfilerConfig config{};
    config.size = sizeof(config);
    EXPECT_EQ(SetupProfilerV1(&config), DD_WIN_PROF_ERROR);
    EXPECT_EQ(Diagnostic().code, DD_WIN_PROF_CODE_ALREADY_RUNNING);
    EXPECT_EQ(StopProfilerV1(), DD_WIN_PROF_SUCCESS);
    EXPECT_EQ(Diagnostic().required, 0u);
    EXPECT_FALSE(profiler->IsStarted());
    EXPECT_EQ(StopProfilerV1(), DD_WIN_PROF_SUCCESS);
  }
  EXPECT_EQ(calls, 2);
}

TEST_F(LifecycleDiagnosticTests, PrivateCallsRetainRichResultsWithoutChangingSlot) {
  EXPECT_EQ(SetupProfilerV1(nullptr), DD_WIN_PROF_ERROR);
  ProfilerConfig config{};
  const auto error = SetupProfilerWithDiagnostics(&config);
  ASSERT_TRUE(error);
  EXPECT_EQ(error->severity, ProfilingSeverity::Error);
  EXPECT_EQ(error->code, ProfilingErrorCode::UnsupportedConfigSize);
  EXPECT_EQ(Diagnostic().code, DD_WIN_PROF_CODE_INVALID_ARGUMENT);
  EXPECT_TRUE(dd_win_prof::diagnosticLoggingEnabled);
}

TEST_F(LifecycleDiagnosticTests, CallbackExceptionIsFatalButStopCompletes) {
  ConfigureCallback([](ProfilingDataResult) {
    throw std::runtime_error("final callback failed");
  });
  ASSERT_EQ(StartProfilerV1(), DD_WIN_PROF_SUCCESS);
  EXPECT_EQ(StopProfilerV1(), DD_WIN_PROF_FATAL);
  Diagnostic diagnostic;
  EXPECT_EQ(diagnostic.code, DD_WIN_PROF_CODE_CALLBACK_FAILED);
  EXPECT_NE(
      std::string(diagnostic.message).find("final callback failed"), std::string::npos
  );
  EXPECT_FALSE(profiler->IsStarted());
  EXPECT_EQ(StopProfilerV1(), DD_WIN_PROF_SUCCESS);
}

TEST_F(LifecycleDiagnosticTests, InvalidUtf8ExceptionMessageIsSanitized) {
  ConfigureCallback([](ProfilingDataResult) {
    throw std::runtime_error("invalid byte: \xe9");
  });
  ASSERT_EQ(StartProfilerV1(), DD_WIN_PROF_SUCCESS);
  EXPECT_EQ(StopProfilerV1(), DD_WIN_PROF_FATAL);
  EXPECT_EQ(Diagnostic().code, DD_WIN_PROF_CODE_CALLBACK_FAILED);
  EXPECT_NE(
      std::string(Diagnostic().message).find("invalid byte: ?"), std::string::npos
  );
}

TEST_F(LifecycleDiagnosticTests, NonStandardCallbackExceptionDoesNotCrossCAbi) {
  ConfigureCallback([](ProfilingDataResult) { throw 42; });
  ASSERT_EQ(StartProfilerV1(), DD_WIN_PROF_SUCCESS);
  EXPECT_EQ(StopProfilerV1(), DD_WIN_PROF_FATAL);
  EXPECT_EQ(Diagnostic().code, DD_WIN_PROF_CODE_CALLBACK_FAILED);
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
  EXPECT_EQ(StartProfilerV1(), DD_WIN_PROF_FATAL);
  EXPECT_EQ(Diagnostic().code, DD_WIN_PROF_CODE_UNEXPECTED_EXCEPTION);
  EXPECT_FALSE(profiler->IsStarted());
  fail = false;
  EXPECT_EQ(StartProfilerV1(), DD_WIN_PROF_SUCCESS);
  EXPECT_EQ(StopProfilerV1(), DD_WIN_PROF_SUCCESS);
}

TEST(LifecycleDiagnosticStorageTests, Utf8TruncationAndBoundedStorage) {
  dd_win_prof::LifecycleDiagnostic diagnostic;
  const std::string text = "a\xc3\xa9\xf0\x9f\x98\x80z";
  diagnostic.Set(DD_WIN_PROF_ERROR, DD_WIN_PROF_CODE_INVALID_ARGUMENT, text);
  char message[16];
  EXPECT_EQ(diagnostic.Copy(nullptr, nullptr, message, 3), text.size() + 1);
  EXPECT_STREQ(message, "a");
  diagnostic.Copy(nullptr, nullptr, message, 4);
  EXPECT_STREQ(message, "a\xc3\xa9");
  diagnostic.Copy(nullptr, nullptr, message, 8);
  EXPECT_STREQ(message, "a\xc3\xa9\xf0\x9f\x98\x80");
  const std::string longText = std::string(1022, 'x') + "\xc3\xa9";
  diagnostic.Set(DD_WIN_PROF_FATAL, DD_WIN_PROF_CODE_UNEXPECTED_EXCEPTION, longText);
  EXPECT_EQ(diagnostic.Copy(nullptr, nullptr, nullptr, 0), 1023u);
  diagnostic.Clear();
  EXPECT_EQ(diagnostic.Copy(nullptr, nullptr, message, sizeof(message)), 0u);
  EXPECT_STREQ(message, "");
}

TEST(LifecycleDiagnosticStorageTests, InvalidUtf8AndEmbeddedNulAreSanitized) {
  dd_win_prof::LifecycleDiagnostic diagnostic;
  for (const std::string invalid :
       {std::string("\xe9"),
        std::string("\xc0\xaf"),
        std::string("\xed\xa0\x80"),
        std::string("\xf4\x90\x80\x80"),
        std::string("\xf0\x9f"),
        std::string("\0", 1)}) {
    diagnostic.Set(DD_WIN_PROF_ERROR, DD_WIN_PROF_CODE_INVALID_ARGUMENT, invalid);
    char message[16];
    diagnostic.Copy(nullptr, nullptr, message, sizeof(message));
    EXPECT_EQ(std::string(message), std::string(invalid.size(), '?'));
  }
}

TEST_F(LifecycleDiagnosticTests, FirstHttpSendFailureIsReportedAtStop) {
  ScopedEnvironment key("DD_API_KEY", nullptr);
  ScopedEnvironment endpoint("DD_TRACE_AGENT_URL", "http://127.0.0.1:1");
  ScopedEnvironment enabled("DD_INTERNAL_PROFILING_EXPORT_ENABLED", "1");
  *Profiler::GetConfiguration() = Configuration();
  ASSERT_EQ(StartProfilerV1(), DD_WIN_PROF_SUCCESS);
  EXPECT_EQ(StopProfilerV1(), DD_WIN_PROF_ERROR);
  EXPECT_EQ(Diagnostic().code, DD_WIN_PROF_CODE_FINAL_EXPORT_FAILED);
  EXPECT_NE(Diagnostic().required, 0u);
  EXPECT_NE(
      std::string(Diagnostic().message).find("Failed to send profile"),
      std::string::npos
  );
  EXPECT_FALSE(profiler->IsStarted());
}

TEST(LifecycleFinalExportTests, FinalHttpRejectionIsNotPeriodicContinuation) {
  Configuration config;
  std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
  ProfileExporter exporter(&config, types);
  for (const uint16_t status : {200, 202, 204}) {
    EXPECT_TRUE(exporter.CheckExportResponse(status, true));
  }
  for (const uint16_t status : {301, 400, 403, 404, 429, 500, 503, 504}) {
    EXPECT_FALSE(exporter.CheckExportResponse(status, true));
    EXPECT_NE(exporter.GetLastError().find(std::to_string(status)), std::string::npos);
  }
  EXPECT_TRUE(exporter.CheckExportResponse(504));
  EXPECT_TRUE(exporter.CheckExportResponse(500));
  EXPECT_FALSE(exporter.CheckExportResponse(403));
}

TEST(LifecycleDiagnosticStorageTests, LoggingScopesRestoreAfterExceptions) {
  EXPECT_TRUE(dd_win_prof::diagnosticLoggingEnabled);
  try {
    dd_win_prof::ScopedDiagnosticLogging outer(false);
    EXPECT_FALSE(dd_win_prof::diagnosticLoggingEnabled);
    {
      dd_win_prof::ScopedDiagnosticLogging inner(true);
      EXPECT_TRUE(dd_win_prof::diagnosticLoggingEnabled);
    }
    EXPECT_FALSE(dd_win_prof::diagnosticLoggingEnabled);
    throw 42;
  } catch (...) {
  }
  EXPECT_TRUE(dd_win_prof::diagnosticLoggingEnabled);
}
