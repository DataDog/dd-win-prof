// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <gtest/gtest.h>

#include <future>
#include <thread>

#include "../dd-win-prof/Profiler.h"
#include "pch.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved);

class ProfilerLifecycleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    _savedConfiguration = *Profiler::GetConfiguration();
    Profiler::GetConfiguration()->ResetToDefaults();
    Profiler::GetConfiguration()->SetExportEnabled(false);
    _profiler = std::make_unique<Profiler>();
  }

  void TearDown() override {
    if (_profiler) {
      _profiler->StopProfiling();
      _profiler.reset();
    }
    *Profiler::GetConfiguration() = _savedConfiguration;
  }

  std::unique_lock<std::mutex> LockLifecycle() {
    return std::unique_lock(_profiler->_lifecycleMutex);
  }

  bool CanReadRumContext() {
    std::shared_lock lock(_profiler->_rumContextMutex, std::try_to_lock);
    return lock.owns_lock();
  }

  Configuration _savedConfiguration;
  std::unique_ptr<Profiler> _profiler;
};

TEST_F(ProfilerLifecycleTest, SessionRegistrationWaitsForLifecycleOperation) {
  auto lock = LockLifecycle();
  std::promise<void> entered;
  auto registration = std::async(std::launch::async, [&] {
    RumSessionContext context{"app-id", "session-id"};
    entered.set_value();
    return _profiler->SetRumSession(&context);
  });

  entered.get_future().wait();
  auto status = registration.wait_for(std::chrono::milliseconds(100));
  auto canReadRumContext = CanReadRumContext();
  lock.unlock();

  EXPECT_TRUE(registration.get());
  EXPECT_EQ(status, std::future_status::timeout);
  EXPECT_TRUE(canReadRumContext);
  EXPECT_EQ(_profiler->GetCurrentSessionId(), "session-id");
}

TEST_F(ProfilerLifecycleTest, ProcessTerminationSkipsLockedProfiler) {
  auto lock = LockLifecycle();
  // A failing implementation deletes the instance, so retain ownership only if it
  // survives.
  auto* profiler = _profiler.release();
  std::promise<void> entered;
  auto detach = std::async(std::launch::async, [&] {
    entered.set_value();
    return DllMain(nullptr, DLL_PROCESS_DETACH, reinterpret_cast<LPVOID>(1));
  });

  entered.get_future().wait();
  auto status = detach.wait_for(std::chrono::seconds(1));
  lock.unlock();
  EXPECT_TRUE(detach.get());

  if (Profiler::GetInstance() == profiler) {
    _profiler.reset(profiler);
  }
  EXPECT_EQ(status, std::future_status::ready);
  EXPECT_NE(_profiler, nullptr);
}

TEST_F(ProfilerLifecycleTest, FirstSessionRegistrationRacesStopWithoutMonitoring) {
  for (int i = 0; i < 20; ++i) {
    _profiler.reset();
    _profiler = std::make_unique<Profiler>();
    ASSERT_TRUE(_profiler->StartProfiling());

    std::promise<void> start;
    auto ready = start.get_future();
    auto registration = std::async(std::launch::async, [&] {
      ready.wait();
      RumSessionContext context{"app-id", "session-id"};
      return _profiler->SetRumSession(&context);
    });

    start.set_value();
    _profiler->StopProfiling();
    EXPECT_TRUE(registration.get());
    EXPECT_EQ(_profiler->GetCurrentSessionId(), "session-id");

    ASSERT_TRUE(_profiler->StartProfiling());
    _profiler->StopProfiling();
  }
}
