// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"

#include "../dd-win-prof/ThreadList.h"

// Test helper to create a thread with a specific handle value
class TestThreadInfo : public ThreadInfo {
 public:
  TestThreadInfo(uint32_t tid, HANDLE hThread) : ThreadInfo(tid, hThread) {}
};

TEST(ThreadListTests, LoopNext_ReturnsThreadsInOrder) {
  ThreadList threadList;

  // Create 3 valid threads
  HANDLE h1 = reinterpret_cast<HANDLE>(0x1000);
  HANDLE h2 = reinterpret_cast<HANDLE>(0x2000);
  HANDLE h3 = reinterpret_cast<HANDLE>(0x3000);

  threadList.AddThread(1, h1);
  threadList.AddThread(2, h2);
  threadList.AddThread(3, h3);

  uint32_t iterator = threadList.CreateIterator();

  // First iteration should return thread 1
  auto thread1 = threadList.LoopNext(iterator);
  ASSERT_NE(thread1, nullptr);
  EXPECT_EQ(thread1->GetThreadId(), 1);

  // Second iteration should return thread 2
  auto thread2 = threadList.LoopNext(iterator);
  ASSERT_NE(thread2, nullptr);
  EXPECT_EQ(thread2->GetThreadId(), 2);

  // Third iteration should return thread 3
  auto thread3 = threadList.LoopNext(iterator);
  ASSERT_NE(thread3, nullptr);
  EXPECT_EQ(thread3->GetThreadId(), 3);

  // Fourth iteration should wrap around to thread 1
  auto thread4 = threadList.LoopNext(iterator);
  ASSERT_NE(thread4, nullptr);
  EXPECT_EQ(thread4->GetThreadId(), 1);
}

TEST(ThreadListTests, LoopNext_SkipsInvalidHandles) {
  ThreadList threadList;

  // Create threads: valid, invalid, valid
  HANDLE h1 = reinterpret_cast<HANDLE>(0x1000);
  HANDLE h2 = INVALID_HANDLE_VALUE;  // Invalid
  HANDLE h3 = reinterpret_cast<HANDLE>(0x3000);

  threadList.AddThread(1, h1);
  threadList.AddThread(2, h2);
  threadList.AddThread(3, h3);

  uint32_t iterator = threadList.CreateIterator();

  // First iteration should return thread 1
  auto thread1 = threadList.LoopNext(iterator);
  ASSERT_NE(thread1, nullptr);
  EXPECT_EQ(thread1->GetThreadId(), 1);

  // Second iteration should SKIP thread 2 and return thread 3
  auto thread2 = threadList.LoopNext(iterator);
  ASSERT_NE(thread2, nullptr);
  EXPECT_EQ(thread2->GetThreadId(), 3);  // NOT 2!

  // Third iteration should wrap around to thread 1
  auto thread3 = threadList.LoopNext(iterator);
  ASSERT_NE(thread3, nullptr);
  EXPECT_EQ(thread3->GetThreadId(), 1);
}

TEST(ThreadListTests, LoopNext_AllInvalidHandles_ReturnsNull) {
  ThreadList threadList;

  // Create all invalid threads
  threadList.AddThread(1, INVALID_HANDLE_VALUE);
  threadList.AddThread(2, NULL);
  threadList.AddThread(3, INVALID_HANDLE_VALUE);

  uint32_t iterator = threadList.CreateIterator();

  // Should return nullptr since all threads are invalid
  auto thread = threadList.LoopNext(iterator);
  EXPECT_EQ(thread, nullptr);
}

TEST(ThreadListTests, LoopNext_IteratorPositionCorrectAfterSkippingInvalid) {
  ThreadList threadList;

  // Create threads: invalid, valid, invalid, valid
  HANDLE h1 = INVALID_HANDLE_VALUE;
  HANDLE h2 = reinterpret_cast<HANDLE>(0x2000);
  HANDLE h3 = INVALID_HANDLE_VALUE;
  HANDLE h4 = reinterpret_cast<HANDLE>(0x4000);

  threadList.AddThread(1, h1);
  threadList.AddThread(2, h2);
  threadList.AddThread(3, h3);
  threadList.AddThread(4, h4);

  uint32_t iterator = threadList.CreateIterator();

  // First call: should skip thread 1, return thread 2
  auto thread1 = threadList.LoopNext(iterator);
  ASSERT_NE(thread1, nullptr);
  EXPECT_EQ(thread1->GetThreadId(), 2);

  // Second call: should skip thread 3, return thread 4
  auto thread2 = threadList.LoopNext(iterator);
  ASSERT_NE(thread2, nullptr);
  EXPECT_EQ(thread2->GetThreadId(), 4);

  // Third call: should skip thread 1, return thread 2 again
  auto thread3 = threadList.LoopNext(iterator);
  ASSERT_NE(thread3, nullptr);
  EXPECT_EQ(thread3->GetThreadId(), 2);  // THIS WILL FAIL with the current bug!
}

TEST(ThreadListTests, RemoveThread_UpdatesIteratorsCorrectly) {
  ThreadList threadList;

  HANDLE h1 = reinterpret_cast<HANDLE>(0x1000);
  HANDLE h2 = reinterpret_cast<HANDLE>(0x2000);
  HANDLE h3 = reinterpret_cast<HANDLE>(0x3000);

  threadList.AddThread(1, h1);
  threadList.AddThread(2, h2);
  threadList.AddThread(3, h3);

  uint32_t iterator = threadList.CreateIterator();

  // Advance iterator to position 2 (thread 3)
  threadList.LoopNext(iterator);
  threadList.LoopNext(iterator);

  // Remove thread 2 (middle thread)
  threadList.RemoveThread(2);

  // Iterator should now point to thread 3 (which is now at position 1)
  // Next call should return thread 3
  auto thread = threadList.LoopNext(iterator);
  ASSERT_NE(thread, nullptr);
  EXPECT_EQ(thread->GetThreadId(), 3);
}

TEST(ThreadListTests, MultipleIterators_IndependentPositions) {
  ThreadList threadList;

  HANDLE h1 = reinterpret_cast<HANDLE>(0x1000);
  HANDLE h2 = reinterpret_cast<HANDLE>(0x2000);
  HANDLE h3 = reinterpret_cast<HANDLE>(0x3000);

  threadList.AddThread(1, h1);
  threadList.AddThread(2, h2);
  threadList.AddThread(3, h3);

  uint32_t iter1 = threadList.CreateIterator();
  uint32_t iter2 = threadList.CreateIterator();

  // Advance iter1 twice
  threadList.LoopNext(iter1);
  threadList.LoopNext(iter1);

  // iter2 should still be at the beginning
  auto thread = threadList.LoopNext(iter2);
  ASSERT_NE(thread, nullptr);
  EXPECT_EQ(thread->GetThreadId(), 1);
}
