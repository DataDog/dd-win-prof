// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <winternl.h>

#include "ThreadInfo.h"
#include "pch.h"

class StackFrameCollector {
 public:
  StackFrameCollector();
  ~StackFrameCollector();

  // for testing purposes
  bool CaptureStack(
      uint32_t threadID, uint64_t* pFrames, uint16_t& framesCount, bool& isTruncated
  );

  // Unwind the (already suspended) thread starting from the provided seed
  // CONTEXT. The seed is expected to come from TrySuspendThread; CaptureStack
  // does not query the thread context itself.
  bool CaptureStack(
      HANDLE hThread,
      const CONTEXT& seedContext,
      uint64_t* pFrames,
      uint16_t& framesCount,
      bool& isTruncated
  );

  // Suspend the target thread and fetch its CONTEXT in a single GetThreadContext
  // call. The successful GetThreadContext also acts as the synchronization fence
  // required after the asynchronous SuspendThread (see
  // https://devblogs.microsoft.com/oldnewthing/20150205-00/?p=44743).
  // On success, outContext is populated with CONTEXT_FULL and the caller MUST
  // eventually call ::ResumeThread on the thread.
  bool TrySuspendThread(
      std::shared_ptr<ThreadInfo> pThreadInfo, CONTEXT& outContext
  );

 private:
  bool TryGetThreadStackBoundaries(
      HANDLE threadHandle, DWORD64* pStackLimit, DWORD64* pStackBase
  );
  bool ValidatePointerInStack(
      DWORD64 pointerValue, DWORD64 stackLimit, DWORD64 stackBase
  );

 private:
  // Function pointer for NtQueryInformationThread
  typedef NTSTATUS(__stdcall* NtQueryInformationThreadDelegate_t)(
      HANDLE ThreadHandle,
      THREADINFOCLASS ThreadInformationClass,
      PVOID ThreadInformation,
      ULONG ThreadInformationLength,
      PULONG ReturnLength
  );
  static NtQueryInformationThreadDelegate_t s_ntQueryInformationThreadDelegate;
};
