// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"
#include "dd-win-prof-internal.h"
#include "Log.h"
#include "Profiler.h"

extern "C" {

DD_WIN_PROF_API bool SetupProfiler(ProfilerConfig* pSettings)
{
    if (pSettings == nullptr)
    {
        Log::Warn("Null profiler configuration structure.");
        return false;
    }

    if (pSettings->size != sizeof(ProfilerConfig))
    {
        Log::Warn("Invalid profiler configuration structure.");
        return false;
    }

    auto profiler = Profiler::GetInstance();
    if (profiler == nullptr)
    {
        Log::Warn("Profiler instance is not created: missing Process Attach event in DllMain.");
        return false;
    }

    if (profiler->IsStarted())
    {
        Log::Warn("SetupProfiler() must be called before StartProfiler().");
        return false;
    }

    return InitializeConfiguration(Profiler::GetConfiguration(), pSettings);
}

DD_WIN_PROF_API bool StartProfiler()
{
    auto profiler = Profiler::GetInstance();
    if (profiler == nullptr)
    {
        Log::Warn("Profiler instance is not created: missing Process Attach event in DllMain.");
        return false;
    }

    if (profiler->IsStarted())
    {
        Log::Warn("Profiler is already running.");
        return false;
    }

    return profiler->StartProfiling();
}

DD_WIN_PROF_API void StopProfiler()
{
    auto profiler = Profiler::GetInstance();
    if (profiler == nullptr)
    {
        Log::Warn("Profiler instance is not created: missing Process Attach event in DllMain.");
        return;
    }

    if (!profiler->IsStarted())
    {
        Log::Warn("Profiler is not running.");
        return;
    }

    profiler->StopProfiling();
}

DD_WIN_PROF_API bool SetRumApplicationId(const char* applicationId)
{
    auto profiler = Profiler::GetInstance();
    if (profiler == nullptr) return false;
    return profiler->SetRumApplicationId(applicationId);
}

DD_WIN_PROF_API bool SetRumSession(const char* sessionId)
{
    auto profiler = Profiler::GetInstance();
    if (profiler == nullptr) return false;
    return profiler->SetRumSession(sessionId);
}

DD_WIN_PROF_API bool SetRumView(const char* viewId, const char* viewName)
{
    auto profiler = Profiler::GetInstance();
    if (profiler == nullptr) return false;
    return profiler->SetRumView(viewId, viewName);
}

DD_WIN_PROF_API bool UpdateRumContext(const RumContextValues* pContext)
{
    auto profiler = Profiler::GetInstance();
    if (profiler == nullptr || pContext == nullptr) return false;

    if (!profiler->SetRumApplicationId(pContext->application_id)) return false;
    profiler->SetRumSession(pContext->session_id);
    profiler->SetRumView(pContext->view_id, pContext->view_name);
    return true;
}

}