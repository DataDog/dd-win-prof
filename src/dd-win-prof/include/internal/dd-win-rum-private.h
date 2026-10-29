// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "dd-win-prof.h"
#include "error.h"

struct ProfilingData {
  // Zero on success; otherwise a ProfilingErrorCode with no pprof bytes.
  uint32_t result = 0;
  // LZ4-compressed pprof.
  std::vector<uint8_t> pprof;
  // Wall-clock milliseconds since Unix epoch, not monotonic timestamps.
  int64_t startUnixTimeMs = 0;
  int64_t endUnixTimeMs = 0;
  // True for the final export from an explicit StopProfiler(), including errors.
  bool isLast = false;
};

// Periodic calls run on the exporter thread; final export runs on the stopping thread.
// Do not block or call lifecycle/registration APIs from this callback.
// Preparation errors can follow a delivered profile. Failed deliveries are not retried.
// Errors do not stop profiling.
using ProfilingDataCallback = std::function<void(ProfilingData)>;

typedef struct _RumSessionContext {
  const char* application_id;  // UUID string, set once per process lifetime
  const char* session_id;      // UUID string, changes on session rotation
} RumSessionContext;

typedef struct _RumViewValues {
  const char* view_id;    // UUID string; nullptr or "" to clear current view
  const char* view_name;  // human-readable name, e.g. "HomePage"
} RumViewValues;

// Register while stopped; serialize with lifecycle calls.
// Copies callback; an empty callable selects HTTP delivery for the next run.
// Registration survives setup and stop/start. Failure preserves the previous callback.
// Register before setup to omit HTTP credentials in noEnvVars mode.
// Callback mode discards RUM records; per-sample labels remain in pprof.
// Stop and unregister before unloading callback code or releasing captured state.
[[nodiscard]] DD_WIN_PROF_API bool SetupProfilingDataCallback(
    const ProfilingDataCallback& callback
) noexcept;

// Set stable RUM session context. Safe to call from any thread.
// application_id: write-once per process. First non-empty value is stored
// as a profile-level tag; subsequent calls with a different value return false.
// session_id: updated on every call. Session transitions are tracked with
// timestamps.
// Pass nullptr to end the current session: completes any pending session
// and view records, then returns true.
// Returns false if application_id or session_id is missing/empty,
// or a different application_id is rejected.
DD_WIN_PROF_API bool SetRumSession(const RumSessionContext* pContext);

// Set volatile RUM view context with explicit view_id. Safe to call from any thread.
// Pass nullptr/empty view_id to clear the current view (signals "between views").
// Returns false if no session is active.
DD_WIN_PROF_API bool SetRumView(const RumViewValues* pContext);
