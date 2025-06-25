#pragma once

#ifdef INPROCPROFILING_EXPORTS
#define INPROCPROFILING_API __declspec(dllexport)
#else
#define INPROCPROFILING_API __declspec(dllimport)
#endif

extern "C" {
    INPROCPROFILING_API bool StartProfiler();
    INPROCPROFILING_API void StopProfiler();
}