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

    std::string GetCpuVendor();
    std::string GetCpuModel();
    bool GetGpuFromRegistry(
        int device,
        std::string& driverDesc,
        std::string& driverVersion,
        std::string& driverDate,
        std::string& gpuName,
        std::string& gpuChip,
        uint64_t& gpuRam);
    bool GetCpuCores(int& physicalCores, int& logicalCores);
    bool GetMemoryInfo(uint64_t& totalPhys, uint64_t& availPhys, uint32_t& memoryLoad);
} // namespace OsSpecificApi
