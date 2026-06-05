// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <gtest/gtest.h>

#include "../dd-win-prof/ThreadList.h"
#include "pch.h"

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

// ============================================================================
// BUG-REVEALING EDGE CASES (from code review)
// ============================================================================

// A single valid thread positioned before an invalid one must keep being
// returned on every round. Currently the 2nd call returns nullptr because the
// "full circle" check (startPos == pos) fires on the same iteration that found
// the valid thread.
TEST(ThreadListTests, LoopNext_ValidBeforeInvalid_KeepsReturningValid) {
  ThreadList threadList;
  threadList.AddThread(1, reinterpret_cast<HANDLE>(0x1000));  // valid
  threadList.AddThread(2, INVALID_HANDLE_VALUE);             // invalid

  uint32_t it = threadList.CreateIterator();

  auto first = threadList.LoopNext(it);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->GetThreadId(), 1);

  auto second = threadList.LoopNext(it);  // currently returns nullptr (BUG)
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->GetThreadId(), 1);
}

// Same defect, single valid element among many: [inv, inv, valid].
TEST(ThreadListTests, LoopNext_SingleValidAtEnd_StableAcrossCalls) {
  ThreadList threadList;
  threadList.AddThread(1, INVALID_HANDLE_VALUE);
  threadList.AddThread(2, NULL);
  threadList.AddThread(3, reinterpret_cast<HANDLE>(0x3000));  // valid

  uint32_t it = threadList.CreateIterator();
  for (int i = 0; i < 4; ++i) {
    auto t = threadList.LoopNext(it);
    ASSERT_NE(t, nullptr) << "iteration " << i;
    EXPECT_EQ(t->GetThreadId(), 3) << "iteration " << i;
  }
}

// ============================================================================
// GUARD-CLAUSE / API EDGE CASES
// ============================================================================

TEST(ThreadListTests, LoopNext_EmptyList_ReturnsNull) {
  ThreadList threadList;
  uint32_t it = threadList.CreateIterator();
  EXPECT_EQ(threadList.LoopNext(it), nullptr);
}

TEST(ThreadListTests, LoopNext_InvalidIteratorIndex_ReturnsNull) {
  ThreadList threadList;
  threadList.AddThread(1, reinterpret_cast<HANDLE>(0x1000));
  // No CreateIterator() call, and an out-of-range index.
  EXPECT_EQ(threadList.LoopNext(0), nullptr);
  EXPECT_EQ(threadList.LoopNext(99), nullptr);
}

TEST(ThreadListTests, LoopNext_SingleValidThread_ReturnedEveryCall) {
  ThreadList threadList;
  threadList.AddThread(42, reinterpret_cast<HANDLE>(0x1000));
  uint32_t it = threadList.CreateIterator();
  for (int i = 0; i < 3; ++i) {
    auto t = threadList.LoopNext(it);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->GetThreadId(), 42);
  }
}

// Only NULL handles (the other half of the skip condition; existing test
// mixes NULL/INVALID_HANDLE_VALUE but never isolates NULL).
TEST(ThreadListTests, LoopNext_SkipsNullHandlesSpecifically) {
  ThreadList threadList;
  threadList.AddThread(1, NULL);
  threadList.AddThread(2, reinterpret_cast<HANDLE>(0x2000));
  uint32_t it = threadList.CreateIterator();
  auto t = threadList.LoopNext(it);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->GetThreadId(), 2);
}

// ============================================================================
// Count() (entirely untested today)
// ============================================================================

TEST(ThreadListTests, Count_TracksAddAndRemove) {
  ThreadList threadList;
  EXPECT_EQ(threadList.Count(), 0u);
  threadList.AddThread(1, reinterpret_cast<HANDLE>(0x1000));
  threadList.AddThread(2, reinterpret_cast<HANDLE>(0x2000));
  EXPECT_EQ(threadList.Count(), 2u);
  threadList.RemoveThread(1);
  EXPECT_EQ(threadList.Count(), 1u);
}

// ============================================================================
// RemoveThread / UpdateIterators branches
// ============================================================================

// Removing a tid that isn't present must be a no-op (loop falls through).
TEST(ThreadListTests, RemoveThread_NonExistentTid_NoOp) {
  ThreadList threadList;
  threadList.AddThread(1, reinterpret_cast<HANDLE>(0x1000));
  threadList.RemoveThread(999);  // not present
  EXPECT_EQ(threadList.Count(), 1u);
  uint32_t it = threadList.CreateIterator();
  auto t = threadList.LoopNext(it);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->GetThreadId(), 1);
}

