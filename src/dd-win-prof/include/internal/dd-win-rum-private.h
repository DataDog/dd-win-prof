// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "dd-win-prof.h"

// SDK-private C++ callback API. Use only when building dd-win-prof from source as
// a dependency, with compatible compiler, standard library and CRT settings.
// No API or ABI compatibility is guaranteed across versions or independent builds.
struct ProfilingData {
  // Owned LZ4-compressed pprof bytes, safe to move to an SDK upload queue.
  std::vector<uint8_t> pprof;
  // Wall-clock milliseconds since Unix epoch, not monotonic timestamps.
  int64_t startUnixTimeMs = 0;
  int64_t endUnixTimeMs = 0;
  // True for the final profile from an explicit StopProfiler().
  bool isLast = false;
};

struct ProfilingError {
  ProfilingSeverity severity;
  ProfilingErrorCode code;
  std::string message;
  // Private adapters can avoid reporting a final-export error twice.
  bool reportedToCallback = false;
};

using ProfilingDataResult = std::variant<ProfilingData, ProfilingError>;
// Receives an owned result. Periodic invocations run on the exporter thread;
// the final invocation runs inside StopProfiler() on its calling thread.
// Do not perform blocking I/O, wait for upload completion, or call profiler
// setup/start/stop/registration APIs here. Callback execution delays export and stop.
// Encoding and next-profile preparation failures have Error severity and do not
// stop profiling. A preparation error can follow a successfully delivered profile.
// StopProfiler() still completes if the final export fails. Setup/start failures
// are reported synchronously, not through this callback.
// Callback exceptions become Fatal diagnostics; failed results are not retried.
// Active-run Fatal diagnostics do not yet trigger automatic shutdown.
using ProfilingDataCallback = std::function<void(ProfilingDataResult)>;

typedef struct _RumSessionContext {
  const char* application_id;  // UUID string, set once per process lifetime
  const char* session_id;      // UUID string, changes on session rotation
} RumSessionContext;

typedef struct _RumViewValues {
  const char* view_id;    // UUID string; nullptr or "" to clear current view
  const char* view_name;  // human-readable name, e.g. "HomePage"
} RumViewValues;

// SDK-private. Register or replace while stopped, including after StopProfiler()
// returns. Do not overlap this call with another registration or setup/start/stop.
// Register before SetupProfiler() to omit HTTP credentials in noEnvVars mode.
// Callback mode discards RUM view/session records; per-sample labels remain in pprof.
// Copies callback; an empty callable unregisters and selects HTTP upload for the next
// run. Registration persists across StopProfiler() and SetupProfiler(). Stop, then
// unregister before unloading callback code. Captured references must remain valid
// through the last callback.
// Returns no error on success. Failures are returned synchronously, never through
// the callback, and leave registration unchanged. Copy exceptions are Fatal;
// the profiler is already stopped when copying is attempted.
[[nodiscard]] DD_WIN_PROF_API std::optional<ProfilingError> SetupProfilingDataCallback(
    const ProfilingDataCallback& callback
);

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
