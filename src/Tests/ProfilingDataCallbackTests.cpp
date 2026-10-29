// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <chrono>
#include <filesystem>
#include <future>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include "Configuration.h"
#include "EnvironmentVariables.h"
#include "LibDatadogHelper.h"
#include "ProfileExporter.h"
#include "Profiler.h"
#include "internal/dd-win-rum-private.h"
#include "pch.h"

namespace {
struct EncodingFailureState {
  bool serialize = false;
  bool bytes = false;
  bool reset = false;
  int serializeCalls = 0;
  int bytesCalls = 0;
  int resetCalls = 0;
};
thread_local EncodingFailureState encodingFailure;

ddog_Error MakeNativeError() {
  // The FFI has no error constructor; tag validation produces a Rust-owned error.
  auto tags = ddog_Vec_Tag_new();
  auto result = ddog_Vec_Tag_push(
      &tags, dd_win_prof::to_CharSlice(""), dd_win_prof::to_CharSlice("")
  );
  ddog_Vec_Tag_drop(tags);
  if (result.tag != DDOG_VEC_TAG_PUSH_RESULT_ERR) {
    throw std::runtime_error("Empty tag unexpectedly accepted");
  }
  return result.err;
}

ddog_prof_Profile_SerializeResult SerializeWithFailure(
    ddog_prof_Profile* profile, const ddog_Timespec* start, const ddog_Timespec* end
) {
  ++encodingFailure.serializeCalls;
  if (std::exchange(encodingFailure.serialize, false)) {
    ddog_prof_Profile_SerializeResult result{};
    result.tag = DDOG_PROF_PROFILE_SERIALIZE_RESULT_ERR;
    result.err = MakeNativeError();
    return result;
  }
  return ddog_prof_Profile_serialize(profile, start, end);
}

ddog_prof_Result_ByteSlice ReadBytesWithFailure(ddog_prof_EncodedProfile* profile) {
  ++encodingFailure.bytesCalls;
  if (std::exchange(encodingFailure.bytes, false)) {
    ddog_prof_Result_ByteSlice result{};
    result.tag = DDOG_PROF_RESULT_BYTE_SLICE_ERR_BYTE_SLICE;
    result.err = MakeNativeError();
    return result;
  }
  return ddog_prof_EncodedProfile_bytes(profile);
}

ddog_prof_Profile_Result ResetWithFailure(ddog_prof_Profile* profile) {
  ++encodingFailure.resetCalls;
  if (std::exchange(encodingFailure.reset, false)) {
    ddog_prof_Profile_Result result{};
    result.tag = DDOG_PROF_PROFILE_RESULT_ERR;
    result.err = MakeNativeError();
    return result;
  }
  return ddog_prof_Profile_reset(profile);
}
}  // namespace

class ProfilingDataCallbackTests : public ::testing::Test {
 protected:
  void SetUp() override {
    previous = *Profiler::GetConfiguration();
    Profiler::GetConfiguration()->ResetToDefaults();
    profiler = std::make_unique<Profiler>();
  }

  void TearDown() override {
    if (profiler->IsStarted()) {
      StopProfiler();
    }
    profiler.reset();
    *Profiler::GetConfiguration() = std::move(*previous);
  }

  std::optional<Configuration> previous;
  std::unique_ptr<Profiler> profiler;
};

TEST_F(ProfilingDataCallbackTests, RegistrationCopiesCallable) {
  int calls = 0;
  ProfilingDataCallback callback = [&](ProfilingData) { ++calls; };
  ASSERT_TRUE(SetupProfilingDataCallback(callback));
  callback = {};
  profiler->GetProfilingDataCallback()(ProfilingData{});
  EXPECT_EQ(calls, 1);
}

TEST_F(ProfilingDataCallbackTests, EmptyCallableUnregistersCallback) {
  ASSERT_TRUE(SetupProfilingDataCallback([](ProfilingData) {}));
  const ProfilingDataCallback empty;
  EXPECT_TRUE(SetupProfilingDataCallback(empty));
  EXPECT_FALSE(profiler->GetProfilingDataCallback());
  EXPECT_EQ(profiler->GetDeliveryMode(), ProfileDeliveryMode::Http);
}

