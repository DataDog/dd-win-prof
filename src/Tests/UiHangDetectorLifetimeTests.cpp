// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <gtest/gtest.h>

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <ostream>
#include <string>
#include <system_error>
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
constexpr uint64_t kSessionId = 7;
constexpr DWORD kUiThreadId = 0x4242;
constexpr DWORD kUnmonitoredThreadId = 0x4343;

// bound for steps that complete at once when the detector behaves
constexpr auto kTimeout = std::chrono::seconds(10);
// time given to an operation that must stay blocked
constexpr auto kBlockedDuration = std::chrono::milliseconds(100);

// runtime calls that acquire, use or release the detector resources
enum class Op {
  CreateStopEvent,
  IsWindow,
  GetWindowThreadProcessId,
  InstallMessageHook,
  StartThread,
  PostMessageW,
  SetEvent,
  WatchdogExit,
  RemoveMessageHook,
  CloseHandle,
};

std::ostream& operator<<(std::ostream& os, Op op) {
  switch (op) {
    case Op::CreateStopEvent:
      return os << "CreateStopEvent";
    case Op::IsWindow:
      return os << "IsWindow";
    case Op::GetWindowThreadProcessId:
      return os << "GetWindowThreadProcessId";
    case Op::InstallMessageHook:
      return os << "InstallMessageHook";
    case Op::StartThread:
      return os << "StartThread";
    case Op::PostMessageW:
      return os << "PostMessageW";
    case Op::SetEvent:
      return os << "SetEvent";
    case Op::WatchdogExit:
      return os << "WatchdogExit";
    case Op::RemoveMessageHook:
      return os << "RemoveMessageHook";
    case Op::CloseHandle:
      return os << "CloseHandle";
  }
  return os << "Op(" << static_cast<int>(op) << ")";
}

// the watchdog is gone before the hook, so it never sees a probe left unprocessed by
// the teardown
std::vector<Op> TeardownOps() {
  return {Op::SetEvent, Op::WatchdogExit, Op::RemoveMessageHook, Op::CloseHandle};
}

// runtime call made to fail by FakeRuntime
enum class Failure {
  None,
  StopEvent,
  InvalidWindow,
  WindowThread,
  ForeignWindow,
  UnmonitoredThread,
  Hook,
  HookThrows,
  Watchdog,
  WatchdogNull,
  WatchdogUnjoinable,
  StopSignal,
  Wait,
};

// the detector's latest probe: sequence 0 before the first post
MSG ProbeMessage(uint64_t sequence = 0) {
  MSG msg{};
  msg.hwnd = kWindow;
  msg.message = kProbeMessage;
  msg.wParam = static_cast<WPARAM>(kSessionId);
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

class NoRumView : public IRumViewContextProvider {
 public:
  bool GetCurrentViewContext(RumViewContext&) const override { return false; }
};
}  // namespace

class UiHangDetectorLifetimeTests : public ::testing::Test {
 protected:
  // State observed by a hook notification starting now.
  struct HookView {
    bool isLockFree = false;
    const UiHangDetector* pDetector = nullptr;
  };

  struct Call {
    Op op;
    HookView hook;
  };

  struct HookCallResult {
    bool isCompleted = false;
    bool reachedDetector = false;
  };

  // Records the runtime calls with the hook state they observed, fails the requested
  // call and parks callers at points chosen by the test. Its stop event only wakes up
  // when signaled or when the test grants a watchdog tick, and its clock never
  // advances, so no hang is ever detected.
  class FakeRuntime final : public IUiHangDetectorRuntime {
   public:
    ~FakeRuntime() {
      ReleaseAll();
      JoinHookCaller();
    }

    void Fail(Failure failure) {
      std::lock_guard<std::mutex> lock(_mutex);
      _failure = failure;
    }

    void FailNextPost() {
      std::lock_guard<std::mutex> lock(_mutex);
      _failNextPost = true;
    }

    // the watchdog runs one loop iteration before StartThread returns
    void TickWatchdogOnStart() {
      std::lock_guard<std::mutex> lock(_mutex);
      _tickWatchdogOnStart = true;
    }

    // another thread delivers a message while the hook is being removed
    void CallHookDuringRemoval() {
      std::lock_guard<std::mutex> lock(_mutex);
      _callHookDuringRemoval = true;
    }

    void BlockNextTimestamp() { Arm(_timestampGate); }
    void BlockNextPost() { Arm(_postGate); }
    bool WaitUntilTimestampBlocked() { return WaitUntilBlocked(_timestampGate); }
    bool WaitUntilPostBlocked() { return WaitUntilBlocked(_postGate); }
    void ReleaseTimestamp() { Release(_timestampGate); }
    void ReleasePost() { Release(_postGate); }

