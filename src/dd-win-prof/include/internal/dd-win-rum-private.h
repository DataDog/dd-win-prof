// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "dd-win-prof.h"
#include "error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ProfilingData {
  // Size of this append-only structure. Future fields must be appended after reserved.
  uint32_t struct_size;
  // Zero on success. Nonzero values are opaque errors with null pprof bytes;
  // pass them to GetProfilingErrorMessage for static descriptive text.
  uint32_t result;
  // Zstd-compressed pprof (libdatadog v47). Older profiler versions emitted LZ4.
  // Consumers supporting both versions must detect compression from the frame bytes.
  // Immutable library-owned bytes, valid only during the synchronous callback.
  const uint8_t* pprof_bytes;
  size_t pprof_size;
  // Wall-clock milliseconds since Unix epoch, not monotonic timestamps.
  int64_t start_unix_time_ms;
  int64_t end_unix_time_ms;
  // True for the final export from an explicit StopProfiler(), including errors.
  uint8_t is_last;
  // Zero-filled tail reserved for alignment. Do not repurpose these bytes.
  uint8_t reserved[7];
} ProfilingData;

// Periodic calls run on the exporter thread; final export runs on the stopping thread.
// Do not block. Lifecycle and registration APIs called from this callback fail without
// effect, including StopProfiler, which returns without stopping.
// Callbacks must not throw or let exceptions cross this C boundary.
// Preparation errors can follow a delivered profile. Failed deliveries are not retried.
// Errors do not stop profiling.
typedef void(DD_WIN_PROF_CALL* ProfilingDataCallback)(
    const ProfilingData* data, void* user_data
);

typedef struct _RumSessionContext {
  const char* application_id;  // UUID string, set once per process lifetime
  const char* session_id;      // UUID string, changes on session rotation
} RumSessionContext;

typedef struct _RumViewValues {
  const char* view_id;    // UUID string; NULL or "" to clear current view
  const char* view_name;  // human-readable name, e.g. "HomePage"
} RumViewValues;

// Register while stopped: waits for an in-progress start or stop, and fails while the
// profiler runs or from the profiling callback. A non-null callback may use
// a null context. (NULL, NULL) unregisters and restores HTTP delivery; (NULL, non-NULL)
// is rejected without changing the current registration.
// Registration survives setup and stop/start. Failure preserves the previous callback.
// Register before setup to omit HTTP credentials in noEnvVars mode; StartProfiler()
// rejects such a setup if the callback is unregistered before it starts.
// Callback mode discards RUM records; per-sample labels remain in pprof.
// The profiler never owns user_data. Safe teardown is StopProfiler(), unregister, then
// destroy user_data. Retaining callers must copy pprof_bytes before returning.
DD_WIN_PROF_API bool DD_WIN_PROF_CALL
SetupProfilingDataCallback(ProfilingDataCallback callback, void* user_data);

// Set stable RUM session context. Safe to call from any thread.
// application_id: write-once per process. First non-empty value is stored
// as a profile-level tag; subsequent calls with a different value return false.
// session_id: updated on every call. Session transitions are tracked with
// timestamps.
// Pass NULL to end the current session: completes any pending session
// and view records, then returns true.
// Returns false if application_id or session_id is missing/empty,
// or a different application_id is rejected.
DD_WIN_PROF_API bool SetRumSession(const RumSessionContext* pContext);

// Set volatile RUM view context with explicit view_id. Safe to call from any thread.
// Pass NULL/empty view_id to clear the current view (signals "between views").
// Returns false if no session is active.
DD_WIN_PROF_API bool SetRumView(const RumViewValues* pContext);

#ifdef __cplusplus
}
#endif
