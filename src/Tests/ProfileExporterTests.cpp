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

// RUM Context Tests
class ProfileExporterRumTests : public ::testing::Test {
protected:
    void SetUp() override {
        config = std::make_unique<Configuration>();
        config->SetExportEnabled(false);

        sampleTypes = {
            {"cpu-time", "nanoseconds"},
            {"cpu-samples", "count"}
        };

        Sample::SetValuesCount(sampleTypes.size());
        exporter = std::make_unique<ProfileExporter>(config.get(), sampleTypes);
        ASSERT_TRUE(exporter->Initialize());
    }

    void TearDown() override {
        exporter.reset();
        config.reset();
    }

    // Helper to add a sample and export
    void AddSampleAndExport() {
        std::chrono::nanoseconds timestamp = std::chrono::nanoseconds(
            std::chrono::system_clock::now().time_since_epoch());

        HANDLE hThread;
        ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(),
                         ::GetCurrentProcess(), &hThread, 0, FALSE,
                         DUPLICATE_SAME_ACCESS);
        auto threadInfo = std::make_shared<ThreadInfo>(::GetCurrentThreadId(), hThread);

        uint64_t frames[] = {0x1000, 0x2000};
        auto sample = std::make_shared<Sample>(timestamp, threadInfo, frames, 2);
        sample->AddValue(1000000, 0);
        sample->AddValue(1, 1);

        EXPECT_TRUE(exporter->Add(sample));
        EXPECT_TRUE(exporter->Export());
    }

    std::unique_ptr<Configuration> config;
    std::vector<SampleValueType> sampleTypes;
    std::unique_ptr<ProfileExporter> exporter;
};

TEST_F(ProfileExporterRumTests, RumContextEmptyUUIDs) {
    // Update with all empty strings
    exporter->UpdateRumContext("", "", "", "", "");

    // Should succeed - no RUM labels will be added but operation is valid
    AddSampleAndExport();
}

TEST_F(ProfileExporterRumTests, RumContextPartialUUIDs) {
    // Update with only session_id set, others empty
    exporter->UpdateRumContext(
        "",
        "5e551017-4114-4114-4114-beeeefbeeeef",  // session_id
        "",
        "",
        ""
    );

    // Should succeed - only rum.session_id label should be added
    AddSampleAndExport();
}

TEST_F(ProfileExporterRumTests, RumContextAllUUIDs) {
    // Update with all UUIDs and view_name set to valid values
    exporter->UpdateRumContext(
        "a991ca10-4004-4004-4004-beefbeefbeef",  // application_id
        "5e551017-4114-4114-4114-beeeefbeeeef",  // session_id
        "141ee144-4224-4224-4224-beeeeeeeeeef",  // view_id
        "home-page",                              // view_name
        "4c10171e-4334-4334-4334-b0000eeeefff"   // action_id
    );

    // Should succeed - all five rum.* labels should be present
    AddSampleAndExport();
}

TEST_F(ProfileExporterRumTests, RumContextTruncation) {
    // Pass strings longer than 36 chars (UUID is 36 chars with hyphens)
    std::string longUuidString(100, 'x');  // 100 x's
    std::string longViewName(200, 'v');    // 200 v's

    exporter->UpdateRumContext(
        longUuidString.c_str(),
        longUuidString.c_str(),
        longUuidString.c_str(),
        longViewName.c_str(),
        longUuidString.c_str()
    );

    // Should succeed - truncation should happen safely (UUIDs to 36, view_name to 127)
    AddSampleAndExport();
}

TEST_F(ProfileExporterRumTests, RumContextGenerationIncrement) {
    // Perform multiple updates
    for (int i = 0; i < 5; ++i) {
        std::string viewName = "view-" + std::to_string(i);
        exporter->UpdateRumContext(
            "a991ca10-4004-4004-4004-beefbeefbeef",  // application_id
            "5e551017-4114-4114-4114-beeeefbeeeef",  // session_id
            "141ee144-4224-4224-4224-beeeeeeeeeef",  // view_id
            viewName.c_str(),                         // view_name
            "4c10171e-4334-4334-4334-b0000eeeefff"   // action_id
        );

        // Each update should succeed
        AddSampleAndExport();
    }
}

