// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"
#include "dd-win-prof.h"
#include "Log.h"
#include "Profiler.h"

extern "C" {

DD_WIN_PROF_API bool StartProfiler()
{
    auto profiler = Profiler::GetInstance();
    if (profiler->IsStarted())
    {
        Log::Warn("Profiler is already running.");
        return false;
    }

    return profiler->StartProfiling();
}

DD_WIN_PROF_API void StopProfiler()
{
    auto profiler = Profiler::GetInstance();
    if (!profiler->IsStarted())
    {
        Log::Warn("Profiler is not running.");
        return;
    }

    profiler->StopProfiling();
}

}