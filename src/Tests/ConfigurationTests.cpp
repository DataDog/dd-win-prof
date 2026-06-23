// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <gtest/gtest.h>

#include "../dd-win-prof/Configuration.h"
#include "../dd-win-prof/EnvironmentVariables.h"
#include "../dd-win-prof/OpSysTools.h"
#include "../dd-win-prof/TagsHelper.h"
#include "../dd-win-prof/dd-win-prof-internal.h"
#include "pch.h"

// ---------------------------------------------------------------------------
// Helper to set/unset environment variables for testing
// ---------------------------------------------------------------------------
class EnvironmentVariableHelper {
 public:
  static void SetEnvironmentVariable(const char* name, const char* value) {
    ::SetEnvironmentVariableA(name, value);
  }

  static void UnsetEnvironmentVariable(const char* name) {
    ::SetEnvironmentVariableA(name, nullptr);
  }

  static std::string GetEnvironmentVariable(const char* name) {
    char buffer[1024] = {0};
    DWORD result = ::GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    return result > 0 ? std::string(buffer) : std::string();
  }
};

// ---------------------------------------------------------------------------
// Helper to build a zero-initialized ProfilerConfig with size pre-set
// ---------------------------------------------------------------------------
static ProfilerConfig MakeProfilerConfig() {
  ProfilerConfig cfg = {};
  cfg.size = sizeof(ProfilerConfig);
  return cfg;
}

// ---------------------------------------------------------------------------
// Fixture that saves / restores multiple env vars touched by tests
// ---------------------------------------------------------------------------
class ConfigurationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    SaveEnvVar(EnvironmentVariables::ProfilerEnabled);
    SaveEnvVar(EnvironmentVariables::Version);
    SaveEnvVar(EnvironmentVariables::Environment);
    SaveEnvVar(EnvironmentVariables::ServiceName);
    SaveEnvVar(EnvironmentVariables::AgentHost);
    SaveEnvVar(EnvironmentVariables::ApiKey);
  }

  void TearDown() override {
    for (auto const& [name, original] : _savedEnvVars) {
      if (original.empty()) {
        EnvironmentVariableHelper::UnsetEnvironmentVariable(name.c_str());
      } else {
        EnvironmentVariableHelper::SetEnvironmentVariable(
            name.c_str(), original.c_str()
        );
      }
    }
  }

  void SetTestEnvVar(const char* name, const char* value) {
    EnvironmentVariableHelper::SetEnvironmentVariable(name, value);
  }

  void UnsetTestEnvVar(const char* name) {
    EnvironmentVariableHelper::UnsetEnvironmentVariable(name);
  }

 private:
  void SaveEnvVar(const char* name) {
    _savedEnvVars.emplace_back(
        name, EnvironmentVariableHelper::GetEnvironmentVariable(name)
    );
  }

  std::vector<std::pair<std::string, std::string>> _savedEnvVars;
};

// ===========================================================================
// Existing tests (updated to use the new fixture)
// ===========================================================================

TEST_F(ConfigurationTest, CanInstantiateConfiguration) {
  Configuration config;
  EXPECT_TRUE(true) << "Configuration object should be instantiable";
}

TEST_F(ConfigurationTest, ConfigurationHasDefaultValues) {
  Configuration config;
  EXPECT_FALSE(config.GetVersion().empty()) << "Version should have a default value";
  EXPECT_FALSE(config.GetEnvironment().empty())
      << "Environment should have a default value";
  EXPECT_FALSE(config.GetHostname().empty()) << "Hostname should have a default value";
  EXPECT_GE(config.GetAgentPort(), 0) << "Agent port should be a valid port number";
}

TEST_F(ConfigurationTest, ConfigurationProfilingSettings) {
  Configuration config;
  EXPECT_TRUE(config.IsProfilerEnabled())
      << "Profiler should be enabled by default (unless DD_PROFILING_ENABLED=false)";
  EXPECT_GT(config.GetUploadInterval().count(), 0)
      << "Upload interval should be positive";
  EXPECT_GE(config.CpuThreadsThreshold(), 0)
      << "CPU threads threshold should be non-negative";
  EXPECT_GE(config.WalltimeThreadsThreshold(), 0)
      << "Walltime threads threshold should be non-negative";
}

