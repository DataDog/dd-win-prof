// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#ifdef DD_WIN_PROF_EXPORTS
#define DD_WIN_PROF_API __declspec(dllexport)
#else
#define DD_WIN_PROF_API __declspec(dllimport)
#endif


// define a struct that will hold configuration parameters
typedef struct _ProfilerConfig
{
    uint32_t size; // size of this struct, for versioning

    // application information
    const char* serviceEnvironment;
    const char* serviceName;
    const char* serviceVersion;

    // DATADOG endpoint
    const char* url;
    const char* apiKey;

    // profiling tuning parameters
    uint64_t cpuWallTimeSamplingPeriodNs; // sampling period in nanoseconds (default: 20ms = 20,000,000ns)
    int32_t walltimeThreadsThreshold;     // number of threads to sample for wall time (default: 5, min: 5, max: 64)
    int32_t cpuThreadsThreshold;          // number of threads to sample for CPU time (default: 64, min: 5, max: 128)
} ProfilerConfig;


extern "C" {
    DD_WIN_PROF_API bool SetupProfiler(ProfilerConfig* pSettings);
    DD_WIN_PROF_API bool StartProfiler();
    DD_WIN_PROF_API void StopProfiler();
}