    // unblocks every caller and lets the watchdog exit
    void ReleaseAll() {
      std::lock_guard<std::mutex> lock(_mutex);
      _isReleasingAll = true;
      _changed.notify_all();
    }

    // lets the watchdog run one loop iteration and waits until it waits again
    void RunWatchdogTick() {
      std::unique_lock<std::mutex> lock(_mutex);
      if (!WaitLocked(lock, "watchdog wait", [&] { return _waitingCount > 0; })) {
        return;
      }
      size_t waitCount = _waitCount;
      ++_ticks;
      _changed.notify_all();
      WaitLocked(lock, "watchdog tick", [&] {
        return _waitCount > waitCount && _waitingCount > 0;
      });
    }

    // lets the watchdog run one loop iteration without waiting for it
    void GrantWatchdogTick() {
      std::lock_guard<std::mutex> lock(_mutex);
      ++_ticks;
      _changed.notify_all();
    }

    bool WaitUntilCalled(Op op) {
      std::unique_lock<std::mutex> lock(_mutex);
      return WaitLocked(lock, "runtime call", [&] {
        return std::any_of(_calls.begin(), _calls.end(), [op](const Call& call) {
          return call.op == op;
        });
      });
    }

    // delivers a message as the hooked thread's message loop would; like Windows, it
    // can still deliver one after the hook was removed
    void InvokeHook(const MSG& message) {
      MessageCallback onMessage = nullptr;
      {
        std::lock_guard<std::mutex> lock(_mutex);
        onMessage = _onMessage;
        if (onMessage == nullptr) {
          _errors.push_back("no message hook was installed");
          return;
        }
      }
      onMessage(message);
    }

    void JoinHookCaller() {
      if (_hookCaller.joinable()) {
        _hookCaller.join();
      }
    }

    void ClearCalls() {
      std::lock_guard<std::mutex> lock(_mutex);
      _calls.clear();
    }

    std::vector<Call> Calls() {
      std::lock_guard<std::mutex> lock(_mutex);
      return _calls;
    }

    std::vector<Op> Ops() {
      std::lock_guard<std::mutex> lock(_mutex);
      std::vector<Op> ops;
      for (const Call& call : _calls) {
        ops.push_back(call.op);
      }
      return ops;
    }

    std::vector<std::pair<HWND, UINT>> Posts() {
      std::lock_guard<std::mutex> lock(_mutex);
      return _posts;
    }

    DWORD HookThreadId() {
      std::lock_guard<std::mutex> lock(_mutex);
      return _hookThreadId;
    }

    size_t TimestampCalls() {
      std::lock_guard<std::mutex> lock(_mutex);
      return _timestampCalls;
    }

    size_t LiveEvents() {
      std::lock_guard<std::mutex> lock(_mutex);
      return static_cast<size_t>(
          std::count_if(_events.begin(), _events.end(), [](const auto& event) {
            return !event.second.isClosed;
          })
      );
    }

    size_t LiveHooks() {
      std::lock_guard<std::mutex> lock(_mutex);
      return _isHookInstalled ? 1u : 0u;
    }

    size_t RunningThreads() {
      std::lock_guard<std::mutex> lock(_mutex);
      return _startedThreads - _exitedThreads;
    }

    HookCallResult HookCallDuringRemoval() {
      std::lock_guard<std::mutex> lock(_mutex);
      return _hookCallDuringRemoval;
    }

    std::vector<std::string> Errors() {
      std::lock_guard<std::mutex> lock(_mutex);
      return _errors;
    }

    HANDLE CreateStopEvent() override {
      Record(Op::CreateStopEvent);
      std::lock_guard<std::mutex> lock(_mutex);
      if (_failure == Failure::StopEvent) {
        return nullptr;
      }
      HANDLE hEvent = reinterpret_cast<HANDLE>(NextHandleValue());
      _events[hEvent] = EventState{};
      return hEvent;
    }

    BOOL SetEvent(HANDLE hEvent) override {
      Record(Op::SetEvent);
      std::lock_guard<std::mutex> lock(_mutex);
      EventState* pEvent = FindEvent(hEvent, "SetEvent");
      if (pEvent == nullptr) {
        return FALSE;
      }
      if (_failure == Failure::StopSignal) {
        // the event stays non-signaled: the watchdog only wakes up on a wait timeout
        ++_ticks;
        _changed.notify_all();
        ::SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
      }
      pEvent->isSignaled = true;
      _changed.notify_all();
      return TRUE;
    }

