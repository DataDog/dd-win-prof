// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <gtest/gtest.h>

#include <condition_variable>
#include <functional>
#include <future>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ThreadList.h"
#include "UiHangDetector.h"
#include "UiHangProvider.h"
#include "pch.h"

namespace {
const HWND kWindow = reinterpret_cast<HWND>(static_cast<uintptr_t>(0xC0DE0));
constexpr UINT kProbeMessage = 0xC0DE;
constexpr DWORD kUiThreadId = 0x4242;
constexpr uint64_t kHangFrame = 0x2345;

// bound for steps that complete at once when the detector behaves
constexpr auto kTimeout = std::chrono::seconds(10);
// time given to an operation that must stay blocked
constexpr auto kBlockedDuration = std::chrono::milliseconds(100);

MSG Probe(uint64_t sessionId, uint64_t sequence) {
  MSG msg{};
  msg.hwnd = kWindow;
  msg.message = kProbeMessage;
  msg.wParam = static_cast<WPARAM>(sessionId);
  msg.lParam = static_cast<LPARAM>(sequence);
  return msg;
}

HANDLE DuplicateCurrentThread() {
  HANDLE handle = nullptr;
  ::DuplicateHandle(
      ::GetCurrentProcess(),
      ::GetCurrentThread(),
      ::GetCurrentProcess(),
      &handle,
      0,
      FALSE,
      DUPLICATE_SAME_ACCESS
  );
  return handle;
}

class FakeRumView : public IRumViewContextProvider {
 public:
  void Set(const std::string& view) {
    std::lock_guard<std::mutex> lock(_mutex);
    _view = view;
  }

  bool GetCurrentViewContext(RumViewContext& context) const override {
    std::lock_guard<std::mutex> lock(_mutex);
    context = {_view, _view};
    return true;
  }

 private:
  mutable std::mutex _mutex;
  std::string _view;
};

// the hang samples of one profiling session
struct SessionProviders {
  SampleValueTypeProvider valueTypes;
  UiHangProvider hangs{valueTypes};

  std::vector<Sample> Drain() {
    std::vector<Sample> samples;
    hangs.MoveSamples(samples);
    return samples;
  }
};
}  // namespace

class UiHangDetectorSessionTests : public ::testing::Test {
 protected:
  // A manual clock and a watchdog that runs one loop iteration per tick granted by the
  // test. Hook notifications are delivered by the test thread; stack captures are
  // counted and return a fixed stack.
  class FakeRuntime final : public IUiHangDetectorRuntime {
   public:
    ~FakeRuntime() {
      std::lock_guard<std::mutex> lock(_mutex);
      _isReleasingAll = true;
      _changed.notify_all();
    }

    void SetTime(std::chrono::milliseconds now) {
      std::lock_guard<std::mutex> lock(_mutex);
      _now = now;
    }

    // lets the watchdog run one loop iteration and waits until it waits again
    void RunTick() {
      std::unique_lock<std::mutex> lock(_mutex);
      if (!WaitLocked(lock, "watchdog wait", [&] { return _waiting > 0; })) {
        return;
      }
      size_t waitCount = _waitCount;
      ++_ticks;
      _changed.notify_all();
      WaitLocked(lock, "watchdog tick", [&] {
        return _waitCount > waitCount && _waiting > 0;
      });
    }

    // lets the watchdog run one loop iteration without waiting for it
    void GrantTick() {
      std::lock_guard<std::mutex> lock(_mutex);
      ++_ticks;
      _changed.notify_all();
    }

    // the next clock read blocks until ReleaseTimestamp()
    void BlockNextTimestamp() {
      std::lock_guard<std::mutex> lock(_mutex);
      _isTimestampArmed = true;
      _isTimestampReleased = false;
    }

    bool WaitUntilTimestampBlocked() {
      std::unique_lock<std::mutex> lock(_mutex);
      return WaitLocked(lock, "blocked clock read", [&] {
        return _isTimestampBlocked;
      });
    }

    void ReleaseTimestamp() {
      std::lock_guard<std::mutex> lock(_mutex);
      _isTimestampReleased = true;
      _changed.notify_all();
    }

    bool WaitUntilStopSignaled() {
      std::unique_lock<std::mutex> lock(_mutex);
      return WaitLocked(lock, "stop signal", [&] { return !_signaled.empty(); });
    }

