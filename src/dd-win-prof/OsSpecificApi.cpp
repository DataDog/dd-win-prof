// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"

#include <winternl.h>
#include <Windows.h>

namespace OsSpecificApi {

    const uint64_t msInSecond = 1000;
    const uint64_t msInMinute = 60 * 1000;
    const uint64_t msInHour = 60 * 60 * 1000;
    const uint64_t msInDay = 24 * 60 * 60 * 1000;

    uint64_t GetTotalMilliseconds(SYSTEMTIME time)
    {
        uint64_t total = time.wMilliseconds;
        if (time.wSecond != 0)
        {
            total += (uint64_t)time.wSecond * msInSecond;
        }
        if (time.wMinute != 0)
        {
            total += (uint64_t)time.wMinute * msInMinute;
        }
        if (time.wHour != 0)
        {
            total += (uint64_t)time.wHour * msInHour;
        }
        if (time.wDay != 0)
        {
            total += (uint64_t)(time.wDay - 1) * msInDay; // january 1st 1601
        }

        // don't deal with month duration...

        return total;
    }

    uint64_t GetTotalMilliseconds(FILETIME fileTime)
    {
        SYSTEMTIME systemTime;
        ::FileTimeToSystemTime(&fileTime, &systemTime);
        return GetTotalMilliseconds(systemTime);
    }

    std::chrono::milliseconds GetThreadCpuTime(HANDLE hThread)
    {
        FILETIME creationTime, exitTime = {}; // not used here
        FILETIME kernelTime = {};
        FILETIME userTime = {};
        static bool isFirstError = true;

        if (::GetThreadTimes(hThread, &creationTime, &exitTime, &kernelTime, &userTime))
        {
            uint64_t milliseconds = GetTotalMilliseconds(userTime) + GetTotalMilliseconds(kernelTime);
            return std::chrono::milliseconds(milliseconds);
        }

        return 0ms;
    }

    typedef LONG KPRIORITY;

    struct CLIENT_ID
    {
        DWORD UniqueProcess; // Process ID
#ifdef BIT64
        ULONG pad1;
#endif
        DWORD UniqueThread; // Thread ID
#ifdef BIT64
        ULONG pad2;
#endif
    };

    typedef struct
    {
        FILETIME KernelTime;
        FILETIME UserTime;
        FILETIME CreateTime;
        ULONG WaitTime;
#ifdef BIT64
        ULONG pad1;
#endif
        PVOID StartAddress;
        CLIENT_ID Client_Id;
        KPRIORITY CurrentPriority;
        KPRIORITY BasePriority;
        ULONG ContextSwitchesPerSec;
        ULONG ThreadState;
        ULONG ThreadWaitReason;
        ULONG pad2;
    } SYSTEM_THREAD_INFORMATION;

    typedef enum
    {
        Initialized,
        Ready,
        Running,
        Standby,
        Terminated,
        Waiting,
        Transition,
        DeferredReady
    } THREAD_STATE;

    #define SYSTEMTHREADINFORMATION 40
    typedef NTSTATUS(WINAPI* NtQueryInformationThread_)(HANDLE, int, PVOID, ULONG, PULONG);
    NtQueryInformationThread_ NtQueryInformationThread = nullptr;

    typedef BOOL(WINAPI* GetLogicalProcessorInformation_)(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD);
    GetLogicalProcessorInformation_ GetLogicalProcessorInformation = nullptr;

    bool InitializeNtQueryInformationThreadCallback()
    {
        auto hModule = GetModuleHandleA("NtDll.dll");
        if (hModule == nullptr)
        {
            return false;
        }

        NtQueryInformationThread = (NtQueryInformationThread_)GetProcAddress(hModule, "NtQueryInformationThread");
        if (NtQueryInformationThread == nullptr)
        {
            return false;
        }

        return true;
    }

    bool IsRunning(ULONG threadState)
    {
        return (THREAD_STATE::Running == threadState) ||
            (THREAD_STATE::DeferredReady == threadState) ||
            (THREAD_STATE::Standby == threadState);

        // Note that THREAD_STATE::Standby, THREAD_STATE::Ready and THREAD_STATE::DeferredReady
        // indicate that threads are simply waiting for an available core to run.
        // If some callstacks show non cpu-bound frames at the top, return true only for Running state
    }

