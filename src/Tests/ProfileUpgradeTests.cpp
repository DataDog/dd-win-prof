// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <filesystem>
#include <fstream>
#include <thread>

#include "CpuTimeProvider.h"
#include "LibDatadogHelper.h"
#include "PprofAggregator.h"
#include "ProfileExporter.h"
#include "WalltimeProvider.h"
#include "pch.h"

using namespace dd_win_prof;

namespace {
constexpr int64_t kStartMs = 1700000000000;
constexpr int64_t kTimestampNs = (kStartMs + 100) * 1000000;

std::string Environment(const char* name) {
  auto size = GetEnvironmentVariableA(name, nullptr, 0);
  if (size == 0) return {};
  std::string value(size, '\0');
  size = GetEnvironmentVariableA(name, value.data(), size);
  value.resize(size);
  return value;
}

void WriteFixture(const std::string& name, const uint8_t* bytes, size_t size) {
  const auto directory = Environment("DD_WIN_PROF_UPGRADE_FIXTURE_DIR");
  if (directory.empty()) return;
  std::filesystem::create_directories(directory);
  std::ofstream output(std::filesystem::path(directory) / name, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes), size);
  ASSERT_TRUE(output.good());
}

std::vector<SampleValueType> LegacyTypes() {
  SampleValueTypeProvider registry;
  CpuTimeProvider cpu(registry);
  WallTimeProvider wall(registry);
  // Freeze the legacy four-column fixture for strict v19 upgrade replay.
  EXPECT_EQ(cpu.GetValueOffsets(), (std::vector<size_t>{0, 1}));
  EXPECT_EQ(wall.GetValueOffsets(), (std::vector<size_t>{2, 3}));
  return registry.GetValueTypes();
}

class ProfileUpgradeTest : public ::testing::Test {
 protected:
  void SetUp() override { previousValuesCount = Sample::ValuesCount; }
  void TearDown() override { Sample::SetValuesCount(previousValuesCount); }
  size_t previousValuesCount;

  void AddInternedSample(PprofAggregator& aggregator, int cycle) {
    auto* profile = aggregator.GetProfile();
    auto name = ddog_prof_Profile_intern_string(profile, to_CharSlice("UpgradeFrame"));
    ASSERT_EQ(name.tag, DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID);
    auto function = ddog_prof_Profile_intern_function(
        profile,
        name.ok,
        ddog_prof_Profile_interned_empty_string(),
        ddog_prof_Profile_interned_empty_string()
    );
    ASSERT_EQ(
        function.tag, DDOG_PROF_FUNCTION_ID_RESULT_OK_GENERATIONAL_ID_FUNCTION_ID
    );
    auto location = ddog_prof_Profile_intern_location(profile, function.ok, 0x1234, 42);
    ASSERT_EQ(
        location.tag, DDOG_PROF_LOCATION_ID_RESULT_OK_GENERATIONAL_ID_LOCATION_ID
    );
    auto key = ddog_prof_Profile_intern_string(profile, to_CharSlice("rum.view_id"));
    auto value = ddog_prof_Profile_intern_string(profile, to_CharSlice("upgrade-view"));
    ASSERT_EQ(key.tag, DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID);
    ASSERT_EQ(value.tag, DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID);
    auto label = ddog_prof_Profile_intern_label_str(profile, key.ok, value.ok);
    ASSERT_EQ(label.tag, DDOG_PROF_LABEL_ID_RESULT_OK_GENERATIONAL_ID_LABEL_ID);
    auto labels = ddog_prof_Profile_intern_labelset(profile, {&label.ok, 1});
    ASSERT_EQ(
        labels.tag, DDOG_PROF_LABEL_SET_ID_RESULT_OK_GENERATIONAL_ID_LABEL_SET_ID
    );
    std::vector<int64_t> values{
        1000000 * cycle, cycle, 2000000 * cycle, 3000000 * cycle
    };
    ASSERT_TRUE(aggregator.AddSample(
        std::span(&location.ok, 1), values, kTimestampNs + cycle, labels.ok
    ));
  }

