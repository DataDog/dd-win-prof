// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <chrono>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <thread>
#include <utility>

#include "Configuration.h"
#include "LibDatadogHelper.h"
#include "ProfileExporter.h"
#include "Profiler.h"
#include "dd-win-rum-private.h"
#include "pch.h"

namespace {
struct EncodingFailureState {
  bool serialize = false;
  bool bytes = false;
  std::string message;
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
  const auto message = ddog_Error_message(&result.err);
  encodingFailure.message.assign(message.ptr, message.len);
  return result.err;
}

ddog_prof_Profile_SerializeResult SerializeWithFailure(
    ddog_prof_Profile* profile, const ddog_Timespec* start, const ddog_Timespec* end
) {
  if (std::exchange(encodingFailure.serialize, false)) {
    ddog_prof_Profile_SerializeResult result{};
    result.tag = DDOG_PROF_PROFILE_SERIALIZE_RESULT_ERR;
    result.err = MakeNativeError();
    return result;
  }
  return ddog_prof_Profile_serialize(profile, start, end);
}

ddog_prof_Result_ByteSlice ReadBytesWithFailure(ddog_prof_EncodedProfile* profile) {
  if (std::exchange(encodingFailure.bytes, false)) {
    ddog_prof_Result_ByteSlice result{};
    result.tag = DDOG_PROF_RESULT_BYTE_SLICE_ERR_BYTE_SLICE;
    result.err = MakeNativeError();
    return result;
  }
  return ddog_prof_EncodedProfile_bytes(profile);
}
}  // namespace

class ProfilingDataCallbackTests : public ::testing::Test {
 protected:
  void SetUp() override {
    previous = *Profiler::GetConfiguration();
    Profiler::GetConfiguration()->ResetToDefaults();
    Profiler::GetConfiguration()->SetProfilingDataCallback({});
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
  ProfilingDataCallback callback = [&](ProfilingDataResult) { ++calls; };
  ASSERT_TRUE(SetupProfilingDataCallback(&callback));
  callback = {};
  Profiler::GetConfiguration()->GetProfilingDataCallback()(ProfilingData{});
  EXPECT_EQ(calls, 1);
}

TEST_F(ProfilingDataCallbackTests, EmptyCallableDoesNotReplaceRegistration) {
  ProfilingDataCallback callback = [](ProfilingDataResult) {};
  ASSERT_TRUE(SetupProfilingDataCallback(&callback));
  ProfilingDataCallback empty;
  EXPECT_FALSE(SetupProfilingDataCallback(&empty));
  EXPECT_TRUE(Profiler::GetConfiguration()->GetProfilingDataCallback());
}

TEST_F(ProfilingDataCallbackTests, FailedCallbackCopyPreservesRegistration) {
  int calls = 0;
  ProfilingDataCallback original = [&](ProfilingDataResult) { ++calls; };
  ASSERT_TRUE(SetupProfilingDataCallback(&original));
  struct ThrowOnCopy {
    bool* fail;
    explicit ThrowOnCopy(bool* flag) : fail(flag) {}
    ThrowOnCopy(const ThrowOnCopy& other) : fail(other.fail) {
      if (*fail) {
        throw std::runtime_error("callback copy failed");
      }
    }
    void operator()(ProfilingDataResult) const {}
  };
  bool fail = false;
  ProfilingDataCallback replacement = ThrowOnCopy(&fail);
  fail = true;
  EXPECT_FALSE(SetupProfilingDataCallback(&replacement));
  Profiler::GetConfiguration()->GetProfilingDataCallback()(ProfilingData{});
  EXPECT_EQ(calls, 1);
}

TEST_F(ProfilingDataCallbackTests, NoHttpSettingsNeededWithNoEnvVars) {
  ProfilingDataCallback callback = [](ProfilingDataResult) {};
  ASSERT_TRUE(SetupProfilingDataCallback(&callback));
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  config.uploadIntervalSeconds = 17;
  ASSERT_TRUE(SetupProfiler(&config));
  EXPECT_EQ(
      Profiler::GetConfiguration()->GetUploadInterval(), std::chrono::seconds(17)
  );
  EXPECT_FALSE(Profiler::GetConfiguration()->IsExportEnabled());
  EXPECT_TRUE(Profiler::GetConfiguration()->GetApiKey().empty());
}

TEST_F(ProfilingDataCallbackTests, UnregisterRestoresHttpRequirements) {
  ProfilingDataCallback callback = [](ProfilingDataResult) {};
  ASSERT_TRUE(SetupProfilingDataCallback(&callback));
  ASSERT_TRUE(SetupProfilingDataCallback(nullptr));
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
  std::vector<ProfilingDataResult> results;
  std::thread::id callbackThread;
  ProfilingDataCallback callback = [&](ProfilingDataResult result) {
    callbackThread = std::this_thread::get_id();
    results.push_back(std::move(result));
  };
  ASSERT_TRUE(SetupProfilingDataCallback(&callback));
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
  EXPECT_FALSE(SetupProfilingDataCallback(nullptr));
  StopProfiler();
  const auto afterStopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch()
  )
                               .count();
  EXPECT_EQ(callbackThread, std::this_thread::get_id());
  ASSERT_EQ(results.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<ProfilingData>(results.front()));
  const auto& data = std::get<ProfilingData>(results.front());
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
  EXPECT_TRUE(SetupProfilingDataCallback(nullptr));
}

