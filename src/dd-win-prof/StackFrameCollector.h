// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "pch.h"
#include <winternl.h>
#include "ThreadInfo.h"

class StackFrameCollector
{
public:
    StackFrameCollector();
    ~StackFrameCollector();

    // for testing purposes
    bool CaptureStack(uint32_t threadID, uint64_t* pFrames, uint16_t& framesCount, bool& isTruncated);

    bool CaptureStack(HANDLE hThread, uint64_t* pFrames, uint16_t& framesCount, bool& isTruncated);
    bool TrySuspendThread(std::shared_ptr<ThreadInfo> pThreadInfo);

private:
    bool TryGetThreadStackBoundaries(HANDLE threadHandle, DWORD64* pStackLimit, DWORD64* pStackBase);
    bool ValidatePointerInStack(DWORD64 pointerValue, DWORD64 stackLimit, DWORD64 stackBase);

    inline bool EnsureThreadIsSuspended(HANDLE hThread)
    {
        CONTEXT ctx;
        ctx.ContextFlags = CONTEXT_INTEGER;

        return ::GetThreadContext(hThread, &ctx);
    }

private:
    // Function pointer for NtQueryInformationThread
    typedef NTSTATUS(__stdcall* NtQueryInformationThreadDelegate_t)(HANDLE ThreadHandle, THREADINFOCLASS ThreadInformationClass, PVOID ThreadInformation, ULONG ThreadInformationLength, PULONG ReturnLength);
    static NtQueryInformationThreadDelegate_t s_ntQueryInformationThreadDelegate;
};

