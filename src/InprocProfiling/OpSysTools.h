// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "pch.h"

class OpSysTools final
{
public:
    static std::chrono::nanoseconds GetHighPrecisionTimestamp();
    static bool SetNativeThreadName(const WCHAR* description);
    static bool GetNativeThreadName(HANDLE threadHandle, std::string& name);
    static std::string GetHostname();
    static std::string GetProcessName();

private:
    typedef HRESULT(__stdcall* SetThreadDescriptionDelegate_t)(HANDLE threadHandle, PCWSTR pThreadDescription);
    typedef HRESULT(__stdcall* GetThreadDescriptionDelegate_t)(HANDLE hThread, PWSTR* ppThreadDescription);

    static void InitWindowsDelegates();
    static bool s_areWindowsDelegateSet;

    static SetThreadDescriptionDelegate_t s_setThreadDescriptionDelegate;
    static GetThreadDescriptionDelegate_t s_getThreadDescriptionDelegate;
    static SetThreadDescriptionDelegate_t GetDelegate_SetThreadDescription();
    static GetThreadDescriptionDelegate_t GetDelegate_GetThreadDescription();
};

inline std::chrono::nanoseconds OpSysTools::GetHighPrecisionTimestamp()
{
    auto now = std::chrono::system_clock::now();

    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch());
}
