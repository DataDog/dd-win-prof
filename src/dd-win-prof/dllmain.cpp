// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

// dllmain.cpp : Defines the entry point for the DLL application.

#include "pch.h"

#include "Log.h"
#include "Profiler.h"

// The DLL entry point is used to create the Profiler instance and detect threads creation
// NOTE: we don't support dynamic linking because we would miss the threads previously created
BOOL APIENTRY DllMain(HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
        case DLL_PROCESS_ATTACH:
        {
            // use the current thread as the application main thread
            auto mainThreadId = ::GetCurrentThreadId();
            Log::Debug(">   Main ", mainThreadId);

            auto profiler = new Profiler();

            // we need to keep track of the main thread here even if the profiler is not started yet
            profiler->AddCurrentThread();

            // Auto-start profiler if DD_PROFILING_AUTO_START is set to true
            if (profiler->IsAutoStartEnabled())
            {
                Log::Info("Auto-starting profiler (DD_PROFILING_AUTO_START=true)");
                profiler->StartProfiling();
            }
        }
        break;

        case DLL_THREAD_ATTACH:
        {
            // the current thread is the new thread being created
            auto threadId = ::GetCurrentThreadId();
            Log::Debug("+ Thread ", threadId);

            auto profiler = Profiler::GetInstance();
            if (profiler == nullptr)
            {
                break;
            }

            profiler->AddCurrentThread();
        }
        break;

        case DLL_THREAD_DETACH:
        {
            // this is called when a thread exits cleanly
            Log::Debug("- Thread ", ::GetCurrentThreadId());

            auto profiler = Profiler::GetInstance();
            if (profiler == nullptr)
            {
                break;
            }

            profiler->RemoveCurrentThread();
        }
        break;

        case DLL_PROCESS_DETACH:
        {
            // this is called when the process exits or the DLL is unloaded (which is not supported)
            Log::Debug("<   Detach from ", ::GetCurrentThreadId());

            auto profiler = Profiler::GetInstance();
            if (profiler != nullptr)
            {
                // Stop profiling with shutdown flag to indicate dangerous cleanup should be skipped
                Log::Debug("    ", profiler->GetThreadCount(), " threads");
                profiler->StopProfiling(true); // shutdownOngoing = true
                delete profiler;
            }
        }
        break;
    }

    return TRUE;
}

