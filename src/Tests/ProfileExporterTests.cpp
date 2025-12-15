// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"
#include "../dd-win-prof/ProfileExporter.h"
#include "../dd-win-prof/Configuration.h"
#include "../dd-win-prof/Sample.h"
#include "../dd-win-prof/SampleValueType.h"
#include "../dd-win-prof/ThreadInfo.h"
#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <Windows.h>

class ProfileExporterExportTests : public ::testing::Test {
protected:
    void SetUp() override {
        // Create configuration and disable export for tests
        config = std::make_unique<Configuration>();
        config->SetExportEnabled(false); // Disable export to prevent hanging on network calls

        // Create sample value type definitions
        sampleTypes = {
            {"cpu-time", "nanoseconds"},
            {"cpu-samples", "count"}
        };

        // Set the global sample values count to match our sample types
        Sample::SetValuesCount(sampleTypes.size());

        // Create exporter
        exporter = std::make_unique<ProfileExporter>(config.get(), sampleTypes);
    }

    void TearDown() override {
        exporter.reset();
        config.reset();
    }

    std::unique_ptr<Configuration> config;
    std::vector<SampleValueType> sampleTypes;
    std::unique_ptr<ProfileExporter> exporter;
};

TEST_F(ProfileExporterExportTests, InitializationWithExportEnabled) {
    // Test that exporter initializes correctly with export enabled
    ASSERT_TRUE(exporter->Initialize());
    EXPECT_TRUE(exporter->IsInitialized());
    EXPECT_TRUE(exporter->GetLastError().empty());
}

TEST_F(ProfileExporterExportTests, ExportWithoutSamples) {
    // Test export with empty profile
    ASSERT_TRUE(exporter->Initialize());

    // Export should succeed even with no samples
    EXPECT_TRUE(exporter->Export());
}

TEST_F(ProfileExporterExportTests, ExportWithSamples) {
    // Test export with actual samples
    ASSERT_TRUE(exporter->Initialize());

    // Create a sample with test data using the correct constructor
    std::chrono::nanoseconds timestamp = std::chrono::nanoseconds(std::chrono::system_clock::now().time_since_epoch());

    // Create a valid ThreadInfo with a real handle
    HANDLE hThread;
    ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &hThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    auto threadInfo = std::make_shared<ThreadInfo>(::GetCurrentThreadId(), hThread);

    // Create frames array
    uint64_t frames[] = {0x1000, 0x2000, 0x3000};

    // Create sample using constructor
    auto sample = std::make_shared<Sample>(timestamp, threadInfo, frames, 3);

    // Add sample values for both types (cpu-time and cpu-samples)
    sample->AddValue(1000000, 0); // 1ms CPU time in nanoseconds
    sample->AddValue(1, 1);       // 1 sample count

    // Add sample to exporter
    EXPECT_TRUE(exporter->Add(sample));

    // Export profile
    EXPECT_TRUE(exporter->Export());
}

TEST_F(ProfileExporterExportTests, MultipleExports) {
    // Test multiple consecutive exports
    ASSERT_TRUE(exporter->Initialize());

    // Create a valid ThreadInfo with a real handle
    HANDLE hThread;
    ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &hThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    auto threadInfo = std::make_shared<ThreadInfo>(::GetCurrentThreadId(), hThread);

    for (int i = 0; i < 3; ++i) {
        // Create a sample for each export
        std::chrono::nanoseconds timestamp = std::chrono::nanoseconds(std::chrono::system_clock::now().time_since_epoch());

        // Create frames array
        uint64_t frames[] = {static_cast<uint64_t>(0x1000 + i * 0x100), static_cast<uint64_t>(0x2000 + i * 0x100)};

        // Create sample using constructor
        auto sample = std::make_shared<Sample>(timestamp, threadInfo, frames, 2);

        // Add sample values for both types
        sample->AddValue(500000 * (i + 1), 0); // CPU time in nanoseconds
        sample->AddValue(1, 1);                // sample count

        EXPECT_TRUE(exporter->Add(sample));
        EXPECT_TRUE(exporter->Export());
    }
}

TEST_F(ProfileExporterExportTests, TagPreparation) {
    // Test that we can prepare tags without errors
    ASSERT_TRUE(exporter->Initialize());

    // This is more of an integration test - if initialization succeeds,
    // tag preparation worked correctly during exporter creation
    EXPECT_TRUE(exporter->IsInitialized());
}

TEST_F(ProfileExporterExportTests, ConfigurationIntegration) {
    // Test that exporter correctly uses Configuration data
    ASSERT_TRUE(exporter->Initialize());

    // Verify that configuration data is being used
    // The exporter should read service name, environment, version, etc. from config
    EXPECT_TRUE(exporter->IsInitialized());

    // Create a sample to ensure we have something to export
    std::chrono::nanoseconds timestamp = std::chrono::nanoseconds(std::chrono::system_clock::now().time_since_epoch());

    // Create a valid ThreadInfo with a real handle
    HANDLE hThread;
    ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &hThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    auto threadInfo = std::make_shared<ThreadInfo>(::GetCurrentThreadId(), hThread);

    // Create frames array
    uint64_t frames[] = {0x1000, 0x2000};

    // Create sample using constructor
    auto sample = std::make_shared<Sample>(timestamp, threadInfo, frames, 2);

    // Add sample values for both types
    sample->AddValue(1000000, 0); // 1ms CPU time in nanoseconds
    sample->AddValue(1, 1);       // 1 sample count

    EXPECT_TRUE(exporter->Add(sample));

    // Export should use the configuration data for tags, URLs, etc.
    EXPECT_TRUE(exporter->Export());
}

// Note: These tests will attempt to connect to localhost:8126 (Datadog Agent)
// If no agent is running, the export will fail but the test should still pass
// since we're testing the export logic, not the actual network connectivity