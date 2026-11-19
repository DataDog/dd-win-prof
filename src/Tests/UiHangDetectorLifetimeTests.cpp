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
constexpr DWORD kUiThreadId = 0x4242;
constexpr DWORD kUnmonitoredThreadId = 0x4343;

// bound for steps that complete at once when the detector behaves
constexpr auto kTimeout = std::chrono::seconds(10);
// time given to an operation that must stay blocked
constexpr auto kBlockedDuration = std::chrono::milliseconds(100);

// platform calls that acquire, use or release the detector resources
enum class Op {
  CreateStopEvent,
  IsWindow,
  GetWindowThreadProcessId,
  SetGetMessageHook,
  StartThread,
  PostMessageW,
  UnhookWindowsHookEx,
  SetEvent,
  WatchdogExit,
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
    case Op::SetGetMessageHook:
      return os << "SetGetMessageHook";
    case Op::StartThread:
      return os << "StartThread";
    case Op::PostMessageW:
      return os << "PostMessageW";
    case Op::UnhookWindowsHookEx:
      return os << "UnhookWindowsHookEx";
    case Op::SetEvent:
      return os << "SetEvent";
    case Op::WatchdogExit:
      return os << "WatchdogExit";
    case Op::CloseHandle:
      return os << "CloseHandle";
  }
  return os << "Op(" << static_cast<int>(op) << ")";
}

std::vector<Op> TeardownOps() {
  return {Op::UnhookWindowsHookEx, Op::SetEvent, Op::WatchdogExit, Op::CloseHandle};
}

// activation stage made to fail by FakePlatform
enum class Failure {
  None,
  StopEvent,
  InvalidWindow,
  WindowThread,
  ForeignWindow,
  UnmonitoredThread,
  Hook,
  Watchdog,
};

