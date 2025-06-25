// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "pch.h"

// Those functions must be defined in the main projects (Linux and Windows)
// Here are forward declarations to avoid hard coupling
namespace OsSpecificApi
{
    std::chrono::milliseconds GetThreadCpuTime(HANDLE hThread);

    //    isRunning,        cpu time          , failed
    std::tuple<bool, std::chrono::milliseconds, bool> IsRunning(HANDLE hThread);

    uint32_t GetProcessorCount();
} // namespace OsSpecificApi
