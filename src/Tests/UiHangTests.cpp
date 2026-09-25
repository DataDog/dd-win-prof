// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "Profiler.h"
#include "StackSamplerLoop.h"
#include "UiHangDetector.h"
#include "pch.h"

class UiHangTestPeer {
 public:
  static void PrepareProbe(UiHangDetector& detector, std::chrono::nanoseconds now) {
    detector._state = UiHangDetector::WatchdogState::Probing;
    detector._postProbeTimestamp = now - dd_win_prof::kHangThresholdMs;
    detector._lastNoHangTimestamp = now - 32ms;
    detector._processedProbeTimestamp.store(0ns);
    detector._probeId.store(42);
  }

  static bool Detect(UiHangDetector& detector, std::chrono::nanoseconds now) {
    return detector.TryDetectHang(now);
  }

  static std::chrono::nanoseconds AcknowledgedAt(UiHangDetector& detector) {
    return detector._processedProbeTimestamp.load();
  }

  static std::chrono::nanoseconds DetectedAt(UiHangDetector& detector) {
    return detector._hangDetectionTimestamp;
  }

  static void SetHang(
      UiHangDetector& detector,
      const std::shared_ptr<ThreadInfo>& thread,
      std::chrono::nanoseconds timestamp
  ) {
    detector._state = UiHangDetector::WatchdogState::Hang;
    detector._hangDetectionTimestamp = timestamp;
    detector._pThreadInfo = thread;
    detector._hangCallstack = std::make_shared<const ThreadInfo::HangCallstack>(
        ThreadInfo::HangCallstack{0x1234, 0x5678}
    );
    thread->SetHangCallstack(detector._hangCallstack);
  }

  static void Recover(UiHangDetector& detector, std::chrono::nanoseconds timestamp) {
    detector.EndHang(timestamp);
  }

  static bool CollectCached(
      StackSamplerLoop& sampler,
      const std::shared_ptr<ThreadInfo>& thread,
      std::chrono::nanoseconds timestamp,
      std::chrono::nanoseconds duration
  ) {
    return sampler.CollectHungThreadWallSample(thread, timestamp, duration);
  }
};

namespace {
constexpr UINT ProbeMessage = WM_APP + 19;

class EmptyRumContext : public IRumViewContextProvider {
 public:
  bool GetCurrentViewContext(RumViewContext& context) const override { return false; }
};

void Acknowledge(UiHangDetector& detector, WPARAM probeId = 42) {
  MSG message{};
  message.message = ProbeMessage;
  message.wParam = probeId;
  detector.ProcessHook(HC_ACTION, PM_REMOVE, reinterpret_cast<LPARAM>(&message));
}
}  // namespace

TEST(UiHangTests, AcknowledgmentBeforeDetectionDoesNotStartHang) {
  UiHangDetector detector(ProbeMessage, nullptr, nullptr);
  auto now = OpSysTools::GetHighPrecisionTimestamp();
  UiHangTestPeer::PrepareProbe(detector, now);
  EXPECT_EQ(UiHangTestPeer::AcknowledgedAt(detector), 0ns);
  Acknowledge(detector);
  EXPECT_FALSE(UiHangTestPeer::Detect(detector, now));
}

TEST(UiHangTests, DetectionRacingAcknowledgmentCannotReverseTimestamps) {
  UiHangDetector detector(ProbeMessage, nullptr, nullptr);
  for (int i = 0; i < 500; ++i) {
    UiHangTestPeer::PrepareProbe(detector, OpSysTools::GetHighPrecisionTimestamp());
    std::atomic<bool> ready{false};
    std::thread hook([&] {
      while (!ready.load()) {
        std::this_thread::yield();
      }
      Acknowledge(detector);
    });
    ready.store(true);
    bool detected =
        UiHangTestPeer::Detect(detector, OpSysTools::GetHighPrecisionTimestamp());
    hook.join();
    if (detected) {
      EXPECT_GE(
          UiHangTestPeer::AcknowledgedAt(detector), UiHangTestPeer::DetectedAt(detector)
      );
    }
  }
}

