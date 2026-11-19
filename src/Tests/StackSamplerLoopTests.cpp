// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <gtest/gtest.h>

#include <future>
#include <thread>

#include "../dd-win-prof/OsSpecificApi.h"
#include "../dd-win-prof/StackSamplerLoop.h"
#include "pch.h"

using namespace std::chrono_literals;

// Bounds waits for events expected almost immediately. It is far shorter than
// the sampling period used to prove that Stop() does not wait for the next
// iteration.
constexpr auto kTimeout = 10s;
constexpr auto kFastPeriod = 1ms;
constexpr auto kNeverElapsingPeriod = 1h;

class StackSamplerLoopTest : public ::testing::Test {
 protected:
  void SetUp() override {
    _previousValuesCount = Sample::ValuesCount;
    Sample::SetValuesCount(_registry.GetValueTypes().size());

    // a parked thread gives the walltime sampler something to sample
    _releaseEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(_releaseEvent, nullptr);
    _sampledThread =
        std::thread([this] { ::WaitForSingleObject(_releaseEvent, INFINITE); });
    HANDLE hThread = nullptr;
    ASSERT_TRUE(
        ::DuplicateHandle(
            ::GetCurrentProcess(),
            _sampledThread.native_handle(),
            ::GetCurrentProcess(),
            &hThread,
            0,
            FALSE,
            DUPLICATE_SAME_ACCESS
        )
    );
    _threadList.AddThread(::GetThreadId(hThread), hThread);
  }

  void TearDown() override {
    if (_releaseEvent != nullptr) {
      ::SetEvent(_releaseEvent);
    }
    if (_sampledThread.joinable()) {
      _sampledThread.join();
    }
    if (_releaseEvent != nullptr) {
      ::CloseHandle(_releaseEvent);
    }
    Sample::SetValuesCount(_previousValuesCount);
  }

  std::unique_ptr<StackSamplerLoop> CreateLoop(std::chrono::nanoseconds period) {
    _configuration.SetCpuWallTimeSamplingPeriod(period);
    return std::make_unique<StackSamplerLoop>(
        &_configuration, &_threadList, nullptr, &_wallTimeProvider
    );
  }

  static bool IsRunning(StackSamplerLoop& loop) {
    std::lock_guard<std::mutex> lock(loop._lifecycleMutex);
    return loop._pLoopThread != nullptr;
  }

  // The returned handle stays usable after the loop object is gone, to check
  // that its thread has exited.
  static HANDLE DuplicateLoopThreadHandle(StackSamplerLoop& loop) {
    std::lock_guard<std::mutex> lock(loop._lifecycleMutex);
    if (loop._pLoopThread == nullptr) {
      return nullptr;
    }
    HANDLE hThread = nullptr;
    if (!::DuplicateHandle(
            ::GetCurrentProcess(),
            loop._pLoopThread->native_handle(),
            ::GetCurrentProcess(),
            &hThread,
            0,
            FALSE,
            DUPLICATE_SAME_ACCESS
        )) {
      return nullptr;
    }
    return hThread;
  }

  template <typename Predicate>
  static bool WaitUntil(Predicate predicate) {
    auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while (!predicate()) {
      if (std::chrono::steady_clock::now() > deadline) {
        return false;
      }
      std::this_thread::sleep_for(1ms);
    }
    return true;
  }

  // With a never elapsing period, the only wait of the loop thread is the one
  // for its next iteration.
  static bool WaitForLoopToWait(HANDLE hLoopThread) {
    return WaitUntil([hLoopThread] {
      auto [isWaiting, waitReason, failure] = OsSpecificApi::IsWaiting(hLoopThread);
      return !failure && isWaiting;
    });
  }

  static bool StopWithinTimeout(StackSamplerLoop& loop) {
    auto stopped = std::async(std::launch::async, [&loop] { loop.Stop(); });
    return stopped.wait_for(kTimeout) == std::future_status::ready;
  }

  size_t DrainSamples() {
    std::vector<Sample> samples;
    return _wallTimeProvider.MoveSamples(samples);
  }

  bool WaitForSamples() {
    return WaitUntil([this] { return DrainSamples() > 0; });
  }

  SampleValueTypeProvider _registry;
  WallTimeProvider _wallTimeProvider{_registry};
  Configuration _configuration;
  ThreadList _threadList;
  size_t _previousValuesCount = 0;
  HANDLE _releaseEvent = nullptr;
  std::thread _sampledThread;
};

