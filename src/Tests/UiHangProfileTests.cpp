// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <thread>

#include "CpuTimeProvider.h"
#include "LocalHttpServer.h"
#include "ProfileExporter.h"
#include "ProfileTestOutput.h"
#include "UiHangProvider.h"
#include "WalltimeProvider.h"
#include "pch.h"

using namespace dd_win_prof;

namespace {
constexpr int64_t kStartNs = 1700000000000000000;
constexpr int64_t kDetectionNs = kStartNs + 100000000;

class UiHangProfileTest : public ::testing::Test {
 protected:
  SampleValueTypeProvider registry;
  CpuTimeProvider cpu{registry};
  WallTimeProvider wall{registry};
  UiHangProvider hang{registry};
  size_t previousValuesCount;
  std::shared_ptr<ThreadInfo> thread;

  void SetUp() override {
    previousValuesCount = Sample::ValuesCount;
    ASSERT_EQ(cpu.GetValueOffsets(), (std::vector<size_t>{0, 1}));
    ASSERT_EQ(wall.GetValueOffsets(), (std::vector<size_t>{2, 3}));
    ASSERT_EQ(hang.GetValueOffsets(), (std::vector<size_t>{4}));
    ASSERT_EQ(registry.GetValueTypes().size(), 5u);
    EXPECT_EQ(registry.GetValueTypes()[4].Name, "ui-hang-time");
    EXPECT_EQ(registry.GetValueTypes()[4].Unit, "nanoseconds");
    Sample::SetValuesCount(registry.GetValueTypes().size());
    HANDLE handle = nullptr;
    ASSERT_TRUE(DuplicateHandle(
        GetCurrentProcess(),
        GetCurrentThread(),
        GetCurrentProcess(),
        &handle,
        0,
        FALSE,
        DUPLICATE_SAME_ACCESS
    ));
    ASSERT_TRUE(SUCCEEDED(SetThreadDescription(handle, L"HangFixture_Main")));
    thread = std::make_shared<ThreadInfo>(GetCurrentThreadId(), handle);
  }

  void TearDown() override {
    SetThreadDescription(GetCurrentThread(), L"");
    Sample::SetValuesCount(previousValuesCount);
  }

  Sample MakeSample(int64_t timestamp, uint64_t address, const char* view) {
    Sample sample(std::chrono::nanoseconds(timestamp), thread, &address, 1);
    sample.SetRumViewContext({view, view});
    return sample;
  }

  std::shared_ptr<Sample> Drain(CollectorBase& provider) {
    std::vector<Sample> samples;
    EXPECT_EQ(provider.MoveSamples(samples), 1u);
    if (samples.size() != 1) return nullptr;
    return std::make_shared<Sample>(std::move(samples[0]));
  }

  std::shared_ptr<Sample> Ordinary(int cycle, bool isCpu) {
    auto sample = MakeSample(kStartNs + cycle, 0x1234, "ordinary-view");
    if (isCpu) {
      cpu.Add(std::move(sample), std::chrono::nanoseconds(1000000 * cycle));
      return Drain(cpu);
    }
    wall.Add(
        std::move(sample),
        std::chrono::nanoseconds(2000000 * cycle),
        std::chrono::nanoseconds(3000000 * cycle),
        0
    );
    return Drain(wall);
  }

  std::shared_ptr<Sample> Endpoint(int cycle, bool detected) {
    auto sample = MakeSample(
        (detected ? kDetectionNs : kDetectionNs + 1410000000LL * cycle) + cycle,
        detected ? 0x2345 : 0x3456,
        detected ? "detected-view" : "recovered-view"
    );
    hang.Add(
        std::move(sample),
        std::chrono::nanoseconds((detected ? 32000000 : 1410000000LL) * cycle),
        detected
    );
    return Drain(hang);
  }

  void ExpectValues(
      const std::shared_ptr<Sample>& sample, const std::vector<int64_t>& expected
  ) {
    ASSERT_NE(sample, nullptr);
    auto values = sample->GetValues();
    EXPECT_EQ(std::vector<int64_t>(values.begin(), values.end()), expected);
  }

  void AddCycle(ProfileExporter& exporter, int cycle) {
    ASSERT_TRUE(exporter.Add(Ordinary(cycle, true)));
    ASSERT_TRUE(exporter.Add(Endpoint(cycle, true)));
    ASSERT_TRUE(exporter.Add(Ordinary(cycle, false)));
    ASSERT_TRUE(exporter.Add(Endpoint(cycle, false)));
  }
};

TEST_F(UiHangProfileTest, ProvidersKeepFullVectorsAndPhaseContextWhenInterleaved) {
  for (int cycle = 1; cycle <= 2; ++cycle) {
    ExpectValues(Ordinary(cycle, true), {1000000 * cycle, 1, 0, 0, 0});
    auto detected = Endpoint(cycle, true);
    ExpectValues(detected, {0, 0, 0, 0, 32000000 * cycle});
    ASSERT_NE(detected, nullptr);
    EXPECT_EQ(detected->GetUiHangSampleKind(), UiHangSampleKind::Detected);
    EXPECT_EQ(detected->GetTimestamp().count(), kDetectionNs + cycle);
    EXPECT_EQ(detected->GetFrames()[0], 0x2345);
    EXPECT_EQ(detected->GetThreadInfo(), thread);
    EXPECT_EQ(detected->GetRumViewContext().view_id, "detected-view");
    ExpectValues(Ordinary(cycle, false), {0, 0, 2000000 * cycle, 3000000 * cycle, 0});
    auto recovered = Endpoint(cycle, false);
    ExpectValues(recovered, {0, 0, 0, 0, 1410000000LL * cycle});
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->GetUiHangSampleKind(), UiHangSampleKind::Recovered);
    EXPECT_EQ(
        recovered->GetTimestamp().count(), kDetectionNs + 1410000000LL * cycle + cycle
    );
    EXPECT_EQ(recovered->GetFrames()[0], 0x3456);
    EXPECT_EQ(recovered->GetThreadInfo(), thread);
    EXPECT_EQ(recovered->GetRumViewContext().view_id, "recovered-view");
  }
  std::vector<Sample> empty;
  EXPECT_EQ(hang.MoveSamples(empty), 0u);
}

