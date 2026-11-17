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
constexpr uint32_t ToResult(ProfilingErrorCode code) {
  return static_cast<uint32_t>(code);
}

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

struct CapturedData {
  uint32_t structSize;
  uint32_t result;
  bool pointerNull;
  size_t pprofSize;
  std::vector<uint8_t> pprof;
  int64_t startUnixTimeMs;
  int64_t endUnixTimeMs;
  bool isLast;
};

struct CallbackContext {
  std::vector<CapturedData> results;
  int calls = 0;
  void* observedUserData = nullptr;
  bool registrationRejected = false;
  bool sawLiveZstd = false;
  std::promise<void>* entered = nullptr;
  std::shared_future<void>* release = nullptr;
  std::thread::id callbackThread;
};

void DD_WIN_PROF_CALL CaptureCallback(const ProfilingData* data, void* userData) {
  auto& context = *static_cast<CallbackContext*>(userData);
  context.observedUserData = userData;
  context.callbackThread = std::this_thread::get_id();
  CapturedData result{
      data->struct_size,
      data->result,
      data->pprof_bytes == nullptr,
      data->pprof_size,
      {},
      data->start_unix_time_ms,
      data->end_unix_time_ms,
      data->is_last != 0
  };
  if (data->pprof_size != 0) {
    context.sawLiveZstd = data->pprof_size >= 4 && data->pprof_bytes[0] == 0x28 &&
                          data->pprof_bytes[1] == 0xb5 &&
                          data->pprof_bytes[2] == 0x2f && data->pprof_bytes[3] == 0xfd;
    result.pprof.assign(data->pprof_bytes, data->pprof_bytes + data->pprof_size);
  }
  context.results.push_back(std::move(result));
  ++context.calls;
}

void DD_WIN_PROF_CALL CountCallback(const ProfilingData*, void* userData) {
  if (userData != nullptr) {
    ++*static_cast<int*>(userData);
  }
}

void DD_WIN_PROF_CALL
RejectRegistrationCallback(const ProfilingData* data, void* userData) {
  CaptureCallback(data, userData);
  auto& context = *static_cast<CallbackContext*>(userData);
  context.registrationRejected = !SetupProfilingDataCallback(nullptr, nullptr);
}

