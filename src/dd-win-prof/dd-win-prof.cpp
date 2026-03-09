// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"
#include "dd-win-prof.h"
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

    auto configuration = Profiler::GetConfiguration();
    if (pSettings->serviceEnvironment != nullptr)
    {
        configuration->SetEnvironmentName(pSettings->serviceEnvironment);
    }
    if (pSettings->serviceName != nullptr)
    {
        configuration->SetServiceName(pSettings->serviceName);
    }
    if (pSettings->serviceVersion != nullptr)
    {
        configuration->SetVersion(pSettings->serviceVersion);
    }
    if (pSettings->url != nullptr)
    {
        configuration->SetEndpoint(pSettings->url);
    }
    if (pSettings->apiKey != nullptr)
    {
        configuration->SetApiKey(pSettings->apiKey);
    }

    // TODO: enforce min/max values as for environment variables?
    if (pSettings->cpuWallTimeSamplingPeriodNs > 0)
    {
        configuration->SetCpuWallTimeSamplingPeriod(std::chrono::nanoseconds(pSettings->cpuWallTimeSamplingPeriodNs));
    }

    if (pSettings->walltimeThreadsThreshold > 0)
    {
        configuration->SetWalltimeThreadsThreshold(pSettings->walltimeThreadsThreshold);
    }

    if (pSettings->cpuThreadsThreshold > 0)
    {
        configuration->SetCpuThreadsThreshold(pSettings->cpuThreadsThreshold);
    }

    if (pSettings->symbolizeCallstacks)
    {
        configuration->EnableSymbolizedCallstacks();
    }

    return true;
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

DD_WIN_PROF_API void UpdateRumContext(const RumContextValues* context)
{
    auto profiler = Profiler::GetStartedInstance();
    if (!profiler) {
        Log::Warn("UpdateRumContext called before profiler started - ignoring");
        return;
    }

    auto exporter = profiler->GetProfileExporter();
    if (!exporter) {
        Log::Warn("UpdateRumContext called but ProfileExporter not available - ignoring");
        return;
    }

    // Pass C strings directly - no allocation needed
    exporter->UpdateRumContext(
        context->application_id,
        context->session_id,
        context->view_id,
        context->view_name,
        context->action_id
    );
}

}