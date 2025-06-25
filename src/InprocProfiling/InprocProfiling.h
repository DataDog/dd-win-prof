// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

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