TEST_F(UiHangProfileTest, CallbackKeepsWaitAndHangCustomSlotsAcrossTwoFlushes) {
  Configuration config;
  config.ResetToDefaults();
  config.SetExportEnabled(false);
  std::vector<ProfilingData> profiles;
  ProfileExporter exporter(
      &config, registry.GetValueTypes(), nullptr, [&](ProfilingData data) {
        profiles.push_back(std::move(data));
      }
  );
  ASSERT_TRUE(exporter.Initialize());
  for (int cycle = 1; cycle <= 2; ++cycle) {
    AddCycle(exporter, cycle);
    ASSERT_TRUE(exporter.Export(cycle == 2));
  }
  exporter.Cleanup();
  ASSERT_EQ(profiles.size(), 2u);
  for (size_t i = 0; i < profiles.size(); ++i) {
    EXPECT_EQ(profiles[i].result, 0u);
    EXPECT_EQ(profiles[i].isLast, i == 1);
    ASSERT_GE(profiles[i].pprof.size(), 4u);
    EXPECT_EQ(
        (std::vector<uint8_t>(
            profiles[i].pprof.begin(), profiles[i].pprof.begin() + 4
        )),
        (std::vector<uint8_t>{0x28, 0xb5, 0x2f, 0xfd})
    );
    EXPECT_LE(profiles[i].startUnixTimeMs, profiles[i].endUnixTimeMs);
    WriteTestProfile("hang-" + std::to_string(i + 1) + ".pprof", profiles[i].pprof);
  }
}

TEST_F(UiHangProfileTest, PartialEndpointsAcrossFlushesDoNotInventDurations) {
  Configuration config;
  config.ResetToDefaults();
  config.SetExportEnabled(false);
  std::vector<ProfilingData> profiles;
  ProfileExporter exporter(
      &config, registry.GetValueTypes(), nullptr, [&](ProfilingData data) {
        profiles.push_back(std::move(data));
      }
  );
  ASSERT_TRUE(exporter.Initialize());
  ASSERT_TRUE(exporter.Add(Endpoint(1, true)));
  ASSERT_TRUE(exporter.Export(false));
  ASSERT_TRUE(exporter.Add(Ordinary(1, false)));
  ASSERT_TRUE(exporter.Export(false));
  ASSERT_TRUE(exporter.Add(Endpoint(1, false)));
  ASSERT_TRUE(exporter.Export(true));
  exporter.Cleanup();
  ASSERT_EQ(profiles.size(), 3u);
  for (size_t i = 0; i < profiles.size(); ++i) {
    EXPECT_EQ(profiles[i].result, 0u);
    EXPECT_EQ(profiles[i].isLast, i == profiles.size() - 1);
    EXPECT_FALSE(profiles[i].pprof.empty());
    WriteTestProfile("partial-" + std::to_string(i + 1) + ".pprof", profiles[i].pprof);
  }
}

TEST_F(UiHangProfileTest, HttpKeepsWaitAndHangCustomSlotsAcrossTwoFlushes) {
  LocalHttpServer server;
  ScopedEnvironment agent("DD_TRACE_AGENT_URL", server.Url());
  ScopedEnvironment pipe("DD_TRACE_PIPE_NAME", "");
  Configuration config;
  config.SetApiKey("");
  config.SetExportEnabled(true);
  config.SetProfilesOutputDirectory({});
  config.SetServiceName("ui-hang-profile-test");
  ProfileExporter exporter(&config, registry.GetValueTypes());
  ASSERT_TRUE(exporter.Initialize());
  auto send = [&](int cycle) {
    AddCycle(exporter, cycle);
    ASSERT_TRUE(exporter.Export(cycle == 2));
  };
  std::thread periodic([&] { send(1); });
  periodic.join();
  send(2);
  exporter.Cleanup();
  EXPECT_TRUE(server.Error().empty()) << server.Error();
  const auto requests = server.Requests();
  ASSERT_EQ(requests.size(), 2u);
  for (size_t i = 0; i < requests.size(); ++i) {
    EXPECT_NE(
        requests[i].body.find("profile_seq:" + std::to_string(i)), std::string::npos
    );
    EXPECT_NE(requests[i].body.find("\"family\":\"windows\""), std::string::npos);
    EXPECT_NE(
        requests[i].body.find(std::string("\x28\xb5\x2f\xfd", 4)), std::string::npos
    );
    WriteHttpTestProfile("http-" + std::to_string(i + 1) + ".pprof", requests[i].body);
  }
}
}  // namespace
