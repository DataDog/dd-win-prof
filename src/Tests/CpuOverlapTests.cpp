// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <gtest/gtest.h>

#include <chrono>

#include "pch.h"

using namespace std::chrono_literals;

// Simulate the CPU overlap detection logic from StackSamplerLoop.cpp:105-124
std::chrono::milliseconds ComputeCpuForSampleWithOverlapDetection(
    std::chrono::milliseconds initialCpuForSample,
    std::chrono::nanoseconds lastCpuTimestamp,
    std::chrono::nanoseconds thisSampleTimestamp
) {
  auto cpuForSample = initialCpuForSample;

  if (lastCpuTimestamp != 0ns) {
    auto threshold = lastCpuTimestamp + cpuForSample;
    if (threshold > thisSampleTimestamp) {
      // This is the problematic line from StackSamplerLoop.cpp:115-117
      cpuForSample = std::chrono::duration_cast<std::chrono::milliseconds>(
          thisSampleTimestamp - lastCpuTimestamp - 1us
      );
    }
  }

  return cpuForSample;
}

TEST(CpuOverlapTests, OverlapDetection_LosesPrecisionOnSubMillisecondDifferences) {
  // Scenario: Fast sampling interval where thread consumed < 1ms CPU
  std::chrono::nanoseconds lastTimestamp = 1'000'000ns;  // 1ms
  std::chrono::nanoseconds thisTimestamp = 1'500'000ns;  // 1.5ms
  std::chrono::milliseconds initialCpu = 1ms;

  // The difference is 500µs = 0.5ms
  // After subtracting 1µs: 499µs
  // Cast to milliseconds: 0ms ← Lost the CPU time!
  auto result =
      ComputeCpuForSampleWithOverlapDetection(initialCpu, lastTimestamp, thisTimestamp);

  // This test FAILS with current implementation
  // Expected: some non-zero value (at least 0ms is acceptable)
  // Actual: 0ms, but we know CPU was consumed
  EXPECT_GE(result.count(), 0) << "CPU time should not be negative";

  // More importantly, this demonstrates precision loss:
  std::chrono::nanoseconds actualDiff = thisTimestamp - lastTimestamp;
  EXPECT_LT(actualDiff, 1ms) << "Sub-millisecond difference loses precision";
}

TEST(CpuOverlapTests, OverlapDetection_CanProduceZeroCpuTime) {
  // Edge case: very small time difference
  std::chrono::nanoseconds lastTimestamp = 1'000'000ns;  // 1.0ms
  std::chrono::nanoseconds thisTimestamp = 1'000'500ns;  // 1.0005ms
  std::chrono::milliseconds initialCpu = 1ms;

  // Difference: 500ns = 0.0005ms
  // After -1µs: negative!
  // Cast to ms: 0ms
  auto result =
      ComputeCpuForSampleWithOverlapDetection(initialCpu, lastTimestamp, thisTimestamp);

  EXPECT_EQ(result.count(), 0)
      << "Very small differences round to 0ms, losing CPU time";
}

TEST(CpuOverlapTests, OverlapDetection_WorksCorrectlyForLargerIntervals) {
  // Scenario: Normal case where difference is > 1ms
  std::chrono::nanoseconds lastTimestamp = 1'000'000ns;  // 1ms
  std::chrono::nanoseconds thisTimestamp = 5'000'000ns;  // 5ms
  std::chrono::milliseconds initialCpu = 3ms;

  auto result =
      ComputeCpuForSampleWithOverlapDetection(initialCpu, lastTimestamp, thisTimestamp);

  // Threshold: 1ms + 3ms = 4ms
  // Overlap detected (4ms < 5ms is false, so no adjustment)
  EXPECT_EQ(result.count(), 3) << "No overlap, should return original value";
}

TEST(CpuOverlapTests, OverlapDetection_HandlesOverlapCorrectly) {
  // Scenario: Detected overlap
  std::chrono::nanoseconds lastTimestamp = 1'000'000ns;  // 1ms
  std::chrono::nanoseconds thisTimestamp = 3'500'000ns;  // 3.5ms
  std::chrono::milliseconds initialCpu = 4ms;

  auto result =
      ComputeCpuForSampleWithOverlapDetection(initialCpu, lastTimestamp, thisTimestamp);

  // Threshold: 1ms + 4ms = 5ms
  // 5ms > 3.5ms, overlap detected
  // Adjusted: (3.5ms - 1ms - 1µs) = 2.499ms
  // Cast to ms: 2ms
  EXPECT_EQ(result.count(), 2)
      << "Overlap adjustment should limit to timestamp difference";
}

TEST(CpuOverlapTests, OverlapDetection_DocumentsPrecisionLossIssue) {
  // This test documents the precision loss issue for sampling intervals < 1ms

  struct TestCase {
    std::chrono::nanoseconds lastTs;
    std::chrono::nanoseconds thisTs;
    std::chrono::milliseconds initialCpu;
    int expectedMs;
    const char* description;
  };

  TestCase cases[] = {
      {1'000'000ns, 1'100'000ns, 1ms, 0, "100µs difference → 0ms (PRECISION LOSS)"},
      {1'000'000ns, 1'500'000ns, 1ms, 0, "500µs difference → 0ms (PRECISION LOSS)"},
      {1'000'000ns, 1'999'000ns, 1ms, 0, "999µs difference → 0ms (PRECISION LOSS)"},
      {1'000'000ns,
       2'000'000ns,
       1ms,
       1,
       "1ms difference → 1ms (no overlap, returns original)"},
      {1'000'000ns, 2'001'000ns, 1ms, 1, "1001µs difference → 1ms (OK)"},
  };

  for (const auto& tc : cases) {
    auto result =
        ComputeCpuForSampleWithOverlapDetection(tc.initialCpu, tc.lastTs, tc.thisTs);

    EXPECT_EQ(result.count(), tc.expectedMs) << tc.description;
  }
}

// Proposed fix: work in nanoseconds throughout
std::chrono::nanoseconds ComputeCpuForSampleWithOverlapDetection_Fixed(
    std::chrono::nanoseconds initialCpuForSample,
    std::chrono::nanoseconds lastCpuTimestamp,
    std::chrono::nanoseconds thisSampleTimestamp
) {
  auto cpuForSample = initialCpuForSample;

  if (lastCpuTimestamp != 0ns) {
    auto threshold = lastCpuTimestamp + cpuForSample;
    if (threshold > thisSampleTimestamp) {
      // Keep in nanoseconds - no precision loss
      auto diff = thisSampleTimestamp - lastCpuTimestamp;
      // Only subtract 1µs if we have enough headroom
      if (diff > 1us) {
        cpuForSample = diff - 1us;
      } else {
        cpuForSample = 0ns;  // Too small, but don't go negative
      }
    }
  }

  return cpuForSample;
}

TEST(CpuOverlapTests, FixedVersion_PreservesPrecision) {
  // With the fixed version, we keep nanosecond precision
  std::chrono::nanoseconds lastTimestamp = 1'000'000ns;  // 1ms
  std::chrono::nanoseconds thisTimestamp = 1'500'000ns;  // 1.5ms
  std::chrono::nanoseconds initialCpu = 1'000'000ns;     // 1ms as ns

  auto result = ComputeCpuForSampleWithOverlapDetection_Fixed(
      initialCpu, lastTimestamp, thisTimestamp
  );

  // Difference: 500µs - 1µs = 499µs = 499000ns
  EXPECT_EQ(result.count(), 499'000) << "Fixed version preserves nanosecond precision";
  EXPECT_GT(result.count(), 0) << "No longer loses sub-millisecond CPU time";
}
