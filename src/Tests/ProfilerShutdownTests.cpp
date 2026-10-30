// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <gtest/gtest.h>

#include <future>

#include "../dd-win-prof/Profiler.h"
#include "pch.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved);

class ProfilerShutdownTest : public ::testing::Test {
 protected:
  void SetUp() override { _profiler = std::make_unique<Profiler>(); }

  std::unique_lock<std::recursive_mutex> LockThreadList() {
    return std::unique_lock(_profiler->_pThreadList->_mutex);
  }

  std::unique_ptr<Profiler> _profiler;
};

TEST_F(ProfilerShutdownTest, ProcessTerminationSkipsLockedProfiler) {
  auto lock = LockThreadList();
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
