// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef DD_WIN_PROF_STATIC_LINK
#define DD_WIN_PROF_API
#elif defined(DD_WIN_PROF_EXPORTS)
#define DD_WIN_PROF_API __declspec(dllexport)
#else
#define DD_WIN_PROF_API __declspec(dllimport)
#endif

// API proposal only. Implementation and callers have not been updated.
// The RUM SDK owns collection cadence, application tags, persistence, and upload.
typedef struct Profiler Profiler;

typedef enum _ProfilingStatus {
  PROFILING_OK = 0,
  PROFILING_NO_DATA,
  PROFILING_INVALID_ARGUMENT,
  PROFILING_INVALID_STATE,
  PROFILING_ERROR
} ProfilingStatus;

// Configuration is explicit; environment variables do not supply these settings.
typedef struct _ProfilerConfig {
  uint32_t size;  // sizeof(ProfilerConfig)

  // Zero selects the default for numeric tuning settings.
  uint64_t cpuWallTimeSamplingPeriodNs;  // default: 20,000,000ns
  int32_t walltimeThreadsThreshold;     // default: 5, min: 5, max: 64
  int32_t cpuThreadsThreshold;          // default: 64, min: 5, max: 128
  uint8_t symbolizeCallstacks;          // default: 0
} ProfilerConfig;

typedef struct _ProfilingData {
  const uint8_t* data;
  size_t size;
} ProfilingData;

#ifdef __cplusplus
extern "C" {
#endif

// Creates one stopped profiler; only one live handle is allowed per process.
// NULL config selects defaults. On failure, a non-NULL out is set to NULL.
DD_WIN_PROF_API ProfilingStatus SetupProfiler(
    const ProfilerConfig* config, Profiler** out
);

// Returns INVALID_STATE if already started.
DD_WIN_PROF_API ProfilingStatus StartProfiler(Profiler* profiler);

// Stops sampling and drains pending samples without discarding collected data.
// Safe to call when already stopped. Retrieve the final batch before destroying.
DD_WIN_PROF_API ProfilingStatus StopProfiler(Profiler* profiler);

// Returns accumulated data since the last successful retrieval and starts a new
// collection window. Also callable after StopProfiler to retrieve the final batch.
// The payload contains pprof and profile/RUM metadata; encoding is to be agreed
// with the RUM SDK. It is not an HTTP request body.
// Borrowed bytes remain valid until the next GetProfilingData call on this handle
// or DestroyProfiler, including across StopProfiler and background sampling.
// NO_DATA and errors clear a non-NULL out to {NULL, 0}; errors retain pending data.
// The caller must serialize setup/start/stop/get/destroy calls and copy the bytes
// before asynchronous persistence or upload. The profiler does not upload data.
DD_WIN_PROF_API ProfilingStatus GetProfilingData(
    Profiler* profiler, ProfilingData* out
);

// Stops sampling and frees the handle and borrowed data. NULL is a no-op.
DD_WIN_PROF_API void DestroyProfiler(Profiler* profiler);

// Generates a view_id and completes any active view first.
// Returns INVALID_STATE if no session is active.
DD_WIN_PROF_API ProfilingStatus EnterView(Profiler* profiler, const char* viewName);

// Completes the active view. Safe to call when no view is active.
DD_WIN_PROF_API ProfilingStatus LeaveCurrentView(Profiler* profiler);

#ifdef __cplusplus
}
#endif