    // reports WAIT_OBJECT_0 instead of failures and timeouts so that a broken detector
    // fails the test instead of spinning or hanging its watchdog
    DWORD WaitForSingleObject(HANDLE hHandle, DWORD) override {
      std::unique_lock<std::mutex> lock(_mutex);
      if (_failure == Failure::Wait) {
        _failure = Failure::None;
        ::SetLastError(ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
      }
      EventState* pEvent = FindEvent(hHandle, "WaitForSingleObject");
      if (pEvent == nullptr) {
        return WAIT_OBJECT_0;
      }
      ++pEvent->waiters;
      ++_waitingCount;
      ++_waitCount;
      _changed.notify_all();
      bool isWoken = WaitLocked(lock, "watchdog stop", [&] {
        return pEvent->isSignaled || _ticks > 0 || _isReleasingAll;
      });
      --pEvent->waiters;
      --_waitingCount;
      if (pEvent->isSignaled || !isWoken || _isReleasingAll) {
        return WAIT_OBJECT_0;
      }
      --_ticks;
      return WAIT_TIMEOUT;
    }

    BOOL CloseHandle(HANDLE hHandle) override {
      Record(Op::CloseHandle);
      std::lock_guard<std::mutex> lock(_mutex);
      EventState* pEvent = FindEvent(hHandle, "CloseHandle");
      if (pEvent == nullptr) {
        return FALSE;
      }
      if (pEvent->waiters > 0) {
        _errors.push_back("stop event closed while the watchdog waits on it");
      }
      pEvent->isClosed = true;
      return TRUE;
    }

    BOOL IsWindow(HWND hWnd) override {
      Record(Op::IsWindow);
      std::lock_guard<std::mutex> lock(_mutex);
      return (hWnd == kWindow) && (_failure != Failure::InvalidWindow);
    }

    DWORD GetWindowThreadProcessId(HWND, DWORD* pProcessId) override {
      Record(Op::GetWindowThreadProcessId);
      std::lock_guard<std::mutex> lock(_mutex);
      if (_failure == Failure::WindowThread) {
        return 0;
      }
      DWORD pid = ::GetCurrentProcessId();
      *pProcessId = (_failure == Failure::ForeignWindow) ? pid + 4 : pid;
      return (_failure == Failure::UnmonitoredThread) ? kUnmonitoredThreadId
                                                      : kUiThreadId;
    }

    bool InstallMessageHook(DWORD threadId, MessageCallback onMessage) override {
      Record(Op::InstallMessageHook);
      std::lock_guard<std::mutex> lock(_mutex);
      if (_failure == Failure::HookThrows) {
        throw std::runtime_error("injected hook failure");
      }
      if (_failure == Failure::Hook) {
        return false;
      }
      if (_isHookInstalled) {
        _errors.push_back("InstallMessageHook while a hook is installed");
        return false;
      }
      _isHookInstalled = true;
      _onMessage = onMessage;
      _hookThreadId = threadId;
      return true;
    }

    bool RemoveMessageHook() override {
      Record(Op::RemoveMessageHook);
      bool callHook = false;
      {
        std::lock_guard<std::mutex> lock(_mutex);
        callHook = std::exchange(_callHookDuringRemoval, false);
      }
      if (callHook) {
        CallHookFromAnotherThread();
      }

      std::lock_guard<std::mutex> lock(_mutex);
      if (!_isHookInstalled) {
        _errors.push_back("RemoveMessageHook without an installed hook");
        return false;
      }
      _isHookInstalled = false;
      return true;
    }

    BOOL PostMessageW(HWND hWnd, UINT message, WPARAM, LPARAM) override {
      Record(Op::PostMessageW);
      std::unique_lock<std::mutex> lock(_mutex);
      _posts.emplace_back(hWnd, message);
      PassGate(lock, _postGate);
      return !std::exchange(_failNextPost, false);
    }

    std::unique_ptr<std::thread> StartThread(
        std::function<void()> threadProc
    ) override {
      Record(Op::StartThread);
      bool tickWatchdog = false;
      {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_failure == Failure::Watchdog) {
          throw std::system_error(
              std::make_error_code(std::errc::resource_unavailable_try_again)
          );
        }
        if (_failure == Failure::WatchdogNull) {
          return nullptr;
        }
        if (_failure == Failure::WatchdogUnjoinable) {
          return std::make_unique<std::thread>();
        }
        ++_startedThreads;
        tickWatchdog = std::exchange(_tickWatchdogOnStart, false);
      }

      auto pThread =
          std::make_unique<std::thread>([this, threadProc = std::move(threadProc)] {
            threadProc();
            Record(Op::WatchdogExit);
            std::lock_guard<std::mutex> lock(_mutex);
            ++_exitedThreads;
          });
      if (tickWatchdog) {
        RunWatchdogTick();
      }
      return pThread;
    }

