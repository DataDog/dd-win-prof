// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#ifdef DD_WIN_PROF_STATIC_LINK
#define DD_WIN_PROF_API
#elif defined(DD_WIN_PROF_EXPORTS)
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

    // symbolization
    bool symbolizeCallstacks;        // whether to symbolize stack traces (default: false)
} ProfilerConfig;


/**
 * RUM (Real User Monitoring) context values used for correlating CPU profiles
 * with user sessions, views, and actions in the Datadog RUM product.
 *
 * All UUID pointers can be NULL or point to empty strings to indicate absence
 * of that particular identifier. Expected UUID format is hyphenated lowercase
 * (e.g., "550e8400-e29b-41d4-a716-446655440000").
 */
typedef struct _RumContextValues
{
    const char* application_id;   // UUID string or NULL
    const char* session_id;       // UUID string or NULL
    const char* view_id;          // UUID string or NULL
    const char* action_id;        // UUID string or NULL
} RumContextValues;


extern "C" {
    DD_WIN_PROF_API bool SetupProfiler(ProfilerConfig* pSettings);
    // Start profiling manually (returns false if already started or explicitly disabled)
    DD_WIN_PROF_API bool StartProfiler();

    // Stop profiling manually (safe to call even if not started)
    DD_WIN_PROF_API void StopProfiler();

    /**
     * Updates the global RUM context used for CPU profile correlation.
     *
     * Thread-safety: Safe to call from any thread at any time. Concurrent calls
     * are allowed; the last update to complete will be visible to future samples.
     *
     * Performance: Wait-free on the update path. Samples will atomically observe
     * either the old or new context (never corrupted state).
     *
     * UUID format: Expected format is hyphenated lowercase UUID
     * (e.g., "550e8400-e29b-41d4-a716-446655440000"). No validation is performed;
     * strings longer than 36 characters will be truncated.
     *
     * Memory allocation: If memory allocation fails, the RUM context will be
     * cleared (all UUIDs set to empty) and samples will have no RUM labels until
     * the next successful update.
     *
     * @param context Pointer to RUM context values. NULL pointers for individual
     *                UUIDs will be treated as empty strings and omitted from labels.
     */
    DD_WIN_PROF_API void UpdateRumContext(const RumContextValues* context);

    // Environment Variables (independent controls):
    // DD_PROFILING_ENABLED: Controls whether profiler CAN be started
    //   - false/0: Blocks all profiling (security override)
    //   - true/1 or not set: Allows profiling
    //
    // Test setting ONLY
    // Zero-code profiling: Use ProfilerInjector.exe to inject the DLL with auto-start
    // DD_PROFILING_AUTO_START: Controls whether profiler auto-starts when DLL loads
    //   - true/1: Auto-start when DLL is injected/loaded
    //   - false/0 or not set: Manual control only
    //
}