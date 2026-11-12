// Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0
#include "PprofAggregator.h"
#include "ProfileExporter.h"
#include "ProfileTestOutput.h"
#include "pch.h"
using namespace dd_win_prof;
class PprofAggregatorTest : public ::testing::Test {
 protected:
  std::vector<SampleValueType> types{
      {"cpu-time", "nanoseconds"}, {"wall-time", "nanoseconds"}
  };
  pa_id Labels(PprofAggregator& a) {
    pa_id id = 0;
    EXPECT_EQ(pa_intern_labels(a.GetProfile(), nullptr, 0, &id), PA_OK);
    return id;
  }
  std::vector<pa_id> Locations(PprofAggregator& a) {
    pa_id name = 0, function = 0, location = 0;
    EXPECT_EQ(pa_intern_string(a.GetProfile(), "Worker", 6, &name), PA_OK);
    pa_function f{name, name, 0, 42};
    EXPECT_EQ(pa_intern_function(a.GetProfile(), &f, &function), PA_OK);
    pa_location l{0, function, 0x123456789ULL, 43};
    EXPECT_EQ(pa_intern_location(a.GetProfile(), &l, &location), PA_OK);
    return {location};
  }
  bool Record(PprofAggregator& a) {
    auto locations = Locations(a);
    int64_t values[] = {7, 11};
    return a.AddSample(locations, values, 123456789, Labels(a));
  }
  void Encoded(const std::vector<uint8_t>& bytes) {
    ASSERT_GT(bytes.size(), 4u);
    EXPECT_EQ(
        std::vector<uint8_t>(bytes.begin(), bytes.begin() + 4),
        (std::vector<uint8_t>{0x28, 0xb5, 0x2f, 0xfd})
    );
  }
};
TEST_F(PprofAggregatorTest, InitializationWithValidSampleTypes) {
  PprofAggregator a(types);
  EXPECT_TRUE(a.IsInitialized());
}
TEST_F(PprofAggregatorTest, InitializationWithMinimalSampleTypes) {
  std::vector<SampleValueType> t{{"cpu", "ns"}};
  PprofAggregator a(t);
  EXPECT_TRUE(a.IsInitialized());
}
TEST_F(PprofAggregatorTest, InitializationWithEmptySampleTypes) {
  PprofAggregator a({});
  EXPECT_FALSE(a.IsInitialized());
}
TEST_F(PprofAggregatorTest, InitializationWithPeriod) {
  PprofAggregator a(types, 10);
  EXPECT_TRUE(a.IsInitialized());
}
TEST_F(PprofAggregatorTest, AddSampleWithRealLocationIds) {
  PprofAggregator a(types);
  EXPECT_TRUE(Record(a));
}
TEST_F(PprofAggregatorTest, AddSampleWithMismatchedValueCount) {
  PprofAggregator a(types);
  int64_t value = 1;
  EXPECT_FALSE(a.AddSample({}, std::span(&value, 1), 1, Labels(a)));
}
TEST_F(PprofAggregatorTest, AddSampleToUninitializedAggregator) {
  PprofAggregator a({});
  EXPECT_FALSE(a.AddSample({}, {}, 1, 0));
}
TEST_F(PprofAggregatorTest, AddMultipleSamples) {
  PprofAggregator a(types);
  for (int i = 0; i < 100; i++) ASSERT_TRUE(Record(a));
}
TEST_F(PprofAggregatorTest, IntegrationWithProfileExporterAndDebugFileWriting) {
  Configuration config;
  config.SetExportEnabled(false);
  auto dir = fs::temp_directory_path() / "dd-prof-c-aggregation-test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  ProfileExporter exporter(&config, types);
  exporter.SetDebugPprofFileWritingEnabled(true);
  exporter.SetDebugPprofPrefix((dir / "profile_").string());
  ASSERT_TRUE(exporter.Initialize());
  ASSERT_TRUE(exporter.Export(true));
  exporter.Cleanup();
  bool found = false;
  for (const auto& f : fs::directory_iterator(dir))
    found |= f.path().filename().string().ends_with(".zstd.pprof");
  EXPECT_TRUE(found);
  fs::remove_all(dir);
}
TEST_F(PprofAggregatorTest, CreateEmptyPprof) {
  PprofAggregator a(types);
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(a.Serialize(1, 2, bytes));
  Encoded(bytes);
}
TEST_F(PprofAggregatorTest, SerializeProfileWithSamples) {
  PprofAggregator a(types);
  ASSERT_TRUE(Record(a));
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(a.Serialize(1, 2, bytes));
  Encoded(bytes);
}
TEST_F(PprofAggregatorTest, CreateEmptyPprofWithCustomTimes) {
  PprofAggregator a(types);
  std::vector<uint8_t> bytes;
  EXPECT_TRUE(a.Serialize(-1000, 1234, bytes));
  EXPECT_FALSE(a.Serialize(2, 1, bytes));
}
TEST_F(PprofAggregatorTest, SerializeUninitializedAggregator) {
  PprofAggregator a({});
  std::vector<uint8_t> bytes;
  EXPECT_FALSE(a.Serialize(1, 2, bytes));
}
TEST_F(PprofAggregatorTest, ResetProfile) {
  PprofAggregator a(types);
  auto labels = Labels(a);
  ASSERT_TRUE(Record(a));
  ASSERT_TRUE(a.Reset());
  int64_t values[] = {1, 1};
  EXPECT_FALSE(a.AddSample({}, values, 1, labels));
  ASSERT_TRUE(Record(a));
}
TEST_F(PprofAggregatorTest, ResetUninitializedAggregator) {
  PprofAggregator a({});
  EXPECT_FALSE(a.Reset());
}
TEST_F(PprofAggregatorTest, MultipleSerializations) {
  PprofAggregator a(types);
  std::vector<uint8_t> owned;
  for (int i = 0; i < 5; i++) {
    ASSERT_TRUE(Record(a));
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(a.Serialize(i, i + 1, bytes));
    ASSERT_TRUE(a.Reset());
    if (!i) owned = bytes;
    Encoded(owned);
  }
}
TEST_F(PprofAggregatorTest, InitializationWithAllocationSampleTypes) {
  std::vector<SampleValueType> t{{"alloc-samples", "count"}, {"alloc-space", "bytes"}};
  PprofAggregator a(t);
  EXPECT_TRUE(a.IsInitialized());
}
TEST_F(PprofAggregatorTest, AddAllocationSamples) {
  std::vector<SampleValueType> t{{"alloc-samples", "count"}, {"alloc-space", "bytes"}};
  PprofAggregator a(t);
  EXPECT_TRUE(Record(a));
}
TEST_F(PprofAggregatorTest, SerializeAfterAddingSamples) {
  std::vector<uint8_t> owned;
  {
    PprofAggregator a(types);
    ASSERT_TRUE(Record(a));
    ASSERT_TRUE(a.Serialize(1, 2, owned));
  }
  Encoded(owned);
}
TEST_F(PprofAggregatorTest, AddSampleWithEmptyLocations) {
  PprofAggregator a(types);
  int64_t values[] = {7, 11};
  EXPECT_TRUE(a.AddSample({}, values, 0, Labels(a)));
}
TEST_F(PprofAggregatorTest, MillisecondsOverflowClearsOutput) {
  PprofAggregator a(types);
  std::vector<uint8_t> bytes{1, 2, 3};
  EXPECT_FALSE(a.Serialize(INT64_MAX, INT64_MAX, bytes));
  EXPECT_TRUE(bytes.empty());
}

