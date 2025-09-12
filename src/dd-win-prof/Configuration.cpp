// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"
#include "Configuration.h"
#include "EnvironmentVariables.h"
#include "OpSysTools.h"

std::string const Configuration::DefaultDevSite = "datad0g.com";
std::string const Configuration::DefaultProdSite = "datadoghq.com";
std::string const Configuration::DefaultVersion = "Unspecified-Version";
std::string const Configuration::DefaultEnvironment = "Unspecified-Environment";
std::string const Configuration::DefaultAgentHost = "localhost";
int32_t const Configuration::DefaultAgentPort = 8126;
std::string const Configuration::DefaultEmptyString = "";
std::chrono::seconds const Configuration::DefaultDevUploadInterval = 20s;
std::chrono::seconds const Configuration::DefaultProdUploadInterval = 60s;


// these are used to override the environment variables via API
void Configuration::SetServiceName(const char* serviceName)
{
    _serviceName = serviceName;
}

void Configuration::SetEnvironmentName(const char* environmentName)
{
    _environmentName = environmentName;
}

void Configuration::SetVersion(const char* version)
{
    _version = version;
}

void Configuration::SetEndpoint(const char* url)
{
    _agentUrl = url;

    // force agentless mode when setting the full URL
    _isAgentLess = true;
}

void Configuration::SetApiKey(const char* apiKey)
{
    _apiKey = apiKey;
}


Configuration::Configuration()
{
    //wchar_t* environment = GetEnvironmentStrings();
    //wchar_t* var = environment;
    //while (*var != L'\0')
    //{
    //    std::wcout << "  " << var << std::endl;

    //    // Move to the next string (skip past the null terminator)
    //    var += wcslen(var) + 1;
    //}

    _debugLogEnabled = GetEnvironmentValue(EnvironmentVariables::DebugLogEnabled, GetDefaultDebugLogEnabled());
    _logDirectory = ExtractLogDirectory();
    _pprofDirectory = ExtractPprofDirectory();
    _isProfilerEnabled = GetEnvironmentValue(EnvironmentVariables::ProfilerEnabled, true); // enabled by default via StartProfiling()
    _isCpuProfilingEnabled = GetEnvironmentValue(EnvironmentVariables::CpuProfilingEnabled, true);
    _isWallTimeProfilingEnabled = GetEnvironmentValue(EnvironmentVariables::WallTimeProfilingEnabled, true);
    _isExportEnabled = GetEnvironmentValue(EnvironmentVariables::ExportEnabled, true);

    _uploadPeriod = ExtractUploadInterval();
    _userTags = ExtractUserTags();

    // TODO: update the IConfiguration interface to allow setting these values via API
    _version = GetEnvironmentValue(EnvironmentVariables::Version, DefaultVersion);
    _environmentName = GetEnvironmentValue(EnvironmentVariables::Environment, DefaultEnvironment);
    _serviceName = GetEnvironmentValue(EnvironmentVariables::ServiceName, OpSysTools::GetProcessName());
    _hostname = GetEnvironmentValue(EnvironmentVariables::Hostname, OpSysTools::GetHostname());
    _cpuWallTimeSamplingPeriod = ExtractCpuWallTimeSamplingRate();
    _walltimeThreadsThreshold = ExtractWallTimeThreadsThreshold();
    _cpuThreadsThreshold = ExtractCpuThreadsThreshold();
    _apiKey = GetEnvironmentValue(EnvironmentVariables::ApiKey, DefaultEmptyString);

    // TODO: see if the Agent mode should be supported and how it should be configured
    _isAgentLess = GetEnvironmentValue(EnvironmentVariables::Agentless, false);
    _agentUrl = GetEnvironmentValue(EnvironmentVariables::AgentUrl, DefaultEmptyString);
    _agentHost = GetEnvironmentValue(EnvironmentVariables::AgentHost, DefaultAgentHost);
    _agentPort = GetEnvironmentValue(EnvironmentVariables::AgentPort, DefaultAgentPort);
    _site = ExtractSite();
    _namedPipeName = GetEnvironmentValue(EnvironmentVariables::NamedPipeName, DefaultEmptyString);

    _minimumCores = GetEnvironmentValue<double>(EnvironmentVariables::CoreMinimumOverride, 1.0);
}



bool EnvironmentExist(const char* name)
{
    auto len = ::GetEnvironmentVariableA(name, nullptr, 0);
    if (len > 0)
    {
        return true;
    }

    return (::GetLastError() != ERROR_ENVVAR_NOT_FOUND);
}

std::string GetEnvironmentValue(const char* name)
{
    const size_t max_buf_size = 4096;
    char buffer[max_buf_size] = { 0 };
    auto len = ::GetEnvironmentVariableA(name, buffer, max_buf_size);

    return std::string(buffer, len);
}