TEST(UiHangTests, IgnoresProbeFromPreviousRegistration) {
  UiHangDetector detector(ProbeMessage, nullptr, nullptr);
  auto now = OpSysTools::GetHighPrecisionTimestamp();
  UiHangTestPeer::PrepareProbe(detector, now);
  Acknowledge(detector, 41);
  EXPECT_EQ(UiHangTestPeer::AcknowledgedAt(detector), 0ns);
  EXPECT_TRUE(UiHangTestPeer::Detect(detector, now));
}

TEST(UiHangTests, RecoveryOnlyPublishesWaitBoundary) {
  ThreadInfo thread(1, nullptr);
  thread.SetLastWaitSampleTimestamp(10ms);
  thread.EndHang(100ms);
  EXPECT_EQ(thread.ComputeWaitDuration(120ms, 5ms), 20ms);
  EXPECT_EQ(thread.ComputeWaitDuration(125ms, 5ms), 5ms);
}

TEST(UiHangTests, RecoveryPreservesCpuWaitReset) {
  ThreadInfo thread(1, nullptr);
  thread.EndHang(100ms);
  thread.SetLastWaitSampleTimestamp(0ns);
  EXPECT_EQ(thread.ComputeWaitDuration(103ms, 5ms), 3ms);
  thread.SetLastWaitSampleTimestamp(0ns);
  EXPECT_EQ(thread.ComputeWaitDuration(120ms, 5ms), 5ms);
}

TEST(UiHangTests, RecoveryAfterSampleTimestampDoesNotProduceNegativeWait) {
  ThreadInfo thread(1, nullptr);
  thread.SetLastWaitSampleTimestamp(10ms);
  thread.EndHang(100ms);
  EXPECT_EQ(thread.ComputeWaitDuration(99ms, 5ms), 0ns);
  EXPECT_EQ(thread.ComputeWaitDuration(120ms, 5ms), 20ms);
}

TEST(UiHangTests, CachedWallSamplesPreserveSkippedIntervalsWithoutSuspension) {
  Configuration configuration;
  configuration.ResetToDefaults();
  ThreadList threads;
  SampleValueTypeProvider types;
  WallTimeProvider wall(types);
  StackSamplerLoop sampler(&configuration, &threads, nullptr, &wall);
  // No thread handle: this path must not suspend or unwind the target.
  auto thread = std::make_shared<ThreadInfo>(1, nullptr);
  thread->SetHangCallstack(std::make_shared<const ThreadInfo::HangCallstack>(
      ThreadInfo::HangCallstack{0x1234, 0x5678}
  ));

  for (int i = 1; i <= 5; ++i) {
    EXPECT_TRUE(UiHangTestPeer::CollectCached(sampler, thread, i * 20ms, 20ms));
  }
  std::vector<Sample> samples;
  ASSERT_EQ(wall.MoveSamples(samples), 5u);
  int64_t totalWall = 0;
  for (auto& sample : samples) {
    totalWall += sample.GetValues()[wall.GetValueOffsets()[0]];
    EXPECT_EQ(sample.GetValues()[wall.GetValueOffsets()[1]], 0);
    EXPECT_EQ(sample.GetFrames()[0], 0x1234u);
    EXPECT_EQ(sample.GetUiHangSampleKind(), UiHangSampleKind::None);
  }
  EXPECT_EQ(totalWall, std::chrono::nanoseconds(100ms).count());

  thread->EndHang(100ms);
  EXPECT_FALSE(UiHangTestPeer::CollectCached(sampler, thread, 120ms, 20ms));
  EXPECT_EQ(wall.MoveSamples(samples), 0u);
}

TEST(UiHangTests, MissingDetectionStackFallsBackToOrdinarySampling) {
  Configuration configuration;
  ThreadList threads;
  SampleValueTypeProvider types;
  WallTimeProvider wall(types);
  StackSamplerLoop sampler(&configuration, &threads, nullptr, &wall);
  auto thread = std::make_shared<ThreadInfo>(1, nullptr);
  EXPECT_FALSE(UiHangTestPeer::CollectCached(sampler, thread, 20ms, 20ms));
}

