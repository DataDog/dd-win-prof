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


// RUM (Real User Monitoring) context values (legacy combined struct).
typedef struct _RumContextValues
{
    const char* application_id;  // UUID string, set once per process lifetime
    const char* session_id;      // UUID string, can change on session rotation
    const char* view_id;         // nullptr or "" to clear current view
    const char* view_name;       // human-readable name, e.g. "HomePage"
} RumContextValues;

extern "C" {
    DD_WIN_PROF_API bool SetupProfiler(ProfilerConfig* pSettings);
    DD_WIN_PROF_API bool StartProfiler();
    DD_WIN_PROF_API void StopProfiler();

    // --- RUM context (split by change frequency) ---

    // Set the RUM application ID. Write-once: the first non-empty value wins;
    // subsequent calls with a *different* ID return false.
    DD_WIN_PROF_API bool SetRumApplicationId(const char* applicationId);

    // Set/rotate the RUM session. When the session ID changes, the previous
    // session is completed with a timestamp and duration.
    DD_WIN_PROF_API bool SetRumSession(const char* sessionId);

    // Set or clear the active view. Pass nullptr or "" to signal "between views";
    // this completes the current view record with a duration.
    DD_WIN_PROF_API bool SetRumView(const char* viewId, const char* viewName);

    // Legacy combined API -- calls the three functions above in order.
    DD_WIN_PROF_API bool UpdateRumContext(const RumContextValues* pContext);
}