// Removing the LAST element while an iterator points past the new end must
// reset that iterator to 0 (exercises the `pos >= size -> 0` branch).
TEST(ThreadListTests, RemoveThread_LastThread_ResetsIterator) {
  ThreadList threadList;
  threadList.AddThread(1, reinterpret_cast<HANDLE>(0x1000));
  threadList.AddThread(2, reinterpret_cast<HANDLE>(0x2000));
  threadList.AddThread(3, reinterpret_cast<HANDLE>(0x3000));

  uint32_t it = threadList.CreateIterator();
  threadList.LoopNext(it);  // -> 1, iter=1
  threadList.LoopNext(it);  // -> 2, iter=2 (points at thread 3)

  threadList.RemoveThread(3);  // removalPos == 2 == size after erase -> reset

  auto t = threadList.LoopNext(it);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->GetThreadId(), 1);  // iterator was reset to position 0
}

// Removing the FIRST element shifts every iterator left by one
// (exercises the `removalPos < pos -> pos - 1` branch).
TEST(ThreadListTests, RemoveThread_FirstThread_ShiftsIterator) {
  ThreadList threadList;
  threadList.AddThread(1, reinterpret_cast<HANDLE>(0x1000));
  threadList.AddThread(2, reinterpret_cast<HANDLE>(0x2000));
  threadList.AddThread(3, reinterpret_cast<HANDLE>(0x3000));

  uint32_t it = threadList.CreateIterator();
  threadList.LoopNext(it);  // -> 1, iter=1
  threadList.LoopNext(it);  // -> 2, iter=2 (points at thread 3)

  threadList.RemoveThread(1);  // removalPos 0 < 2 -> iter becomes 1

  auto t = threadList.LoopNext(it);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->GetThreadId(), 3);  // still pointing at thread 3
}

// Remove every thread, then LoopNext must safely return null.
TEST(ThreadListTests, RemoveThread_DownToEmpty_LoopNextReturnsNull) {
  ThreadList threadList;
  threadList.AddThread(1, reinterpret_cast<HANDLE>(0x1000));
  threadList.AddThread(2, reinterpret_cast<HANDLE>(0x2000));
  uint32_t it = threadList.CreateIterator();
  threadList.RemoveThread(1);
  threadList.RemoveThread(2);
  EXPECT_EQ(threadList.Count(), 0u);
  EXPECT_EQ(threadList.LoopNext(it), nullptr);
}

// RemoveThread must fix up MULTIPLE iterators at different positions
// (existing removal test only has one iterator).
TEST(ThreadListTests, RemoveThread_UpdatesMultipleIterators) {
  ThreadList threadList;
  threadList.AddThread(1, reinterpret_cast<HANDLE>(0x1000));
  threadList.AddThread(2, reinterpret_cast<HANDLE>(0x2000));
  threadList.AddThread(3, reinterpret_cast<HANDLE>(0x3000));

  uint32_t a = threadList.CreateIterator();
  uint32_t b = threadList.CreateIterator();
  threadList.LoopNext(a);  // a -> 1, pos 1
  threadList.LoopNext(b);
  threadList.LoopNext(b);  // b -> pos 2

  threadList.RemoveThread(1);  // pos 0 removed: a stays valid, b shifts left

  auto ta = threadList.LoopNext(a);
  ASSERT_NE(ta, nullptr);
  EXPECT_EQ(ta->GetThreadId(), 2);
  auto tb = threadList.LoopNext(b);
  ASSERT_NE(tb, nullptr);
  EXPECT_EQ(tb->GetThreadId(), 3);
}

// ============================================================================
// Ordering / lifecycle interplay
// ============================================================================

// Iterator created before any thread exists, threads added afterwards.
TEST(ThreadListTests, CreateIterator_BeforeAddingThreads) {
  ThreadList threadList;
  uint32_t it = threadList.CreateIterator();
  EXPECT_EQ(threadList.LoopNext(it), nullptr);  // empty -> null
  threadList.AddThread(1, reinterpret_cast<HANDLE>(0x1000));
  auto t = threadList.LoopNext(it);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->GetThreadId(), 1);
}

// A thread added mid-iteration is eventually returned by the round-robin.
TEST(ThreadListTests, AddThread_DuringIteration_IsEventuallyReturned) {
  ThreadList threadList;
  threadList.AddThread(1, reinterpret_cast<HANDLE>(0x1000));
  threadList.AddThread(2, reinterpret_cast<HANDLE>(0x2000));
  uint32_t it = threadList.CreateIterator();

  threadList.LoopNext(it);  // -> 1
  threadList.AddThread(3, reinterpret_cast<HANDLE>(0x3000));  // appended

  EXPECT_EQ(threadList.LoopNext(it)->GetThreadId(), 2);
  EXPECT_EQ(threadList.LoopNext(it)->GetThreadId(), 3);  // new one picked up
  EXPECT_EQ(threadList.LoopNext(it)->GetThreadId(), 1);  // wrap
}