TEST(UiHangTests, RecoveryReusesDetectionStackWithoutSuspension) {
  SampleValueTypeProvider types;
  UiHangProvider hangs(types);
  EmptyRumContext rum;
  UiHangDetector detector(ProbeMessage, &hangs, &rum);
  auto thread = std::make_shared<ThreadInfo>(1, nullptr);
  UiHangTestPeer::SetHang(detector, thread, 100ms);
  UiHangTestPeer::Recover(detector, 1500ms);

  EXPECT_EQ(thread->GetHangCallstack(), nullptr);
  std::vector<Sample> samples;
  ASSERT_EQ(hangs.MoveSamples(samples), 1u);
  EXPECT_EQ(samples[0].GetFrames()[0], 0x1234u);
  EXPECT_EQ(samples[0].GetUiHangSampleKind(), UiHangSampleKind::Recovered);
  EXPECT_EQ(
      samples[0].GetValues()[hangs.GetValueOffsets()[1]],
      std::chrono::nanoseconds(1400ms).count()
  );
}

TEST(UiHangTests, StopClearsActiveHangAndClosesItsSamplePair) {
  SampleValueTypeProvider types;
  UiHangProvider hangs(types);
  EmptyRumContext rum;
  UiHangDetector detector(ProbeMessage, &hangs, &rum);
  auto thread = std::make_shared<ThreadInfo>(1, nullptr);
  UiHangTestPeer::SetHang(
      detector, thread, OpSysTools::GetHighPrecisionTimestamp() - 100ms
  );
  detector.Stop();
  detector.Stop();

  EXPECT_EQ(thread->GetHangCallstack(), nullptr);
  std::vector<Sample> samples;
  ASSERT_EQ(hangs.MoveSamples(samples), 1u);
  EXPECT_EQ(samples[0].GetUiHangSampleKind(), UiHangSampleKind::Recovered);
  EXPECT_GE(
      samples[0].GetValues()[hangs.GetValueOffsets()[1]],
      std::chrono::nanoseconds(100ms).count()
  );
}

class UiHangLifecycleTests : public ::testing::Test {
 protected:
  void SetUp() override {
    _savedConfiguration = *Profiler::GetConfiguration();
    Profiler::GetConfiguration()->ResetToDefaults();
    Profiler::GetConfiguration()->SetExportEnabled(false);
    _profiler = std::make_unique<Profiler>();
    ASSERT_TRUE(_profiler->AddCurrentThread());
    _window = CreateWindowExW(
        0,
        L"STATIC",
        L"Hang test",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        nullptr,
        nullptr
    );
    ASSERT_NE(_window, nullptr);
    ASSERT_TRUE(_profiler->StartProfiling());
  }

  void TearDown() override {
    _profiler->StopProfiling();
    _profiler.reset();
    if (_window != nullptr) {
      DestroyWindow(_window);
    }
    *Profiler::GetConfiguration() = _savedConfiguration;
  }

  Configuration _savedConfiguration;
  std::unique_ptr<Profiler> _profiler;
  HWND _window = nullptr;
};

TEST_F(UiHangLifecycleTests, RestartProfilerAllowsMonitoringAgain) {
  ASSERT_TRUE(_profiler->MonitorWindowHangs(_window));
  _profiler->StopProfiling();
  ASSERT_TRUE(_profiler->StartProfiling());
  EXPECT_TRUE(_profiler->MonitorWindowHangs(_window));
}

TEST_F(UiHangLifecycleTests, UnregisterAllowsReplacementWindow) {
  ASSERT_TRUE(_profiler->MonitorWindowHangs(_window));
  EXPECT_FALSE(_profiler->MonitorWindowHangs(_window));
  _profiler->StopMonitoringWindowHangs();
  _profiler->StopMonitoringWindowHangs();
  EXPECT_TRUE(_profiler->IsStarted());
  DestroyWindow(_window);
  _window = CreateWindowExW(
      0,
      L"STATIC",
      L"Replacement",
      0,
      0,
      0,
      0,
      0,
      HWND_MESSAGE,
      nullptr,
      nullptr,
      nullptr
  );
  ASSERT_NE(_window, nullptr);
  EXPECT_TRUE(_profiler->MonitorWindowHangs(_window));
}