    //    isRunning,        cpu time          , failed
    std::tuple<bool, std::chrono::milliseconds, bool> IsRunning(HANDLE hThread)
    {
        if (NtQueryInformationThread == nullptr)
        {
            if (!InitializeNtQueryInformationThreadCallback())
            {
                return { false, 0ms, true };
            }
        }

        SYSTEM_THREAD_INFORMATION sti = { 0 };
        auto size = sizeof(SYSTEM_THREAD_INFORMATION);
        ULONG buflen = 0;
        static bool isFirstError = true;
        NTSTATUS lResult = NtQueryInformationThread(hThread, SYSTEMTHREADINFORMATION, &sti, static_cast<ULONG>(size), &buflen);
        // deal with an invalid thread handle case (thread might have died)
        if (lResult != 0)
        {
            // This always happens in 32 bit so uses another API to at least get the CPU consumption
            return { false, GetThreadCpuTime(hThread), true };
        }

        auto cpuTime = std::chrono::milliseconds(GetTotalMilliseconds(sti.UserTime) + GetTotalMilliseconds(sti.KernelTime));

        return { IsRunning(sti.ThreadState), cpuTime, false };
    }

    uint32_t GetProcessorCount()
    {
        // https://devblogs.microsoft.com/oldnewthing/20200824-00/?p=104116
        auto nbProcs = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (nbProcs == 0)
        {
            return 1;
        }

        return nbProcs;
    }

    std::string GetCpuVendor()
    {
        int cpuInfo[4];

        // the function id 0 returns model of CPU
        ::ZeroMemory(cpuInfo, sizeof(cpuInfo));
        __cpuid(cpuInfo, 0);

        char vendorName[13]; // 3 x 4 characters + trailing \0
        ::ZeroMemory(vendorName, sizeof(vendorName));
        memcpy(vendorName, &(cpuInfo[1]), 4);
        memcpy(vendorName + 4, &(cpuInfo[3]), 4);
        memcpy(vendorName + 8, &(cpuInfo[2]), 4);

        return vendorName;
    }

    std::string GetCpuModel()
    {
        int cpuInfo[4];

        // the function id 0x80000000 returns the "last" number of extended slots (as function id)
        ::ZeroMemory(cpuInfo, sizeof(cpuInfo));
        __cpuid(cpuInfo, 0x80000000);

        // the characters of the CPU name are stored in extended information at slots 2 up to 4
        unsigned int lastSlot = cpuInfo[0];

        char szModel[49]; // 3 x 4 x 4 characters + trailing \0
        ::ZeroMemory(szModel, sizeof(szModel));

        // get the information associated with each slot where the name can be found
        for (uint32_t i = 0x80000002; i <= max(lastSlot, 0x80000004); ++i)
        {
            // get the extended information up to the slot 4
            ::ZeroMemory(cpuInfo, sizeof(cpuInfo));
            __cpuid(cpuInfo, i);

            // each slot can contain up to 16 characters (hence the +16 offset to copy characters of each slot)
            if (i == 0x80000002)
            {
                memcpy(szModel, cpuInfo, sizeof(cpuInfo));
            }
            else if (i == 0x80000003)
            {
                // no more character to add
                if (cpuInfo[0] == 0)
                {
                    break;
                }

                memcpy(szModel + 16, cpuInfo, sizeof(cpuInfo));
            }
            else if (i == 0x80000004)
            {
                // no more character to add
                if (cpuInfo[0] == 0)
                {
                    break;
                }

                memcpy(szModel + 32, cpuInfo, sizeof(cpuInfo));
                break;
            }
        }

        return szModel;
    }

    std::string GetCpuArchitecture()
    {
        SYSTEM_INFO sysInfo;
        ::GetNativeSystemInfo(&sysInfo);

        switch (sysInfo.wProcessorArchitecture)
        {
            case PROCESSOR_ARCHITECTURE_AMD64:
                return "amd64";
            case PROCESSOR_ARCHITECTURE_ARM:
                return "arm";
            case PROCESSOR_ARCHITECTURE_ARM64:
                return "arm64";
            case PROCESSOR_ARCHITECTURE_IA64:
                return "ia64";
            case PROCESSOR_ARCHITECTURE_INTEL:
                return "x86";
            default:
                return "unknown";
        }
    }