TEST_F(ConfigurationTest, ConfigurationUserTags) {
  Configuration config;
  auto userTags = config.GetUserTags();
  EXPECT_TRUE(userTags.empty() || !userTags.empty())
      << "User tags should be accessible";
}

TEST_F(ConfigurationTest, ProfilerEnabledFlag) {
  Configuration config;
  bool isEnabled = config.IsProfilerEnabled();
  EXPECT_TRUE(isEnabled == true || isEnabled == false)
      << "IsProfilerEnabled should return a valid boolean";
}

TEST_F(ConfigurationTest, ProfilerNotSetScenario) {
  UnsetTestEnvVar(EnvironmentVariables::ProfilerEnabled);
  Configuration config;
  EXPECT_TRUE(config.IsProfilerEnabled())
      << "Profiler should be enabled by default when DD_PROFILING_ENABLED is not set";
}

TEST_F(ConfigurationTest, ProfilerExplicitlyEnabledScenario) {
  SetTestEnvVar(EnvironmentVariables::ProfilerEnabled, "true");
  Configuration config;
  EXPECT_TRUE(config.IsProfilerEnabled())
      << "Profiler should be enabled when DD_PROFILING_ENABLED=true";
}

TEST_F(ConfigurationTest, ProfilerExplicitlyDisabledScenario) {
  SetTestEnvVar(EnvironmentVariables::ProfilerEnabled, "false");
  Configuration config;
  EXPECT_FALSE(config.IsProfilerEnabled())
      << "Profiler should be disabled when DD_PROFILING_ENABLED=false";
}

TEST_F(ConfigurationTest, ProfilerVariousValueScenarios) {
  struct TestCase {
    const char* value;
    bool expectedEnabled;
    const char* description;
  };

  TestCase testCases[] = {
      {"1", true, "DD_PROFILING_ENABLED=1"},
      {"0", false, "DD_PROFILING_ENABLED=0"},
      {"TRUE", true, "DD_PROFILING_ENABLED=TRUE"},
      {"FALSE", false, "DD_PROFILING_ENABLED=FALSE"},
      {"yes", true, "DD_PROFILING_ENABLED=yes"},
      {"no", false, "DD_PROFILING_ENABLED=no"},
      {"true", true, "DD_PROFILING_ENABLED=true"},
      {"false", false, "DD_PROFILING_ENABLED=false"},
      {"invalid", false, "DD_PROFILING_ENABLED=invalid (should default to false)"}
  };

  for (const auto& testCase : testCases) {
    SetTestEnvVar(EnvironmentVariables::ProfilerEnabled, testCase.value);
    Configuration config;
    EXPECT_EQ(config.IsProfilerEnabled(), testCase.expectedEnabled)
        << "Failed for " << testCase.description;
  }
}

TEST_F(ConfigurationTest, ConfigurationCanBeOverriden) {
  Configuration config;
  config.SetEnvironmentName("test-env");
  config.SetServiceName("test-service");
  config.SetVersion("1.0.0");
  config.SetCpuThreadsThreshold(10);
  config.SetWalltimeThreadsThreshold(15);
  config.SetCpuWallTimeSamplingPeriod(100ms);
  config.SetApiKey("xxx-xxxx-xxxxx");
  config.SetEndpoint("http://localhost:8126");

  EXPECT_EQ(config.GetEnvironment(), "test-env");
  EXPECT_EQ(config.GetServiceName(), "test-service");
  EXPECT_EQ(config.GetVersion(), "1.0.0");
  EXPECT_EQ(config.CpuThreadsThreshold(), 10);
  EXPECT_EQ(config.WalltimeThreadsThreshold(), 15);
  EXPECT_EQ(config.CpuWallTimeSamplingPeriod(), 100ms);
  EXPECT_EQ(config.GetApiKey(), "xxx-xxxx-xxxxx");
  EXPECT_EQ(config.GetSite(), "http://localhost:8126");
  EXPECT_TRUE(config.IsAgentless())
      << "Should be in agentless mode when endpoint is set";
}

// ===========================================================================
// ResetToDefaults tests
// ===========================================================================