TEST_F(ProfilingDataCallbackTests, FailedCallbackCopyPreservesRegistration) {
  int calls = 0;
  ASSERT_TRUE(SetupProfilingDataCallback([&](ProfilingData) { ++calls; }));
  struct ThrowOnCopy {
    bool* fail;
    explicit ThrowOnCopy(bool* flag) : fail(flag) {}
    ThrowOnCopy(const ThrowOnCopy& other) : fail(other.fail) {
      if (*fail) {
        throw std::runtime_error("callback copy failed");
      }
    }
    void operator()(ProfilingData) const {}
  };
  bool fail = false;
  ProfilingDataCallback replacement = ThrowOnCopy(&fail);
  fail = true;
  EXPECT_FALSE(SetupProfilingDataCallback(replacement));
  EXPECT_FALSE(profiler->IsStarted());
  EXPECT_EQ(calls, 0);
  profiler->GetProfilingDataCallback()(ProfilingData{});
  EXPECT_EQ(calls, 1);
}

TEST_F(ProfilingDataCallbackTests, NonStandardCopyExceptionPreservesRegistration) {
  int calls = 0;
  ASSERT_TRUE(SetupProfilingDataCallback([&](ProfilingData) { ++calls; }));
  struct ThrowOnCopy {
    ThrowOnCopy() = default;
    ThrowOnCopy(ThrowOnCopy&&) = default;
    ThrowOnCopy(const ThrowOnCopy&) { throw 42; }
    void operator()(ProfilingData) const {}
  };
  ProfilingDataCallback replacement = ThrowOnCopy{};
  EXPECT_FALSE(SetupProfilingDataCallback(replacement));
  EXPECT_FALSE(profiler->IsStarted());
  EXPECT_EQ(calls, 0);
  profiler->GetProfilingDataCallback()(ProfilingData{});
  EXPECT_EQ(calls, 1);
}

TEST_F(ProfilingDataCallbackTests, RegistrationLifetimeMatchesProfiler) {
  auto captured = std::make_shared<int>(0);
  std::weak_ptr<int> lifetime = captured;
  {
    ProfilingDataCallback callback = [captured](ProfilingData) { ++*captured; };
    ASSERT_TRUE(SetupProfilingDataCallback(callback));
  }
  captured.reset();
  EXPECT_FALSE(lifetime.expired());
  profiler.reset();
  EXPECT_TRUE(lifetime.expired());
  profiler = std::make_unique<Profiler>();
  EXPECT_FALSE(profiler->GetProfilingDataCallback());
}

TEST_F(ProfilingDataCallbackTests, NoHttpSettingsNeededWithNoEnvVars) {
  ASSERT_TRUE(SetupProfilingDataCallback([](ProfilingData) {}));
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  config.uploadIntervalSeconds = 17;
  ASSERT_TRUE(SetupProfiler(config));
  EXPECT_EQ(
      Profiler::GetConfiguration()->GetUploadInterval(), std::chrono::seconds(17)
  );
  EXPECT_EQ(profiler->GetDeliveryMode(), ProfileDeliveryMode::Callback);
  EXPECT_FALSE(
      Profiler::GetConfiguration()->IsExportEnabled(profiler->GetDeliveryMode())
  );
  EXPECT_TRUE(Profiler::GetConfiguration()->GetApiKey().empty());
}

TEST_F(ProfilingDataCallbackTests, FailedSetupDoesNotInvokeCallback) {
  int calls = 0;
  ASSERT_TRUE(SetupProfilingDataCallback([&](ProfilingData) { ++calls; }));
  ProfilerConfig config{};
  config.size = sizeof(config) - 1;
  EXPECT_FALSE(SetupProfiler(config));
  EXPECT_FALSE(profiler->IsStarted());
  EXPECT_TRUE(profiler->GetProfilingDataCallback());
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(StopProfiler());
  EXPECT_EQ(calls, 0);
}

