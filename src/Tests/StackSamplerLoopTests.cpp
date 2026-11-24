// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <thread>

#include "../dd-win-prof/OsSpecificApi.h"
#include "../dd-win-prof/StackSamplerLoop.h"
#include "pch.h"

using namespace std::chrono_literals;

// Bounds waits for events expected almost immediately. It is far shorter than
// the sampling period used to prove that Stop() does not wait for the next
// iteration.
constexpr auto kTimeout = 10s;
// Must exceed every in-test wait, so that a slow wait fails its test instead of
// killing the process.
constexpr auto kProcessDeadline = 60s;
constexpr auto kFastPeriod = 1ms;
constexpr auto kNeverElapsingPeriod = 1h;
// KWAIT_REASON::UserRequest: reported for WaitForSingleObject, not for Sleep
constexpr ULONG kUserRequestWaitReason = 6;

// A cancellation regression must fail the process instead of hanging while a
// blocked thread is joined.
class ProcessDeadline {
 public:
  ProcessDeadline(const char* operation, std::chrono::milliseconds timeout)
      : _completed(::CreateEventW(nullptr, TRUE, FALSE, nullptr)),
        _operation(operation) {
    if (_completed.IsValid()) {
      DWORD timeoutMs = static_cast<DWORD>(timeout.count());
      _watchdog = std::thread([this, timeoutMs] {
        if (::WaitForSingleObject(_completed, timeoutMs) == WAIT_TIMEOUT) {
          std::fprintf(stderr, "%s exceeded the process deadline\n", _operation);
          std::fflush(stderr);
          ::TerminateProcess(::GetCurrentProcess(), ERROR_TIMEOUT);
          std::abort();
        }
      });
    }
  }

  ~ProcessDeadline() {
    if (_completed.IsValid()) {
      ::SetEvent(_completed);
    }
    if (_watchdog.joinable()) {
      _watchdog.join();
    }
  }

  bool IsValid() { return _completed.IsValid(); }

 private:
  ScopedHandle _completed;
  const char* _operation;
  std::thread _watchdog;
};

