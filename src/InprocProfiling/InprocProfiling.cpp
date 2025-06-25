#include "pch.h"
#include "InprocProfiling.h"
#include "Log.h"
#include "Profiler.h"

extern "C" {

INPROCPROFILING_API bool StartProfiler()
{
    auto profiler = Profiler::GetInstance();
    if (profiler->IsStarted())
    {
        Log::Warn("Profiler is already running.");
        return false;
    }

    return profiler->StartProfiling();
}

INPROCPROFILING_API void StopProfiler()
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