TEST_F(ProfilingDataCallbackTests, FailedStartDoesNotInvokeCallback) {
  const auto* name = EnvironmentVariables::ProfilerEnabled;
  const auto size = ::GetEnvironmentVariableA(name, nullptr, 0);
  std::string previousValue(size, '\0');
  if (size != 0) {
    ASSERT_NE(::GetEnvironmentVariableA(name, previousValue.data(), size), 0u);
  }
  struct RestoreEnvironment {
    const char* name;
    const std::string& value;
    ~RestoreEnvironment() {
      EXPECT_TRUE(
          ::SetEnvironmentVariableA(name, value.empty() ? nullptr : value.c_str())
      );
    }
  } restore{name, previousValue};
  ASSERT_TRUE(::SetEnvironmentVariableA(name, "0"));
  *Profiler::GetConfiguration() = Configuration();
  ASSERT_TRUE(Profiler::GetConfiguration()->IsProfilerExplicitlyDisabled());
  int calls = 0;
  ASSERT_TRUE(SetupProfilingDataCallback([&](ProfilingData) { ++calls; }));
  ProfilerConfig config{};
  config.size = sizeof(config);
  ASSERT_TRUE(SetupProfiler(config));
  EXPECT_FALSE(StartProfiler());
  EXPECT_FALSE(profiler->IsStarted());
  EXPECT_TRUE(profiler->GetProfilingDataCallback());
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(StopProfiler());
  EXPECT_EQ(calls, 0);
}

TEST_F(ProfilingDataCallbackTests, UnregisterRestoresHttpRequirements) {
  ASSERT_TRUE(SetupProfilingDataCallback([](ProfilingData) {}));
  ASSERT_TRUE(SetupProfilingDataCallback({}));
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  EXPECT_FALSE(SetupProfiler(config));
  EXPECT_TRUE(Profiler::GetConfiguration()->IsExportEnabled());
}

TEST_F(
    ProfilingDataCallbackTests,
    StopDeliversFinalProfileAndRegistrationIsFrozenWhileRunning
) {
  std::vector<ProfilingData> results;
  std::thread::id callbackThread;
  ASSERT_TRUE(SetupProfilingDataCallback([&](ProfilingData data) {
    callbackThread = std::this_thread::get_id();
    results.push_back(std::move(data));
  }));
  RumSessionContext session{"app-1", "session-1"};
  RumViewValues view{"view-1", "HomePage"};
  ASSERT_TRUE(SetRumSession(&session));
  ASSERT_TRUE(SetRumView(&view));
  ASSERT_TRUE(SetRumSession(nullptr));
  const auto beforeStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch()
  )
                                 .count();
  ASSERT_TRUE(StartProfiler());
  EXPECT_FALSE(SetupProfilingDataCallback({}));
  EXPECT_TRUE(StopProfiler());
  const auto afterStopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch()
  )
                               .count();
  EXPECT_TRUE(profiler->GetProfilingDataCallback());
  EXPECT_EQ(callbackThread, std::this_thread::get_id());
  ASSERT_EQ(results.size(), 1u);
  const auto& data = results.front();
  EXPECT_EQ(data.result, 0u);
  EXPECT_TRUE(data.isLast);
  EXPECT_FALSE(data.pprof.empty());
  EXPECT_GE(data.startUnixTimeMs, beforeStartMs);
  EXPECT_LE(data.startUnixTimeMs, data.endUnixTimeMs);
  EXPECT_LE(data.endUnixTimeMs, afterStopMs);
  std::vector<RumViewRecord> remainingViews;
  std::vector<RumSessionRecord> remainingSessions;
  profiler->ConsumeViewRecords(remainingViews);
  profiler->ConsumeSessionRecords(remainingSessions);
  EXPECT_TRUE(remainingViews.empty());
  EXPECT_TRUE(remainingSessions.empty());
  EXPECT_TRUE(SetupProfilingDataCallback({}));
}