    // runs while Stop() removes the hook, before it clears the active detector
    void OnRemoveHook(std::function<void()> action) {
      std::lock_guard<std::mutex> lock(_mutex);
      _onRemoveHook = std::move(action);
    }

    // delivers a message as the hooked thread's message loop would, also after the
    // hook was removed
    void Deliver(const MSG& message) {
      MessageCallback onMessage = nullptr;
      {
        std::lock_guard<std::mutex> lock(_mutex);
        onMessage = _onMessage;
      }
      ASSERT_NE(onMessage, nullptr);
      onMessage(message);
    }

    std::vector<MSG> Posts() {
      std::lock_guard<std::mutex> lock(_mutex);
      return _posts;
    }

    size_t Captures() {
      std::lock_guard<std::mutex> lock(_mutex);
      return _captures;
    }

    std::vector<std::string> Errors() {
      std::lock_guard<std::mutex> lock(_mutex);
      return _errors;
    }

    HANDLE CreateStopEvent() override {
      std::lock_guard<std::mutex> lock(_mutex);
      _signaled.clear();
      return reinterpret_cast<HANDLE>(++_nextHandle);
    }

    BOOL SetEvent(HANDLE hEvent) override {
      std::lock_guard<std::mutex> lock(_mutex);
      _signaled.insert(hEvent);
      _changed.notify_all();
      return TRUE;
    }

    DWORD WaitForSingleObject(HANDLE hHandle, DWORD) override {
      std::unique_lock<std::mutex> lock(_mutex);
      ++_waiting;
      ++_waitCount;
      _changed.notify_all();
      bool isWoken = WaitLocked(lock, "watchdog stop", [&] {
        return _signaled.count(hHandle) > 0 || _ticks > 0 || _isReleasingAll;
      });
      --_waiting;
      if (!isWoken || _signaled.count(hHandle) > 0 || _isReleasingAll) {
        return WAIT_OBJECT_0;
      }
      --_ticks;
      return WAIT_TIMEOUT;
    }

    BOOL CloseHandle(HANDLE) override { return TRUE; }
    BOOL IsWindow(HWND hWnd) override { return hWnd == kWindow; }

    DWORD GetWindowThreadProcessId(HWND, DWORD* pProcessId) override {
      *pProcessId = ::GetCurrentProcessId();
      return kUiThreadId;
    }

    bool InstallMessageHook(DWORD, MessageCallback onMessage) override {
      std::lock_guard<std::mutex> lock(_mutex);
      _onMessage = onMessage;
      return true;
    }

    bool RemoveMessageHook() override {
      std::function<void()> action;
      {
        std::lock_guard<std::mutex> lock(_mutex);
        action = std::exchange(_onRemoveHook, nullptr);
      }
      if (action) {
        action();
      }
      return true;
    }

    BOOL PostMessageW(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) override {
      std::lock_guard<std::mutex> lock(_mutex);
      MSG msg{};
      msg.hwnd = hWnd;
      msg.message = message;
      msg.wParam = wParam;
      msg.lParam = lParam;
      _posts.push_back(msg);
      return TRUE;
    }

    std::unique_ptr<std::thread> StartThread(
        std::function<void()> threadProc
    ) override {
      return std::make_unique<std::thread>(std::move(threadProc));
    }

    std::chrono::nanoseconds GetHighPrecisionTimestamp() override {
      std::unique_lock<std::mutex> lock(_mutex);
      if (std::exchange(_isTimestampArmed, false)) {
        _isTimestampBlocked = true;
        _changed.notify_all();
        WaitLocked(lock, "clock release", [&] {
          return _isTimestampReleased || _isReleasingAll;
        });
        _isTimestampBlocked = false;
      }
      return _now;
    }

    bool CaptureStack(
        const std::shared_ptr<ThreadInfo>&, std::vector<uint64_t>& frames
    ) override {
      std::lock_guard<std::mutex> lock(_mutex);
      ++_captures;
      frames = {kHangFrame, kHangFrame + 1};
      return true;
    }

   private:
    template <typename Predicate>
    bool WaitLocked(
        std::unique_lock<std::mutex>& lock, const char* what, Predicate predicate
    ) {
      if (_changed.wait_for(lock, kTimeout, predicate)) {
        return true;
      }
      _errors.push_back(std::string("timed out waiting for ") + what);
      return false;
    }