  std::shared_ptr<Sample> MakeSample(UiHangSampleKind kind, int cycle) {
    uint64_t frames[] = {0x1234};
    auto sample = std::make_shared<Sample>(
        std::chrono::nanoseconds(kTimestampNs + cycle + static_cast<int>(kind)),
        nullptr,
        frames,
        1
    );
    if (kind == UiHangSampleKind::None) {
      sample->AddValue(1000000 * cycle, 0);
      sample->AddValue(cycle, 1);
      sample->AddValue(2000000 * cycle, 2);
      sample->AddValue(3000000 * cycle, 3);
    } else {
      sample->AddValue(4000000 * cycle, 3);
    }
    sample->SetUiHangSampleKind(kind);
    sample->SetRumViewContext({"upgrade-view", "Upgrade view"});
    return sample;
  }
};

TEST_F(ProfileUpgradeTest, SerializedWaitTypeSurvivesReset) {
  const auto types = LegacyTypes();
  ASSERT_EQ(types.size(), 4u);
  auto storage = ddog_prof_ManagedStringStorage_new();
  ASSERT_EQ(storage.tag, DDOG_PROF_MANAGED_STRING_STORAGE_NEW_RESULT_OK);
  {
    PprofAggregator aggregator(types, storage.ok, 10);
    ASSERT_TRUE(aggregator.IsInitialized());
    for (int cycle = 1; cycle <= 2; ++cycle) {
      AddInternedSample(aggregator, cycle);
      auto* encoded = aggregator.Serialize(kStartMs, kStartMs + 1000);
      ASSERT_NE(encoded, nullptr);
      auto bytes = ddog_prof_EncodedProfile_bytes(encoded);
      ASSERT_EQ(bytes.tag, DDOG_PROF_RESULT_BYTE_SLICE_OK_BYTE_SLICE);
      EXPECT_GT(bytes.ok.len, 0u);
      WriteFixture(
          "direct-" + std::to_string(cycle) + ".pprof", bytes.ok.ptr, bytes.ok.len
      );
      ddog_prof_EncodedProfile_drop(encoded);
      delete encoded;
      ASSERT_TRUE(aggregator.Reset());
    }
  }
  ddog_prof_ManagedStringStorage_drop(storage.ok);
}

TEST_F(ProfileUpgradeTest, CustomPeriodPreservesWirePair) {
  std::vector<SampleValueType> types{{"wait-time", "nanoseconds"}};
  auto storage = ddog_prof_ManagedStringStorage_new();
  ASSERT_EQ(storage.tag, DDOG_PROF_MANAGED_STRING_STORAGE_NEW_RESULT_OK);
  {
    PprofAggregator aggregator(types, storage.ok, 10);
    ASSERT_TRUE(aggregator.IsInitialized());
    auto* encoded = aggregator.Serialize(kStartMs, kStartMs + 1000);
    ASSERT_NE(encoded, nullptr);
    auto bytes = ddog_prof_EncodedProfile_bytes(encoded);
    ASSERT_EQ(bytes.tag, DDOG_PROF_RESULT_BYTE_SLICE_OK_BYTE_SLICE);
    WriteFixture("custom-period.pprof", bytes.ok.ptr, bytes.ok.len);
    ddog_prof_EncodedProfile_drop(encoded);
    delete encoded;
  }
  ddog_prof_ManagedStringStorage_drop(storage.ok);
}