void DD_WIN_PROF_CALL BlockingCallback(const ProfilingData* data, void* userData) {
  auto& context = *static_cast<CallbackContext*>(userData);
  const bool firstPeriodic =
      data->result == 0 && data->is_last == 0 && context.results.empty();
  CaptureCallback(data, userData);
  if (firstPeriodic) {
    context.registrationRejected = !SetupProfilingDataCallback(nullptr, nullptr);
    context.entered->set_value();
    context.release->wait();
  }
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

TEST_F(ProfilingDataCallbackTests, RegisterAndUnregisterCallback) {
  int calls = 0;
  ASSERT_TRUE(SetupProfilingDataCallback(CountCallback, &calls));
  EXPECT_EQ(profiler->GetProfilingDataCallback(), CountCallback);
  EXPECT_EQ(profiler->GetProfilingDataCallbackUserData(), &calls);
  EXPECT_TRUE(SetupProfilingDataCallback(nullptr, nullptr));
  EXPECT_FALSE(profiler->GetProfilingDataCallback());
  EXPECT_EQ(profiler->GetDeliveryMode(), ProfileDeliveryMode::Http);
}

TEST_F(ProfilingDataCallbackTests, NullCallbackWithContextPreservesRegistration) {
  int calls = 0;
  ASSERT_TRUE(SetupProfilingDataCallback(CountCallback, &calls));
  EXPECT_FALSE(SetupProfilingDataCallback(nullptr, &calls));
  EXPECT_EQ(profiler->GetProfilingDataCallback(), CountCallback);
  EXPECT_EQ(profiler->GetProfilingDataCallbackUserData(), &calls);
}

TEST_F(ProfilingDataCallbackTests, ProfilerDestructionDoesNotDestroyCallerState) {
  int calls = 0;
  ASSERT_TRUE(SetupProfilingDataCallback(CountCallback, &calls));
  profiler.reset();
  EXPECT_EQ(calls, 0);
  profiler = std::make_unique<Profiler>();
  EXPECT_FALSE(profiler->GetProfilingDataCallback());
}

TEST_F(ProfilingDataCallbackTests, NoHttpSettingsNeededWithNoEnvVars) {
  ASSERT_TRUE(SetupProfilingDataCallback(CountCallback, nullptr));
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  config.uploadIntervalSeconds = 17;
  ASSERT_TRUE(SetupProfiler(&config));
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
  ASSERT_TRUE(SetupProfilingDataCallback(CountCallback, &calls));
  ProfilerConfig config{};
  config.size = sizeof(config) - 1;
  EXPECT_FALSE(SetupProfiler(&config));
  EXPECT_FALSE(profiler->IsStarted());
  EXPECT_TRUE(profiler->GetProfilingDataCallback());
  EXPECT_EQ(calls, 0);
  StopProfiler();
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
  ASSERT_TRUE(SetupProfilingDataCallback(CountCallback, &calls));
  ProfilerConfig config{};
  config.size = sizeof(config);
  ASSERT_TRUE(SetupProfiler(&config));
  EXPECT_FALSE(StartProfiler());
  EXPECT_FALSE(profiler->IsStarted());
  EXPECT_TRUE(profiler->GetProfilingDataCallback());
  EXPECT_EQ(calls, 0);
  StopProfiler();
  EXPECT_EQ(calls, 0);
}

TEST_F(ProfilingDataCallbackTests, UnregisterRestoresHttpRequirements) {
  ASSERT_TRUE(SetupProfilingDataCallback(CountCallback, nullptr));
  ASSERT_TRUE(SetupProfilingDataCallback(nullptr, nullptr));
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  EXPECT_FALSE(SetupProfiler(&config));
  EXPECT_TRUE(Profiler::GetConfiguration()->IsExportEnabled());
}

TEST_F(
    ProfilingDataCallbackTests,
    StopDeliversFinalProfileAndRegistrationIsFrozenWhileRunning
) {
  CallbackContext context;
  ASSERT_TRUE(SetupProfilingDataCallback(RejectRegistrationCallback, &context));
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
  EXPECT_FALSE(SetupProfilingDataCallback(nullptr, nullptr));
  StopProfiler();
  const auto afterStopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch()
  )
                               .count();
  EXPECT_TRUE(profiler->GetProfilingDataCallback());
  EXPECT_EQ(context.callbackThread, std::this_thread::get_id());
  EXPECT_TRUE(context.registrationRejected);
  EXPECT_EQ(context.observedUserData, &context);
  ASSERT_EQ(context.results.size(), 1u);
  const auto& data = context.results.front();
  EXPECT_EQ(data.structSize, sizeof(ProfilingData));
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
  EXPECT_TRUE(SetupProfilingDataCallback(nullptr, nullptr));
}

