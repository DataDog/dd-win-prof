// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"
#include "StackFrameCollector.h"

StackFrameCollector::NtQueryInformationThreadDelegate_t StackFrameCollector::s_ntQueryInformationThreadDelegate = nullptr;

StackFrameCollector::StackFrameCollector()
{
}

StackFrameCollector::~StackFrameCollector()
{
}

bool StackFrameCollector::CaptureStack(uint32_t threadID, uint64_t* pFrames, uint16_t& framesCount, bool& isTruncated)
{
    return false;
}

bool StackFrameCollector::CaptureStack(HANDLE hThread, uint64_t* pFrames, uint16_t& framesCount, bool& isTruncated)
{
    isTruncated = false;
    uint16_t maxFramesCount = framesCount;

    CONTEXT context;
    context.ContextFlags = CONTEXT_FULL;
    BOOL hasInfo = ::GetThreadContext(hThread, &context);

    if (!hasInfo)
    {
        return false;
    }

    // Get thread stack limits:
    DWORD64 stackLimit = 0;
    DWORD64 stackBase = 0;
    TryGetThreadStackBoundaries(hThread, &stackLimit, &stackBase);

    uint64_t imageBaseAddress = 0;
    UNWIND_HISTORY_TABLE historyTable;
    ::RtlZeroMemory(&historyTable, sizeof(UNWIND_HISTORY_TABLE));
    historyTable.Search = TRUE;

    void* pHandlerData = nullptr;
    DWORD64 establisherFrame = 0;
    const PKNONVOLATILE_CONTEXT_POINTERS pNonVolatileContextPtrsIsNull = nullptr;
    RUNTIME_FUNCTION* pFunctionTableEntry;

    framesCount = 0;
    do
    {
        if (framesCount >= maxFramesCount)
        {
            isTruncated = true;
            break;
        }
        pFrames[framesCount++] = context.Rip;

        __try
        {
            // Sometimes, we could hit an access violation, so catch it and just return.
            // We want to prevent this from killing the application
            pFunctionTableEntry = ::RtlLookupFunctionEntry(context.Rip, &imageBaseAddress, &historyTable);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        // RtlLookupFunctionEntry() may try to acquire global locks. The StackSamplerLoopManager should detect it and resume the
        // target thread, which will eventually allow the lookup to complete. In such case, the stack is invalid. Give up:
        // TODO: no need to handle this case for the POC - look at StackSamplerLoopManager implementation for more details

        if (nullptr == pFunctionTableEntry)
        {
            // So, we have a leaf function on the top of the stack. The calling convention rules imply:
            //     a) No RUNTIME_FUNCTION entry => Leaf function => this function does not modify stack
            //     b) RSP points to the top of the stack and
            //        the address it points to contains the return pointer from this leaf function.
            // So, we unwind one frame manually:
            //     1) Perform a virtual return:
            //         - The value at the top of the stack is the return address of this frame.
            //           That value needs to be written to the virtual instruction pointer register (RIP)
            //         - To access that value, we dereference the virtual stack register RSP,
            //           which contains the address of the logical top of the stack
            //     2) Perform a virtual stack pop to account for the value we just used from the stack:
            //         - Remember that x64 stack grows physically downwards.
            //         - So, add 8 bytes (=64 bits = sizeof(pointer)) to the virtual stack register RSP
            //
            __try
            {
                // FIX: For a customer using the SentinelOne solution, it was not possible to walk the stack
                //      of a thread so RSP was not valid
                context.Rip = *reinterpret_cast<uint64_t*>(context.Rsp);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }

            context.Rsp += 8;
        }
        else
        {
            // So, pFunctionTableEntry is not NULL. Unwind one frame.
            __try
            {
                // Sometimes, we could hit an access violation, so catch it and jus return.
                // We want to prevent this from killing the main application
                // Maybe cause by an incomplete context.
                ::RtlVirtualUnwind(UNW_FLAG_NHANDLER,
                    imageBaseAddress,
                    context.Rip,
                    pFunctionTableEntry,
                    &context,
                    &pHandlerData,
                    &establisherFrame,
                    pNonVolatileContextPtrsIsNull);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }

            // RtlVirtualUnwind() may try to acquire global locks. The StackSamplerLoopManager should detect it and resume the
            // target thread, which will eventually allow the unwind to complete. In such case, the stack is invalid. Give up:
            // TODO: no need to handle this case for the POC - look at StackSamplerLoopManager implementation for more details

            // Sanity checks:
            if (!ValidatePointerInStack(establisherFrame, stackLimit, stackBase))
            {
                return false;
            }
        }

        if (!ValidatePointerInStack(context.Rsp, stackLimit, stackBase))
        {
            return false;
        }

    } while (context.Rip != 0);

    return true;
}

bool StackFrameCollector::TrySuspendThread(std::shared_ptr<ThreadInfo> pThreadInfo)
{
    HANDLE hThread = pThreadInfo->GetOsThreadHandle();
    DWORD suspendCount = ::SuspendThread(hThread);
    if (suspendCount == static_cast<DWORD>(-1))
    {
        // We wanted to suspend, but it resulted in error.
        // This can happen when the thread died after we called LoopNext()
        // Give up.

        return false;
    }

    // if suspendCount > 0, it means that we are not the only one who suspended the thread.
    // Might be a debugger or a different profiler. Assuming that we do correct suspension management
    // (and we have biger problems if we do not), this should be benign.

    // SuspendThread is asynchronous and requires GetThreadContext to be called.
    // https://devblogs.microsoft.com/oldnewthing/20150205-00/?p=44743
    if (EnsureThreadIsSuspended(hThread))
    {
        return true;
    }

    // The thread might have exited or being terminated
    // Same as in the CLR: https://sourcegraph.com/github.com/dotnet/runtime/-/blob/src/coreclr/vm/threadsuspend.cpp?L272
    ::ResumeThread(hThread);

    return false;
}

bool StackFrameCollector::ValidatePointerInStack(DWORD64 pointerValue, DWORD64 stackLimit, DWORD64 stackBase)
{
    // Validate that the establisherFrame 8-byte aligned:
    if (0 != (0x7 & pointerValue))
    {
        return false;
    }

    // Validate stack limits. Attention! This may not apply to kernel frames / DPC stacks (http://www.nynaeve.net/?p=106):
    if ((stackLimit != 0 || stackBase != 0) && (pointerValue < stackLimit || stackBase <= pointerValue))
    {
        // Remember that stack grows downwards, i.e. stackLimit <= stackBase.
        return false;
    }

    return true;
}

constexpr THREADINFOCLASS ThreadInfoClass_ThreadBasicInformation = static_cast<THREADINFOCLASS>(0x0);
typedef struct _THREAD_BASIC_INFORMATION
{
    NTSTATUS ExitStatus;
    PVOID TebBaseAddress;
    CLIENT_ID ClientId;
    KAFFINITY AffinityMask;
    KPRIORITY Priority;
    KPRIORITY BasePriority;
} THREAD_BASIC_INFORMATION, * PTHREAD_BASIC_INFORMATION;

bool StackFrameCollector::TryGetThreadStackBoundaries(HANDLE threadHandle, DWORD64* pStackLimit, DWORD64* pStackBase)
{
    *pStackLimit = *pStackBase = 0;
    NtQueryInformationThreadDelegate_t ntQueryInformationThreadDelegate = s_ntQueryInformationThreadDelegate;
    if (nullptr == ntQueryInformationThreadDelegate)
    {
        HMODULE moduleHandle = ::GetModuleHandleW(L"ntdll.dll");
        if (NULL == moduleHandle)
        {
            moduleHandle = ::LoadLibraryW(L"ntdll.dll");
        }

        if (NULL != moduleHandle)
        {
            ntQueryInformationThreadDelegate = reinterpret_cast<NtQueryInformationThreadDelegate_t>(GetProcAddress(moduleHandle, "NtQueryInformationThread"));
            s_ntQueryInformationThreadDelegate = ntQueryInformationThreadDelegate;
        }
    }
    if (nullptr == ntQueryInformationThreadDelegate)
    {
        return false;
    }

    THREAD_BASIC_INFORMATION threadBasicInfo;
    ULONG threadInfoResultSize;
    bool hasThreadInfo = (0 == ntQueryInformationThreadDelegate(threadHandle,
        ThreadInfoClass_ThreadBasicInformation,
        &threadBasicInfo,
        sizeof(THREAD_BASIC_INFORMATION),
        &threadInfoResultSize));
    hasThreadInfo = hasThreadInfo && (threadInfoResultSize <= sizeof(THREAD_BASIC_INFORMATION));

    if (hasThreadInfo)
    {
        NT_TIB* pThreadTib = static_cast<NT_TIB*>(threadBasicInfo.TebBaseAddress);
        if (nullptr != pThreadTib)
        {
            *pStackBase = reinterpret_cast<DWORD64>(pThreadTib->StackBase);
            *pStackLimit = reinterpret_cast<DWORD64>(pThreadTib->StackLimit);
            return true;
        }
    }

    return false;
}