    std::chrono::nanoseconds GetHighPrecisionTimestamp() override {
      std::unique_lock<std::mutex> lock(_mutex);
      ++_timestampCalls;
      PassGate(lock, _timestampGate);
      return std::chrono::seconds(1);
    }

    // never called: the clock never advances
    bool CaptureStack(
        const std::shared_ptr<ThreadInfo>&, std::vector<uint64_t>&
    ) override {
      std::lock_guard<std::mutex> lock(_mutex);
      _errors.push_back("unexpected stack capture");
      return false;
    }

   private:
    struct EventState {
      bool isSignaled = false;
      bool isClosed = false;
      int waiters = 0;
    };

    // a caller reaching an armed gate blocks there until the test releases it
    struct Gate {
      bool isArmed = false;
      bool isBlocked = false;
      bool isReleased = false;
    };

    void Record(Op op) {
      HookView hook = ObserveHook();
      std::lock_guard<std::mutex> lock(_mutex);
      _calls.push_back({op, hook});
      _changed.notify_all();
    }

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

    void Arm(Gate& gate) {
      std::lock_guard<std::mutex> lock(_mutex);
      gate = Gate{true, false, false};
    }

    bool WaitUntilBlocked(Gate& gate) {
      std::unique_lock<std::mutex> lock(_mutex);
      return WaitLocked(lock, "blocked call", [&] { return gate.isBlocked; });
    }

    void Release(Gate& gate) {
      std::lock_guard<std::mutex> lock(_mutex);
      gate.isReleased = true;
      _changed.notify_all();
    }

    void PassGate(std::unique_lock<std::mutex>& lock, Gate& gate) {
      if (!gate.isArmed) {
        return;
      }
      gate.isArmed = false;
      gate.isBlocked = true;
      _changed.notify_all();
      WaitLocked(lock, "gate release", [&] {
        return gate.isReleased || _isReleasingAll;
      });
      gate.isBlocked = false;
    }

    EventState* FindEvent(HANDLE hHandle, const char* caller) {
      auto event = _events.find(hHandle);
      if (event == _events.end() || event->second.isClosed) {
        _errors.push_back(std::string(caller) + " on a closed or unknown handle");
        return nullptr;
      }
      return &event->second;
    }

    uintptr_t NextHandleValue() {
      _nextHandleValue += 4;
      return _nextHandleValue;
    }

    // a stuck callback is joined only after the detector released the instance mutex
    void CallHookFromAnotherThread() {
      JoinHookCaller();
      size_t timestampCalls = TimestampCalls();
      {
        std::lock_guard<std::mutex> lock(_mutex);
        _hookCallDuringRemoval = {};
      }
      _hookCaller = std::thread([this] {
        InvokeHook(ProbeMessage());
        std::lock_guard<std::mutex> lock(_mutex);
        _hookCallDuringRemoval.isCompleted = true;
        _changed.notify_all();
      });

      std::unique_lock<std::mutex> lock(_mutex);
      if (WaitLocked(lock, "hook callback during removal", [&] {
            return _hookCallDuringRemoval.isCompleted;
          })) {
        // only the detector reads the clock once the watchdog has exited
        _hookCallDuringRemoval.reachedDetector = _timestampCalls != timestampCalls;
      }
    }

    std::mutex _mutex;
    std::condition_variable _changed;
    Failure _failure = Failure::None;
    std::vector<Call> _calls;
    std::vector<std::string> _errors;
    uintptr_t _nextHandleValue = 0x10000;
    std::map<HANDLE, EventState> _events;
    bool _isHookInstalled = false;
    MessageCallback _onMessage = nullptr;
    DWORD _hookThreadId = 0;
    std::vector<std::pair<HWND, UINT>> _posts;
    bool _failNextPost = false;
    size_t _startedThreads = 0;
    size_t _exitedThreads = 0;
    bool _tickWatchdogOnStart = false;
    size_t _waitingCount = 0;
    size_t _waitCount = 0;
    size_t _ticks = 0;
    size_t _timestampCalls = 0;
    Gate _timestampGate;
    Gate _postGate;
    bool _isReleasingAll = false;
    bool _callHookDuringRemoval = false;
    HookCallResult _hookCallDuringRemoval;
    std::thread _hookCaller;
  };

  void SetUp() override {
    _threadList.AddThread(kUiThreadId, DuplicateCurrentThread());
  }

  void TearDown() override {
    _runtime.JoinHookCaller();
    if (ObserveHook().pDetector != nullptr) {
      ADD_FAILURE() << "an active detector remains";
      ClearInstance();
    }
    EXPECT_EQ(_runtime.LiveEvents(), 0u);
    EXPECT_EQ(_runtime.LiveHooks(), 0u);
    EXPECT_EQ(_runtime.RunningThreads(), 0u);
    for (const std::string& error : _runtime.Errors()) {
      ADD_FAILURE() << error;
    }
  }