TEST_F(ProfilingDataCallbackTests, CallbackDiscardsRumRecordsWithoutMetadataFiles) {
  int calls = 0;
  ASSERT_TRUE(SetupProfilingDataCallback([&](ProfilingData data) {
    EXPECT_EQ(data.result, 0u);
    ++calls;
  }));
  const auto outputDir =
      fs::temp_directory_path() /
      ("dd-win-prof-callback-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  auto config = Profiler::GetConfiguration();
  config->SetProfilesOutputDirectory(outputDir);
  std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
  ProfileExporter exporter(
      config, types, profiler.get(), profiler->GetProfilingDataCallback()
  );
  ASSERT_TRUE(exporter.Initialize());
  for (int cycle = 0; cycle < 2; ++cycle) {
    RumSessionContext session{"app-1", "session-1"};
    RumViewValues view{"view-1", "HomePage"};
    ASSERT_TRUE(SetRumSession(&session));
    ASSERT_TRUE(SetRumView(&view));
    ASSERT_TRUE(SetRumSession(nullptr));
    ASSERT_TRUE(exporter.Export(cycle == 1));
    std::vector<RumViewRecord> remainingViews;
    std::vector<RumSessionRecord> remainingSessions;
    profiler->ConsumeViewRecords(remainingViews);
    profiler->ConsumeSessionRecords(remainingSessions);
    EXPECT_TRUE(remainingViews.empty());
    EXPECT_TRUE(remainingSessions.empty());
  }
  EXPECT_EQ(calls, 2);
  exporter.Cleanup();
  bool foundPprof = false;
  for (const auto& entry : fs::directory_iterator(outputDir)) {
    EXPECT_EQ(entry.path().extension(), ".pprof");
    foundPprof |= entry.path().extension() == ".pprof";
  }
  EXPECT_TRUE(foundPprof);
  fs::remove_all(outputDir);
}

TEST_F(ProfilingDataCallbackTests, EncodingErrorsReturnCodesAndNextExportRecovers) {
  for (const auto code :
       {ProfilingErrorCode::SerializationFailed,
        ProfilingErrorCode::ProfileBytesUnavailable}) {
    SCOPED_TRACE(static_cast<uint32_t>(code));
    encodingFailure = {};
    encodingFailure.serialize = code == ProfilingErrorCode::SerializationFailed;
    encodingFailure.bytes = code == ProfilingErrorCode::ProfileBytesUnavailable;
    std::vector<ProfilingData> results;
    ProfilingDataCallback callback = [&](ProfilingData data) {
      results.push_back(std::move(data));
    };
    ASSERT_TRUE(SetupProfilingDataCallback(callback));
    const dd_win_prof::ProfileEncodingApi api{
        SerializeWithFailure, ReadBytesWithFailure, ResetWithFailure
    };
    std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
    ProfileExporter exporter(
        Profiler::GetConfiguration(), types, profiler.get(), callback, api
    );
    ASSERT_TRUE(exporter.Initialize());
    RumSessionContext session{"app-1", "session-1"};
    RumViewValues view{"view-1", "HomePage"};
    ASSERT_TRUE(SetRumSession(&session));
    ASSERT_TRUE(SetRumView(&view));
    ASSERT_TRUE(SetRumSession(nullptr));
    EXPECT_FALSE(exporter.Export());
    EXPECT_EQ(encodingFailure.serializeCalls, 1);
    EXPECT_EQ(
        encodingFailure.bytesCalls,
        code == ProfilingErrorCode::SerializationFailed ? 0 : 1
    );
    EXPECT_EQ(encodingFailure.resetCalls, 1);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().result, static_cast<uint32_t>(code));
    EXPECT_TRUE(results.front().pprof.empty());
    EXPECT_FALSE(results.front().isLast);
    const char* explanation = GetProfilingErrorMessage(results.front().result);
    EXPECT_STRNE(explanation, "Unknown profiling error");
    std::vector<RumViewRecord> remainingViews;
    std::vector<RumSessionRecord> remainingSessions;
    profiler->ConsumeViewRecords(remainingViews);
    profiler->ConsumeSessionRecords(remainingSessions);
    EXPECT_TRUE(remainingViews.empty());
    EXPECT_TRUE(remainingSessions.empty());
    EXPECT_TRUE(exporter.Export(true));
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results.back().result, 0u);
    EXPECT_FALSE(results.back().pprof.empty());
    EXPECT_TRUE(results.back().isLast);
    encodingFailure.serialize = code == ProfilingErrorCode::SerializationFailed;
    encodingFailure.bytes = code == ProfilingErrorCode::ProfileBytesUnavailable;
    EXPECT_FALSE(exporter.Export(true));
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results.back().result, static_cast<uint32_t>(code));
    EXPECT_TRUE(results.back().pprof.empty());
    EXPECT_TRUE(results.back().isLast);
    exporter.Cleanup();
    EXPECT_STREQ(explanation, GetProfilingErrorMessage(results.front().result));
  }
}