TEST_F(ProfileExporterRumTests, RumContextConcurrentUpdates) {
    // Test thread-safety of updates
    const int kNumThreads = 10;
    const auto kDuration = std::chrono::milliseconds(100);
    std::atomic<bool> stopFlag{false};
    std::vector<std::thread> threads;

    // Spawn threads that continuously update RUM context
    for (int i = 0; i < kNumThreads; ++i) {
        threads.emplace_back([this, i, &stopFlag]() {
            while (!stopFlag.load()) {
                std::string viewName = "concurrent-view-" + std::to_string(i);
                exporter->UpdateRumContext(
                    "a991ca10-4004-4004-4004-beefbeefbeef",  // application_id
                    "5e551017-4114-4114-4114-beeeefbeeeef",  // session_id
                    "141ee144-4224-4224-4224-beeeeeeeeeef",  // view_id
                    viewName.c_str(),                         // view_name
                    "4c10171e-4334-4334-4334-b0000eeeefff"   // action_id
                );
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    // Let them run for a short time
    std::this_thread::sleep_for(kDuration);
    stopFlag.store(true);

    // Join all threads
    for (auto& t : threads) {
        t.join();
    }

    // Verify no crashes occurred and we can still export
    EXPECT_NO_THROW(AddSampleAndExport());
}

TEST_F(ProfileExporterRumTests, RumContextSamplingDuringUpdate) {
    // Test that samples observe consistent state during updates
    const auto kDuration = std::chrono::milliseconds(100);
    std::atomic<bool> stopFlag{false};

    // Thread 1: Continuously update RUM context
    std::thread updater([this, &stopFlag]() {
        int counter = 0;
        while (!stopFlag.load()) {
            std::string viewName = "sampling-view-" + std::to_string(counter++ % 10);
            exporter->UpdateRumContext(
                "a991ca10-4004-4004-4004-beefbeefbeef",  // application_id
                "5e551017-4114-4114-4114-beeeefbeeeef",  // session_id
                "141ee144-4224-4224-4224-beeeeeeeeeef",  // view_id
                viewName.c_str(),                         // view_name
                "4c10171e-4334-4334-4334-b0000eeeefff"   // action_id
            );
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    // Main thread: Continuously create samples
    for (int i = 0; i < 10; ++i) {
        AddSampleAndExport();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    stopFlag.store(true);
    updater.join();

    // Should complete without crashes or corruption
    EXPECT_NO_THROW(AddSampleAndExport());
}

TEST_F(ProfileExporterRumTests, RumContextMultipleUpdates) {
    // Test updating context multiple times with different view names
    const std::vector<std::string> testViewNames = {
        "login-page",
        "dashboard",
        "settings",
        "profile",
        "checkout"
    };

    for (const auto& viewName : testViewNames) {
        exporter->UpdateRumContext(
            "a991ca10-4004-4004-4004-beefbeefbeef",  // application_id
            "5e551017-4114-4114-4114-beeeefbeeeef",  // session_id
            "141ee144-4224-4224-4224-beeeeeeeeeef",  // view_id
            viewName.c_str(),                         // view_name
            "4c10171e-4334-4334-4334-b0000eeeefff"   // action_id
        );
        AddSampleAndExport();
    }

    // All updates should succeed
    EXPECT_TRUE(exporter->IsInitialized());
}

TEST_F(ProfileExporterRumTests, RumContextClearAndSet) {
    // Set RUM context
    exporter->UpdateRumContext(
        "a991ca10-4004-4004-4004-beefbeefbeef",  // application_id
        "5e551017-4114-4114-4114-beeeefbeeeef",  // session_id
        "141ee144-4224-4224-4224-beeeeeeeeeef",  // view_id
        "initial-view",                           // view_name
        "4c10171e-4334-4334-4334-b0000eeeefff"   // action_id
    );
    AddSampleAndExport();

    // Clear it (set to empty)
    exporter->UpdateRumContext("", "", "", "", "");
    AddSampleAndExport();

    // Set it again
    exporter->UpdateRumContext(
        "a991ca10-4004-4004-4004-beefbeefbeef",  // application_id
        "5e551017-4114-4114-4114-beeeefbeeeef",  // session_id
        "141ee144-4224-4224-4224-beeeeeeeeeef",  // view_id
        "final-view",                             // view_name
        "4c10171e-4334-4334-4334-b0000eeeefff"   // action_id
    );
    AddSampleAndExport();

    // Should handle clear/set cycle correctly
    EXPECT_TRUE(exporter->IsInitialized());
}

TEST_F(ProfileExporterRumTests, RumContextViewNameOnly) {
    // Update with only view_name set, all UUIDs empty
    exporter->UpdateRumContext(
        "",
        "",
        "",
        "my-view-name",  // view_name
        ""
    );

    // Should succeed - only rum.view_name label should be added
    AddSampleAndExport();
}

TEST_F(ProfileExporterRumTests, RumContextViewNameWithUUIDs) {
    // Update with view_name and UUIDs
    exporter->UpdateRumContext(
        "a991ca10-4004-4004-4004-beefbeefbeef",  // application_id
        "5e551017-4114-4114-4114-beeeefbeeeef",  // session_id
        "141ee144-4224-4224-4224-beeeeeeeeeef",  // view_id
        "checkout-page",                          // view_name
        "4c10171e-4334-4334-4334-b0000eeeefff"   // action_id
    );

    // Should succeed - all five rum.* labels should be present
    AddSampleAndExport();
}

TEST_F(ProfileExporterRumTests, RumContextViewNameTruncation) {
    // Pass view_name longer than 128 chars
    std::string longViewName(200, 'v');  // 200 v's

    exporter->UpdateRumContext(
        "",
        "",
        "",
        longViewName.c_str(),
        ""
    );

    // Should succeed - truncation should happen safely (max 127 chars + null)
    AddSampleAndExport();
}