bool TryParseBooleanEnvironmentValue(const char* valueToParse, bool& parsedValue)
{
    std::string valueToParseStr(valueToParse);

    if (valueToParseStr == "false"
        || valueToParseStr == "False"
        || valueToParseStr == "FALSE"
        || valueToParseStr == "no"
        || valueToParseStr == "No"
        || valueToParseStr == "NO"
        || valueToParseStr == "f"
        || valueToParseStr == "F"
        || valueToParseStr == "N"
        || valueToParseStr == "n"
        || valueToParseStr == "0")
    {
        parsedValue = false;
        return true;
    }

    if (valueToParseStr == "true"
        || valueToParseStr == "True"
        || valueToParseStr == "TRUE"
        || valueToParseStr == "yes"
        || valueToParseStr == "Yes"
        || valueToParseStr == "YES"
        || valueToParseStr == "t"
        || valueToParseStr == "T"
        || valueToParseStr == "Y"
        || valueToParseStr == "y"
        || valueToParseStr == "1")
    {
        parsedValue = true;
        return true;
    }

    return false;
}

static bool convert_to(char const* s, bool& result)
{
    return TryParseBooleanEnvironmentValue(s, result);
}

static bool convert_to(char const* s, std::string& result)
{
    result = std::string(s);
    return true;
}

static bool TryParse(char const* s, int32_t& result)
{
    if ((s == nullptr) || (strlen(s) == 0))
    {
        result = 0;
        return false;
    }

    try
    {
        result = std::stoi(std::string(s));
        return true;
    }
    catch (std::exception const&)
    {
    }

    result = 0;
    return false;
}

static bool TryParse(char const* s, uint64_t& result)
{
    if ((s == nullptr) || (strlen(s) == 0))
    {
        result = 0;
        return false;
    }

    try
    {
        result = std::stoull(std::string(s));
        return true;
    }
    catch (std::exception const&)
    {
    }

    result = 0;
    return false;
}

static bool convert_to(char const* s, int32_t& result)
{
    return TryParse(s, result);
}

static bool convert_to(char const* s, uint64_t& result)
{
    return TryParse(s, result);
}

static bool convert_to(char const* s, double& result)
{
    char* endPtr = nullptr;
    result = strtod(s, &endPtr);

    // non numeric input
    if (s == endPtr)
    {
        return false;
    }

    // Based on tests, numbers such as "0.1.2" are converted into 0.1 without error
    return (errno != ERANGE);
}

static bool convert_to(char const* s, std::chrono::milliseconds& result)
{
    std::uint64_t value;
    auto parsed = TryParse(s, value);
    if (parsed)
    {
        result = std::chrono::milliseconds(value);
    }
    return parsed;
}


fs::path Configuration::ExtractLogDirectory()
{
    auto value = ::GetEnvironmentValue(EnvironmentVariables::LogDirectory);
    if (value.empty())
        return GetDefaultLogDirectoryPath();

    return fs::path(value);
}

fs::path const& Configuration::GetLogDirectory() const
{
    return _logDirectory;
}

fs::path Configuration::ExtractPprofDirectory()
{
    auto value = ::GetEnvironmentValue(EnvironmentVariables::ProfilesOutputDir);
    if (value.empty())
        return fs::path();

    return fs::path(value);
}

fs::path const& Configuration::GetProfilesOutputDirectory() const
{
    return _pprofDirectory;
}

bool Configuration::GetDefaultDebugLogEnabled()
{
    auto r = ::GetEnvironmentValue(EnvironmentVariables::DevelopmentConfiguration);

    bool isDev;
    return TryParseBooleanEnvironmentValue(r.c_str(), isDev) && isDev;
}

bool Configuration::IsProfilerEnabled() const
{
    return _isProfilerEnabled;
}

bool Configuration::IsCpuProfilingEnabled() const
{
    return _isCpuProfilingEnabled;
}

bool Configuration::IsWallTimeProfilingEnabled() const
{
    return _isWallTimeProfilingEnabled;
}

bool Configuration::IsExportEnabled() const
{
    return _isExportEnabled;
}

void Configuration::SetExportEnabled(bool enabled)
{
    _isExportEnabled = enabled;
}

std::chrono::nanoseconds Configuration::CpuWallTimeSamplingPeriod() const
{
    return _cpuWallTimeSamplingPeriod;
}

int32_t Configuration::WalltimeThreadsThreshold() const
{
    return _walltimeThreadsThreshold;
}

int32_t Configuration::CpuThreadsThreshold() const
{
    return _cpuThreadsThreshold;
}