TEST_F(ProfilingDataCallbackTests, FinalEncodingErrorsReturnFalseAndNotifyCallback) {
  for (const auto code :
       {ProfilingErrorCode::SerializationFailed,
        ProfilingErrorCode::ProfileBytesUnavailable,
        ProfilingErrorCode::ProfileResetFailed}) {
    SCOPED_TRACE(static_cast<uint32_t>(code));
    encodingFailure = {};
    encodingFailure.serialize = code == ProfilingErrorCode::SerializationFailed;
    encodingFailure.bytes = code == ProfilingErrorCode::ProfileBytesUnavailable;
    encodingFailure.reset = code == ProfilingErrorCode::ProfileResetFailed;
    int errors = 0;
    ProfilingDataCallback callback = [&](ProfilingData data) {
      EXPECT_TRUE(data.isLast);
      if (data.result != 0) {
        EXPECT_EQ(data.result, static_cast<uint32_t>(code));
        EXPECT_TRUE(data.pprof.empty());
        ++errors;
      }
    };
    const dd_win_prof::ProfileEncodingApi api{
        SerializeWithFailure, ReadBytesWithFailure, ResetWithFailure
    };
    std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
    ProfileExporter exporter(
        Profiler::GetConfiguration(), types, nullptr, callback, api
    );
    ASSERT_TRUE(exporter.Initialize());
    SamplesCollector collector(Profiler::GetConfiguration(), &exporter);
    collector.Start();
    EXPECT_FALSE(collector.Stop());
    exporter.Cleanup();
    EXPECT_EQ(errors, 1);
  }
}

TEST_F(ProfilingDataCallbackTests, ThrowingErrorCallbackDoesNotRecurse) {
  encodingFailure = {};
  encodingFailure.serialize = true;
  encodingFailure.reset = true;
  std::vector<uint32_t> errors;
  ProfilingDataCallback callback = [&](ProfilingData data) {
    errors.push_back(data.result);
    throw std::runtime_error("diagnostic callback failed");
  };
  const dd_win_prof::ProfileEncodingApi api{
      SerializeWithFailure, ReadBytesWithFailure, ResetWithFailure
  };
  std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
  ProfileExporter exporter(Profiler::GetConfiguration(), types, nullptr, callback, api);
  ASSERT_TRUE(exporter.Initialize());
  EXPECT_FALSE(exporter.Export(true));
  exporter.Cleanup();
  ASSERT_EQ(errors.size(), 2u);
  EXPECT_EQ(errors[0], static_cast<uint32_t>(ProfilingErrorCode::SerializationFailed));
  EXPECT_EQ(errors[1], static_cast<uint32_t>(ProfilingErrorCode::ProfileResetFailed));
}

TEST_F(ProfilingDataCallbackTests, InternalExportUsesBooleanResults) {
  encodingFailure = {};
  Profiler::GetConfiguration()->SetExportEnabled(false);
  const dd_win_prof::ProfileEncodingApi api{
      SerializeWithFailure, ReadBytesWithFailure, ResetWithFailure
  };
  std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
  ProfileExporter exporter(Profiler::GetConfiguration(), types, nullptr, {}, api);
  EXPECT_FALSE(exporter.Export());
  EXPECT_EQ(encodingFailure.serializeCalls, 0);
  EXPECT_EQ(encodingFailure.resetCalls, 0);
  ASSERT_TRUE(exporter.Initialize());
  encodingFailure.serialize = true;
  EXPECT_FALSE(exporter.Export(true));
  EXPECT_EQ(encodingFailure.bytesCalls, 0);
  EXPECT_EQ(encodingFailure.resetCalls, 1);
  EXPECT_TRUE(exporter.Export(true));
  exporter.Cleanup();
}

