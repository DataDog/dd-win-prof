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