  std::unique_ptr<UiHangDetector> MakeDetector() {
    return std::unique_ptr<UiHangDetector>(new UiHangDetector(
        kSessionId, kProbeMessage, &_hangProvider, &_rumViewProvider, _runtime
    ));
  }

  static HookView ObserveHook() {
    // probed from another thread: a std::mutex can't be try-locked by its owner
    std::mutex& instanceMutex = UiHangDetector::_instanceMutex;
    HookView view;
    view.pDetector = UiHangDetector::_instance.load(std::memory_order_acquire);
    std::thread observer([&] {
      std::unique_lock<std::mutex> lock(instanceMutex, std::try_to_lock);
      if (lock.owns_lock()) {
        view.isLockFree = true;
      }
    });
    observer.join();
    return view;
  }

  static void ClearInstance() {
    std::lock_guard<std::mutex> lock(UiHangDetector::_instanceMutex);
    UiHangDetector::_instance.store(nullptr, std::memory_order_release);
  }

  static bool IsProbeObserved(const UiHangDetector& detector) {
    return detector._isProcessed.load();
  }

  static bool HasProviders(const UiHangDetector& detector) {
    return detector._pHangProvider != nullptr &&
           detector._pRumViewContextProvider != nullptr;
  }

  // no active detector, hook, watchdog, event, window or thread is left behind
  static void ExpectReleased(const UiHangDetector& detector) {
    EXPECT_NE(ObserveHook().pDetector, &detector);
    EXPECT_FALSE(detector._isHookInstalled);
    EXPECT_EQ(detector._pWatchdogThread, nullptr);
    EXPECT_EQ(detector._stopEvent, nullptr);
    EXPECT_EQ(detector._hWnd, nullptr);
    EXPECT_EQ(detector._pThreadInfo, nullptr);
  }

  // the teardown never holds the instance mutex and clears _instance before closing
  // the stop event
  void ExpectTeardownWithoutInstanceMutex() {
    EXPECT_EQ(_runtime.Ops(), TeardownOps());
    std::vector<Call> calls = _runtime.Calls();
    for (const Call& call : calls) {
      EXPECT_TRUE(call.hook.isLockFree) << call.op;
    }
    ASSERT_FALSE(calls.empty());
    EXPECT_EQ(calls.back().hook.pDetector, nullptr);
  }

  // Fails activation at one stage, then checks that the stages before it were released
  // without the instance mutex and that the same detector activates once the failure
  // is gone.
  void ExpectRollbackAndRetry(Failure failure, const std::vector<Op>& expectedOps) {
    auto detector = MakeDetector();
    _runtime.Fail(failure);

    EXPECT_FALSE(detector->MonitorWindowHangs(kWindow, &_threadList));

    EXPECT_EQ(_runtime.Ops(), expectedOps);
    for (const Call& call : _runtime.Calls()) {
      EXPECT_TRUE(call.hook.isLockFree) << call.op;
      EXPECT_EQ(call.hook.pDetector, nullptr) << call.op;
    }
    EXPECT_EQ(_runtime.LiveEvents(), 0u);
    EXPECT_EQ(_runtime.LiveHooks(), 0u);
    EXPECT_EQ(_runtime.RunningThreads(), 0u);
    ExpectReleased(*detector);
    EXPECT_TRUE(HasProviders(*detector));

    _runtime.Fail(Failure::None);
    EXPECT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
    EXPECT_EQ(ObserveHook().pDetector, detector.get());

    detector->Stop();
    ExpectReleased(*detector);
  }

  SampleValueTypeProvider _valueTypes;
  UiHangProvider _hangProvider{_valueTypes};
  NoRumView _rumViewProvider;
  ThreadList _threadList;
  FakeRuntime _runtime;
};

TEST_F(UiHangDetectorLifetimeTests, UnusedDetectorTouchesNothing) {
  auto detector = MakeDetector();
  EXPECT_EQ(ObserveHook().pDetector, nullptr);

  detector->Stop();
  detector->Stop();
  detector.reset();

  EXPECT_TRUE(_runtime.Ops().empty());
}

TEST_F(UiHangDetectorLifetimeTests, PublicConstructorUsesProductionRuntime) {
  UiHangDetector detector(kSessionId, kProbeMessage, &_hangProvider, &_rumViewProvider);
  detector.Stop();
}

