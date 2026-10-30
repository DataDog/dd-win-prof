// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "dd-win-prof.h"

namespace dd_win_prof {
// Serialize lifecycle/registration calls; do not call from profiling callbacks.
DD_WIN_PROF_API bool SetupProfiler(const ProfilerConfig& settings) noexcept;
DD_WIN_PROF_API bool StartProfiler() noexcept;

// Waits for callbacks, not queued SDK work. Cleanup completes even if delivery fails.
DD_WIN_PROF_API bool StopProfiler() noexcept;
}  // namespace dd_win_prof