TEST_F(ProfilingDataCallbackTests, BackgroundErrorCanBeExplainedAfterCallbackReturns) {
  std::vector<ProfilingData> results;
  const bool failed =
      std::async(std::launch::async, [&] {
        encodingFailure = {};
        encodingFailure.serialize = true;
        const dd_win_prof::ProfileEncodingApi api{
            SerializeWithFailure, ReadBytesWithFailure, ResetWithFailure
        };
        std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
        ProfileExporter exporter(
            Profiler::GetConfiguration(),
            types,
            nullptr,
            [&](ProfilingData data) { results.push_back(std::move(data)); },
            api
        );
        EXPECT_TRUE(exporter.Initialize());
        const bool success = exporter.Export();
        exporter.Cleanup();
        return !success;
      }).get();
  EXPECT_TRUE(failed);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(
      results.front().result,
      static_cast<uint32_t>(ProfilingErrorCode::SerializationFailed)
  );
  EXPECT_TRUE(results.front().pprof.empty());
  EXPECT_STREQ(
      GetProfilingErrorMessage(results.front().result), "Profile serialization failed"
  );
}

TEST_F(ProfilingDataCallbackTests, CanReconfigureAndReplaceCallbackAfterStop) {
  int firstCalls = 0;
  int secondCalls = 0;
  ASSERT_TRUE(SetupProfilingDataCallback([&](ProfilingData) { ++firstCalls; }));
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  config.uploadIntervalSeconds = 3600;
  for (int cycle = 0; cycle < 2; ++cycle) {
    ASSERT_TRUE(SetupProfiler(config));
    ASSERT_TRUE(StartProfiler());
    EXPECT_TRUE(StopProfiler());
    EXPECT_EQ(firstCalls, cycle + 1);
    EXPECT_TRUE(StopProfiler());
    EXPECT_EQ(firstCalls, cycle + 1);
  }
  ASSERT_TRUE(SetupProfilingDataCallback([&](ProfilingData) { ++secondCalls; }));
  ASSERT_TRUE(SetupProfiler(config));
  ASSERT_TRUE(StartProfiler());
  EXPECT_TRUE(StopProfiler());
  EXPECT_EQ(firstCalls, 2);
  EXPECT_EQ(secondCalls, 1);
}

TEST_F(
    ProfilingDataCallbackTests, StopWaitsForInFlightCallbackAndDeliversFinalProfile
) {
  std::promise<void> periodicEntered;
  auto entered = periodicEntered.get_future();
  std::promise<void> releasePeriodic;
  auto released = releasePeriodic.get_future();
  std::vector<ProfilingData> results;
  ASSERT_TRUE(SetupProfilingDataCallback([&](ProfilingData data) {
    const bool firstPeriodic = data.result == 0 && !data.isLast && results.empty();
    results.push_back(std::move(data));
    if (firstPeriodic) {
      periodicEntered.set_value();
      released.wait();
    }
  }));
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  config.uploadIntervalSeconds = 1;
  ASSERT_TRUE(SetupProfiler(config));
  ASSERT_TRUE(StartProfiler());
  const auto status = entered.wait_for(std::chrono::seconds(5));
  EXPECT_EQ(status, std::future_status::ready);
  auto stopped = std::async(std::launch::async, [] { return StopProfiler(); });
  if (status == std::future_status::ready) {
    EXPECT_EQ(
        stopped.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout
    );
  }
  releasePeriodic.set_value();
  EXPECT_TRUE(stopped.get());
  ASSERT_GE(results.size(), 2u);
  EXPECT_EQ(results.front().result, 0u);
  EXPECT_EQ(results.back().result, 0u);
  EXPECT_FALSE(results.front().isLast);
  EXPECT_TRUE(results.back().isLast);
}

TEST_F(ProfilingDataCallbackTests, MissingProfilerRejectsRegistration) {
  profiler.reset();
  EXPECT_FALSE(SetupProfilingDataCallback({}));
  profiler = std::make_unique<Profiler>();
}

