// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <ddc/profiling.h>

namespace dd_win_prof {
// Capture and serialization share libdd-c's optimized stack-depth limit.
inline constexpr size_t kMaxStackDepth{DDC_PROFILE_MAX_STACK_DEPTH};

// name of the registered Windows message used to detect a UI hang
inline constexpr const wchar_t* kHangProbeMessageName = L"DD_HANG_PROBE_MSG";

// TODO: add corresponding configuration settings
// UI hang watchdog frequency
// Note: usually workstation Windows scheduling quanta ~15.6ms (120ms for server
// Windows)
constexpr int kWatchdogTickMs = 32;

// duration threshold to trigger a hang sample
constexpr std::chrono::nanoseconds kHangThresholdMs = 96ms;

// Other shared constants can be added here as needed
}  // namespace dd_win_prof