int32_t Configuration::ExtractCpuThreadsThreshold()
{
    // default threads to sample for CPU profiling is 64; could be changed via env vars from 5 to 128
    int32_t threshold = GetEnvironmentValue(EnvironmentVariables::CpuTimeThreadsThreshold, 64);
    if (threshold < 5)
    {
        threshold = 5;
    }
    else if (threshold > 128)
    {
        threshold = 128;
    }

    return threshold;
}

double Configuration::MinimumCores() const
{
    return _minimumCores;
}

std::chrono::seconds Configuration::GetUploadInterval() const
{
    return _uploadPeriod;
}

tags const& Configuration::GetUserTags() const
{
    return _userTags;
}

bool Configuration::IsDebugLogEnabled() const
{
    return _debugLogEnabled;
}

std::string const& Configuration::GetVersion() const
{
    return _version;
}

std::string const& Configuration::GetEnvironment() const
{
    return _environmentName;
}

std::string const& Configuration::GetHostname() const
{
    return _hostname;
}

std::string const& Configuration::GetAgentUrl() const
{
    return _agentUrl;
}

std::string const& Configuration::GetAgentHost() const
{
    return _agentHost;
}

int32_t Configuration::GetAgentPort() const
{
    return _agentPort;
}

bool Configuration::IsAgentless() const
{
    return _isAgentLess;
}

std::string const& Configuration::GetSite() const
{
    return _site;
}

std::string const& Configuration::GetApiKey() const
{
    return _apiKey;
}

std::string const& Configuration::GetServiceName() const
{
    return _serviceName;
}

fs::path Configuration::GetApmBaseDirectory()
{
    char output[MAX_PATH] = { 0 };
    auto result = ExpandEnvironmentStringsA("%PROGRAMDATA%", output, MAX_PATH);
    if (result != 0)
    {
        return fs::path(output);
    }

    return fs::path();
}

fs::path Configuration::GetDefaultLogDirectoryPath()
{
    auto baseDirectory = fs::path(GetApmBaseDirectory());
    return baseDirectory / R"(Datadog Tracer\logs)";
}

tags Configuration::ExtractUserTags()
{
    return TagsHelper::Parse(GetEnvironmentValue(EnvironmentVariables::Tags, DefaultEmptyString));
}

std::string Configuration::GetDefaultSite()
{
    auto isDev = GetEnvironmentValue(EnvironmentVariables::DevelopmentConfiguration, false);

    if (isDev)
    {
        return DefaultDevSite;
    }

    return DefaultProdSite;
}

std::string Configuration::ExtractSite()
{
    auto r = GetEnvironmentValue(EnvironmentVariables::Site, DefaultEmptyString);

    if (r.empty())
        return GetDefaultSite();

    return r;
}

const std::string& Configuration::GetNamedPipeName() const
{
    return _namedPipeName;
}


std::chrono::seconds Configuration::GetDefaultUploadInterval()
{
    auto r = GetEnvironmentValue(EnvironmentVariables::DevelopmentConfiguration, DefaultEmptyString);

    bool isDev;
    if (TryParseBooleanEnvironmentValue(r.c_str(), isDev) && isDev)
        return DefaultDevUploadInterval;
    return DefaultProdUploadInterval;
}

std::chrono::seconds Configuration::ExtractUploadInterval()
{
    auto r = GetEnvironmentValue(EnvironmentVariables::UploadInterval, DefaultEmptyString);
    int32_t interval;
    if (TryParse(r.c_str(), interval))
    {
        return std::chrono::seconds(interval);
    }

    return GetDefaultUploadInterval();
}

std::chrono::nanoseconds Configuration::ExtractCpuWallTimeSamplingRate()
{
    // default sampling rate is 18 ms; could be changed via env vars but down to a minimum of 5 ms
    uint64_t rate = GetEnvironmentValue(EnvironmentVariables::CpuWallTimeSamplingPeriod, DefaultSamplingPeriod);
    if (rate < MinimumSamplingPeriod)
    {
        rate = MinimumSamplingPeriod;
    }
    rate *= 1000000;
    return std::chrono::nanoseconds(rate);
}

int32_t Configuration::ExtractWallTimeThreadsThreshold()
{
    // default threads to sample for wall time is 5; could be changed via env vars from 5 to 64
    int32_t threshold = GetEnvironmentValue(EnvironmentVariables::WalltimeThreadsThreshold, 5);
    if (threshold < 5)
    {
        threshold = 5;
    }
    else if (threshold > 64)
    {
        threshold = 64;
    }
    return threshold;
}


template <typename T>
T Configuration::GetEnvironmentValue(char const* name, T const& defaultValue)
{
    if (!EnvironmentExist(name))
    {
        return defaultValue;
    }

    T result{};
    auto r = ::GetEnvironmentValue(name);
    if (!convert_to(r.c_str(), result))
    {
        return defaultValue;
    }

    return result;
}
