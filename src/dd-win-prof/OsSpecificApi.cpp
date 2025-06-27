// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"

#include <winternl.h>

namespace OsSpecificApi {

    const uint64_t msInSecond = 1000;
    const uint64_t msInMinute = 60 * 1000;
    const uint64_t msInHour = 60 * 60 * 1000;
    const uint64_t msInDay = 24 * 60 * 60 * 1000;

    uint64_t GetTotalMilliseconds(SYSTEMTIME time)
    {
        uint64_t total = time.wMilliseconds;
        if (time.wSecond != 0)
        {
            total += (uint64_t)time.wSecond * msInSecond;
        }
        if (time.wMinute != 0)
        {
            total += (uint64_t)time.wMinute * msInMinute;
        }
        if (time.wHour != 0)
        {
            total += (uint64_t)time.wHour * msInHour;
        }
        if (time.wDay != 0)
        {
            total += (uint64_t)(time.wDay - 1) * msInDay; // january 1st 1601
        }

        // don't deal with month duration...

        return total;
    }

    uint64_t GetTotalMilliseconds(FILETIME fileTime)
    {
        SYSTEMTIME systemTime;
        ::FileTimeToSystemTime(&fileTime, &systemTime);
        return GetTotalMilliseconds(systemTime);
    }

    std::chrono::milliseconds GetThreadCpuTime(HANDLE hThread)
    {
        FILETIME creationTime, exitTime = {}; // not used here
        FILETIME kernelTime = {};
        FILETIME userTime = {};
        static bool isFirstError = true;

        if (::GetThreadTimes(hThread, &creationTime, &exitTime, &kernelTime, &userTime))
        {
            uint64_t milliseconds = GetTotalMilliseconds(userTime) + GetTotalMilliseconds(kernelTime);
            return std::chrono::milliseconds(milliseconds);
        }

        return 0ms;
    }

    typedef LONG KPRIORITY;

    struct CLIENT_ID
    {
        DWORD UniqueProcess; // Process ID
#ifdef BIT64
        ULONG pad1;
#endif
        DWORD UniqueThread; // Thread ID
#ifdef BIT64
        ULONG pad2;
#endif
    };

    typedef struct
    {
        FILETIME KernelTime;
        FILETIME UserTime;
        FILETIME CreateTime;
        ULONG WaitTime;
#ifdef BIT64
        ULONG pad1;
#endif
        PVOID StartAddress;
        CLIENT_ID Client_Id;
        KPRIORITY CurrentPriority;
        KPRIORITY BasePriority;
        ULONG ContextSwitchesPerSec;
        ULONG ThreadState;
        ULONG ThreadWaitReason;
        ULONG pad2;
    } SYSTEM_THREAD_INFORMATION;

    typedef enum
    {
        Initialized,
        Ready,
        Running,
        Standby,
        Terminated,
        Waiting,
        Transition,
        DeferredReady
    } THREAD_STATE;

    #define SYSTEMTHREADINFORMATION 40
    typedef NTSTATUS(WINAPI* NtQueryInformationThread_)(HANDLE, int, PVOID, ULONG, PULONG);
    NtQueryInformationThread_ NtQueryInformationThread = nullptr;

    typedef BOOL(WINAPI* GetLogicalProcessorInformation_)(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD);
    GetLogicalProcessorInformation_ GetLogicalProcessorInformation = nullptr;

    bool InitializeNtQueryInformationThreadCallback()
    {
        auto hModule = GetModuleHandleA("NtDll.dll");
        if (hModule == nullptr)
        {
            return false;
        }

        NtQueryInformationThread = (NtQueryInformationThread_)GetProcAddress(hModule, "NtQueryInformationThread");
        if (NtQueryInformationThread == nullptr)
        {
            return false;
        }

        return true;
    }

    bool IsRunning(ULONG threadState)
    {
        return (THREAD_STATE::Running == threadState) ||
            (THREAD_STATE::DeferredReady == threadState) ||
            (THREAD_STATE::Standby == threadState);

        // Note that THREAD_STATE::Standby, THREAD_STATE::Ready and THREAD_STATE::DeferredReady
        // indicate that threads are simply waiting for an available core to run.
        // If some callstacks show non cpu-bound frames at the top, return true only for Running state
    }

    //    isRunning,        cpu time          , failed
    std::tuple<bool, std::chrono::milliseconds, bool> IsRunning(HANDLE hThread)
    {
        if (NtQueryInformationThread == nullptr)
        {
            if (!InitializeNtQueryInformationThreadCallback())
            {
                return { false, 0ms, true };
            }
        }

        SYSTEM_THREAD_INFORMATION sti = { 0 };
        auto size = sizeof(SYSTEM_THREAD_INFORMATION);
        ULONG buflen = 0;
        static bool isFirstError = true;
        NTSTATUS lResult = NtQueryInformationThread(hThread, SYSTEMTHREADINFORMATION, &sti, static_cast<ULONG>(size), &buflen);
        // deal with an invalid thread handle case (thread might have died)
        if (lResult != 0)
        {
            // This always happens in 32 bit so uses another API to at least get the CPU consumption
            return { false, GetThreadCpuTime(hThread), true };
        }

        auto cpuTime = std::chrono::milliseconds(GetTotalMilliseconds(sti.UserTime) + GetTotalMilliseconds(sti.KernelTime));

        return { IsRunning(sti.ThreadState), cpuTime, false };
    }

    uint32_t GetProcessorCount()
    {
        // https://devblogs.microsoft.com/oldnewthing/20200824-00/?p=104116
        auto nbProcs = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (nbProcs == 0)
        {
            return 1;
        }

        return nbProcs;
    }
} // namespace OsSpecificApi