TEST_F(UiHangDetectorLifetimeTests, ActivationSetsInstanceAfterEveryStageSucceeded) {
  auto detector = MakeDetector();

  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));

  EXPECT_EQ(
      _runtime.Ops(),
      (std::vector<Op>{
          Op::CreateStopEvent,
          Op::IsWindow,
          Op::GetWindowThreadProcessId,
          Op::InstallMessageHook,
          Op::StartThread
      })
  );
  for (const Call& call : _runtime.Calls()) {
    EXPECT_TRUE(call.hook.isLockFree) << call.op;
    EXPECT_EQ(call.hook.pDetector, nullptr) << "active before " << call.op;
  }
  EXPECT_EQ(ObserveHook().pDetector, detector.get());
  EXPECT_EQ(_runtime.HookThreadId(), kUiThreadId);

  // an active detector rejects another window without touching its resources
  _runtime.ClearCalls();
  EXPECT_FALSE(detector->MonitorWindowHangs(kWindow, &_threadList));
  EXPECT_TRUE(_runtime.Ops().empty());
  EXPECT_EQ(ObserveHook().pDetector, detector.get());

  detector->Stop();
}

TEST_F(UiHangDetectorLifetimeTests, NullThreadListIsRejectedBeforeActivation) {
  auto detector = MakeDetector();
  EXPECT_FALSE(detector->MonitorWindowHangs(kWindow, nullptr));
  EXPECT_TRUE(_runtime.Ops().empty());
}

TEST_F(UiHangDetectorLifetimeTests, StopEventFailureLeavesNothingAndRetrySucceeds) {
  ExpectRollbackAndRetry(Failure::StopEvent, {Op::CreateStopEvent});
}

TEST_F(UiHangDetectorLifetimeTests, InvalidWindowClosesEventAndRetrySucceeds) {
  ExpectRollbackAndRetry(
      Failure::InvalidWindow, {Op::CreateStopEvent, Op::IsWindow, Op::CloseHandle}
  );
}

TEST_F(UiHangDetectorLifetimeTests, WindowThreadFailureClosesEventAndRetrySucceeds) {
  ExpectRollbackAndRetry(
      Failure::WindowThread,
      {Op::CreateStopEvent, Op::IsWindow, Op::GetWindowThreadProcessId, Op::CloseHandle}
  );
}

TEST_F(UiHangDetectorLifetimeTests, ForeignWindowClosesEventAndRetrySucceeds) {
  ExpectRollbackAndRetry(
      Failure::ForeignWindow,
      {Op::CreateStopEvent, Op::IsWindow, Op::GetWindowThreadProcessId, Op::CloseHandle}
  );
}

TEST_F(UiHangDetectorLifetimeTests, UnmonitoredThreadClosesEventAndRetrySucceeds) {
  ExpectRollbackAndRetry(
      Failure::UnmonitoredThread,
      {Op::CreateStopEvent, Op::IsWindow, Op::GetWindowThreadProcessId, Op::CloseHandle}
  );
}

TEST_F(UiHangDetectorLifetimeTests, HookFailureClosesEventAndRetrySucceeds) {
  ExpectRollbackAndRetry(
      Failure::Hook,
      {Op::CreateStopEvent,
       Op::IsWindow,
       Op::GetWindowThreadProcessId,
       Op::InstallMessageHook,
       Op::CloseHandle}
  );
}

TEST_F(UiHangDetectorLifetimeTests, ThrowingHookRollsBackAndRetrySucceeds) {
  ExpectRollbackAndRetry(
      Failure::HookThrows,
      {Op::CreateStopEvent,
       Op::IsWindow,
       Op::GetWindowThreadProcessId,
       Op::InstallMessageHook,
       Op::CloseHandle}
  );
}

TEST_F(UiHangDetectorLifetimeTests, WatchdogFailureRemovesHookAndRetrySucceeds) {
  ExpectRollbackAndRetry(
      Failure::Watchdog,
      {Op::CreateStopEvent,
       Op::IsWindow,
       Op::GetWindowThreadProcessId,
       Op::InstallMessageHook,
       Op::StartThread,
       Op::RemoveMessageHook,
       Op::CloseHandle}
  );
}

TEST_F(UiHangDetectorLifetimeTests, NullWatchdogRollsBackAndRetrySucceeds) {
  ExpectRollbackAndRetry(
      Failure::WatchdogNull,
      {Op::CreateStopEvent,
       Op::IsWindow,
       Op::GetWindowThreadProcessId,
       Op::InstallMessageHook,
       Op::StartThread,
       Op::RemoveMessageHook,
       Op::CloseHandle}
  );
}

TEST_F(UiHangDetectorLifetimeTests, UnjoinableWatchdogRollsBackAndRetrySucceeds) {
  ExpectRollbackAndRetry(
      Failure::WatchdogUnjoinable,
      {Op::CreateStopEvent,
       Op::IsWindow,
       Op::GetWindowThreadProcessId,
       Op::InstallMessageHook,
       Op::StartThread,
       Op::RemoveMessageHook,
       Op::CloseHandle}
  );
}