TEST_F(ConfigurationTest, ResetToDefaults_RestoresAllFieldsToDefaults) {
  Configuration config;
  config.ResetToDefaults();

  EXPECT_TRUE(config.IsProfilerEnabled());
  EXPECT_FALSE(config.IsProfilerAutoStartEnabled());
  EXPECT_TRUE(config.IsCpuProfilingEnabled());
  EXPECT_TRUE(config.IsWallTimeProfilingEnabled());
  EXPECT_TRUE(config.IsExportEnabled());
  EXPECT_FALSE(config.AreCallstacksSymbolized());
  EXPECT_FALSE(config.IsDebugLogEnabled());
  EXPECT_TRUE(config.GetProfilesOutputDirectory().empty());

  EXPECT_EQ(config.GetVersion(), "Unspecified-Version");
  EXPECT_EQ(config.GetEnvironment(), "Unspecified-Environment");
  EXPECT_EQ(config.GetServiceName(), OpSysTools::GetProcessName());
  EXPECT_EQ(config.GetHostname(), OpSysTools::GetHostname());

  EXPECT_EQ(config.GetUploadInterval(), std::chrono::seconds(60));
  EXPECT_TRUE(config.GetUserTags().empty());
  EXPECT_EQ(config.CpuWallTimeSamplingPeriod(), std::chrono::nanoseconds(20'000'000));
  EXPECT_EQ(config.WalltimeThreadsThreshold(), 5);
  EXPECT_EQ(config.CpuThreadsThreshold(), 64);

  EXPECT_TRUE(config.GetApiKey().empty());
  EXPECT_FALSE(config.IsAgentless());
  EXPECT_TRUE(config.GetAgentUrl().empty());
  EXPECT_EQ(config.GetAgentHost(), "localhost");
  EXPECT_EQ(config.GetAgentPort(), 8126);
  EXPECT_EQ(config.GetSite(), "datadoghq.com");
  EXPECT_TRUE(config.GetNamedPipeName().empty());

  EXPECT_FALSE(config.GetLogDirectory().empty())
      << "Log directory should have a default path";
}

TEST_F(ConfigurationTest, ResetToDefaults_IgnoresEnvironmentVariables) {
  SetTestEnvVar(EnvironmentVariables::Version, "from-env");
  SetTestEnvVar(EnvironmentVariables::ServiceName, "from-env");
  SetTestEnvVar(EnvironmentVariables::Environment, "from-env");
  SetTestEnvVar(EnvironmentVariables::AgentHost, "envhost");
  SetTestEnvVar(EnvironmentVariables::ApiKey, "envkey");

  Configuration config;
  EXPECT_EQ(config.GetVersion(), "from-env");
  EXPECT_EQ(config.GetServiceName(), "from-env");
  EXPECT_EQ(config.GetEnvironment(), "from-env");
  EXPECT_EQ(config.GetAgentHost(), "envhost");
  EXPECT_EQ(config.GetApiKey(), "envkey");

  config.ResetToDefaults();

  EXPECT_EQ(config.GetVersion(), "Unspecified-Version");
  EXPECT_EQ(config.GetServiceName(), OpSysTools::GetProcessName());
  EXPECT_EQ(config.GetEnvironment(), "Unspecified-Environment");
  EXPECT_EQ(config.GetAgentHost(), "localhost");
  EXPECT_TRUE(config.GetApiKey().empty());
}

// ===========================================================================
// New setter tests
// ===========================================================================

TEST_F(ConfigurationTest, SetUploadInterval_Works) {
  Configuration config;
  config.SetUploadInterval(std::chrono::seconds(30));
  EXPECT_EQ(config.GetUploadInterval(), std::chrono::seconds(30));
}

TEST_F(ConfigurationTest, SetUserTags_Works) {
  Configuration config;
  config.SetUserTags(TagsHelper::Parse("key1:val1,key2:val2"));

  auto const& t = config.GetUserTags();
  ASSERT_EQ(t.size(), 2u);
  EXPECT_EQ(t[0].first, "key1");
  EXPECT_EQ(t[0].second, "val1");
  EXPECT_EQ(t[1].first, "key2");
  EXPECT_EQ(t[1].second, "val2");
}

TEST_F(ConfigurationTest, SetProfilesOutputDirectory_Works) {
  Configuration config;
  config.SetProfilesOutputDirectory(fs::path("C:\\temp\\pprof"));
  EXPECT_EQ(config.GetProfilesOutputDirectory(), fs::path("C:\\temp\\pprof"));
}

// ===========================================================================
// ProfilerConfig struct defaults
// ===========================================================================

