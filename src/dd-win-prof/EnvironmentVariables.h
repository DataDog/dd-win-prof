// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "pch.h"

class EnvironmentVariables final
{
public:
    constexpr static const char* ProfilerEnabled = "DD_PROFILING_ENABLED";
    constexpr static const char* ProfilerAutoStart = "DD_PROFILING_AUTO_START";
    constexpr static const char* CpuProfilingEnabled = "DD_PROFILING_CPU_ENABLED";
    constexpr static const char* WallTimeProfilingEnabled = "DD_PROFILING_WALLTIME_ENABLED";
    constexpr static const char* ExportEnabled = "DD_INTERNAL_PROFILING_EXPORT_ENABLED";
    constexpr static const char* CpuWallTimeSamplingPeriod = "DD_INTERNAL_PROFILING_SAMPLING_RATE";
    constexpr static const char* WalltimeThreadsThreshold = "DD_INTERNAL_PROFILING_WALLTIME_THREADS_THRESHOLD";
    constexpr static const char* CpuTimeThreadsThreshold = "DD_INTERNAL_PROFILING_CPUTIME_THREADS_THRESHOLD";

    constexpr static const char* Version = "DD_VERSION";
    constexpr static const char* ServiceName = "DD_SERVICE";
    constexpr static const char* Environment = "DD_ENV";
    constexpr static const char* Site = "DD_SITE";
    constexpr static const char* UploadInterval = "DD_PROFILING_UPLOAD_PERIOD";
    constexpr static const char* AgentUrl = "DD_TRACE_AGENT_UR";
    constexpr static const char* AgentHost = "DD_AGENT_HOST";
    constexpr static const char* AgentPort = "DD_TRACE_AGENT_PORT";
    constexpr static const char* NamedPipeName = "DD_TRACE_PIPE_NAME";
    constexpr static const char* ApiKey = "DD_API_KEY";
    constexpr static const char* Hostname = "DD_HOSTNAME";
    constexpr static const char* Agentless = "DD_PROFILING_AGENTLESS";
    constexpr static const char* Tags = "DD_TAGS";

    constexpr static const char* ProfilesOutputDir = "DD_INTERNAL_PROFILING_OUTPUT_DIR";
    constexpr static const char* DevelopmentConfiguration = "DD_INTERNAL_USE_DEVELOPMENT_CONFIGURATION";
    constexpr static const char* DebugLogEnabled = "DD_TRACE_DEBUG";
    constexpr static const char* LogDirectory = "DD_TRACE_LOG_DIRECTORY";
    constexpr static const char* LogLevel = "DD_PROFILING_LOG_LEVEL";
    constexpr static const char* LogToConsole = "DD_PROFILING_LOG_TO_CONSOLE";
    constexpr static const char* CoreMinimumOverride = "DD_PROFILING_MIN_CORES_THRESHOLD";
    constexpr static const char* SymbolizeCallstacks = "DD_PROFILING_INTERNAL_SYMBOLIZE_CALLSTACKS";
};
