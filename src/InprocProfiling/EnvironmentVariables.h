// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "pch.h"

class EnvironmentVariables final
{
public:
    inline static const char* ProfilerEnabled = "DD_PROFILING_ENABLED";
    inline static const char* CpuProfilingEnabled = "DD_PROFILING_CPU_ENABLED";
    inline static const char* WallTimeProfilingEnabled = "DD_PROFILING_WALLTIME_ENABLED";
    inline static const char* ExportEnabled = "DD_INTERNAL_PROFILING_EXPORT_ENABLED";
    inline static const char* CpuWallTimeSamplingRate = "DD_INTERNAL_PROFILING_SAMPLING_RATE";
    inline static const char* WalltimeThreadsThreshold = "DD_INTERNAL_PROFILING_WALLTIME_THREADS_THRESHOLD";
    inline static const char* CpuTimeThreadsThreshold = "DD_INTERNAL_PROFILING_CPUTIME_THREADS_THRESHOLD";

    inline static const char* Version = "DD_VERSION";
    inline static const char* ServiceName = "DD_SERVICE";
    inline static const char* Environment = "DD_ENV";
    inline static const char* Site = "DD_SITE";
    inline static const char* UploadInterval = "DD_PROFILING_UPLOAD_PERIOD";
    inline static const char* AgentUrl = "DD_TRACE_AGENT_UR";
    inline static const char* AgentHost = "DD_AGENT_HOST";
    inline static const char* AgentPort = "DD_TRACE_AGENT_PORT";
    inline static const char* NamedPipeName = "DD_TRACE_PIPE_NAME";
    inline static const char* ApiKey = "DD_API_KEY";
    inline static const char* Hostname = "DD_HOSTNAME";
    inline static const char* Agentless = "DD_PROFILING_AGENTLESS";
    inline static const char* Tags = "DD_TAGS";

    inline static const char* ProfilesOutputDir = "DD_INTERNAL_PROFILING_OUTPUT_DIR";
    inline static const char* DevelopmentConfiguration = "DD_INTERNAL_USE_DEVELOPMENT_CONFIGURATION";
    inline static const char* DebugLogEnabled = "DD_TRACE_DEBUG";
    inline static const char* LogDirectory = "DD_TRACE_LOG_DIRECTORY";
    inline static const char* LogLevel = "DD_PROFILING_LOG_LEVEL";
    inline static const char* LogToConsole = "DD_PROFILING_LOG_TO_CONSOLE";
    inline static const char* CoreMinimumOverride = "DD_PROFILING_MIN_CORES_THRESHOLD";
};