TEST(ProfilingDataExportTests, ResultIsFirstAggregateFieldAndDefaultsToSuccess) {
  static_assert(std::is_same_v<decltype(ProfilingData::result), uint32_t>);
  ProfilingData success;
  EXPECT_EQ(success.result, 0u);
  EXPECT_TRUE(success.pprof.empty());
  EXPECT_EQ(success.startUnixTimeMs, 0);
  EXPECT_EQ(success.endUnixTimeMs, 0);
  EXPECT_FALSE(success.isLast);
  ProfilingData failure{
      static_cast<uint32_t>(ProfilingErrorCode::SerializationFailed), {}, 0, 0, true
  };
  EXPECT_EQ(
      failure.result, static_cast<uint32_t>(ProfilingErrorCode::SerializationFailed)
  );
  EXPECT_TRUE(failure.isLast);
}

TEST(ProfilingDataExportTests, CallbackBypassesHttpAndOwnsBytes) {
  Configuration config;
  config.ResetToDefaults();
  config.SetApiKey("unused");
  std::vector<ProfilingData> results;
  ProfilingDataCallback callback = [&](ProfilingData data) {
    results.push_back(std::move(data));
  };
  ASSERT_TRUE(config.ValidateTransportConfig(ProfileDeliveryMode::Callback));
  EXPECT_FALSE(config.IsExportEnabled(ProfileDeliveryMode::Callback));
  std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
  {
    ProfileExporter exporter(&config, types, nullptr, callback);
    ASSERT_TRUE(exporter.Initialize());
    ASSERT_TRUE(exporter.Export());
    ASSERT_TRUE(exporter.Export(true));
    exporter.Cleanup();
  }
  ASSERT_EQ(results.size(), 2u);
  const auto& first = results[0];
  const auto& second = results[1];
  EXPECT_EQ(first.result, 0u);
  EXPECT_EQ(second.result, 0u);
  EXPECT_FALSE(first.isLast);
  EXPECT_TRUE(second.isLast);
  EXPECT_FALSE(first.pprof.empty());
  EXPECT_FALSE(second.pprof.empty());
  EXPECT_LE(first.startUnixTimeMs, first.endUnixTimeMs);
  EXPECT_EQ(first.endUnixTimeMs, second.startUnixTimeMs);
}

TEST(ProfilingDataExportTests, ResetErrorIsDeliveredAndNextExportRecovers) {
  for (const bool lastCall : {false, true}) {
    SCOPED_TRACE(lastCall);
    encodingFailure = {};
    encodingFailure.reset = true;
    Configuration config;
    config.ResetToDefaults();
    std::vector<ProfilingData> results;
    ProfilingDataCallback callback = [&](ProfilingData data) {
      results.push_back(std::move(data));
    };
    dd_win_prof::ProfileEncodingApi api;
    api.reset = ResetWithFailure;
    std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
    ProfileExporter exporter(&config, types, nullptr, callback, api);
    ASSERT_TRUE(exporter.Initialize());
    EXPECT_FALSE(exporter.Export(lastCall));
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results.front().result, 0u);
    EXPECT_EQ(results.front().isLast, lastCall);
    EXPECT_FALSE(results.front().pprof.empty());
    EXPECT_EQ(
        results.back().result,
        static_cast<uint32_t>(ProfilingErrorCode::ProfileResetFailed)
    );
    EXPECT_EQ(results.back().isLast, lastCall);
    EXPECT_TRUE(results.back().pprof.empty());
    EXPECT_STREQ(
        GetProfilingErrorMessage(results.back().result), "Profile reset failed"
    );
    EXPECT_TRUE(exporter.Export(true));
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results.back().result, 0u);
    EXPECT_FALSE(results.back().pprof.empty());
    exporter.Cleanup();
  }
}

TEST(ProfilingDataExportTests, ThrowingCallbackDoesNotPreventNextExport) {
  Configuration config;
  config.ResetToDefaults();
  int calls = 0;
  ProfilingDataCallback callback = [&](ProfilingData) {
    if (++calls == 1) {
      throw std::runtime_error("queue unavailable");
    }
    if (calls == 2) {
      throw 42;
    }
  };
  std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
  ProfileExporter exporter(&config, types, nullptr, callback);
  ASSERT_TRUE(exporter.Initialize());
  EXPECT_FALSE(exporter.Export());
  EXPECT_FALSE(exporter.Export());
  EXPECT_TRUE(exporter.Export(true));
  EXPECT_EQ(calls, 3);
  exporter.Cleanup();
}
