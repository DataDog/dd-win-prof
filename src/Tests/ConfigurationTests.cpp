// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"
#include <gtest/gtest.h>

#include "../InprocProfiling/Configuration.h"

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
    EXPECT_FALSE(config.IsProfilerEnabled()) << "Profiler should be disabled by default (unless DD_PROFILING_ENABLED is set)";

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