    std::mutex _mutex;
    std::condition_variable _changed;
    std::vector<std::string> _errors;
    std::chrono::nanoseconds _now{0};
    uintptr_t _nextHandle = 0x10000;
    std::set<HANDLE> _signaled;
    size_t _waiting = 0;
    size_t _waitCount = 0;
    size_t _ticks = 0;
    bool _isTimestampArmed = false;
    bool _isTimestampBlocked = false;
    bool _isTimestampReleased = false;
    bool _isReleasingAll = false;
    MessageCallback _onMessage = nullptr;
    std::function<void()> _onRemoveHook;
    std::vector<MSG> _posts;
    size_t _captures = 0;
  };

  void SetUp() override {
    _previousValuesCount = Sample::ValuesCount;
    Sample::SetValuesCount(1);
    _threadList.AddThread(kUiThreadId, DuplicateCurrentThread());
    _pThread = _threadList.GetThread(kUiThreadId);
    ASSERT_NE(_pThread, nullptr);
    _rumView.Set("detected-view");
  }

  void TearDown() override {
    EXPECT_EQ(UiHangDetector::_instance.load(), nullptr);
    for (const std::string& error : _runtime.Errors()) {
      ADD_FAILURE() << error;
    }
    Sample::SetValuesCount(_previousValuesCount);
  }

  std::unique_ptr<UiHangDetector> MakeDetector(
      uint64_t sessionId, SessionProviders& providers
  ) {
    return std::unique_ptr<UiHangDetector>(new UiHangDetector(
        sessionId, kProbeMessage, &providers.hangs, &_rumView, _runtime
    ));
  }

  // monitors the window and posts the first probe at time base
  std::unique_ptr<UiHangDetector> StartDetector(
      uint64_t sessionId, SessionProviders& providers, std::chrono::milliseconds base
  ) {
    auto detector = MakeDetector(sessionId, providers);
    _runtime.SetTime(base);
    EXPECT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
    _runtime.RunTick();
    return detector;
  }

  // the probe stays unprocessed past the threshold: a hang is detected at base + 200ms
  void DetectHang(std::chrono::milliseconds base) {
    _runtime.SetTime(base + std::chrono::milliseconds(200));
    _runtime.RunTick();
    ASSERT_TRUE(_pThread->IsHangDetected());
  }

  static bool IsProbeObserved(const UiHangDetector& detector) {
    return detector._isProcessed.load();
  }

  // the hang marker and the cached hang are gone, with the providers
  void ExpectSessionCleared(const UiHangDetector& detector) {
    EXPECT_FALSE(_pThread->IsHangDetected());
    EXPECT_FALSE(detector._activeHang.has_value());
    EXPECT_EQ(detector._pThreadInfo, nullptr);
    EXPECT_EQ(detector._pHangProvider, nullptr);
    EXPECT_EQ(UiHangDetector::_instance.load(), nullptr);
  }

  static void ExpectSample(
      Sample& sample,
      UiHangSampleKind kind,
      std::chrono::milliseconds timestamp,
      std::chrono::milliseconds duration,
      const std::string& view
  ) {
    EXPECT_EQ(sample.GetUiHangSampleKind(), kind);
    EXPECT_EQ(sample.GetTimestamp(), timestamp);
    auto values = sample.GetValues();
    ASSERT_EQ(values.size(), 1u);
    EXPECT_EQ(values[0], std::chrono::nanoseconds(duration).count());
    auto frames = sample.GetFrames();
    EXPECT_EQ(
        std::vector<uint64_t>(frames.begin(), frames.end()),
        (std::vector<uint64_t>{kHangFrame, kHangFrame + 1})
    );
    EXPECT_EQ(sample.GetRumViewContext().view_id, view);
  }

  size_t _previousValuesCount = 0;
  ThreadList _threadList;
  std::shared_ptr<ThreadInfo> _pThread;
  FakeRumView _rumView;
  FakeRuntime _runtime;
};

using namespace std::chrono_literals;