MSG ProbeMessage() {
  MSG msg{};
  msg.hwnd = kWindow;
  msg.message = kProbeMessage;
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
  // what a hook callback starting now would find
  struct RouterView {
    bool isLockFree = false;
    const UiHangDetector* pRouted = nullptr;
  };

  struct Call {
    Op op;
    RouterView router;
  };

  struct HookCallResult {
    bool isCompleted = false;
    bool reachedDetector = false;
  };

  // Records the platform calls with the router state they observed, fails the
  // requested activation stage and parks callers at points chosen by the test. Its
  // stop event only wakes up when signaled or when the test grants a watchdog tick, and
  // its clock never advances, so no hang is ever detected.
  class FakePlatform final : public IUiHangDetectorPlatform {
   public:
    ~FakePlatform() override {
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

    // another thread calls the hook procedure while the hook is being removed
    void CallHookDuringUnhook() {
      std::lock_guard<std::mutex> lock(_mutex);
      _callHookDuringUnhook = true;
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
      return WaitLocked(lock, "platform call", [&] {
        return std::any_of(_calls.begin(), _calls.end(), [op](const Call& call) {
          return call.op == op;
        });
      });
    }

    // calls the hook procedure as the window thread's message loop would
    void InvokeHook(const MSG& msg) {
      HOOKPROC hookProc = nullptr;
      {
        std::lock_guard<std::mutex> lock(_mutex);
        hookProc = _hookProc;
        if (hookProc == nullptr) {
          _errors.push_back("no hook procedure was registered");
          return;
        }
      }
      MSG message = msg;
      hookProc(HC_ACTION, PM_REMOVE, reinterpret_cast<LPARAM>(&message));
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
      return static_cast<size_t>(std::count_if(
          _hooks.begin(), _hooks.end(), [](const auto& hook) { return hook.second; }
      ));
    }

    size_t RunningThreads() {
      std::lock_guard<std::mutex> lock(_mutex);
      return _startedThreads - _exitedThreads;
    }

    HookCallResult HookCallDuringUnhook() {
      std::lock_guard<std::mutex> lock(_mutex);
      return _hookCallDuringUnhook;
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
      pEvent->isSignaled = true;
      _changed.notify_all();
      return TRUE;
    }

    // reports WAIT_OBJECT_0 instead of failures and timeouts so that a broken detector
    // fails the test instead of spinning or hanging its watchdog
    DWORD WaitForSingleObject(HANDLE hHandle, DWORD) override {
      std::unique_lock<std::mutex> lock(_mutex);
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

    HHOOK SetGetMessageHook(HOOKPROC hookProc, DWORD threadId) override {
      Record(Op::SetGetMessageHook);
      std::lock_guard<std::mutex> lock(_mutex);
      if (_failure == Failure::Hook) {
        return nullptr;
      }
      HHOOK hHook = reinterpret_cast<HHOOK>(NextHandleValue());
      _hooks[hHook] = true;
      _hookProc = hookProc;
      _hookThreadId = threadId;
      return hHook;
    }

    BOOL UnhookWindowsHookEx(HHOOK hHook) override {
      Record(Op::UnhookWindowsHookEx);
      bool callHook = false;
      {
        std::lock_guard<std::mutex> lock(_mutex);
        callHook = std::exchange(_callHookDuringUnhook, false);
      }
      if (callHook) {
        CallHookFromAnotherThread();
      }

      std::lock_guard<std::mutex> lock(_mutex);
      auto hook = _hooks.find(hHook);
      if (hook == _hooks.end() || !hook->second) {
        _errors.push_back("UnhookWindowsHookEx on a removed or unknown hook");
        return FALSE;
      }
      hook->second = false;
      return TRUE;
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
      RouterView router = ObserveRouter();
      std::lock_guard<std::mutex> lock(_mutex);
      _calls.push_back({op, router});
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

    // a stuck callback is joined only after the detector released the router lock
    void CallHookFromAnotherThread() {
      JoinHookCaller();
      size_t timestampCalls = TimestampCalls();
      _hookCaller = std::thread([this] {
        InvokeHook(ProbeMessage());
        std::lock_guard<std::mutex> lock(_mutex);
        _hookCallDuringUnhook.isCompleted = true;
        _changed.notify_all();
      });

      std::unique_lock<std::mutex> lock(_mutex);
      if (WaitLocked(lock, "hook callback during unhook", [&] {
            return _hookCallDuringUnhook.isCompleted;
          })) {
        // only the detector reads the clock while the watchdog waits for a tick
        _hookCallDuringUnhook.reachedDetector = _timestampCalls != timestampCalls;
      }
    }

    std::mutex _mutex;
    std::condition_variable _changed;
    Failure _failure = Failure::None;
    std::vector<Call> _calls;
    std::vector<std::string> _errors;
    uintptr_t _nextHandleValue = 0x10000;
    std::map<HANDLE, EventState> _events;
    std::map<HHOOK, bool> _hooks;  // true until unhooked
    HOOKPROC _hookProc = nullptr;
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
    bool _callHookDuringUnhook = false;
    HookCallResult _hookCallDuringUnhook;
    std::thread _hookCaller;
  };

  void SetUp() override {
    _threadList.AddThread(kUiThreadId, DuplicateCurrentThread());
  }

  void TearDown() override {
    _platform.JoinHookCaller();
    if (ObserveRouter().pRouted != nullptr) {
      ADD_FAILURE() << "a detector is still routed";
      ClearRouter();
    }
    EXPECT_EQ(_platform.LiveEvents(), 0u);
    EXPECT_EQ(_platform.LiveHooks(), 0u);
    EXPECT_EQ(_platform.RunningThreads(), 0u);
    for (const std::string& error : _platform.Errors()) {
      ADD_FAILURE() << error;
    }
  }

  std::unique_ptr<UiHangDetector> MakeDetector() {
    return std::make_unique<UiHangDetector>(
        kProbeMessage, &_hangProvider, &_rumViewProvider, _platform
    );
  }

  static RouterView ObserveRouter() {
    // probed from another thread: a std::mutex can't be try-locked by its owner
    std::mutex& routerMutex = UiHangDetector::_instanceMutex;
    UiHangDetector* const& pRouted = UiHangDetector::_this;
    RouterView view;
    std::thread observer([&] {
      std::unique_lock<std::mutex> lock(routerMutex, std::try_to_lock);
      if (lock.owns_lock()) {
        view = {true, pRouted};
      }
    });
    observer.join();
    return view;
  }

  static void ClearRouter() {
    std::lock_guard<std::mutex> lock(UiHangDetector::_instanceMutex);
    UiHangDetector::_this = nullptr;
  }

  static bool IsRouted(const UiHangDetector& detector) {
    return detector._isRouted.load();
  }

  static bool IsProbeObserved(const UiHangDetector& detector) {
    return detector._isProcessed.load();
  }

  static bool HasProviders(const UiHangDetector& detector) {
    return detector._pHangProvider != nullptr &&
           detector._pRumViewContextProvider != nullptr;
  }

  // no routing, hook, watchdog, event, window or thread is left behind
  static void ExpectReleased(const UiHangDetector& detector) {
    EXPECT_NE(ObserveRouter().pRouted, &detector);
    EXPECT_FALSE(detector._isRouted.load());
    EXPECT_EQ(detector._hGetMessageHook, nullptr);
    EXPECT_EQ(detector._pWatchdogThread, nullptr);
    EXPECT_EQ(detector._stopEvent, nullptr);
    EXPECT_EQ(detector._hWnd, nullptr);
    EXPECT_EQ(detector._pThreadInfo, nullptr);
  }

  // Fails activation at one stage, then checks that the stages before it were released
  // without the router lock and that the same detector activates once the failure is
  // gone.
  void ExpectRollbackAndRetry(Failure failure, const std::vector<Op>& expectedOps) {
    auto detector = MakeDetector();
    _platform.Fail(failure);

    EXPECT_FALSE(detector->MonitorWindowHangs(kWindow, &_threadList));

    EXPECT_EQ(_platform.Ops(), expectedOps);
    for (const Call& call : _platform.Calls()) {
      EXPECT_TRUE(call.router.isLockFree) << call.op;
      EXPECT_EQ(call.router.pRouted, nullptr) << call.op;
    }
    EXPECT_EQ(_platform.LiveEvents(), 0u);
    EXPECT_EQ(_platform.LiveHooks(), 0u);
    EXPECT_EQ(_platform.RunningThreads(), 0u);
    ExpectReleased(*detector);
    EXPECT_TRUE(HasProviders(*detector));

    _platform.Fail(Failure::None);
    EXPECT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
    EXPECT_EQ(ObserveRouter().pRouted, detector.get());

    detector->Stop();
    ExpectReleased(*detector);
  }

  SampleValueTypeProvider _valueTypes;
  UiHangProvider _hangProvider{_valueTypes};
  NoRumView _rumViewProvider;
  ThreadList _threadList;
  FakePlatform _platform;
};

TEST_F(UiHangDetectorLifetimeTests, UnusedDetectorTouchesNothing) {
  auto detector = MakeDetector();
  EXPECT_EQ(ObserveRouter().pRouted, nullptr);

  detector->Stop();
  detector->Stop();
  detector.reset();

  EXPECT_TRUE(_platform.Ops().empty());
}

TEST_F(UiHangDetectorLifetimeTests, ActivationPublishesOnceEveryStageSucceeded) {
  auto detector = MakeDetector();

  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));

  EXPECT_EQ(
      _platform.Ops(),
      (std::vector<Op>{
          Op::CreateStopEvent,
          Op::IsWindow,
          Op::GetWindowThreadProcessId,
          Op::SetGetMessageHook,
          Op::StartThread
      })
  );
  for (const Call& call : _platform.Calls()) {
    EXPECT_TRUE(call.router.isLockFree) << call.op;
    EXPECT_EQ(call.router.pRouted, nullptr) << "published before " << call.op;
  }
  EXPECT_EQ(ObserveRouter().pRouted, detector.get());
  EXPECT_TRUE(IsRouted(*detector));
  EXPECT_EQ(_platform.HookThreadId(), kUiThreadId);

  // an active detector rejects another window without touching its resources
  _platform.ClearCalls();
  EXPECT_FALSE(detector->MonitorWindowHangs(kWindow, &_threadList));
  EXPECT_TRUE(_platform.Ops().empty());
  EXPECT_EQ(ObserveRouter().pRouted, detector.get());

  detector->Stop();
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
       Op::SetGetMessageHook,
       Op::CloseHandle}
  );
}