TEST_F(UiHangDetectorLifetimeTests, SecondDetectorIsRejectedWhileAnotherIsActive) {
  auto active = MakeDetector();
  ASSERT_TRUE(active->MonitorWindowHangs(kWindow, &_threadList));
  _runtime.ClearCalls();

  auto detector = MakeDetector();
  EXPECT_FALSE(detector->MonitorWindowHangs(kWindow, &_threadList));

  EXPECT_TRUE(_runtime.Ops().empty());
  ExpectReleased(*detector);
  EXPECT_TRUE(HasProviders(*detector));
  // the active detector keeps its hook, watchdog and event
  EXPECT_EQ(ObserveHook().pDetector, active.get());
  EXPECT_EQ(_runtime.LiveHooks(), 1u);
  EXPECT_EQ(_runtime.RunningThreads(), 1u);
  EXPECT_EQ(_runtime.LiveEvents(), 1u);

  active->Stop();
  EXPECT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
  EXPECT_EQ(ObserveHook().pDetector, detector.get());

  detector->Stop();
}

TEST_F(UiHangDetectorLifetimeTests, StopTearsDownWithoutInstanceMutex) {
  auto detector = MakeDetector();
  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
  _runtime.ClearCalls();
  _runtime.CallHookDuringRemoval();

  detector->Stop();

  ExpectTeardownWithoutInstanceMutex();
  // a message delivered while the hook is removed still reaches the live detector
  HookCallResult hookCall = _runtime.HookCallDuringRemoval();
  EXPECT_TRUE(hookCall.isCompleted);
  EXPECT_TRUE(hookCall.reachedDetector);
  ExpectReleased(*detector);
  EXPECT_FALSE(HasProviders(*detector));
}

TEST_F(UiHangDetectorLifetimeTests, FailedStopSignalStillStopsWatchdog) {
  auto detector = MakeDetector();
  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
  _runtime.RunWatchdogTick();
  _runtime.ClearCalls();
  _runtime.Fail(Failure::StopSignal);

  detector->Stop();

  ExpectTeardownWithoutInstanceMutex();
  ExpectReleased(*detector);
}

TEST_F(UiHangDetectorLifetimeTests, FailedWaitExitsWatchdog) {
  auto detector = MakeDetector();
  _runtime.Fail(Failure::Wait);

  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
  ASSERT_TRUE(_runtime.WaitUntilCalled(Op::WatchdogExit));
  detector->Stop();

  ExpectReleased(*detector);
}

TEST_F(UiHangDetectorLifetimeTests, StopWaitsForRunningHookCallback) {
  auto detector = MakeDetector();
  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
  _runtime.ClearCalls();

  // the callback parks inside the detector while holding the instance mutex
  _runtime.BlockNextTimestamp();
  auto callback =
      std::async(std::launch::async, [&] { _runtime.InvokeHook(ProbeMessage()); });
  ASSERT_TRUE(_runtime.WaitUntilTimestampBlocked());

  auto stop = std::async(std::launch::async, [&] { detector->Stop(); });
  // the watchdog and the hook are gone, but the detector is still in use
  EXPECT_TRUE(_runtime.WaitUntilCalled(Op::RemoveMessageHook));
  EXPECT_EQ(stop.wait_for(kBlockedDuration), std::future_status::timeout);
  EXPECT_EQ(
      _runtime.Ops(),
      (std::vector<Op>{Op::SetEvent, Op::WatchdogExit, Op::RemoveMessageHook})
  );

  _runtime.ReleaseTimestamp();
  callback.get();
  stop.get();

  // the callback completed on the live detector before it was released
  EXPECT_TRUE(IsProbeObserved(*detector));
  EXPECT_EQ(_runtime.Ops(), TeardownOps());
  ExpectReleased(*detector);
}

TEST_F(UiHangDetectorLifetimeTests, DestructionWaitsForRunningHookCallback) {
  auto detector = MakeDetector();
  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
  _runtime.ClearCalls();

  _runtime.BlockNextTimestamp();
  auto callback =
      std::async(std::launch::async, [&] { _runtime.InvokeHook(ProbeMessage()); });
  ASSERT_TRUE(_runtime.WaitUntilTimestampBlocked());

  auto destroy = std::async(std::launch::async, [&] { detector.reset(); });
  EXPECT_TRUE(_runtime.WaitUntilCalled(Op::RemoveMessageHook));
  EXPECT_EQ(destroy.wait_for(kBlockedDuration), std::future_status::timeout);
  EXPECT_EQ(
      _runtime.Ops(),
      (std::vector<Op>{Op::SetEvent, Op::WatchdogExit, Op::RemoveMessageHook})
  );

  _runtime.ReleaseTimestamp();
  callback.get();
  destroy.get();
  EXPECT_EQ(_runtime.Ops(), TeardownOps());

  // later callbacks find no detector
  size_t timestampCalls = _runtime.TimestampCalls();
  _runtime.InvokeHook(ProbeMessage());
  EXPECT_EQ(_runtime.TimestampCalls(), timestampCalls);
}

