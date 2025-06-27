// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

namespace dd_win_prof {
    // Maximum depth for a single stack
    inline constexpr size_t kMaxStackDepth{512};

    // Other shared constants can be added here as needed
    // inline constexpr size_t kMaxCacheSize{10000};
    // inline constexpr uint32_t kCacheCleanupThreshold{5};
}