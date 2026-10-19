// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "dd-win-prof.h"

typedef struct _RumSessionContext {
  const char* application_id;  // UUID string, set once per process lifetime
  const char* session_id;      // UUID string, changes on session rotation
} RumSessionContext;

typedef struct _RumViewValues {
  const char* view_id;    // UUID string; NULL or "" to clear current view
  const char* view_name;  // human-readable name, e.g. "HomePage"
} RumViewValues;

#ifdef __cplusplus
extern "C" {
#endif

// Context strings are copied during each call. These setters are thread-safe;
// the caller must ensure they do not race with DestroyProfiler.
// application_id is write-once per process and retained as profile metadata.
// session_id changes record timestamped session transitions.
// NULL context ends the session and completes pending session/view records.
// Missing IDs or a different application_id return INVALID_ARGUMENT.
DD_WIN_PROF_API ProfilingStatus SetRumSession(
    Profiler* profiler, const RumSessionContext* context
);

// The RUM SDK supplies view IDs; NULL context or an empty ID clears the view.
// Returns INVALID_STATE if no session is active.
DD_WIN_PROF_API ProfilingStatus SetRumView(
    Profiler* profiler, const RumViewValues* context
);

#ifdef __cplusplus
}
#endif