TEST_F(UiHangDetectorSessionTests, ProbeCarriesSessionIdAndSequence) {
  SessionProviders providers;
  auto detector = StartDetector(41, providers, 0ms);

  _runtime.Deliver(Probe(41, 1));
  _runtime.RunTick();

  std::vector<MSG> posts = _runtime.Posts();
  ASSERT_EQ(posts.size(), 2u);
  for (size_t i = 0; i < posts.size(); ++i) {
    EXPECT_EQ(posts[i].hwnd, kWindow);
    EXPECT_EQ(posts[i].message, kProbeMessage);
    EXPECT_EQ(posts[i].wParam, 41u);
    EXPECT_EQ(posts[i].lParam, static_cast<LPARAM>(i + 1));
  }
  detector->Stop();
}

TEST_F(
    UiHangDetectorSessionTests, AcknowledgementNeedsWindowMessageSessionAndSequence
) {
  SessionProviders providers;
  auto detector = StartDetector(42, providers, 0ms);

  MSG otherWindow = Probe(42, 1);
  otherWindow.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0xBAD));
  MSG otherMessage = Probe(42, 1);
  ++otherMessage.message;
  for (const MSG& message :
       {otherWindow, otherMessage, Probe(41, 1), Probe(42, 0), Probe(42, 2)}) {
    _runtime.Deliver(message);
    EXPECT_FALSE(IsProbeObserved(*detector));
  }

  _runtime.Deliver(Probe(42, 1));
  EXPECT_TRUE(IsProbeObserved(*detector));
  detector->Stop();
}

TEST_F(UiHangDetectorSessionTests, QueuedProbeOfPreviousSessionIsIgnoredAfterRestart) {
  SessionProviders first;
  auto firstDetector = StartDetector(41, first, 0ms);
  // probe (41, 1) stays queued on the window while session 41 stops
  firstDetector->Stop();
  firstDetector.reset();
  _runtime.Deliver(Probe(41, 1));

  SessionProviders second;
  auto secondDetector = StartDetector(42, second, 1000ms);
  _runtime.Deliver(Probe(41, 1));
  EXPECT_FALSE(IsProbeObserved(*secondDetector));

  // the stale probe did not acknowledge session 42: its own probe hangs
  DetectHang(1000ms);
  _runtime.Deliver(Probe(42, 1));
  _runtime.RunTick();
  EXPECT_FALSE(_pThread->IsHangDetected());
  std::vector<Sample> samples = second.Drain();
  ASSERT_EQ(samples.size(), 2u);
  EXPECT_EQ(samples[0].GetUiHangSampleKind(), UiHangSampleKind::Detected);
  EXPECT_EQ(samples[1].GetUiHangSampleKind(), UiHangSampleKind::Recovered);
  EXPECT_TRUE(first.Drain().empty());

  secondDetector->Stop();
  EXPECT_TRUE(second.Drain().empty());
}

TEST_F(UiHangDetectorSessionTests, StopBeforeDetectionCommitEmitsNoHang) {
  SessionProviders providers;
  auto detector = StartDetector(41, providers, 0ms);

  // the watchdog has seen the threshold reached but has not committed the hang yet
  _runtime.SetTime(200ms);
  _runtime.BlockNextTimestamp();
  _runtime.GrantTick();
  ASSERT_TRUE(_runtime.WaitUntilTimestampBlocked());
  auto stop = std::async(std::launch::async, [&] { detector->Stop(); });
  ASSERT_TRUE(_runtime.WaitUntilStopSignaled());
  EXPECT_EQ(stop.wait_for(kBlockedDuration), std::future_status::timeout);
  _runtime.ReleaseTimestamp();
  stop.get();

  EXPECT_TRUE(providers.Drain().empty());
  EXPECT_EQ(_runtime.Captures(), 0u);
  ExpectSessionCleared(*detector);
}

TEST_F(
    UiHangDetectorSessionTests, AcknowledgementBeforeCutoffClosesHangAtItsTimestamp
) {
  SessionProviders providers;
  auto detector = StartDetector(41, providers, 0ms);
  DetectHang(0ms);

  // processed before the cutoff, but not yet seen by the watchdog
  _runtime.SetTime(300ms);
  _runtime.Deliver(Probe(41, 1));
  _rumView.Set("stop-view");
  _runtime.SetTime(500ms);
  detector->Stop();

  std::vector<Sample> samples = providers.Drain();
  ASSERT_EQ(samples.size(), 2u);
  ExpectSample(samples[0], UiHangSampleKind::Detected, 200ms, 200ms, "detected-view");
  ExpectSample(samples[1], UiHangSampleKind::Recovered, 300ms, 100ms, "detected-view");
  // no capture during Stop
  EXPECT_EQ(_runtime.Captures(), 1u);
  EXPECT_EQ(_pThread->SetLastWaitSampleTimestamp(0ns), 300ms);
  ExpectSessionCleared(*detector);
}

