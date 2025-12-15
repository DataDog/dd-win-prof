// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.
//
// Profiler version identifier used for tagging and exports
#pragma once

// keeping a w just to differentiate with ddprof versions (although runtime OS should be enough)
inline constexpr const char* kProfilerVersion = "v0.1.0w";
inline constexpr const char* kProfilerUserAgent = "dd-win-prof";
