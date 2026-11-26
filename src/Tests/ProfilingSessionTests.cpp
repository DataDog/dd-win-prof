// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include "../dd-win-prof/ProfilingConstants.h"
#include "../dd-win-prof/ProfilingSession.h"
#include "pch.h"

using Step = ProfilingSession::Step;

namespace {
const std::vector<Step> kStartSteps = {
    Step::CreateProviders,
    Step::InitializeExporter,
    Step::CreateCollector,
    Step::CreateSampler,
    Step::StartCollector,
    Step::StartSampler,
};

// teardown of the four created components, in their release order
const std::vector<Step> kReleaseSteps = {
    Step::StopSampler,
    Step::StopCollector,
    Step::ReleaseExporter,
    Step::ReleaseProviders,
};

struct Recorder {
  std::mutex mutex;
  std::vector<Step> steps;
  int callbacks = 0;
  int finalCallbacks = 0;
  size_t stepsAtFinalCallback = 0;

  std::vector<Step> Steps() {
    std::lock_guard lock(mutex);
    return steps;
  }
};

void DD_WIN_PROF_CALL RecordCallback(const ProfilingData* data, void* userData) {
  auto& recorder = *static_cast<Recorder*>(userData);
  std::lock_guard lock(recorder.mutex);
  ++recorder.callbacks;
  if (data->is_last != 0) {
    ++recorder.finalCallbacks;
    recorder.stepsAtFinalCallback = recorder.steps.size();
  }
}

ProfilingSession::Hooks RecordingHooks(
    Recorder& recorder, std::optional<Step> failingStep = std::nullopt
) {
  ProfilingSession::Hooks hooks;
  hooks.failStep = [failingStep](Step step) { return step == failingStep; };
  hooks.onStep = [&recorder](Step step) {
    std::lock_guard lock(recorder.mutex);
    recorder.steps.push_back(step);
  };
  return hooks;
}

std::vector<Step> Concat(std::vector<Step> first, const std::vector<Step>& second) {
  first.insert(first.end(), second.begin(), second.end());
  return first;
}
}  // namespace

class ProfilingSessionTests : public ::testing::Test {
 protected:
  void SetUp() override {
    _previousValuesCount = Sample::ValuesCount;
    _configuration.ResetToDefaults();
    // no periodic export during a test: only the final export reaches the callback
    _configuration.SetUploadInterval(std::chrono::hours(1));
  }

  void TearDown() override {
    if (_hWnd != nullptr) {
      ::DestroyWindow(_hWnd);
    }
    Sample::SetValuesCount(_previousValuesCount);
  }

  std::unique_ptr<ProfilingSession> CreateSession(
      Recorder& recorder, ProfilingSession::Hooks hooks
  ) {
    return std::make_unique<ProfilingSession>(
        ++_lastSessionId,
        _configuration,
        RecordCallback,
        &recorder,
        ProfilingSession::ProcessServices{&_threadList, &_rumViewContext},
        std::move(hooks)
    );
  }

  // A message-only window owned by the test thread, which the detector requires to be
  // a monitored thread.
  HWND CreateMonitoredWindow() {
    HANDLE hThread = nullptr;
    if (!::DuplicateHandle(
            ::GetCurrentProcess(),
            ::GetCurrentThread(),
            ::GetCurrentProcess(),
            &hThread,
            THREAD_ALL_ACCESS,
            FALSE,
            0
        )) {
      return nullptr;
    }
    _threadList.AddThread(::GetCurrentThreadId(), hThread);
    _hWnd = ::CreateWindowExW(
        0, L"STATIC", nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr
    );
    return _hWnd;
  }

  static UINT ProbeMessageId() {
    return ::RegisterWindowMessageW(dd_win_prof::kHangProbeMessageName);
  }

  class NoRumView : public IRumViewContextProvider {
   public:
    bool GetCurrentViewContext(RumViewContext&) const override { return false; }
  };

  size_t _previousValuesCount = 0;
  Configuration _configuration;
  ThreadList _threadList;
  NoRumView _rumViewContext;
  uint64_t _lastSessionId = 0;
  HWND _hWnd = nullptr;
};

