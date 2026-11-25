// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <latch>
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
static_assert(
    offsetof(ProfilingData, reserved) == offsetof(ProfilingData, is_last) + 1
);
static_assert(sizeof(ProfilingData{}.reserved) == 7);
static_assert(
    sizeof(ProfilingData) ==
    offsetof(ProfilingData, reserved) + sizeof(ProfilingData{}.reserved)
);

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
  bool reservedBytesAreZero;
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
      data->is_last != 0,
      std::all_of(
          std::begin(data->reserved), std::end(data->reserved), [](uint8_t byte) {
            return byte == 0;
          }
      )
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

struct CoherentCallbackContext {
  int registrationId;
  std::atomic<int> calls{0};
  std::atomic<int> mismatches{0};
};

void CaptureCoherentRegistration(void* userData, int expectedRegistrationId) {
  auto& context = *static_cast<CoherentCallbackContext*>(userData);
  context.calls.fetch_add(1, std::memory_order_relaxed);
  if (context.registrationId != expectedRegistrationId) {
    context.mismatches.fetch_add(1, std::memory_order_relaxed);
  }
}

void DD_WIN_PROF_CALL FirstCoherentCallback(const ProfilingData*, void* userData) {
  CaptureCoherentRegistration(userData, 1);
}

void DD_WIN_PROF_CALL SecondCoherentCallback(const ProfilingData*, void* userData) {
  CaptureCoherentRegistration(userData, 2);
}

ProfilerConfig MakeHttpConfig(const char* version) {
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  config.url = "datadoghq.com";
  config.apiKey = "api-key";
  config.serviceEnvironment = "test";
  config.serviceName = "setup-tests";
  config.serviceVersion = version;
  config.cpuWallTimeSamplingPeriodNs = 11'000'000;
  config.walltimeThreadsThreshold = 7;
  config.cpuThreadsThreshold = 13;
  config.uploadIntervalSeconds = 3600;
  config.tags = "source:setup-tests";
  config.pprofOutputDirectory = "C:\\temp\\setup-tests";
  config.symbolizeCallstacks = true;
  return config;
}