TEST_F(ProfilingDataCallbackTests, CallbackDiscardsRumRecordsWithoutMetadataFiles) {
  CallbackContext context;
  ASSERT_TRUE(SetupProfilingDataCallback(CaptureCallback, &context));
  const auto outputDir =
      fs::temp_directory_path() /
      ("dd-win-prof-callback-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  auto config = Profiler::GetConfiguration();
  config->SetProfilesOutputDirectory(outputDir);
  std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
  ProfileExporter exporter(
      config,
      types,
      profiler.get(),
      profiler->GetProfilingDataCallback(),
      profiler->GetProfilingDataCallbackUserData()
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
  EXPECT_EQ(context.calls, 2);
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
    SCOPED_TRACE(ToResult(code));
    encodingFailure = {};
    encodingFailure.serialize = code == ProfilingErrorCode::SerializationFailed;
    encodingFailure.bytes = code == ProfilingErrorCode::ProfileBytesUnavailable;
    CallbackContext context;
    ASSERT_TRUE(SetupProfilingDataCallback(CaptureCallback, &context));
    const dd_win_prof::ProfileEncodingApi api{
        SerializeWithFailure, ReadBytesWithFailure, ResetWithFailure
    };
    std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
    ProfileExporter exporter(
        Profiler::GetConfiguration(),
        types,
        profiler.get(),
        CaptureCallback,
        &context,
        api
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
    ASSERT_EQ(context.results.size(), 1u);
    EXPECT_EQ(context.results.front().result, ToResult(code));
    EXPECT_EQ(context.results.front().structSize, sizeof(ProfilingData));
    EXPECT_TRUE(context.results.front().pointerNull);
    EXPECT_EQ(context.results.front().pprofSize, 0u);
    EXPECT_TRUE(context.results.front().pprof.empty());
    EXPECT_FALSE(context.results.front().isLast);
    const char* explanation = GetProfilingErrorMessage(context.results.front().result);
    EXPECT_STRNE(explanation, "Unknown profiling error");
    std::vector<RumViewRecord> remainingViews;
    std::vector<RumSessionRecord> remainingSessions;
    profiler->ConsumeViewRecords(remainingViews);
    profiler->ConsumeSessionRecords(remainingSessions);
    EXPECT_TRUE(remainingViews.empty());
    EXPECT_TRUE(remainingSessions.empty());
    EXPECT_TRUE(exporter.Export(true));
    ASSERT_EQ(context.results.size(), 2u);
    EXPECT_EQ(context.results.back().result, 0u);
    EXPECT_FALSE(context.results.back().pprof.empty());
    EXPECT_TRUE(context.results.back().isLast);
    encodingFailure.serialize = code == ProfilingErrorCode::SerializationFailed;
    encodingFailure.bytes = code == ProfilingErrorCode::ProfileBytesUnavailable;
    EXPECT_FALSE(exporter.Export(true));
    ASSERT_EQ(context.results.size(), 3u);
    EXPECT_EQ(context.results.back().result, ToResult(code));
    EXPECT_TRUE(context.results.back().pprof.empty());
    EXPECT_TRUE(context.results.back().isLast);
    exporter.Cleanup();
    EXPECT_STREQ(explanation, GetProfilingErrorMessage(context.results.front().result));
  }
}

TEST_F(ProfilingDataCallbackTests, InternalExportUsesBooleanResults) {
  encodingFailure = {};
  Profiler::GetConfiguration()->SetExportEnabled(false);
  const dd_win_prof::ProfileEncodingApi api{
      SerializeWithFailure, ReadBytesWithFailure, ResetWithFailure
  };
  std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
  ProfileExporter exporter(
      Profiler::GetConfiguration(), types, nullptr, nullptr, nullptr, api
  );
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
  CallbackContext context;
  const bool failed =
      std::async(std::launch::async, [&] {
        encodingFailure = {};
        encodingFailure.serialize = true;
        const dd_win_prof::ProfileEncodingApi api{
            SerializeWithFailure, ReadBytesWithFailure, ResetWithFailure
        };
        std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
        ProfileExporter exporter(
            Profiler::GetConfiguration(), types, nullptr, CaptureCallback, &context, api
        );
        EXPECT_TRUE(exporter.Initialize());
        const bool success = exporter.Export();
        exporter.Cleanup();
        return !success;
      }).get();
  EXPECT_TRUE(failed);
  ASSERT_EQ(context.results.size(), 1u);
  EXPECT_EQ(
      context.results.front().result, ToResult(ProfilingErrorCode::SerializationFailed)
  );
  EXPECT_TRUE(context.results.front().pprof.empty());
  EXPECT_STREQ(
      GetProfilingErrorMessage(context.results.front().result),
      "Profile serialization failed"
  );
}

TEST_F(ProfilingDataCallbackTests, CanReconfigureAndReplaceCallbackAfterStop) {
  int firstCalls = 0;
  int secondCalls = 0;
  ASSERT_TRUE(SetupProfilingDataCallback(CountCallback, &firstCalls));
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  config.uploadIntervalSeconds = 3600;
  for (int cycle = 0; cycle < 2; ++cycle) {
    ASSERT_TRUE(SetupProfiler(&config));
    ASSERT_TRUE(StartProfiler());
    StopProfiler();
    EXPECT_EQ(firstCalls, cycle + 1);
    StopProfiler();
    EXPECT_EQ(firstCalls, cycle + 1);
  }
  ASSERT_TRUE(SetupProfilingDataCallback(CountCallback, &secondCalls));
  ASSERT_TRUE(SetupProfiler(&config));
  ASSERT_TRUE(StartProfiler());
  StopProfiler();
  EXPECT_EQ(firstCalls, 2);
  EXPECT_EQ(secondCalls, 1);
}

TEST_F(
    ProfilingDataCallbackTests, StopWaitsForInFlightCallbackAndDeliversFinalProfile
) {
  std::promise<void> periodicEntered;
  auto entered = periodicEntered.get_future();
  std::promise<void> releasePeriodic;
  auto released = releasePeriodic.get_future().share();
  CallbackContext context;
  context.entered = &periodicEntered;
  context.release = &released;
  ASSERT_TRUE(SetupProfilingDataCallback(BlockingCallback, &context));
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  config.uploadIntervalSeconds = 1;
  ASSERT_TRUE(SetupProfiler(&config));
  ASSERT_TRUE(StartProfiler());
  const auto status = entered.wait_for(std::chrono::seconds(5));
  EXPECT_EQ(status, std::future_status::ready);
  auto stopped = std::async(std::launch::async, [] { StopProfiler(); });
  if (status == std::future_status::ready) {
    EXPECT_TRUE(context.registrationRejected);
    EXPECT_EQ(
        stopped.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout
    );
  }
  releasePeriodic.set_value();
  stopped.get();
  ASSERT_GE(context.results.size(), 2u);
  EXPECT_EQ(context.results.front().result, 0u);
  EXPECT_EQ(context.results.back().result, 0u);
  EXPECT_FALSE(context.results.front().isLast);
  EXPECT_TRUE(context.results.back().isLast);
}

TEST_F(ProfilingDataCallbackTests, MissingProfilerRejectsRegistration) {
  profiler.reset();
  EXPECT_FALSE(SetupProfilingDataCallback(nullptr, nullptr));
  profiler = std::make_unique<Profiler>();
}

TEST(ProfilingDataExportTests, CallbackBorrowsBytesAndCallerCopySurvives) {
  Configuration config;
  config.ResetToDefaults();
  config.SetApiKey("unused");
  CallbackContext context;
  ASSERT_TRUE(config.ValidateTransportConfig(ProfileDeliveryMode::Callback));
  EXPECT_FALSE(config.IsExportEnabled(ProfileDeliveryMode::Callback));
  std::vector<SampleValueType> types{
      {"wait-time", "nanoseconds"}, {"cpu-time", "nanoseconds"}
  };
  std::vector<uint8_t> retained;
  {
    ProfileExporter exporter(&config, types, nullptr, CaptureCallback, &context);
    ASSERT_TRUE(exporter.Initialize());
    ASSERT_TRUE(exporter.Export());
    ASSERT_EQ(context.results.size(), 1u);
    retained = context.results.front().pprof;
    ASSERT_TRUE(exporter.Export(true));
    exporter.Cleanup();
  }
  ASSERT_EQ(context.results.size(), 2u);
  EXPECT_EQ(context.results.front().pprof, retained);
  EXPECT_TRUE(context.sawLiveZstd);
  const auto& first = context.results[0];
  const auto& second = context.results[1];
  EXPECT_EQ(first.result, 0u);
  EXPECT_EQ(second.result, 0u);
  EXPECT_FALSE(first.isLast);
  EXPECT_TRUE(second.isLast);
  for (const auto* data : {&first, &second}) {
    ASSERT_GE(data->pprof.size(), 4u);
    EXPECT_EQ(
        (std::vector<uint8_t>(data->pprof.begin(), data->pprof.begin() + 4)),
        (std::vector<uint8_t>{0x28, 0xb5, 0x2f, 0xfd})
    );
  }
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
    CallbackContext context;
    dd_win_prof::ProfileEncodingApi api;
    api.reset = ResetWithFailure;
    std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
    ProfileExporter exporter(&config, types, nullptr, CaptureCallback, &context, api);
    ASSERT_TRUE(exporter.Initialize());
    EXPECT_FALSE(exporter.Export(lastCall));
    ASSERT_EQ(context.results.size(), 2u);
    EXPECT_EQ(context.results.front().result, 0u);
    EXPECT_EQ(context.results.front().isLast, lastCall);
    EXPECT_FALSE(context.results.front().pprof.empty());
    EXPECT_EQ(
        context.results.back().result, ToResult(ProfilingErrorCode::ProfileResetFailed)
    );
    EXPECT_EQ(context.results.back().isLast, lastCall);
    EXPECT_TRUE(context.results.back().pprof.empty());
    EXPECT_STREQ(
        GetProfilingErrorMessage(context.results.back().result), "Profile reset failed"
    );

    Sample::SetValuesCount(types.size());
    HANDLE thread = nullptr;
    ASSERT_TRUE(
        ::DuplicateHandle(
            ::GetCurrentProcess(),
            ::GetCurrentThread(),
            ::GetCurrentProcess(),
            &thread,
            0,
            FALSE,
            DUPLICATE_SAME_ACCESS
        )
    );
    auto threadInfo = std::make_shared<ThreadInfo>(::GetCurrentThreadId(), thread);
    uint64_t frames[] = {reinterpret_cast<uint64_t>(&ResetWithFailure)};
    auto sample = std::make_shared<Sample>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ),
        threadInfo,
        frames,
        1
    );
    sample->AddValue(42, 0);
    ASSERT_TRUE(exporter.Add(sample));

    EXPECT_TRUE(exporter.Export(true));
    ASSERT_EQ(context.results.size(), 3u);
    EXPECT_EQ(context.results.back().result, 0u);
    EXPECT_FALSE(context.results.back().pprof.empty());
    exporter.Cleanup();
  }
}
