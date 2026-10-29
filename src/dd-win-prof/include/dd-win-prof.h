// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <cstdint>

#ifdef DD_WIN_PROF_STATIC_LINK
#define DD_WIN_PROF_API
#elif defined(DD_WIN_PROF_EXPORTS)
#define DD_WIN_PROF_API __declspec(dllexport)
#else
#define DD_WIN_PROF_API __declspec(dllimport)
#endif

struct ProfilerConfig {
  uint32_t size;  // size of this struct, for versioning

  // env vars usage
  bool noEnvVars;  // if true, ignore all environment variables and use only the values
                   // provided in this struct (default: false)

  // application information
  const char* serviceEnvironment;
  const char* serviceName;
  const char* serviceVersion;

  // Required for built-in HTTP upload when noEnvVars is true; otherwise they can
  // come from environment variables. Not required with an SDK-private callback.
  const char* url;
  const char* apiKey;

  // profiling tuning parameters
  uint64_t cpuWallTimeSamplingPeriodNs;  // sampling period in nanoseconds (default:
                                         // 20ms = 20,000,000ns)
  int32_t walltimeThreadsThreshold;      // number of threads to sample for wall time
                                         // (default: 5, min: 5, max: 64)
  int32_t cpuThreadsThreshold;    // number of threads to sample for CPU time (default:
                                  // 64, min: 5, max: 128)
  int32_t uploadIntervalSeconds;  // HTTP upload or SDK callback interval in seconds
                                  // (default: 60s or 20s in dev mode)
  const char* tags;  // additional tags to attach to profiles as comma separated list
                     // (default: empty)

  // symbolization
  bool symbolizeCallstacks;  // whether to symbolize stack traces (default: false)

  // debug / test settings (normally set via environment variables)
  // NOTE: log directory is NOT configurable here -- it is set at DLL load time
  //       via DD_TRACE_LOG_DIRECTORY (default: %PROGRAMDATA%\Datadog Tracer\logs)
  const char* pprofOutputDirectory;  // override for pprof debug output directory
                                     // (default: empty = disabled)
};

// C++ API, not a versioned ABI. Consumers need matching headers and toolchains.
// Setup/start/stop return false on failure; details are logged internally.
// Serialize lifecycle/registration
// calls and do not invoke them from profiling callbacks.
// Configure while stopped, including after stop. Preserves callback registration.
DD_WIN_PROF_API bool SetupProfiler(const ProfilerConfig& settings) noexcept;

// May be called again after stop. Returns false if already running or disabled.
DD_WIN_PROF_API bool StartProfiler() noexcept;

// Stops collection, waits for the current export and attempts a final export.
// Does not wait for the upload interval. The final SDK callback runs on this thread.
// Blocks until callbacks finish, not until SDK work they queued finishes. No overall
// timeout is imposed. Succeeds if already stopped.
// After return, no callbacks run until the next start; setup/registration may run
// again. Do not overlap lifecycle calls or call them from an SDK callback.
DD_WIN_PROF_API bool StopProfiler() noexcept;

// Enter a named view. Generates a unique view_id internally.
// Updates per-sample pprof labels: rum.view_id and trace endpoint (viewName).
// If a view is already active, it is completed first (record produced).
// Returns false if no session is active (a view requires an app+session).
DD_WIN_PROF_API bool EnterView(const char* viewName);

// Leave the current view (signals "between views").
// Completes the active view record. No-op if no view is active.
// Returns true if a view was active and is now cleared, false if no view was active.
DD_WIN_PROF_API bool LeaveCurrentView();

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