    const char* KEY_GPU_PREFIX = "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\000";

    // could have more than one GPU so need to iterate on 0000, 0001 and so forth until no more key
    bool GetGpuFromRegistry(
        int device,
        std::string& driverDesc,
        std::string& driverVersion,
        std::string& driverDate,
        std::string& gpuName,
        std::string& gpuChip,
        uint64_t& gpuRam)
    {
        HKEY hKey;
        std::string keyName = KEY_GPU_PREFIX + std::to_string(device);
        //char keyName[256];
        //::ZeroMemory(keyName, sizeof(keyName));
        //strcpy_s(keyName, sizeof(keyName), KEY_GPU_PREFIX);
        //int keyLen = strlen(keyName);
        //keyName[keyLen] = static_cast<char>(48 + device);
        //keyName[keyLen+1] = '\0';

        if (::RegOpenKeyExA(
            HKEY_LOCAL_MACHINE,
            keyName.c_str(),
            0, KEY_READ, &hKey) == ERROR_SUCCESS)
        {
            char szValue[256];
            DWORD size = sizeof(szValue);
            if (::RegQueryValueExA(hKey, "DriverDesc", nullptr, nullptr, (LPBYTE)szValue, &size) == ERROR_SUCCESS)
            {
                driverDesc = szValue;
            }

            size = sizeof(szValue);
            if (::RegQueryValueExA(hKey, "DriverVersion", nullptr, nullptr, (LPBYTE)szValue, &size) == ERROR_SUCCESS)
            {
                driverVersion = szValue;
            }

            size = sizeof(szValue);
            if (::RegQueryValueExA(hKey, "DriverDate", nullptr, nullptr, (LPBYTE)szValue, &size) == ERROR_SUCCESS)
            {
                driverDate = szValue;
            }

            size = sizeof(szValue);
            if (::RegQueryValueExA(hKey, "HardwareInformation.AdapterString", nullptr, nullptr, (LPBYTE)szValue, &size) == ERROR_SUCCESS)
            {
                gpuName = szValue;
            }

            size = sizeof(szValue);
            if (::RegQueryValueExA(hKey, "HardwareInformation.ChipType", nullptr, nullptr, (LPBYTE)szValue, &size) == ERROR_SUCCESS)
            {
                gpuChip = szValue;
            }

            gpuRam = 0;
            size = sizeof(gpuRam);

            // I don't know what HardwareInformation.MemorySize contains...
            if (::RegQueryValueExA(hKey, "HardwareInformation.qwMemorySize", nullptr, nullptr, (LPBYTE)&gpuRam, &size) == ERROR_SUCCESS)
            {
                // RAM on the display card
            }

            ::RegCloseKey(hKey);
            return true;
        }

        ::RegCloseKey(hKey);
        return false;
    }

    bool GetCpuCores(int& physicalCores, int& logicalCores)
    {
        SYSTEM_INFO sysInfo;
        ::ZeroMemory(&sysInfo, sizeof(sysInfo));
        ::GetSystemInfo(&sysInfo);
        logicalCores = sysInfo.dwNumberOfProcessors;

        // get the count of cores information
        DWORD len = 0;
        ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
        std::vector<uint8_t> buffer(len);

        // iterate on all cores
        if (::GetLogicalProcessorInformationEx(RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &len))
        {
            physicalCores = 0;
            auto ptr = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
            size_t offset = 0;
            while (offset < len)
            {
                if (ptr->Relationship == RelationProcessorCore)
                    physicalCores++;
                offset += ptr->Size;
                ptr = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
            }

            return true;
        }

        return false;
    }

    bool GetMemoryInfo(uint64_t& totalPhys, uint64_t& availPhys, uint32_t& memoryLoad)
    {
        MEMORYSTATUSEX statex;
        statex.dwLength = sizeof(statex);
        if (::GlobalMemoryStatusEx(&statex))
        {
            totalPhys = statex.ullTotalPhys;
            availPhys = statex.ullAvailPhys;
            memoryLoad = statex.dwMemoryLoad;
            return true;
        }

        totalPhys = 0;
        availPhys = 0;
        memoryLoad = 0;
        return false;
    }


} // namespace OsSpecificApi