TEST_F(UiHangDetectorLifetimeTests, WatchdogFailureUnhooksAndRetrySucceeds) {
  ExpectRollbackAndRetry(
      Failure::Watchdog,
      {Op::CreateStopEvent,
       Op::IsWindow,
       Op::GetWindowThreadProcessId,
       Op::SetGetMessageHook,
       Op::StartThread,
       Op::UnhookWindowsHookEx,
       Op::CloseHandle}
  );
}

TEST_F(UiHangDetectorLifetimeTests, OccupiedRouterRollsBackEveryStageAndRetrySucceeds) {
  auto active = MakeDetector();
  ASSERT_TRUE(active->MonitorWindowHangs(kWindow, &_threadList));
  _platform.ClearCalls();

  auto detector = MakeDetector();
  EXPECT_FALSE(detector->MonitorWindowHangs(kWindow, &_threadList));

  EXPECT_EQ(
      _platform.Ops(),
      (std::vector<Op>{
          Op::CreateStopEvent,
          Op::IsWindow,
          Op::GetWindowThreadProcessId,
          Op::SetGetMessageHook,
          Op::StartThread,
          Op::UnhookWindowsHookEx,
          Op::SetEvent,
          Op::WatchdogExit,
          Op::CloseHandle
      })
  );
  ExpectReleased(*detector);
  EXPECT_TRUE(HasProviders(*detector));
  // the active detector keeps its routing, hook, watchdog and event
  EXPECT_EQ(ObserveRouter().pRouted, active.get());
  EXPECT_EQ(_platform.LiveHooks(), 1u);
  EXPECT_EQ(_platform.RunningThreads(), 1u);
  EXPECT_EQ(_platform.LiveEvents(), 1u);

  active->Stop();
  EXPECT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
  EXPECT_EQ(ObserveRouter().pRouted, detector.get());

  detector->Stop();
}