void ExpectSameConfiguration(
    const Configuration& actual, const Configuration& expected
) {
  EXPECT_EQ(actual.GetLogDirectory(), expected.GetLogDirectory());
  EXPECT_EQ(actual.GetProfilesOutputDirectory(), expected.GetProfilesOutputDirectory());
  EXPECT_EQ(actual.GetUploadInterval(), expected.GetUploadInterval());
  EXPECT_EQ(actual.GetUserTags(), expected.GetUserTags());
  EXPECT_EQ(actual.IsDebugLogEnabled(), expected.IsDebugLogEnabled());
  EXPECT_EQ(actual.GetVersion(), expected.GetVersion());
  EXPECT_EQ(actual.GetEnvironment(), expected.GetEnvironment());
  EXPECT_EQ(actual.GetHostname(), expected.GetHostname());
  EXPECT_EQ(actual.GetAgentUrl(), expected.GetAgentUrl());
  EXPECT_EQ(actual.GetAgentHost(), expected.GetAgentHost());
  EXPECT_EQ(actual.GetAgentPort(), expected.GetAgentPort());
  EXPECT_EQ(actual.IsAgentless(), expected.IsAgentless());
  EXPECT_EQ(actual.GetSite(), expected.GetSite());
  EXPECT_EQ(actual.GetApiKey(), expected.GetApiKey());
  EXPECT_EQ(actual.GetServiceName(), expected.GetServiceName());
  EXPECT_EQ(actual.GetNamedPipeName(), expected.GetNamedPipeName());
  EXPECT_EQ(actual.IsProfilerEnabled(), expected.IsProfilerEnabled());
  EXPECT_EQ(
      actual.IsProfilerExplicitlyDisabled(), expected.IsProfilerExplicitlyDisabled()
  );
  EXPECT_EQ(actual.IsProfilerAutoStartEnabled(), expected.IsProfilerAutoStartEnabled());
  EXPECT_EQ(actual.IsCpuProfilingEnabled(), expected.IsCpuProfilingEnabled());
  EXPECT_EQ(actual.IsWallTimeProfilingEnabled(), expected.IsWallTimeProfilingEnabled());
  EXPECT_EQ(
      actual.IsExportEnabled(ProfileDeliveryMode::Http),
      expected.IsExportEnabled(ProfileDeliveryMode::Http)
  );
  EXPECT_EQ(
      actual.IsExportEnabled(ProfileDeliveryMode::Callback),
      expected.IsExportEnabled(ProfileDeliveryMode::Callback)
  );
  EXPECT_EQ(actual.AreCallstacksSymbolized(), expected.AreCallstacksSymbolized());
  EXPECT_EQ(actual.CpuWallTimeSamplingPeriod(), expected.CpuWallTimeSamplingPeriod());
  EXPECT_EQ(actual.WalltimeThreadsThreshold(), expected.WalltimeThreadsThreshold());
  EXPECT_EQ(actual.CpuThreadsThreshold(), expected.CpuThreadsThreshold());
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

void DD_WIN_PROF_CALL BlockingFinalCallback(const ProfilingData* data, void* userData) {
  CaptureCallback(data, userData);
  if (data->is_last != 0) {
    auto& context = *static_cast<CallbackContext*>(userData);
    context.entered->set_value();
    context.release->wait();
  }
}

struct ReentrantCallsContext {
  bool insideCallback = false;
  bool otherThreadInsideCallback = true;
  bool setupRejected = false;
  bool registrationRejected = false;
  bool startRejected = false;
  bool monitorRejected = false;
  bool stopReturned = false;
  std::promise<void>* periodicDone = nullptr;
};

void CallLifecycleApis(ReentrantCallsContext& context) {
  context.insideCallback = ProfileExporter::IsInsideProfilingCallback();
  context.otherThreadInsideCallback =
      std::async(std::launch::async, [] {
        return ProfileExporter::IsInsideProfilingCallback();
      }).get();
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  context.setupRejected = !SetupProfiler(&config);
  context.registrationRejected = !SetupProfilingDataCallback(nullptr, nullptr);
  context.startRejected = !StartProfiler();
  context.monitorRejected = !MonitorWindowHangs(nullptr);
  StopProfiler();
  context.stopReturned = true;
}

void ExpectLifecycleCallsRejected(const ReentrantCallsContext& context) {
  EXPECT_TRUE(context.insideCallback);
  EXPECT_FALSE(context.otherThreadInsideCallback);
  EXPECT_TRUE(context.setupRejected);
  EXPECT_TRUE(context.registrationRejected);
  EXPECT_TRUE(context.startRejected);
  EXPECT_TRUE(context.monitorRejected);
  EXPECT_TRUE(context.stopReturned);
}

void DD_WIN_PROF_CALL
ReentrantFinalCallback(const ProfilingData* data, void* userData) {
  if (data->is_last != 0) {
    CallLifecycleApis(*static_cast<ReentrantCallsContext*>(userData));
  }
}

void DD_WIN_PROF_CALL
ReentrantPeriodicCallback(const ProfilingData* data, void* userData) {
  auto& context = *static_cast<ReentrantCallsContext*>(userData);
  if (data->is_last == 0 && context.periodicDone != nullptr) {
    CallLifecycleApis(context);
    std::exchange(context.periodicDone, nullptr)->set_value();
  }
}

struct CallbackFlagContext {
  bool insideCallback = false;
};

void DD_WIN_PROF_CALL ThrowingCallback(const ProfilingData*, void* userData) {
  static_cast<CallbackFlagContext*>(userData)->insideCallback =
      ProfileExporter::IsInsideProfilingCallback();
  throw std::runtime_error("callback failure");
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

  // Holds the lifecycle lock as an in-progress StartProfiling or StopProfiling would.
  std::unique_lock<std::mutex> LockLifecycle() {
    return std::unique_lock<std::mutex>(profiler->_lifecycleMutex);
  }
  void SetStarted(bool started) { profiler->_isStarted = started; }

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

TEST_F(
    ProfilingDataCallbackTests, ConcurrentReplacementAndStartUseOneCoherentRegistration
) {
  CoherentCallbackContext first{1};
  CoherentCallbackContext second{2};
  ASSERT_TRUE(SetupProfilingDataCallback(FirstCoherentCallback, &first));
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  config.uploadIntervalSeconds = 3600;
  ASSERT_TRUE(SetupProfiler(&config));

  std::latch ready(2);
  std::latch go(1);
  auto replacement = std::async(std::launch::async, [&] {
    ready.count_down();
    go.wait();
    return SetupProfilingDataCallback(SecondCoherentCallback, &second);
  });
  auto start = std::async(std::launch::async, [&] {
    ready.count_down();
    go.wait();
    return StartProfiler();
  });
  ready.wait();
  go.count_down();

  const bool replacementSucceeded = replacement.get();
  ASSERT_TRUE(start.get());
  StopProfiler();

  EXPECT_EQ(first.mismatches.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(second.mismatches.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(
      first.calls.load(std::memory_order_relaxed) +
          second.calls.load(std::memory_order_relaxed),
      1
  );
  EXPECT_EQ(
      replacementSucceeded ? second.calls.load(std::memory_order_relaxed)
                           : first.calls.load(std::memory_order_relaxed),
      1
  );

  EXPECT_TRUE(SetupProfilingDataCallback(SecondCoherentCallback, &second));
  EXPECT_TRUE(SetupProfilingDataCallback(nullptr, nullptr));
}

TEST_F(ProfilingDataCallbackTests, FailedSetupPreservesConfigurationAndCanRetry) {
  auto committedSettings = MakeHttpConfig("committed-version");
  ASSERT_TRUE(SetupProfiler(&committedSettings));
  const Configuration committed = *Profiler::GetConfiguration();

  auto invalidSettings = MakeHttpConfig("invalid-version");
  invalidSettings.apiKey = nullptr;
  EXPECT_FALSE(SetupProfiler(&invalidSettings));
  ExpectSameConfiguration(*Profiler::GetConfiguration(), committed);

  auto retrySettings = MakeHttpConfig("retry-version");
  ASSERT_TRUE(SetupProfiler(&retrySettings));
  EXPECT_EQ(Profiler::GetConfiguration()->GetVersion(), "retry-version");
}

TEST_F(ProfilingDataCallbackTests, SetupWaitsForLifecycleAndIsRejectedOnceStarted) {
  auto committedSettings = MakeHttpConfig("committed-version");
  ASSERT_TRUE(SetupProfiler(&committedSettings));
  auto replacementSettings = MakeHttpConfig("replacement-version");

  auto lifecycle = LockLifecycle();
  auto setup = std::async(std::launch::async, [&] {
    return SetupProfiler(&replacementSettings);
  });
  EXPECT_EQ(
      setup.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout
  );
  SetStarted(true);
  lifecycle.unlock();
  EXPECT_FALSE(setup.get());
  SetStarted(false);
  EXPECT_EQ(Profiler::GetConfiguration()->GetVersion(), "committed-version");

  ASSERT_TRUE(SetupProfiler(&replacementSettings));
  EXPECT_EQ(Profiler::GetConfiguration()->GetVersion(), "replacement-version");
}

TEST_F(
    ProfilingDataCallbackTests, RegistrationWaitsForLifecycleAndCommitsWhileStopped
) {
  int first = 0;
  int second = 0;
  ASSERT_TRUE(SetupProfilingDataCallback(CountCallback, &first));

  auto lifecycle = LockLifecycle();
  auto registration = std::async(std::launch::async, [&] {
    return SetupProfilingDataCallback(CountCallback, &second);
  });
  EXPECT_EQ(
      registration.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout
  );
  lifecycle.unlock();
  EXPECT_TRUE(registration.get());
  EXPECT_EQ(profiler->GetProfilingDataCallback(), CountCallback);
  EXPECT_EQ(profiler->GetProfilingDataCallbackUserData(), &second);

  SetStarted(true);
  EXPECT_FALSE(SetupProfilingDataCallback(nullptr, nullptr));
  SetStarted(false);
  EXPECT_EQ(profiler->GetProfilingDataCallbackUserData(), &second);
  EXPECT_TRUE(SetupProfilingDataCallback(nullptr, nullptr));
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

TEST_F(ProfilingDataCallbackTests, UnregisterAfterCallbackOnlySetupRejectsStart) {
  int calls = 0;
  ASSERT_TRUE(SetupProfilingDataCallback(CountCallback, &calls));
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  config.uploadIntervalSeconds = 3600;
  ASSERT_TRUE(SetupProfiler(&config));

  ASSERT_TRUE(SetupProfilingDataCallback(nullptr, nullptr));
  EXPECT_FALSE(StartProfiler());
  EXPECT_FALSE(profiler->IsStarted());

  ASSERT_TRUE(SetupProfilingDataCallback(CountCallback, &calls));
  ASSERT_TRUE(StartProfiler());
  StopProfiler();
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(SetupProfilingDataCallback(nullptr, nullptr));
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
  EXPECT_TRUE(data.reservedBytesAreZero);
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
    if (cycle == 0) {
      ASSERT_TRUE(exporter.Export());
    } else {
      ASSERT_EQ(exporter.ExportFinal(), FinalExportStatus::Delivered);
    }
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
    EXPECT_TRUE(context.results.front().reservedBytesAreZero);
    const char* explanation = GetProfilingErrorMessage(context.results.front().result);
    EXPECT_STRNE(explanation, "Unknown profiling error");
    std::vector<RumViewRecord> remainingViews;
    std::vector<RumSessionRecord> remainingSessions;
    profiler->ConsumeViewRecords(remainingViews);
    profiler->ConsumeSessionRecords(remainingSessions);
    EXPECT_TRUE(remainingViews.empty());
    EXPECT_TRUE(remainingSessions.empty());
    EXPECT_TRUE(exporter.Export());
    ASSERT_EQ(context.results.size(), 2u);
    EXPECT_EQ(context.results.back().result, 0u);
    EXPECT_FALSE(context.results.back().pprof.empty());
    EXPECT_FALSE(context.results.back().isLast);
    EXPECT_TRUE(context.results.back().reservedBytesAreZero);
    encodingFailure.serialize = code == ProfilingErrorCode::SerializationFailed;
    encodingFailure.bytes = code == ProfilingErrorCode::ProfileBytesUnavailable;
    EXPECT_EQ(exporter.ExportFinal(), FinalExportStatus::ProfileBuildFailed);
    ASSERT_EQ(context.results.size(), 3u);
    EXPECT_EQ(context.results.back().result, ToResult(code));
    EXPECT_TRUE(context.results.back().pprof.empty());
    EXPECT_TRUE(context.results.back().isLast);
    EXPECT_TRUE(context.results.back().reservedBytesAreZero);
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
  EXPECT_FALSE(exporter.Export());
  EXPECT_EQ(encodingFailure.bytesCalls, 0);
  EXPECT_EQ(encodingFailure.resetCalls, 1);
  EXPECT_TRUE(IsExportSuccessful(exporter.ExportFinal()));
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

// StopProfiler holds the lifecycle lock on the thread running the final callback: any
// lifecycle call that took the lock from there would deadlock.
TEST_F(ProfilingDataCallbackTests, LifecycleCallsFailFromFinalCallback) {
  ReentrantCallsContext context;
  ASSERT_TRUE(SetupProfilingDataCallback(ReentrantFinalCallback, &context));
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  config.uploadIntervalSeconds = 3600;
  config.serviceVersion = "committed-version";
  ASSERT_TRUE(SetupProfiler(&config));
  ASSERT_TRUE(StartProfiler());

  StopProfiler();

  ExpectLifecycleCallsRejected(context);
  EXPECT_FALSE(profiler->IsStarted());
  EXPECT_FALSE(ProfileExporter::IsInsideProfilingCallback());
  EXPECT_EQ(profiler->GetProfilingDataCallback(), ReentrantFinalCallback);
  EXPECT_EQ(Profiler::GetConfiguration()->GetVersion(), "committed-version");
  EXPECT_TRUE(SetupProfilingDataCallback(nullptr, nullptr));
}

// StopProfiler called from a periodic callback would wait for its own exporter thread.
TEST_F(ProfilingDataCallbackTests, LifecycleCallsFailFromPeriodicCallback) {
  std::promise<void> periodicDone;
  auto done = periodicDone.get_future();
  ReentrantCallsContext context;
  context.periodicDone = &periodicDone;
  ASSERT_TRUE(SetupProfilingDataCallback(ReentrantPeriodicCallback, &context));
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  config.uploadIntervalSeconds = 1;
  ASSERT_TRUE(SetupProfiler(&config));
  ASSERT_TRUE(StartProfiler());

  const auto status = done.wait_for(std::chrono::seconds(5));
  EXPECT_EQ(status, std::future_status::ready);
  if (status == std::future_status::ready) {
    ExpectLifecycleCallsRejected(context);
    EXPECT_TRUE(profiler->IsStarted());
  }

  StopProfiler();
  EXPECT_FALSE(profiler->IsStarted());
  EXPECT_EQ(profiler->GetProfilingDataCallback(), ReentrantPeriodicCallback);
  EXPECT_TRUE(SetupProfilingDataCallback(nullptr, nullptr));
}

TEST_F(ProfilingDataCallbackTests, OtherThreadsAreRejectedWhileRunningAndWaitForStop) {
  std::promise<void> finalEntered;
  auto entered = finalEntered.get_future();
  std::promise<void> releaseFinal;
  auto released = releaseFinal.get_future().share();
  CallbackContext context;
  context.entered = &finalEntered;
  context.release = &released;
  ASSERT_TRUE(SetupProfilingDataCallback(BlockingFinalCallback, &context));
  ProfilerConfig config{};
  config.size = sizeof(config);
  config.noEnvVars = true;
  config.uploadIntervalSeconds = 3600;
  config.serviceVersion = "committed-version";
  ASSERT_TRUE(SetupProfiler(&config));
  ASSERT_TRUE(StartProfiler());

  auto replacement = config;
  replacement.serviceVersion = "replacement-version";
  EXPECT_FALSE(SetupProfiler(&replacement));
  EXPECT_FALSE(SetupProfilingDataCallback(CountCallback, nullptr));

  auto stopped = std::async(std::launch::async, [] { StopProfiler(); });
  const auto status = entered.wait_for(std::chrono::seconds(5));
  EXPECT_EQ(status, std::future_status::ready);
  auto setup =
      std::async(std::launch::async, [&] { return SetupProfiler(&replacement); });
  auto registration = std::async(std::launch::async, [] {
    return SetupProfilingDataCallback(CountCallback, nullptr);
  });
  if (status == std::future_status::ready) {
    EXPECT_EQ(
        setup.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout
    );
    EXPECT_EQ(
        registration.wait_for(std::chrono::milliseconds(10)),
        std::future_status::timeout
    );
  }
  releaseFinal.set_value();
  stopped.get();

  EXPECT_TRUE(setup.get());
  EXPECT_TRUE(registration.get());
  EXPECT_EQ(Profiler::GetConfiguration()->GetVersion(), "replacement-version");
  EXPECT_EQ(profiler->GetProfilingDataCallback(), CountCallback);
  EXPECT_TRUE(SetupProfilingDataCallback(nullptr, nullptr));
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
    ASSERT_EQ(exporter.ExportFinal(), FinalExportStatus::Delivered);
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

TEST(ProfilingDataExportTests, CallbackFlagIsSetOnlyDuringCallbackEvenWhenItThrows) {
  Configuration config;
  config.ResetToDefaults();
  CallbackFlagContext context;
  std::vector<SampleValueType> types{{"cpu-time", "nanoseconds"}};
  ProfileExporter exporter(&config, types, nullptr, ThrowingCallback, &context);
  ASSERT_TRUE(exporter.Initialize());

  EXPECT_FALSE(ProfileExporter::IsInsideProfilingCallback());
  EXPECT_FALSE(exporter.Export());
  EXPECT_TRUE(context.insideCallback);
  EXPECT_FALSE(ProfileExporter::IsInsideProfilingCallback());

  context.insideCallback = false;
  EXPECT_EQ(exporter.ExportFinal(), FinalExportStatus::CallbackFailed);
  EXPECT_TRUE(context.insideCallback);
  EXPECT_FALSE(ProfileExporter::IsInsideProfilingCallback());
  exporter.Cleanup();
}

TEST(ProfilingDataExportTests, ResetErrorIsDeliveredAndNextExportRecovers) {
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
  EXPECT_FALSE(exporter.Export());
  ASSERT_EQ(context.results.size(), 2u);
  EXPECT_EQ(context.results.front().result, 0u);
  EXPECT_FALSE(context.results.front().isLast);
  EXPECT_FALSE(context.results.front().pprof.empty());
  EXPECT_TRUE(context.results.front().reservedBytesAreZero);
  EXPECT_EQ(
      context.results.back().result, ToResult(ProfilingErrorCode::ProfileResetFailed)
  );
  EXPECT_FALSE(context.results.back().isLast);
  EXPECT_TRUE(context.results.back().pprof.empty());
  EXPECT_TRUE(context.results.back().reservedBytesAreZero);
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

  // The final export seals the exporter instead of resetting the profile, so a
  // reset failure cannot follow the last profile.
  encodingFailure.reset = true;
  EXPECT_EQ(exporter.ExportFinal(), FinalExportStatus::Delivered);
  EXPECT_EQ(encodingFailure.resetCalls, 1);
  ASSERT_EQ(context.results.size(), 3u);
  EXPECT_EQ(context.results.back().result, 0u);
  EXPECT_TRUE(context.results.back().isLast);
  EXPECT_FALSE(context.results.back().pprof.empty());
  exporter.Cleanup();
}