class StackSamplerLoopTests : public ::testing::Test {
 protected:
  void SetUp() override {
    // armed until the end of TearDown() so that it also covers loop destruction
    _deadline.emplace(
        ::testing::UnitTest::GetInstance()->current_test_info()->name(),
        kProcessDeadline
    );
    ASSERT_TRUE(_deadline->IsValid());

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
    _sampledThreadId = ::GetThreadId(hThread);
    _threadList.AddThread(_sampledThreadId, hThread);
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
    _deadline.reset();
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

  // Runs one iteration on the calling thread, without any loop thread.
  static void RunIteration(StackSamplerLoop& loop) { loop.MainLoopIteration(); }

  // Simulates a Stop() committed while an iteration is about to run.
  static void RequestStop(StackSamplerLoop& loop) {
    loop._stopRequested.store(true, std::memory_order_release);
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

  // With a never elapsing period, the only object wait of the loop thread is the
  // one for its next iteration.
  static bool WaitForLoopToWait(HANDLE hLoopThread) {
    return WaitUntil([hLoopThread] {
      auto [isWaiting, waitReason, failure] = OsSpecificApi::IsWaiting(hLoopThread);
      return !failure && isWaiting && (waitReason == kUserRequestWaitReason);
    });
  }

  size_t DrainSamples() {
    std::vector<Sample> samples;
    return _wallTimeProvider.MoveSamples(samples);
  }

  bool WaitForSamples() {
    return WaitUntil([this] { return DrainSamples() > 0; });
  }

  std::optional<ProcessDeadline> _deadline;
  SampleValueTypeProvider _registry;
  WallTimeProvider _wallTimeProvider{_registry};
  Configuration _configuration;
  ThreadList _threadList;
  size_t _previousValuesCount = 0;
  HANDLE _releaseEvent = nullptr;
  DWORD _sampledThreadId = 0;
  std::thread _sampledThread;
};

TEST_F(StackSamplerLoopTests, NewLoopSamplesAfterPreviousLoopIsDestroyed) {
  auto loop = CreateLoop(kFastPeriod);

  loop->Start();
  ASSERT_TRUE(WaitForSamples()) << "first loop did not sample";
  loop.reset();

  // the loop thread is joined, so nothing can be added after this drain
  DrainSamples();
  EXPECT_EQ(DrainSamples(), 0u);

  // ThreadInfo baselines and iterators are reset before each session's loop is
  // created, as Profiler::StartProfiling does
  _threadList.ResetProfilingState();
  loop = CreateLoop(kFastPeriod);
  loop->Start();
  EXPECT_TRUE(IsRunning(*loop));
  ASSERT_TRUE(WaitForSamples()) << "second loop did not sample";
  loop->Stop();
  EXPECT_FALSE(IsRunning(*loop));
}

TEST_F(StackSamplerLoopTests, StartAfterStopIsIgnored) {
  auto loop = CreateLoop(kFastPeriod);
  loop->Start();
  ASSERT_TRUE(WaitForSamples()) << "loop did not sample";
  loop->Stop();
  DrainSamples();

  loop->Start();
  EXPECT_FALSE(IsRunning(*loop));

  auto neverStarted = CreateLoop(kFastPeriod);
  neverStarted->Stop();
  neverStarted->Start();
  EXPECT_FALSE(IsRunning(*neverStarted));

  EXPECT_EQ(DrainSamples(), 0u);
}

TEST_F(StackSamplerLoopTests, StopWakesLoopWaitingForNextIteration) {
  auto loop = CreateLoop(kNeverElapsingPeriod);

  loop->Start();
  HANDLE hLoopThread = DuplicateLoopThreadHandle(*loop);
  ASSERT_NE(hLoopThread, nullptr);
  EXPECT_TRUE(WaitForLoopToWait(hLoopThread));

  loop->Stop();
  EXPECT_EQ(::WaitForSingleObject(hLoopThread, 0), WAIT_OBJECT_0);
  EXPECT_FALSE(IsRunning(*loop));
  EXPECT_EQ(DrainSamples(), 0u);
  ::CloseHandle(hLoopThread);
}

TEST_F(StackSamplerLoopTests, StopIsIdempotentInEveryState) {
  auto neverStarted = CreateLoop(kFastPeriod);
  neverStarted->Stop();
  neverStarted->Stop();
  EXPECT_FALSE(IsRunning(*neverStarted));

  auto loop = CreateLoop(kFastPeriod);
  loop->Start();
  ASSERT_TRUE(WaitForSamples()) << "loop did not sample";
  loop->Stop();
  loop->Stop();
  EXPECT_FALSE(IsRunning(*loop));
}

TEST_F(StackSamplerLoopTests, StartWhileRunningKeepsCurrentRun) {
  auto loop = CreateLoop(kNeverElapsingPeriod);

  loop->Start();
  HANDLE hLoopThread = DuplicateLoopThreadHandle(*loop);
  ASSERT_NE(hLoopThread, nullptr);

  loop->Start();
  HANDLE hCurrentLoopThread = DuplicateLoopThreadHandle(*loop);
  ASSERT_NE(hCurrentLoopThread, nullptr);
  EXPECT_EQ(::GetThreadId(hCurrentLoopThread), ::GetThreadId(hLoopThread));

  loop->Stop();
  EXPECT_FALSE(IsRunning(*loop));
  ::CloseHandle(hCurrentLoopThread);
  ::CloseHandle(hLoopThread);
}

TEST_F(StackSamplerLoopTests, DestructionStopsWaitingLoop) {
  auto loop = CreateLoop(kNeverElapsingPeriod);

  loop->Start();
  HANDLE hLoopThread = DuplicateLoopThreadHandle(*loop);
  ASSERT_NE(hLoopThread, nullptr);
  EXPECT_TRUE(WaitForLoopToWait(hLoopThread));

  loop.reset();

  EXPECT_EQ(::WaitForSingleObject(hLoopThread, 0), WAIT_OBJECT_0)
      << "loop thread still running after destruction";
  EXPECT_EQ(DrainSamples(), 0u);
  ::CloseHandle(hLoopThread);
}

TEST_F(StackSamplerLoopTests, NoSampleIsTakenAfterStopRequest) {
  auto loop = CreateLoop(kFastPeriod);

  // a single iteration may capture no stack while the sampled thread starts
  ASSERT_TRUE(WaitUntil([this, &loop] {
    RunIteration(*loop);
    return DrainSamples() > 0;
  })) << "iterations without stop request did not sample";

  RequestStop(*loop);
  for (int i = 0; i < 10; i++) {
    RunIteration(*loop);
  }
  EXPECT_EQ(DrainSamples(), 0u);
}

TEST_F(StackSamplerLoopTests, DestructionWithoutStartIsSafe) {
  auto loop = CreateLoop(kFastPeriod);
  loop->Stop();
  loop.reset();

  loop = CreateLoop(kFastPeriod);
  loop.reset();
  EXPECT_EQ(DrainSamples(), 0u);
}