TEST_F(
    UiHangDetectorLifetimeTests, StopWithdrawsRoutingBeforeTeardownWithoutRouterLock
) {
  auto detector = MakeDetector();
  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
  _platform.ClearCalls();
  _platform.CallHookDuringUnhook();

  detector->Stop();

  EXPECT_EQ(_platform.Ops(), TeardownOps());
  for (const Call& call : _platform.Calls()) {
    EXPECT_TRUE(call.router.isLockFree) << call.op;
    EXPECT_EQ(call.router.pRouted, nullptr) << call.op;
  }
  // a callback entering the hook during teardown returns without reaching the detector
  HookCallResult hookCall = _platform.HookCallDuringUnhook();
  EXPECT_TRUE(hookCall.isCompleted);
  EXPECT_FALSE(hookCall.reachedDetector);
  ExpectReleased(*detector);
  EXPECT_FALSE(HasProviders(*detector));
}

TEST_F(UiHangDetectorLifetimeTests, StopWaitsForRunningHookCallbackBeforeTeardown) {
  auto detector = MakeDetector();
  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
  _platform.ClearCalls();

  // the callback parks inside the detector while holding the router lock
  _platform.BlockNextTimestamp();
  auto callback =
      std::async(std::launch::async, [&] { _platform.InvokeHook(ProbeMessage()); });
  ASSERT_TRUE(_platform.WaitUntilTimestampBlocked());

  auto stop = std::async(std::launch::async, [&] { detector->Stop(); });
  EXPECT_EQ(stop.wait_for(kBlockedDuration), std::future_status::timeout);
  EXPECT_TRUE(_platform.Ops().empty());

  _platform.ReleaseTimestamp();
  callback.get();
  stop.get();

  // the callback completed on the live detector before the first teardown step
  EXPECT_TRUE(IsProbeObserved(*detector));
  EXPECT_EQ(_platform.Ops(), TeardownOps());
  ExpectReleased(*detector);
}