TEST_F(ProfilingSessionTests, StopFollowsTheDocumentedOrderAndExportsOnce) {
  HWND hWnd = CreateMonitoredWindow();
  ASSERT_NE(hWnd, nullptr);
  Recorder recorder;
  auto session = CreateSession(recorder, RecordingHooks(recorder));

  ASSERT_TRUE(session->Start());
  EXPECT_TRUE(session->IsRunning());
  ASSERT_TRUE(session->MonitorWindowHangs(hWnd, ProbeMessageId()));
  EXPECT_FALSE(session->MonitorWindowHangs(hWnd, ProbeMessageId()));
  session->Stop();
  EXPECT_FALSE(session->IsRunning());

  const auto stopSteps = Concat({Step::StopDetector}, kReleaseSteps);
  EXPECT_EQ(recorder.Steps(), Concat(kStartSteps, stopSteps));
  EXPECT_EQ(recorder.callbacks, 1);
  EXPECT_EQ(recorder.finalCallbacks, 1);
  // the final callback runs once the producers are stopped, inside the collector stop
  EXPECT_EQ(recorder.stepsAtFinalCallback, kStartSteps.size() + 2);

  // a stopped session neither exports nor restarts
  session->Stop();
  EXPECT_FALSE(session->Start());
  EXPECT_FALSE(session->MonitorWindowHangs(hWnd, ProbeMessageId()));
  session.reset();
  EXPECT_EQ(recorder.Steps().size(), kStartSteps.size() + stopSteps.size());
  EXPECT_EQ(recorder.callbacks, 1);
}

TEST_F(ProfilingSessionTests, EachStartFailureRollsBackInReverseOrderWithoutCallback) {
  for (size_t failing = 0; failing < kStartSteps.size(); ++failing) {
    SCOPED_TRACE(static_cast<int>(kStartSteps[failing]));
    Recorder recorder;
    auto session =
        CreateSession(recorder, RecordingHooks(recorder, kStartSteps[failing]));

    EXPECT_FALSE(session->Start());
    EXPECT_FALSE(session->IsRunning());

    // each of the first four steps creates one component, released in reverse order
    const size_t created = (std::min)(failing, kReleaseSteps.size());
    const std::vector<Step> done(kStartSteps.begin(), kStartSteps.begin() + failing);
    const std::vector<Step> released(
        kReleaseSteps.end() - created, kReleaseSteps.end()
    );
    EXPECT_EQ(recorder.Steps(), Concat(done, released));
    session->Stop();
    session.reset();
    EXPECT_EQ(recorder.Steps(), Concat(done, released));
    EXPECT_EQ(recorder.callbacks, 0);

    // a fresh session can start right away
    Recorder retryRecorder;
    auto retry = CreateSession(retryRecorder, {});
    ASSERT_TRUE(retry->Start());
    retry->Stop();
    EXPECT_EQ(retryRecorder.finalCallbacks, 1);
  }
}

TEST_F(ProfilingSessionTests, WorkerCreationFailureJoinsPartialWorker) {
  Recorder recorder;
  auto hooks = RecordingHooks(recorder);
  int created = 0;
  std::atomic<int> returned = 0;
  hooks.collectorThreadFactory = [&](std::function<void()> routine) {
    if (++created == 2) {
      throw std::runtime_error("injected thread creation failure");
    }
    return std::thread([&returned, routine = std::move(routine)] {
      routine();
      ++returned;
    });
  };
  auto session = CreateSession(recorder, std::move(hooks));

  EXPECT_FALSE(session->Start());
  EXPECT_EQ(created, 2);
  EXPECT_EQ(returned, 1);
  EXPECT_EQ(
      recorder.Steps(),
      Concat(
          std::vector<Step>(kStartSteps.begin(), kStartSteps.begin() + 4), kReleaseSteps
      )
  );
  EXPECT_EQ(recorder.callbacks, 0);
}

TEST_F(ProfilingSessionTests, DestructionWithoutStopReleasesWithoutExport) {
  Recorder recorder;
  auto session = CreateSession(recorder, RecordingHooks(recorder));
  ASSERT_TRUE(session->Start());

  session.reset();

  EXPECT_EQ(recorder.Steps(), Concat(kStartSteps, kReleaseSteps));
  EXPECT_EQ(recorder.callbacks, 0);
}

TEST_F(ProfilingSessionTests, SequentialSessionsShareOnlyProcessState) {
  HWND hWnd = CreateMonitoredWindow();
  ASSERT_NE(hWnd, nullptr);

  Recorder first;
  auto firstSession = CreateSession(first, {});
  ASSERT_TRUE(firstSession->Start());
  ASSERT_TRUE(firstSession->MonitorWindowHangs(hWnd, ProbeMessageId()));
  // a shutdown stop skips the final export
  firstSession->Stop(true);
  const auto firstId = firstSession->Id();
  firstSession.reset();
  EXPECT_EQ(first.callbacks, 0);

  // fresh detector, workers, shutdown state and exporter with its own callback
  Recorder second;
  auto secondSession = CreateSession(second, {});
  ASSERT_TRUE(secondSession->Start());
  EXPECT_NE(secondSession->Id(), firstId);
  ASSERT_TRUE(secondSession->MonitorWindowHangs(hWnd, ProbeMessageId()));
  EXPECT_FALSE(secondSession->MonitorWindowHangs(hWnd, ProbeMessageId()));
  secondSession->Stop();

  EXPECT_EQ(second.finalCallbacks, 1);
  EXPECT_EQ(first.callbacks, 0);
}