TEST_F(ConfigurationTest, ProfilerConfig_ZeroInitialized_NoEnvVarsIsFalse) {
  auto cfg = MakeProfilerConfig();
  EXPECT_FALSE(
      cfg.noEnvVars
  ) << "noEnvVars must default to false for backward compatibility";
}

TEST_F(ConfigurationTest, ProfilerConfig_ZeroInitialized_AllPointersAreNull) {
  auto cfg = MakeProfilerConfig();
  EXPECT_EQ(cfg.serviceEnvironment, nullptr);
  EXPECT_EQ(cfg.serviceName, nullptr);
  EXPECT_EQ(cfg.serviceVersion, nullptr);
  EXPECT_EQ(cfg.url, nullptr);
  EXPECT_EQ(cfg.apiKey, nullptr);
  EXPECT_EQ(cfg.tags, nullptr);
  EXPECT_EQ(cfg.pprofOutputDirectory, nullptr);
}

TEST_F(ConfigurationTest, ProfilerConfig_ZeroInitialized_NumericFieldsAreZero) {
  auto cfg = MakeProfilerConfig();
  EXPECT_EQ(cfg.cpuWallTimeSamplingPeriodNs, 0u);
  EXPECT_EQ(cfg.walltimeThreadsThreshold, 0);
  EXPECT_EQ(cfg.cpuThreadsThreshold, 0);
  EXPECT_EQ(cfg.uploadIntervalSeconds, 0);
  EXPECT_FALSE(cfg.symbolizeCallstacks);
}

// ===========================================================================
// InitializeConfiguration -- noEnvVars=false keeps env vars (zero-init default)
// ===========================================================================

TEST_F(ConfigurationTest, InitConfig_ZeroInitDefault_DoesNotResetEnvVars) {
  SetTestEnvVar(EnvironmentVariables::Version, "env-version");

  Configuration config;
  EXPECT_EQ(config.GetVersion(), "env-version");

  auto cfg = MakeProfilerConfig();
  // noEnvVars is false by zero-init -- env vars must survive
  ASSERT_TRUE(InitializeConfiguration(&config, &cfg));
  EXPECT_EQ(config.GetVersion(), "env-version")
      << "Zero-initialized ProfilerConfig should not reset env vars";
}

// ===========================================================================
// InitializeConfiguration -- mandatory field enforcement (noEnvVars=true)
// ===========================================================================

TEST_F(ConfigurationTest, InitConfig_NoEnvVars_MissingUrl_ReturnsFalse) {
  Configuration config;
  auto cfg = MakeProfilerConfig();
  cfg.noEnvVars = true;
  cfg.apiKey = "some-key";
  cfg.url = nullptr;

  EXPECT_FALSE(InitializeConfiguration(&config, &cfg));
}

TEST_F(ConfigurationTest, InitConfig_NoEnvVars_MissingApiKey_ReturnsFalse) {
  Configuration config;
  auto cfg = MakeProfilerConfig();
  cfg.noEnvVars = true;
  cfg.url = "http://intake.datadoghq.com";
  cfg.apiKey = nullptr;

  EXPECT_FALSE(InitializeConfiguration(&config, &cfg));
}

TEST_F(ConfigurationTest, InitConfig_NoEnvVars_MandatoryFieldsSet_ReturnsTrue) {
  Configuration config;
  auto cfg = MakeProfilerConfig();
  cfg.noEnvVars = true;
  cfg.url = "http://intake.datadoghq.com";
  cfg.apiKey = "my-api-key";

  EXPECT_TRUE(InitializeConfiguration(&config, &cfg));
  EXPECT_EQ(config.GetSite(), "http://intake.datadoghq.com");
  EXPECT_EQ(config.GetApiKey(), "my-api-key");
  EXPECT_TRUE(config.IsAgentless());
}

// ===========================================================================
// InitializeConfiguration -- all fields applied (noEnvVars=true)
// ===========================================================================