TEST_F(UiHangDetectorLifetimeTests, DestructionWaitsForRunningHookCallback) {
  auto detector = MakeDetector();
  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
  _platform.ClearCalls();

  _platform.BlockNextTimestamp();
  auto callback =
      std::async(std::launch::async, [&] { _platform.InvokeHook(ProbeMessage()); });
  ASSERT_TRUE(_platform.WaitUntilTimestampBlocked());

  auto destroy = std::async(std::launch::async, [&] { detector.reset(); });
  EXPECT_EQ(destroy.wait_for(kBlockedDuration), std::future_status::timeout);
  EXPECT_TRUE(_platform.Ops().empty());

  _platform.ReleaseTimestamp();
  callback.get();
  destroy.get();
  EXPECT_EQ(_platform.Ops(), TeardownOps());

  // later callbacks find no detector
  size_t timestampCalls = _platform.TimestampCalls();
  _platform.InvokeHook(ProbeMessage());
  EXPECT_EQ(_platform.TimestampCalls(), timestampCalls);
}

TEST_F(UiHangDetectorLifetimeTests, StopJoinsWatchdogPostingProbeWithoutRouterLock) {
  auto detector = MakeDetector();
  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
  _platform.ClearCalls();

  _platform.BlockNextPost();
  _platform.GrantWatchdogTick();
  ASSERT_TRUE(_platform.WaitUntilPostBlocked());

  auto stop = std::async(std::launch::async, [&] { detector->Stop(); });
  EXPECT_TRUE(_platform.WaitUntilCalled(Op::SetEvent));
  // Stop is now joining the watchdog, which is still inside PostMessageW
  EXPECT_EQ(stop.wait_for(kBlockedDuration), std::future_status::timeout);
  RouterView router = ObserveRouter();
  EXPECT_TRUE(router.isLockFree);
  EXPECT_EQ(router.pRouted, nullptr);

  _platform.ReleasePost();
  stop.get();

  EXPECT_EQ(
      _platform.Ops(),
      (std::vector<Op>{
          Op::PostMessageW,
          Op::UnhookWindowsHookEx,
          Op::SetEvent,
          Op::WatchdogExit,
          Op::CloseHandle
      })
  );
  ExpectReleased(*detector);
}

TEST_F(UiHangDetectorLifetimeTests, WatchdogPostsFirstProbeOnlyOncePublished) {
  auto detector = MakeDetector();
  _platform.TickWatchdogOnStart();

  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
  // a probe posted before publication would be processed without being observed
  EXPECT_TRUE(_platform.Posts().empty());

  _platform.RunWatchdogTick();
  EXPECT_EQ(
      _platform.Posts(), (std::vector<std::pair<HWND, UINT>>{{kWindow, kProbeMessage}})
  );

  // the processed probe reaches the detector, so the next tick probes again
  _platform.InvokeHook(ProbeMessage());
  EXPECT_TRUE(IsProbeObserved(*detector));
  _platform.RunWatchdogTick();
  EXPECT_EQ(_platform.Posts().size(), 2u);

  detector->Stop();
}

TEST_F(UiHangDetectorLifetimeTests, FailedProbePostIsRetriedOnNextTick) {
  auto detector = MakeDetector();
  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));

  _platform.FailNextPost();
  _platform.RunWatchdogTick();
  _platform.RunWatchdogTick();
  EXPECT_EQ(_platform.Posts().size(), 2u);

  detector->Stop();
  ExpectReleased(*detector);
}

TEST_F(UiHangDetectorLifetimeTests, RepeatedStopAndDestructionDoNothing) {
  auto detector = MakeDetector();
  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
  detector->Stop();
  _platform.ClearCalls();

  detector->Stop();
  EXPECT_TRUE(_platform.Ops().empty());
  ExpectReleased(*detector);
  EXPECT_FALSE(HasProviders(*detector));

  // a stopped detector stays stopped
  EXPECT_FALSE(detector->MonitorWindowHangs(kWindow, &_threadList));
  detector.reset();
  EXPECT_TRUE(_platform.Ops().empty());
}

TEST_F(UiHangDetectorLifetimeTests, DestructionTearsDownActiveDetector) {
  auto detector = MakeDetector();
  ASSERT_TRUE(detector->MonitorWindowHangs(kWindow, &_threadList));
  _platform.ClearCalls();

  detector.reset();

  EXPECT_EQ(_platform.Ops(), TeardownOps());
  for (const Call& call : _platform.Calls()) {
    EXPECT_TRUE(call.router.isLockFree) << call.op;
    EXPECT_EQ(call.router.pRouted, nullptr) << call.op;
  }
}