TEST_F(UiHangDetectorLifetimeTests, StopJoinsWatchdogPostingProbeWithoutInstanceMutex) {
  auto detector = MakeDetector();
  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
  _runtime.ClearCalls();

  _runtime.BlockNextPost();
  _runtime.GrantWatchdogTick();
  ASSERT_TRUE(_runtime.WaitUntilPostBlocked());

  auto stop = std::async(std::launch::async, [&] { detector->Stop(); });
  EXPECT_TRUE(_runtime.WaitUntilCalled(Op::SetEvent));
  // Stop is now joining the watchdog, which is still inside PostMessageW, while hook
  // notifications still reach the detector
  EXPECT_EQ(stop.wait_for(kBlockedDuration), std::future_status::timeout);
  HookView hook = ObserveHook();
  EXPECT_TRUE(hook.isLockFree);
  EXPECT_EQ(hook.pDetector, detector.get());

  _runtime.ReleasePost();
  stop.get();

  std::vector<Op> expectedOps = TeardownOps();
  expectedOps.insert(expectedOps.begin(), Op::PostMessageW);
  EXPECT_EQ(_runtime.Ops(), expectedOps);
  ExpectReleased(*detector);
}

TEST_F(UiHangDetectorLifetimeTests, HookIgnoresOtherWindowsAndMessages) {
  auto detector = MakeDetector();
  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));

  MSG otherWindow = ProbeMessage();
  otherWindow.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0xBAD));
  MSG otherMessage = ProbeMessage();
  ++otherMessage.message;
  size_t timestampCalls = _runtime.TimestampCalls();
  _runtime.InvokeHook(otherWindow);
  _runtime.InvokeHook(otherMessage);
  EXPECT_EQ(_runtime.TimestampCalls(), timestampCalls);
  EXPECT_FALSE(IsProbeObserved(*detector));

  _runtime.InvokeHook(ProbeMessage());
  EXPECT_EQ(_runtime.TimestampCalls(), timestampCalls + 1);
  EXPECT_TRUE(IsProbeObserved(*detector));

  detector->Stop();
}

TEST_F(UiHangDetectorLifetimeTests, WatchdogPostsFirstProbeOnlyOnceInstanceIsSet) {
  auto detector = MakeDetector();
  _runtime.TickWatchdogOnStart();

  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
  // a probe processed before the detector is the instance would never be observed
  EXPECT_TRUE(_runtime.Posts().empty());

  _runtime.RunWatchdogTick();
  EXPECT_EQ(
      _runtime.Posts(), (std::vector<std::pair<HWND, UINT>>{{kWindow, kProbeMessage}})
  );

  // the processed probe reaches the detector, so the next tick probes again
  _runtime.InvokeHook(ProbeMessage(1));
  EXPECT_TRUE(IsProbeObserved(*detector));
  _runtime.RunWatchdogTick();
  EXPECT_EQ(_runtime.Posts().size(), 2u);

  detector->Stop();
}

TEST_F(UiHangDetectorLifetimeTests, FailedProbePostIsRetriedOnNextTick) {
  auto detector = MakeDetector();
  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));

  _runtime.FailNextPost();
  _runtime.RunWatchdogTick();
  _runtime.RunWatchdogTick();
  EXPECT_EQ(_runtime.Posts().size(), 2u);

  detector->Stop();
  ExpectReleased(*detector);
}

TEST_F(UiHangDetectorLifetimeTests, RepeatedStopAndDestructionDoNothing) {
  auto detector = MakeDetector();
  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
  detector->Stop();
  _runtime.ClearCalls();

  detector->Stop();
  EXPECT_TRUE(_runtime.Ops().empty());
  ExpectReleased(*detector);
  EXPECT_FALSE(HasProviders(*detector));

  // a stopped detector stays stopped
  EXPECT_FALSE(detector->MonitorWindowHangs(kWindow, &_threadList));
  detector.reset();
  EXPECT_TRUE(_runtime.Ops().empty());
}

TEST_F(UiHangDetectorLifetimeTests, DestructionTearsDownActiveDetector) {
  auto detector = MakeDetector();
  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
  _runtime.ClearCalls();

  detector.reset();

  ExpectTeardownWithoutInstanceMutex();
}
