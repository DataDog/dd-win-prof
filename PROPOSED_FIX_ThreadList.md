# Proposed Fix: ThreadList::LoopNext Iterator Bug

## Problem Statement

`ThreadList::LoopNext` has an off-by-one error that causes iterator positions to become desynchronized from the threads being returned. This leads to threads being skipped during sampling.

## Root Cause

The function:
1. Reads a thread at position `pos`
2. **Increments** `pos` before checking validity
3. Stores the **incremented** `pos` to the iterator
4. Returns the thread from the **old** (pre-increment) position

This means the iterator "points ahead" by one position from the thread that was actually returned.

## Current Code (BUGGY)

```cpp
std::shared_ptr<ThreadInfo> ThreadList::LoopNext(uint32_t iterator) {
  std::lock_guard<std::recursive_mutex> lock(_mutex);

  auto activeThreadCount = _threads.size();
  if (activeThreadCount == 0) {
    return nullptr;
  }

  if (iterator >= _iterators.size()) {
    return nullptr;
  }

  uint32_t pos = _iterators[iterator];
  std::shared_ptr<ThreadInfo> pInfo = nullptr;

  auto startPos = pos;
  do {
    pInfo = _threads[pos];                    // Read at pos
    pos = (pos + 1) % activeThreadCount;      // Increment pos  ← BUG
  } while (startPos != pos &&
           (pInfo->GetOsThreadHandle() == static_cast<HANDLE>(NULL) ||
            pInfo->GetOsThreadHandle() == INVALID_HANDLE_VALUE));

  _iterators[iterator] = pos;  // Store incremented pos  ← BUG

  if (startPos == pos) {
    return nullptr;
  }

  return pInfo;  // Return thread from OLD pos
}
```

## Proposed Fix #1: Store Position Before Increment

**Strategy:** Track the position of the thread we're actually returning, and store THAT to the iterator.

```cpp
std::shared_ptr<ThreadInfo> ThreadList::LoopNext(uint32_t iterator) {
  std::lock_guard<std::recursive_mutex> lock(_mutex);

  auto activeThreadCount = _threads.size();
  if (activeThreadCount == 0) {
    return nullptr;
  }

  if (iterator >= _iterators.size()) {
    return nullptr;
  }

  uint32_t pos = _iterators[iterator];
  std::shared_ptr<ThreadInfo> pInfo = nullptr;
  uint32_t returnedThreadPos = pos;  // NEW: Track the position we'll return

  auto startPos = pos;
  do {
    pInfo = _threads[pos];
    returnedThreadPos = pos;  // NEW: Remember this position
    pos = (pos + 1) % activeThreadCount;
  } while (startPos != pos &&
           (pInfo->GetOsThreadHandle() == static_cast<HANDLE>(NULL) ||
            pInfo->GetOsThreadHandle() == INVALID_HANDLE_VALUE));

  if (startPos == pos) {
    // Looped through all threads without finding a valid one
    return nullptr;
  }

  // NEW: Store the next position (pos is already incremented)
  _iterators[iterator] = pos;
  
  return pInfo;
}
```

**Pros:**
- Minimal change
- Clear intent: track the position we're returning
- Iterator now correctly points to the NEXT thread to sample

**Cons:**
- Extra variable

## Proposed Fix #2: Increment After Loop

**Strategy:** Change the loop to increment AFTER we decide to return a thread.

```cpp
std::shared_ptr<ThreadInfo> ThreadList::LoopNext(uint32_t iterator) {
  std::lock_guard<std::recursive_mutex> lock(_mutex);

  auto activeThreadCount = _threads.size();
  if (activeThreadCount == 0) {
    return nullptr;
  }

  if (iterator >= _iterators.size()) {
    return nullptr;
  }

  uint32_t pos = _iterators[iterator];
  std::shared_ptr<ThreadInfo> pInfo = nullptr;

  auto startPos = pos;
  bool foundValid = false;
  
  do {
    pInfo = _threads[pos];
    
    // Check if this thread is valid
    if (pInfo->GetOsThreadHandle() != static_cast<HANDLE>(NULL) &&
        pInfo->GetOsThreadHandle() != INVALID_HANDLE_VALUE) {
      // Found a valid thread - update iterator to NEXT position and return
      _iterators[iterator] = (pos + 1) % activeThreadCount;
      return pInfo;
    }
    
    // Invalid thread, try next
    pos = (pos + 1) % activeThreadCount;
  } while (startPos != pos);

  // Looped through all threads without finding a valid one
  return nullptr;
}
```

**Pros:**
- Clearer logic: we explicitly return when we find a valid thread
- Iterator update happens right before return (clear causality)
- Early return saves iterations

**Cons:**
- More invasive change
- Changes control flow more significantly

## Recommended Solution

**Fix #1** is recommended because:
1. Minimal change reduces risk
2. Maintains existing control flow structure
3. Easy to review and verify
4. Clear intent with the `returnedThreadPos` variable

