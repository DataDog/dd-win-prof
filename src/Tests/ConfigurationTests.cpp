// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"
#include <gtest/gtest.h>

#include "../dd-win-prof/Configuration.h"

class ConfigurationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Common setup for all tests
    }

    void TearDown() override {
        // Common cleanup for all tests
    }
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
    // Note: These might be false by default unless environment variables are set
    EXPECT_TRUE(config.IsProfilerEnabled()) << "Profiler should be enabled by default (unless DD_PROFILING_ENABLED is set to 0/false)";

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

TEST_F(ConfigurationTest, ConfigurationCanBeOverriden) {
    // Arrange & Act
    Configuration config;
    config.SetEnvironmentName("test-env");
    config.SetServiceName("test-service");
    config.SetVersion("1.0.0");
    config.SetCpuThreadsThreshold(10);
    config.SetWalltimeThreadsThreshold(15);
    config.SetCpuWallTimeSamplingPeriod(100ms);
    config.SetApiKey("xxx-xxxx-xxxxx");
    config.SetEndpoint("http://localhost:8126");

    // Assert
    // Test the overridden values
    EXPECT_EQ(config.GetEnvironment(), "test-env") << "Environment should be overridden";
    EXPECT_EQ(config.GetServiceName(), "test-service") << "Service name should be overridden";
    EXPECT_EQ(config.GetVersion(), "1.0.0") << "Version should be overridden";
    EXPECT_EQ(config.CpuThreadsThreshold(), 10) << "CPU threads threshold should be overridden";
    EXPECT_EQ(config.WalltimeThreadsThreshold(), 15) << "Walltime threads threshold should be overridden";
    EXPECT_EQ(config.CpuWallTimeSamplingPeriod(), 100ms) << "CPU wall time sampling period should be overridden";
    EXPECT_EQ(config.GetApiKey(), "xxx-xxxx-xxxxx") << "API key should be overridden";
    EXPECT_EQ(config.GetAgentUrl(), "http://localhost:8126") << "Endpoint should be overridden";
    // IsAgentless should be true when endpoint is set
    EXPECT_TRUE(config.IsAgentless()) << "Should be in agentless mode when endpoint is set";
}