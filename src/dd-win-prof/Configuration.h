// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "pch.h"

#include "TagsHelper.h"

class Configuration
{
public:
    Configuration();
    ~Configuration() = default;

    fs::path const& GetLogDirectory() const;
    fs::path const& GetProfilesOutputDirectory() const;
    std::chrono::seconds GetUploadInterval() const;
    tags const& GetUserTags() const;
    bool IsDebugLogEnabled() const;

    std::string const& GetVersion() const;
    std::string const& GetEnvironment() const;
    std::string const& GetHostname() const;
    std::string const& GetAgentUrl() const;
    std::string const& GetAgentHost() const;
    int32_t GetAgentPort() const;
    bool IsAgentless() const;
    std::string const& GetSite() const;
    std::string const& GetApiKey() const;
    std::string const& GetServiceName() const;
    const std::string& GetNamedPipeName() const;

    bool IsProfilerEnabled() const;
    bool IsCpuProfilingEnabled() const;
    bool IsWallTimeProfilingEnabled() const;
    bool IsExportEnabled() const;
    double MinimumCores() const;

    // Manual configuration methods (primarily for testing)
    void SetExportEnabled(bool enabled);

    std::chrono::nanoseconds CpuWallTimeSamplingRate() const;
    int32_t WalltimeThreadsThreshold() const;
    int32_t CpuThreadsThreshold() const;

    template <typename T>
    static T GetEnvironmentValue(char const* name, T const& defaultValue);

private:
    static tags ExtractUserTags();
    static std::string GetDefaultSite();
    static std::string ExtractSite();
    static std::chrono::seconds ExtractUploadInterval();
    static fs::path GetDefaultLogDirectoryPath();
    static fs::path GetApmBaseDirectory();
    static fs::path ExtractLogDirectory();
    static fs::path ExtractPprofDirectory();
    static std::chrono::seconds GetDefaultUploadInterval();
    static bool GetDefaultDebugLogEnabled();
    static std::chrono::nanoseconds ExtractCpuWallTimeSamplingRate();
    static int32_t ExtractWallTimeThreadsThreshold();
    static int32_t ExtractCpuThreadsThreshold();

private:
    // default values
    static std::string const DefaultProdSite;
    static std::string const DefaultDevSite;
    static std::string const DefaultVersion;
    static std::string const DefaultEnvironment;
    static std::string const DefaultAgentHost;
    static std::string const DefaultEmptyString;
    static int32_t const DefaultAgentPort;
    static std::chrono::seconds const DefaultDevUploadInterval;
    static std::chrono::seconds const DefaultProdUploadInterval;
    static std::chrono::milliseconds const DefaultCpuProfilingInterval;

private:
    bool _isProfilerEnabled;
    bool _isCpuProfilingEnabled;
    bool _isWallTimeProfilingEnabled;
    bool _isExportEnabled;
    bool _debugLogEnabled;
    fs::path _logDirectory;
    fs::path _pprofDirectory;
    std::string _version;
    std::string _serviceName;
    std::string _environmentName;
    std::chrono::seconds _uploadPeriod;
    std::string _agentUrl;
    std::string _agentHost;
    std::int32_t _agentPort;
    std::string _apiKey;
    std::string _hostname;
    std::string _site;
    std::string _namedPipeName;
    tags _userTags;
    bool _isAgentLess;
    std::chrono::nanoseconds _cpuWallTimeSamplingRate;
    int32_t _walltimeThreadsThreshold;
    int32_t _cpuThreadsThreshold;

    double _minimumCores;

    static const uint64_t DefaultSamplingPeriod = 18;
    static const uint64_t MinimumSamplingPeriod = 5;
};