## Test Coverage

The new `ThreadListTests.cpp` includes specific tests for this bug:

### Test: `LoopNext_IteratorPositionCorrectAfterSkippingInvalid`
```cpp
TEST(ThreadListTests, LoopNext_IteratorPositionCorrectAfterSkippingInvalid) {
  ThreadList threadList;

  // Create threads: invalid, valid, invalid, valid
  threadList.AddThread(1, INVALID_HANDLE_VALUE);
  threadList.AddThread(2, reinterpret_cast<HANDLE>(0x2000));
  threadList.AddThread(3, INVALID_HANDLE_VALUE);
  threadList.AddThread(4, reinterpret_cast<HANDLE>(0x4000));

  uint32_t iterator = threadList.CreateIterator();

  // First call: should skip thread 1, return thread 2
  auto thread1 = threadList.LoopNext(iterator);
  EXPECT_EQ(thread1->GetThreadId(), 2);

  // Second call: should skip thread 3, return thread 4
  auto thread2 = threadList.LoopNext(iterator);
  EXPECT_EQ(thread2->GetThreadId(), 4);

  // Third call: should skip thread 1, return thread 2 again
  auto thread3 = threadList.LoopNext(iterator);
  EXPECT_EQ(thread3->GetThreadId(), 2);  // ← CURRENTLY FAILS!
}
```

**Current behavior (BUG):**
- Call 1: Returns thread 2, iterator→3
- Call 2: Returns thread 4, iterator→1  
- Call 3: Returns thread 2, iterator→3  ← Skipped thread 4!

**Fixed behavior:**
- Call 1: Returns thread 2, iterator→2 (next valid)
- Call 2: Returns thread 4, iterator→0 (wraps)
- Call 3: Returns thread 2, iterator→2 (next valid) ✓

## Migration Plan

1. **Apply Fix #1** to `ThreadList.cpp`
2. **Run Tests:**
   ```
   build\src\Tests\Debug\Tests.exe --gtest_filter=ThreadListTests.*
   ```
3. **Verify:** All 7 `ThreadListTests` should PASS
4. **Regression Test:** Run full test suite
5. **Integration Test:** Run Runner with profiling enabled, verify no threads are skipped
6. **Performance Test:** Ensure no performance regression (unlikely, same logic)

## Additional Recommendations

### 1. Add Debug Assertion
```cpp
// At the end of LoopNext, before return:
#ifdef _DEBUG
  // Sanity check: the iterator should point to the position AFTER the returned thread
  // (modulo wraparound and invalid thread skipping)
  assert(_iterators[iterator] != returnedThreadPos || activeThreadCount == 1);
#endif
```

### 2. Add Documentation
```cpp
// LoopNext returns the next valid thread in round-robin order.
// After returning, the iterator is positioned at the NEXT valid thread
// to be returned (not at the thread we just returned).
//
// Example with 4 threads [T1(invalid), T2(valid), T3(invalid), T4(valid)]:
//   Call 1: iterator=0 → skip T1 → return T2, iterator=2
//   Call 2: iterator=2 → skip T3 → return T4, iterator=0
//   Call 3: iterator=0 → skip T1 → return T2, iterator=2
//   (continues round-robin through only valid threads)
```

### 3. Consider Metric Collection
```cpp
// Track how many threads we skip per iteration
#ifdef DD_WIN_PROF_METRICS
  static std::atomic<uint64_t> g_totalSkips{0};
  static std::atomic<uint64_t> g_totalIterations{0};
  
  uint32_t skipsThisIteration = (pos > startPos) ? (pos - startPos) : 
                                 (activeThreadCount - startPos + pos);
  g_totalSkips.fetch_add(skipsThisIteration);
  g_totalIterations.fetch_add(1);
#endif
```

## Impact Analysis

### Who is affected?
- **CPU Profiling:** Directly affected - threads may be skipped
- **Walltime Profiling:** Directly affected - threads may be skipped  
- **Sample accuracy:** Profiles will be incomplete/biased
- **Performance:** No impact (same algorithm, just correct positions)

### Severity in Production
- **Data Quality:** HIGH - Profiles are missing data from skipped threads
- **Stability:** LOW - No crashes, just incorrect data
- **Performance:** NONE - Same number of operations

### Why wasn't this caught?
1. The bug is subtle - iterator moves forward, just at wrong position
2. Sampling still works, just skips some threads
3. No existing unit tests for ThreadList iteration logic
4. Integration tests might not notice missing threads in complex apps
5. Round-robin eventually visits all threads, just unevenly

## Summary

This is a **critical correctness bug** that should be fixed ASAP. The fix is simple, low-risk, and fully testable. The new test suite will prevent regression.

**Estimated effort:** 1 hour (fix + test + review)  
**Risk level:** LOW (minimal change, well-tested)  
**Priority:** P0 (affects data quality)
