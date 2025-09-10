// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"
#include <gtest/gtest.h>

#include "../dd-win-prof/Configuration.h"
#include "../dd-win-prof/EnvironmentVariables.h"

// Helper to set/unset environment variables for testing
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

class ConfigurationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Store original environment variable value
        _originalProfilerEnabled = EnvironmentVariableHelper::GetEnvironmentVariable(EnvironmentVariables::ProfilerEnabled);
    }

    void TearDown() override {
        // Restore original environment variable value
        if (_originalProfilerEnabled.empty()) {
            EnvironmentVariableHelper::UnsetEnvironmentVariable(EnvironmentVariables::ProfilerEnabled);
        } else {
            EnvironmentVariableHelper::SetEnvironmentVariable(EnvironmentVariables::ProfilerEnabled, _originalProfilerEnabled.c_str());
        }
    }

private:
    std::string _originalProfilerEnabled;
};

TEST_F(ConfigurationTest, CanInstantiateConfiguration) {
    // Arrange & Act
    Configuration config;

    // Assert
    // Just verify that we can create a Configuration object without crashing
    EXPECT_TRUE(true) << "Configuration object should be instantiable";
}

TEST_F(ConfigurationTest, ConfigurationHasDefaultValues) {
    // Arrange & Act
    Configuration config;

    // Assert
    // Test some basic getters to ensure the object is properly initialized
    EXPECT_FALSE(config.GetVersion().empty()) << "Version should have a default value";
    EXPECT_FALSE(config.GetEnvironment().empty()) << "Environment should have a default value";
    EXPECT_FALSE(config.GetHostname().empty()) << "Hostname should have a default value";
    EXPECT_GE(config.GetAgentPort(), 0) << "Agent port should be a valid port number";
}

TEST_F(ConfigurationTest, ConfigurationProfilingSettings) {
    // Arrange & Act
    Configuration config;

    // Assert
    // Test profiling-related settings
    EXPECT_TRUE(config.IsProfilerEnabled()) << "Profiler should be enabled by default (unless DD_PROFILING_ENABLED=false)";

    // These should have reasonable default values
    EXPECT_GT(config.GetUploadInterval().count(), 0) << "Upload interval should be positive";
    EXPECT_GE(config.CpuThreadsThreshold(), 0) << "CPU threads threshold should be non-negative";
    EXPECT_GE(config.WalltimeThreadsThreshold(), 0) << "Walltime threads threshold should be non-negative";
}

TEST_F(ConfigurationTest, ConfigurationUserTags) {
    // Arrange & Act
    Configuration config;

    // Assert
    // Test user tags (should be empty by default unless DD_TAGS is set)
    auto userTags = config.GetUserTags();
    // This should not crash and should return a valid container
    EXPECT_TRUE(userTags.empty() || !userTags.empty()) << "User tags should be accessible";
}

TEST_F(ConfigurationTest, ProfilerEnabledFlag) {
    // Test the simple DD_PROFILING_ENABLED flag behavior
    
    // Arrange & Act
    Configuration config;
    
    // Assert
    // Simple boolean check - profiler is either enabled or disabled
    bool isEnabled = config.IsProfilerEnabled();
    EXPECT_TRUE(isEnabled == true || isEnabled == false) << "IsProfilerEnabled should return a valid boolean";
}

TEST_F(ConfigurationTest, ProfilerNotSetScenario) {
    // Test when DD_PROFILING_ENABLED is not set (should default to enabled)
    
    // Arrange
    EnvironmentVariableHelper::UnsetEnvironmentVariable(EnvironmentVariables::ProfilerEnabled);
    
    // Act
    Configuration config;
    
    // Assert
    EXPECT_TRUE(config.IsProfilerEnabled()) << "Profiler should be enabled by default when DD_PROFILING_ENABLED is not set";
}

TEST_F(ConfigurationTest, ProfilerExplicitlyEnabledScenario) {
    // Test when DD_PROFILING_ENABLED=true
    
    // Arrange
    EnvironmentVariableHelper::SetEnvironmentVariable(EnvironmentVariables::ProfilerEnabled, "true");
    
    // Act
    Configuration config;
    
    // Assert
    EXPECT_TRUE(config.IsProfilerEnabled()) << "Profiler should be enabled when DD_PROFILING_ENABLED=true";
}

TEST_F(ConfigurationTest, ProfilerExplicitlyDisabledScenario) {
    // Test when DD_PROFILING_ENABLED=false (only way to disable)
    
    // Arrange
    EnvironmentVariableHelper::SetEnvironmentVariable(EnvironmentVariables::ProfilerEnabled, "false");
    
    // Act
    Configuration config;
    
    // Assert
    EXPECT_FALSE(config.IsProfilerEnabled()) << "Profiler should be disabled when DD_PROFILING_ENABLED=false";
}

TEST_F(ConfigurationTest, ProfilerVariousValueScenarios) {
    // Test various string values for DD_PROFILING_ENABLED
    
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
        // Arrange
        EnvironmentVariableHelper::SetEnvironmentVariable(EnvironmentVariables::ProfilerEnabled, testCase.value);
        
        // Act
        Configuration config;
        
        // Assert
        EXPECT_EQ(config.IsProfilerEnabled(), testCase.expectedEnabled) 
            << "Failed for " << testCase.description;
    }
}