TEST_F(UiHangDetectorSessionTests, AcknowledgementSeenByWatchdogClosesHangOnce) {
  SessionProviders providers;
  auto detector = StartDetector(41, providers, 0ms);
  DetectHang(0ms);

  _runtime.SetTime(300ms);
  _runtime.Deliver(Probe(41, 1));
  _runtime.RunTick();
  EXPECT_FALSE(_pThread->IsHangDetected());
  _runtime.SetTime(500ms);
  detector->Stop();

  std::vector<Sample> samples = providers.Drain();
  ASSERT_EQ(samples.size(), 2u);
  EXPECT_EQ(samples[0].GetUiHangSampleKind(), UiHangSampleKind::Detected);
  EXPECT_EQ(samples[1].GetUiHangSampleKind(), UiHangSampleKind::Recovered);
  EXPECT_EQ(samples[1].GetTimestamp(), 300ms);
  EXPECT_EQ(_runtime.Captures(), 2u);
  ExpectSessionCleared(*detector);
}

TEST_F(UiHangDetectorSessionTests, HangActiveAtCutoffEndsWithCachedProfilerStopSample) {
  SessionProviders providers;
  auto detector = StartDetector(41, providers, 0ms);
  DetectHang(0ms);
  _rumView.Set("stop-view");

  // an acknowledgement processed after the cutoff, while Stop removes the hook, still
  // reaches the detector but does not end the hang
  bool isLateProbeObserved = false;
  _runtime.OnRemoveHook([&] {
    _runtime.SetTime(600ms);
    _runtime.Deliver(Probe(41, 1));
    isLateProbeObserved = IsProbeObserved(*detector);
  });
  _runtime.SetTime(500ms);
  detector->Stop();
  EXPECT_TRUE(isLateProbeObserved);

  std::vector<Sample> samples = providers.Drain();
  ASSERT_EQ(samples.size(), 2u);
  ExpectSample(samples[0], UiHangSampleKind::Detected, 200ms, 200ms, "detected-view");
  ExpectSample(
      samples[1], UiHangSampleKind::ProfilerStop, 500ms, 300ms, "detected-view"
  );
  // the terminal sample comes from the detection capture
  EXPECT_EQ(_runtime.Captures(), 1u);
  EXPECT_EQ(_pThread->SetLastWaitSampleTimestamp(0ns), 500ms);
  ExpectSessionCleared(*detector);

  detector->Stop();
  EXPECT_TRUE(providers.Drain().empty());
}

TEST_F(UiHangDetectorSessionTests, RepeatedSessionsOnSameWindowStayIndependent) {
  std::vector<std::unique_ptr<SessionProviders>> sessions;
  for (uint64_t sessionId = 1; sessionId <= 3; ++sessionId) {
    auto base = std::chrono::milliseconds(1000 * sessionId);
    sessions.push_back(std::make_unique<SessionProviders>());
    SessionProviders& providers = *sessions.back();
    auto detector = StartDetector(sessionId, providers, base);

    // probes of the stopped sessions reach the hook but acknowledge nothing
    for (uint64_t previous = 1; previous < sessionId; ++previous) {
      _runtime.Deliver(Probe(previous, 1));
    }
    EXPECT_FALSE(IsProbeObserved(*detector));
    DetectHang(base);
    _runtime.SetTime(base + 500ms);
    detector->Stop();
    ExpectSessionCleared(*detector);
    detector.reset();
    _runtime.Deliver(Probe(sessionId, 1));

    std::vector<Sample> samples = providers.Drain();
    ASSERT_EQ(samples.size(), 2u);
    EXPECT_EQ(samples[0].GetUiHangSampleKind(), UiHangSampleKind::Detected);
    EXPECT_EQ(samples[1].GetUiHangSampleKind(), UiHangSampleKind::ProfilerStop);
    EXPECT_EQ(samples[1].GetTimestamp(), base + 500ms);
    for (auto& session : sessions) {
      EXPECT_TRUE(session->Drain().empty());
    }
  }
  EXPECT_EQ(_runtime.Captures(), 3u);
}
