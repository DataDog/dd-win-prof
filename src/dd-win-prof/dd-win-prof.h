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

    // env vars usage
    bool noEnvVars; // if true, ignore all environment variables and use only the values provided in this struct (default: false)

    // application information
    const char* serviceEnvironment;
    const char* serviceName;
    const char* serviceVersion;

    // Datadog endpoint: these are MANDATORY if noEnvVars is true, otherwise they can be set via environment variables instead
    const char* url;
    const char* apiKey;

    // profiling tuning parameters
    uint64_t cpuWallTimeSamplingPeriodNs;   // sampling period in nanoseconds (default: 20ms = 20,000,000ns)
    int32_t walltimeThreadsThreshold;       // number of threads to sample for wall time (default: 5, min: 5, max: 64)
    int32_t cpuThreadsThreshold;            // number of threads to sample for CPU time (default: 64, min: 5, max: 128)
    int32_t uploadIntervalSeconds;          // how often to upload profiles in seconds (default: 60s or 20s in dev mode)
    const char* tags;                       // additional tags to attach to profiles as comma separated list (default: empty)

    // symbolization
    bool symbolizeCallstacks;        // whether to symbolize stack traces (default: false)

    // debug / test settings (normally set via environment variables)
    // NOTE: log directory is NOT configurable here -- it is set at DLL load time
    //       via DD_TRACE_LOG_DIRECTORY (default: %PROGRAMDATA%\Datadog Tracer\logs)
    const char* pprofOutputDirectory;   // override for pprof debug output directory (default: empty = disabled)
} ProfilerConfig;


// RUM (Real User Monitoring) context — split into stable vs volatile APIs.
//
// Stable context: application_id and session_id change infrequently (session start,
// session rotation). These are used for profile-level pprof tags. Can allocate.
typedef struct _RumSessionContext
{
    const char* application_id;  // UUID string, set once per process lifetime
    const char* session_id;      // UUID string, changes on session rotation
} RumSessionContext;

// Volatile context: view_id and view_name change on every navigation.
// HOT PATH for per-sample pprof labels (rum.view_id, trace endpoint).
typedef struct _RumViewValues
{
    const char* view_id;    // UUID string; nullptr or "" to clear current view
    const char* view_name;  // human-readable name, e.g. "HomePage"
} RumViewValues;

extern "C" {
    DD_WIN_PROF_API bool SetupProfiler(const ProfilerConfig* pSettings);

    // Start profiling manually (returns false if already started or explicitly disabled)
    DD_WIN_PROF_API bool StartProfiler();

    // Stop profiling manually (safe to call even if not started)
    DD_WIN_PROF_API void StopProfiler();

    // Set stable RUM session context. Safe to call from any thread.
    // application_id: write-once per process. First non-empty value is stored as a
    //   profile-level tag; subsequent calls with a different value are rejected (returns false).
    // session_id: updated on every call. Session transitions are tracked with timestamps;
    //   the profile-level rum.session_id tag lists all session_ids since the last export.
    DD_WIN_PROF_API bool SetRumSession(const RumSessionContext* pContext);

    // Set volatile RUM view context. Safe to call from any thread.
    // Updates per-sample pprof labels: rum.view_id and trace endpoint (view_name).
    // Pass nullptr/empty view_id to clear the current view (signals "between views").
    DD_WIN_PROF_API void SetRumView(const RumViewValues* pContext);

    // Clear all RUM context (both session and view). Call on session end.
    DD_WIN_PROF_API void ClearRumContext();

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