TEST_F(PprofAggregatorTest, CustomSampleTypesPreserveRepeatedAndDistinctDefinitions) {
  std::vector<SampleValueType> definitions;
  for (int i = 0; i < 6; ++i) {
    definitions.push_back({"custom-" + std::to_string(i), "nanoseconds"});
  }
  definitions.push_back(definitions.front());
  PprofAggregator aggregator(definitions, 7);
  ASSERT_TRUE(aggregator.IsInitialized());
  definitions.clear();
  definitions.shrink_to_fit();
  for (int cycle = 1; cycle <= 2; ++cycle) {
    const std::vector<int64_t> values{
        11 * cycle,
        22 * cycle,
        33 * cycle,
        44 * cycle,
        55 * cycle,
        66 * cycle,
        77 * cycle
    };
    ASSERT_TRUE(aggregator.AddSample(
        Locations(aggregator), values, 1700000000000000000 + cycle, Labels(aggregator)
    ));
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(aggregator.Serialize(1700000000000, 1700000001000, bytes));
    Encoded(bytes);
    WriteTestProfile("wide-" + std::to_string(cycle) + ".pprof", bytes);
    ASSERT_TRUE(aggregator.Reset());
  }
}

TEST_F(PprofAggregatorTest, InvalidCustomDefinitionFailsInitialization) {
  std::vector<SampleValueType> definitions{{std::string(1, '\xff'), "nanoseconds"}};
  PprofAggregator aggregator(definitions);
  EXPECT_FALSE(aggregator.IsInitialized());
  EXPECT_EQ(aggregator.GetProfile(), nullptr);
}

TEST_F(PprofAggregatorTest, CustomPeriodAndDefinitionsSurviveInputLifetimeAndReset) {
  std::vector<SampleValueType> definitions{
      {"wait-time", "nanoseconds"},
      {"cpu-time", "nanoseconds"},
      {"cpu-time", "count"},
      {"wait-time", "count"},
      {"prototype-time", "nanoseconds"}
  };
  PprofAggregator aggregator(definitions, 10);
  ASSERT_TRUE(aggregator.IsInitialized());
  definitions.clear();
  definitions.shrink_to_fit();
  for (int cycle = 1; cycle <= 2; ++cycle) {
    auto locations = Locations(aggregator);
    auto labels = Labels(aggregator);
    const std::vector<int64_t> values{3000 * cycle, 1000 * cycle, cycle, cycle, 42};
    ASSERT_TRUE(
        aggregator.AddSample(locations, values, 1700000000000000000 + cycle, labels)
    );
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(aggregator.Serialize(1700000000000, 1700000001000, bytes));
    Encoded(bytes);
    WriteTestProfile("custom-" + std::to_string(cycle) + ".pprof", bytes);
    ASSERT_TRUE(aggregator.Reset());
  }
}

TEST_F(PprofAggregatorTest, EmbeddedNulDefinitionsSurviveReset) {
  const std::vector<SampleValueType> definitions{
      {std::string("wait\0time", 9), std::string("nano\0seconds", 12)},
      {"cpu-time", "nanoseconds"}
  };
  PprofAggregator aggregator(definitions, 10);
  ASSERT_TRUE(aggregator.IsInitialized());
  for (int cycle = 1; cycle <= 2; ++cycle) {
    ASSERT_TRUE(Record(aggregator));
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(aggregator.Serialize(1700000000000, 1700000001000, bytes));
    WriteTestProfile("nul-" + std::to_string(cycle) + ".pprof", bytes);
    ASSERT_TRUE(aggregator.Reset());
  }
}