TEST_F(ProfilingDataCallbackTests, CallbackDiscardsRumRecordsWithoutMetadataFiles) {
  int calls = 0;
  ProfilingDataCallback callback = [&](ProfilingDataResult result) {
    EXPECT_TRUE(std::holds_alternative<ProfilingData>(result));
    ++calls;
  };
  ASSERT_TRUE(SetupProfilingDataCallback(&callback));
  const auto outputDir =
      fs::temp_directory_path() /
      ("dd-win-prof-callback-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  auto config = Profiler::GetConfiguration();
  config->SetProfilesOutputDirectory(outputDir);
  std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
  ProfileExporter exporter(config, types, profiler.get());
  ASSERT_TRUE(exporter.Initialize()) << exporter.GetLastError();

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

TEST_F(ProfilingDataCallbackTests, EncodingErrorsAreDeliveredAndNextExportRecovers) {
  for (const auto code :
       {ProfilingErrorCode::SerializationFailed,
        ProfilingErrorCode::ProfileBytesUnavailable}) {
    SCOPED_TRACE(static_cast<int>(code));
    encodingFailure = {};
    encodingFailure.serialize = code == ProfilingErrorCode::SerializationFailed;
    encodingFailure.bytes = code == ProfilingErrorCode::ProfileBytesUnavailable;
    std::vector<ProfilingDataResult> results;
    ProfilingDataCallback callback = [&](ProfilingDataResult result) {
      results.push_back(std::move(result));
    };
    ASSERT_TRUE(SetupProfilingDataCallback(&callback));
    const dd_win_prof::ProfileEncodingApi api{
        SerializeWithFailure, ReadBytesWithFailure
    };
    std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
    ProfileExporter exporter(Profiler::GetConfiguration(), types, profiler.get(), api);
    ASSERT_TRUE(exporter.Initialize()) << exporter.GetLastError();
    RumSessionContext session{"app-1", "session-1"};
    RumViewValues view{"view-1", "HomePage"};
    ASSERT_TRUE(SetRumSession(&session));
    ASSERT_TRUE(SetRumView(&view));
    ASSERT_TRUE(SetRumSession(nullptr));

    EXPECT_FALSE(exporter.Export());
    ASSERT_EQ(results.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<ProfilingError>(results.front()));
    const auto& error = std::get<ProfilingError>(results.front());
    EXPECT_EQ(error.code, code);
    ASSERT_FALSE(encodingFailure.message.empty());
    EXPECT_NE(error.message.find(encodingFailure.message), std::string::npos);
    std::vector<RumViewRecord> remainingViews;
    std::vector<RumSessionRecord> remainingSessions;
    profiler->ConsumeViewRecords(remainingViews);
    profiler->ConsumeSessionRecords(remainingSessions);
    EXPECT_TRUE(remainingViews.empty());
    EXPECT_TRUE(remainingSessions.empty());

    EXPECT_TRUE(exporter.Export(true));
    ASSERT_EQ(results.size(), 2u);
    ASSERT_TRUE(std::holds_alternative<ProfilingData>(results.back()));
    EXPECT_FALSE(std::get<ProfilingData>(results.back()).pprof.empty());
    EXPECT_TRUE(std::get<ProfilingData>(results.back()).isLast);
    EXPECT_TRUE(exporter.GetLastError().empty());
    exporter.Cleanup();
  }
}

TEST_F(ProfilingDataCallbackTests, CanReconfigureAndReplaceCallbackAfterStop) {
  int firstCalls = 0;
  int secondCalls = 0;
  ProfilingDataCallback first = [&](ProfilingDataResult) { ++firstCalls; };
  ProfilingDataCallback second = [&](ProfilingDataResult) { ++secondCalls; };
  ASSERT_TRUE(SetupProfilingDataCallback(&first));
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  config.uploadIntervalSeconds = 3600;

  ASSERT_TRUE(SetupProfiler(&config));
  ASSERT_TRUE(StartProfiler());
  StopProfiler();
  EXPECT_EQ(firstCalls, 1);
  StopProfiler();
  EXPECT_EQ(firstCalls, 1);

  ASSERT_TRUE(SetupProfiler(&config));
  ASSERT_TRUE(StartProfiler());
  StopProfiler();
  EXPECT_EQ(firstCalls, 2);

  ASSERT_TRUE(SetupProfilingDataCallback(&second));
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
  auto released = releasePeriodic.get_future();
  std::vector<ProfilingDataResult> results;
  ProfilingDataCallback callback = [&](ProfilingDataResult result) {
    const auto* data = std::get_if<ProfilingData>(&result);
    const bool firstPeriodic = data != nullptr && !data->isLast && results.empty();
    results.push_back(std::move(result));
    if (firstPeriodic) {
      periodicEntered.set_value();
      released.wait();
    }
  };
  ASSERT_TRUE(SetupProfilingDataCallback(&callback));
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
    EXPECT_EQ(
        stopped.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout
    );
  }
  releasePeriodic.set_value();
  stopped.get();

  ASSERT_GE(results.size(), 2u);
  ASSERT_TRUE(std::holds_alternative<ProfilingData>(results.front()));
  ASSERT_TRUE(std::holds_alternative<ProfilingData>(results.back()));
  EXPECT_FALSE(std::get<ProfilingData>(results.front()).isLast);
  EXPECT_TRUE(std::get<ProfilingData>(results.back()).isLast);
}

TEST_F(ProfilingDataCallbackTests, MissingProfilerRejectsRegistration) {
  profiler.reset();
  EXPECT_FALSE(SetupProfilingDataCallback(nullptr));
  profiler = std::make_unique<Profiler>();
}

TEST(ProfilingDataExportTests, CallbackBypassesHttpAndOwnsBytes) {
  Configuration config;
  config.ResetToDefaults();
  config.SetApiKey("unused");
  std::vector<ProfilingDataResult> results;
  config.SetProfilingDataCallback([&](ProfilingDataResult result) {
    results.push_back(std::move(result));
  });
  ASSERT_TRUE(config.ValidateTransportConfig());
  EXPECT_FALSE(config.IsExportEnabled());
  std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
  {
    ProfileExporter exporter(&config, types);
    ASSERT_TRUE(exporter.Initialize()) << exporter.GetLastError();
    ASSERT_TRUE(exporter.Export());
    ASSERT_TRUE(exporter.Export(true));
    exporter.Cleanup();
  }
  ASSERT_EQ(results.size(), 2u);
  ASSERT_TRUE(std::holds_alternative<ProfilingData>(results[0]));
  ASSERT_TRUE(std::holds_alternative<ProfilingData>(results[1]));
  const auto& first = std::get<ProfilingData>(results[0]);
  const auto& second = std::get<ProfilingData>(results[1]);
  EXPECT_FALSE(first.isLast);
  EXPECT_TRUE(second.isLast);
  EXPECT_FALSE(first.pprof.empty());
  EXPECT_FALSE(second.pprof.empty());
  EXPECT_LE(first.startUnixTimeMs, first.endUnixTimeMs);
  EXPECT_EQ(first.endUnixTimeMs, second.startUnixTimeMs);
}

TEST(ProfilingDataExportTests, ThrowingCallbackDoesNotPreventNextExport) {
  Configuration config;
  config.ResetToDefaults();
  int calls = 0;
  config.SetProfilingDataCallback([&](ProfilingDataResult) {
    if (++calls == 1) {
      throw std::runtime_error("queue unavailable");
    }
    if (calls == 2) {
      throw 42;
    }
  });
  std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
  ProfileExporter exporter(&config, types);
  ASSERT_TRUE(exporter.Initialize());
  EXPECT_FALSE(exporter.Export());
  EXPECT_NE(exporter.GetLastError().find("queue unavailable"), std::string::npos);
  EXPECT_FALSE(exporter.Export());
  EXPECT_NE(exporter.GetLastError().find("non-standard"), std::string::npos);
  EXPECT_TRUE(exporter.Export(true));
  EXPECT_TRUE(exporter.GetLastError().empty());
  EXPECT_EQ(calls, 3);
  exporter.Cleanup();
}