TEST_F(ConfigurationTest, InitConfig_NoEnvVars_AllFieldsApplied) {
  Configuration config;
  auto cfg = MakeProfilerConfig();
  cfg.noEnvVars = true;
  cfg.url = "https://intake.datadoghq.com";
  cfg.apiKey = "dd-api-key-123";
  cfg.serviceEnvironment = "staging";
  cfg.serviceName = "my-service";
  cfg.serviceVersion = "2.5.0";
  cfg.cpuWallTimeSamplingPeriodNs = 10'000'000;
  cfg.walltimeThreadsThreshold = 10;
  cfg.cpuThreadsThreshold = 32;
  cfg.uploadIntervalSeconds = 45;
  cfg.tags = "team:backend,region:us-east";
  cfg.symbolizeCallstacks = true;
  cfg.pprofOutputDirectory = "C:\\temp\\dd-pprof";

  ASSERT_TRUE(InitializeConfiguration(&config, &cfg));

  EXPECT_EQ(config.GetSite(), "https://intake.datadoghq.com");
  EXPECT_EQ(config.GetApiKey(), "dd-api-key-123");
  EXPECT_TRUE(config.IsAgentless());
  EXPECT_EQ(config.GetEnvironment(), "staging");
  EXPECT_EQ(config.GetServiceName(), "my-service");
  EXPECT_EQ(config.GetVersion(), "2.5.0");
  EXPECT_EQ(config.CpuWallTimeSamplingPeriod(), std::chrono::nanoseconds(10'000'000));
  EXPECT_EQ(config.WalltimeThreadsThreshold(), 10);
  EXPECT_EQ(config.CpuThreadsThreshold(), 32);
  EXPECT_EQ(config.GetUploadInterval(), std::chrono::seconds(45));
  EXPECT_TRUE(config.AreCallstacksSymbolized());
  EXPECT_EQ(config.GetProfilesOutputDirectory(), fs::path("C:\\temp\\dd-pprof"));

  auto const& t = config.GetUserTags();
  ASSERT_EQ(t.size(), 2u);
  EXPECT_EQ(t[0].first, "team");
  EXPECT_EQ(t[0].second, "backend");
  EXPECT_EQ(t[1].first, "region");
  EXPECT_EQ(t[1].second, "us-east");
}

// ===========================================================================
// InitializeConfiguration -- partial fields, defaults preserved (noEnvVars=true)
// ===========================================================================

TEST_F(ConfigurationTest, InitConfig_NoEnvVars_PartialFields_DefaultsPreserved) {
  Configuration config;
  auto cfg = MakeProfilerConfig();
  cfg.noEnvVars = true;
  cfg.url = "https://intake.datadoghq.com";
  cfg.apiKey = "key";
  cfg.serviceName = "partial-svc";

  ASSERT_TRUE(InitializeConfiguration(&config, &cfg));

  EXPECT_EQ(config.GetServiceName(), "partial-svc");

  EXPECT_EQ(config.GetVersion(), "Unspecified-Version");
  EXPECT_EQ(config.GetEnvironment(), "Unspecified-Environment");
  EXPECT_EQ(config.CpuThreadsThreshold(), 64);
  EXPECT_EQ(config.WalltimeThreadsThreshold(), 5);
  EXPECT_EQ(config.GetUploadInterval(), std::chrono::seconds(60));
  EXPECT_EQ(config.CpuWallTimeSamplingPeriod(), std::chrono::nanoseconds(20'000'000));
  EXPECT_FALSE(config.AreCallstacksSymbolized());
  EXPECT_TRUE(config.GetUserTags().empty());
  EXPECT_TRUE(config.GetProfilesOutputDirectory().empty());
  EXPECT_FALSE(config.GetLogDirectory().empty());
  EXPECT_EQ(config.GetHostname(), OpSysTools::GetHostname());
}

// ===========================================================================
// InitializeConfiguration -- noEnvVars=false: env vars are preserved
// ===========================================================================

TEST_F(ConfigurationTest, InitConfig_NoEnvVars_False_EnvVarsPreserved) {
  SetTestEnvVar(EnvironmentVariables::Version, "env-ver");
  SetTestEnvVar(EnvironmentVariables::Environment, "env-env");

  Configuration config;
  EXPECT_EQ(config.GetVersion(), "env-ver");
  EXPECT_EQ(config.GetEnvironment(), "env-env");

  auto cfg = MakeProfilerConfig();
  cfg.noEnvVars = false;
  cfg.serviceName = "api-svc";

  ASSERT_TRUE(InitializeConfiguration(&config, &cfg));

  EXPECT_EQ(config.GetVersion(), "env-ver")
      << "Env var value should survive when noEnvVars=false";
  EXPECT_EQ(config.GetEnvironment(), "env-env")
      << "Env var value should survive when noEnvVars=false";
  EXPECT_EQ(config.GetServiceName(), "api-svc") << "API override should apply";
}
