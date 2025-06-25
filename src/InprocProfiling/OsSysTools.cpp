#include "pch.h"

#include "OpSysTools.h"

#define MAX_CHAR 512

bool OpSysTools::s_areWindowsDelegateSet = false;
OpSysTools::SetThreadDescriptionDelegate_t OpSysTools::s_setThreadDescriptionDelegate = nullptr;
OpSysTools::GetThreadDescriptionDelegate_t OpSysTools::s_getThreadDescriptionDelegate = nullptr;

bool OpSysTools::SetNativeThreadName(const WCHAR* description)
{
    // The SetThreadDescription(..) API is only available on recent Windows versions and must be called dynamically.
    // We attempt to link to it at runtime, and if we do not succeed, this operation is a No-Op.
    // https://docs.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreaddescription#remarks

    SetThreadDescriptionDelegate_t setThreadDescriptionDelegate = GetDelegate_SetThreadDescription();
    if (nullptr == setThreadDescriptionDelegate)
    {
        return false;
    }

    HRESULT hr = setThreadDescriptionDelegate(GetCurrentThread(), description);
    return SUCCEEDED(hr);
}

bool OpSysTools::GetNativeThreadName(HANDLE handle, std::string& name)
{
    // The SetThreadDescription(..) API is only available on recent Windows versions and must be called dynamically.
    // We attempt to link to it at runtime, and if we do not succeed, this operation is a No-Op.
    // https://docs.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreaddescription#remarks

    GetThreadDescriptionDelegate_t getThreadDescriptionDelegate = GetDelegate_GetThreadDescription();
    if (nullptr == getThreadDescriptionDelegate)
    {
        return false;
    }

    wchar_t* pThreadDescr = nullptr;
    HRESULT hr = getThreadDescriptionDelegate(handle, &pThreadDescr);

    if (pThreadDescr == nullptr)
    {
        return false;
    }

    if (FAILED(hr))
    {
        ::LocalFree(pThreadDescr);
        return false;
    }

    if (wcslen(pThreadDescr) == 0)
    {
        ::LocalFree(pThreadDescr);
        return false;
    }

    int sizeNeeded = ::WideCharToMultiByte(CP_UTF8, 0, pThreadDescr, -1, NULL, 0, NULL, NULL);
    name = std::string(sizeNeeded, 0);
    ::WideCharToMultiByte(CP_UTF8, 0, pThreadDescr, -1, &name[0], sizeNeeded, NULL, NULL);

    // Remove null terminator
    if (!name.empty() && name.back() == '\0') {
        name.pop_back();
    }

    return true;
}


void OpSysTools::InitWindowsDelegates()
{
    if (s_areWindowsDelegateSet)
    {
        // s_isRunTimeLinkingThreadDescriptionDone is not volatile. benign init race.
        return;
    }

    // We do not bother unloading KernelBase.dll later. It is probably already loaded, and if not, we can keep it in mem.
    HMODULE moduleHandle = ::GetModuleHandle(L"KernelBase.dll");
    if (NULL == moduleHandle)
    {
        moduleHandle = ::LoadLibrary(L"KernelBase.dll");
    }

    if (NULL != moduleHandle)
    {
        s_setThreadDescriptionDelegate = reinterpret_cast<SetThreadDescriptionDelegate_t>(GetProcAddress(moduleHandle, "SetThreadDescription"));
        s_getThreadDescriptionDelegate = reinterpret_cast<GetThreadDescriptionDelegate_t>(GetProcAddress(moduleHandle, "GetThreadDescription"));
    }

    s_areWindowsDelegateSet = true;
}

OpSysTools::SetThreadDescriptionDelegate_t OpSysTools::GetDelegate_SetThreadDescription()
{
    SetThreadDescriptionDelegate_t setThreadDescriptionDelegate = s_setThreadDescriptionDelegate;
    if (nullptr == setThreadDescriptionDelegate)
    {
        InitWindowsDelegates();
        setThreadDescriptionDelegate = s_setThreadDescriptionDelegate;
    }

    return setThreadDescriptionDelegate;
}

OpSysTools::GetThreadDescriptionDelegate_t OpSysTools::GetDelegate_GetThreadDescription()
{
    GetThreadDescriptionDelegate_t getThreadDescriptionDelegate = s_getThreadDescriptionDelegate;
    if (nullptr == getThreadDescriptionDelegate)
    {
        InitWindowsDelegates();
        getThreadDescriptionDelegate = s_getThreadDescriptionDelegate;
    }

    return getThreadDescriptionDelegate;
}

std::string OpSysTools::GetHostname()
{
    char hostname[MAX_CHAR];
    DWORD length = MAX_CHAR;
    if (GetComputerNameA(hostname, &length) != 0)
    {
        return std::string(hostname);
    }

    return "Unknown-hostname";
}

std::string OpSysTools::GetProcessName()
{
    const DWORD length = 260;
    char pathName[length]{};

    const DWORD len = GetModuleFileNameA(nullptr, pathName, length);
    return fs::path(pathName).filename().string();
}