TEST_F(StackSamplerLoopTest, SecondRunSamplesAfterStop) {
  auto loop = CreateLoop(kFastPeriod);

  loop->Start();
  ASSERT_TRUE(WaitForSamples()) << "first run did not sample";
  loop->Stop();
  EXPECT_FALSE(IsRunning(*loop));

  // the loop thread is joined, so nothing can be added after this drain
  DrainSamples();
  EXPECT_EQ(DrainSamples(), 0u);

  loop->Start();
  EXPECT_TRUE(IsRunning(*loop));
  ASSERT_TRUE(WaitForSamples()) << "second run inherited the first run's stop";
  loop->Stop();
  EXPECT_FALSE(IsRunning(*loop));
}

TEST_F(StackSamplerLoopTest, StopWakesLoopWaitingForNextIteration) {
  auto loop = CreateLoop(kNeverElapsingPeriod);

  loop->Start();
  HANDLE hLoopThread = DuplicateLoopThreadHandle(*loop);
  ASSERT_NE(hLoopThread, nullptr);
  EXPECT_TRUE(WaitForLoopToWait(hLoopThread));

  EXPECT_TRUE(StopWithinTimeout(*loop)) << "Stop() waited for the sampling period";
  EXPECT_EQ(::WaitForSingleObject(hLoopThread, 0), WAIT_OBJECT_0);
  EXPECT_FALSE(IsRunning(*loop));
  EXPECT_EQ(DrainSamples(), 0u);
  ::CloseHandle(hLoopThread);
}

TEST_F(StackSamplerLoopTest, StopIsIdempotentInEveryState) {
  auto loop = CreateLoop(kFastPeriod);

  // before any start: these stop requests must not leak into the first run
  loop->Stop();
  loop->Stop();
  EXPECT_FALSE(IsRunning(*loop));

  for (int run = 0; run < 3; run++) {
    loop->Start();
    ASSERT_TRUE(WaitForSamples()) << "run " << run << " did not sample";
    ASSERT_TRUE(StopWithinTimeout(*loop)) << "run " << run;
    ASSERT_TRUE(StopWithinTimeout(*loop)) << "run " << run;
    EXPECT_FALSE(IsRunning(*loop));
  }
}

TEST_F(StackSamplerLoopTest, StartWhileRunningKeepsCurrentRun) {
  auto loop = CreateLoop(kNeverElapsingPeriod);

  loop->Start();
  HANDLE hLoopThread = DuplicateLoopThreadHandle(*loop);
  ASSERT_NE(hLoopThread, nullptr);

  loop->Start();
  HANDLE hCurrentLoopThread = DuplicateLoopThreadHandle(*loop);
  ASSERT_NE(hCurrentLoopThread, nullptr);
  EXPECT_EQ(::GetThreadId(hCurrentLoopThread), ::GetThreadId(hLoopThread));

  EXPECT_TRUE(StopWithinTimeout(*loop));
  EXPECT_FALSE(IsRunning(*loop));
  ::CloseHandle(hCurrentLoopThread);
  ::CloseHandle(hLoopThread);
}

TEST_F(StackSamplerLoopTest, DestructionAfterRestartStopsWaitingLoop) {
  auto loop = CreateLoop(kNeverElapsingPeriod);

  loop->Start();
  HANDLE hFirstLoopThread = DuplicateLoopThreadHandle(*loop);
  ASSERT_NE(hFirstLoopThread, nullptr);
  EXPECT_TRUE(WaitForLoopToWait(hFirstLoopThread));
  ASSERT_TRUE(StopWithinTimeout(*loop));
  EXPECT_EQ(::WaitForSingleObject(hFirstLoopThread, 0), WAIT_OBJECT_0);
  ::CloseHandle(hFirstLoopThread);

  loop->Start();
  HANDLE hLoopThread = DuplicateLoopThreadHandle(*loop);
  ASSERT_NE(hLoopThread, nullptr);
  EXPECT_TRUE(WaitForLoopToWait(hLoopThread)) << "second run is not waiting";

  auto destroyed = std::async(std::launch::async, [&loop] { loop.reset(); });
  EXPECT_EQ(destroyed.wait_for(kTimeout), std::future_status::ready)
      << "destructor waited for the sampling period";
  destroyed.wait();

  EXPECT_EQ(::WaitForSingleObject(hLoopThread, 0), WAIT_OBJECT_0)
      << "loop thread still running after destruction";
  EXPECT_EQ(DrainSamples(), 0u);
  ::CloseHandle(hLoopThread);
}

TEST_F(StackSamplerLoopTest, DestructionWithoutStartIsSafe) {
  auto loop = CreateLoop(kFastPeriod);
  loop->Stop();
  loop.reset();

  loop = CreateLoop(kFastPeriod);
  loop.reset();
  EXPECT_EQ(DrainSamples(), 0u);
}