TEST_F(ProfileUpgradeTest, CallbackRetainsWaitAndHangValuesAcrossExports) {
  const auto types = LegacyTypes();
  Sample::SetValuesCount(types.size());
  Configuration config;
  config.ResetToDefaults();
  config.SetExportEnabled(false);
  std::vector<ProfilingData> profiles;
  {
    ProfileExporter exporter(&config, types, nullptr, [&](ProfilingData data) {
      profiles.push_back(std::move(data));
    });
    ASSERT_TRUE(exporter.Initialize());
    for (int cycle = 1; cycle <= 2; ++cycle) {
      ASSERT_TRUE(exporter.Add(MakeSample(UiHangSampleKind::None, cycle)));
      ASSERT_TRUE(exporter.Add(MakeSample(UiHangSampleKind::Detected, cycle)));
      ASSERT_TRUE(exporter.Add(MakeSample(UiHangSampleKind::Recovered, cycle)));
      ASSERT_TRUE(exporter.Export(cycle == 2));
    }
    exporter.Cleanup();
  }
  ASSERT_EQ(profiles.size(), 2u);
  for (size_t i = 0; i < profiles.size(); ++i) {
    const auto& data = profiles[i];
    EXPECT_EQ(data.result, 0u);
    EXPECT_EQ(data.isLast, i == 1);
    EXPECT_LE(data.startUnixTimeMs, data.endUnixTimeMs);
    EXPECT_FALSE(data.pprof.empty());
    WriteFixture(
        "callback-" + std::to_string(i + 1) + ".pprof",
        data.pprof.data(),
        data.pprof.size()
    );
  }
}

class UpgradeRumRecords : public IRumRecordProvider {
 public:
  void ConsumeViewRecords(std::vector<RumViewRecord>& records) override {
    records = {{kStartMs, 1000, "upgrade-view", "Upgrade view", {1000000, 3000000}}};
  }
  void ConsumeSessionRecords(std::vector<RumSessionRecord>& records) override {
    records = {{kStartMs, 1000, "upgrade-session"}};
  }
  std::string GetCurrentSessionId() const override { return "upgrade-session"; }
};

TEST_F(ProfileUpgradeTest, HttpRetainsWaitAndHangValuesAcrossExports) {
  const auto url = Environment("DD_WIN_PROF_UPGRADE_AGENT_URL");
  if (url.empty())
    GTEST_SKIP() << "Run validate_libdatadog_upgrade.py for local HTTP coverage";
  const auto types = LegacyTypes();
  Sample::SetValuesCount(types.size());
  Configuration config;
  ASSERT_EQ(config.GetAgentUrl(), url);
  config.SetApiKey("");
  config.SetExportEnabled(true);
  config.SetProfilesOutputDirectory({});
  config.SetServiceName("libdatadog-upgrade-test");
  UpgradeRumRecords records;
  ProfileExporter exporter(&config, types, &records);
  ASSERT_TRUE(exporter.Initialize());
  auto send = [&](int cycle) {
    ASSERT_TRUE(exporter.Add(MakeSample(UiHangSampleKind::None, cycle)));
    ASSERT_TRUE(exporter.Add(MakeSample(UiHangSampleKind::Detected, cycle)));
    ASSERT_TRUE(exporter.Add(MakeSample(UiHangSampleKind::Recovered, cycle)));
    ASSERT_TRUE(exporter.Export(cycle == 2));
  };
  std::thread periodic([&] { send(1); });
  periodic.join();
  send(2);
  exporter.Cleanup();
}

TEST_F(ProfileUpgradeTest, HttpResponseAndTransportPolicy) {
  const auto mode = Environment("DD_WIN_PROF_UPGRADE_HTTP_MODE");
  if (mode.empty()) GTEST_SKIP() << "Run validate_libdatadog_upgrade.py";
  const auto types = LegacyTypes();
  Sample::SetValuesCount(types.size());
  Configuration config;
  config.SetApiKey("");
  config.SetExportEnabled(true);
  config.SetProfilesOutputDirectory({});
  ProfileExporter exporter(&config, types);
  ASSERT_TRUE(exporter.Initialize());
  auto send = [&](int cycle, bool expected) {
    ASSERT_TRUE(exporter.Add(MakeSample(UiHangSampleKind::None, cycle)));
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(exporter.Export(cycle == 3), expected);
    if (mode == "timeout" && cycle == 1) {
      const auto elapsed = std::chrono::steady_clock::now() - start;
      EXPECT_GE(elapsed, std::chrono::seconds(8));
      EXPECT_LT(elapsed, std::chrono::seconds(14));
    }
  };
  std::thread periodic([&] {
    send(1, mode != "403" && mode != "404");
    send(2, mode != "403" && mode != "404");
  });
  periodic.join();
  send(3, mode != "403" && mode != "404" && mode != "refused");
  exporter.Cleanup();
}
}  // namespace
