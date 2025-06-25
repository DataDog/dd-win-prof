#include "pch.h"
#include <gtest/gtest.h>

// Include libdatadog headers FIRST to get complete type definitions
#include "datadog/profiling.h"
#include "datadog/common.h"

#include "../InprocProfiling/PprofAggregator.h"
#include "../InprocProfiling/ProfileExporter.h"
#include "../InprocProfiling/Configuration.h"
#include <memory>
#include <vector>
#include <iostream>

using namespace InprocProfiling;

// Forward declaration of test function from SymbolicationTests.cpp
extern void GlobalTestFunction();

class PprofAggregatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Common setup for all tests
        config = std::make_unique<Configuration>();
        config->SetExportEnabled(false); // Disable export for all tests to prevent network calls
    }

    void TearDown() override {
        // Common cleanup for all tests
        config.reset();
    }

    std::unique_ptr<Configuration> config;

    // Helper method to create string storage for tests
    ddog_prof_ManagedStringStorage CreateStringStorage() {
        auto result = ddog_prof_ManagedStringStorage_new();
        if (result.tag != DDOG_PROF_MANAGED_STRING_STORAGE_NEW_RESULT_OK) {
            std::cout << "Failed to create string storage for test" << std::endl;
            // Return a default-constructed storage (will be invalid but tests should handle this)
            return ddog_prof_ManagedStringStorage{};
        }
        return result.ok;
    }

    // Helper method to create default sample types for testing
    std::vector<SampleValueType> CreateDefaultSampleTypes() {
        std::vector<SampleValueType> types;
        types.push_back({"cpu-samples", "count"});
        types.push_back({"cpu-time", "nanoseconds"});
        types.push_back({"wall-time", "nanoseconds"});
        return types;
    }

    // Helper method to create a single sample type for minimal testing
    std::vector<SampleValueType> CreateMinimalSampleTypes() {
        std::vector<SampleValueType> types;
        types.push_back({"cpu-time", "nanoseconds"});
        types.push_back({"cpu-samples", "count"});
        std::cout << "CreateMinimalSampleTypes: Created " << types.size() << " sample types" << std::endl;
        return types;
    }

    // Helper method to create sample value type definitions for ProfileExporter
    std::vector<SampleValueType> CreateSampleValueTypeDefinitions() {
        std::vector<SampleValueType> definitions;
        definitions.push_back({"cpu-samples", "count"});
        definitions.push_back({"cpu-time", "nanoseconds"});
        definitions.push_back({"wall-time", "nanoseconds"});
        return definitions;
    }

    // Helper method to create minimal sample value type definitions
    std::vector<SampleValueType> CreateMinimalSampleValueTypeDefinitions() {
        std::vector<SampleValueType> definitions;
        definitions.push_back({"cpu-time", "nanoseconds"});
        definitions.push_back({"cpu-samples", "count"});
        return definitions;
    }

    // Helper method to safely clean up encoded profiles
    void SafeCleanupEncodedProfile(ddog_prof_EncodedProfile* profile) {
        if (profile) {
            ddog_prof_EncodedProfile_drop(profile);
        }
    }

    // Helper method to create real location IDs using ProfileExporter
    std::vector<ddog_prof_LocationId> CreateRealLocationIds(PprofAggregator& aggregator, size_t count) {
        std::vector<ddog_prof_LocationId> locationIds;
        
        // Get the profile from the aggregator
        auto* profile = aggregator.GetProfile();
        if (!profile) {
            std::cout << "CreateRealLocationIds: Failed to get profile" << std::endl;
            return locationIds; // Return empty vector if no profile
        }

        // Create some realistic function names and addresses
        std::vector<std::string> functionNames = {
            "main", "ProcessRequest", "AllocateMemory", "ComputeHash", "NetworkCall"
        };
        
        std::cout << "CreateRealLocationIds: Creating " << count << " location IDs" << std::endl;
        
        for (size_t i = 0; i < count && i < functionNames.size(); ++i) {
            // Intern function name as string
            auto functionNameResult = ddog_prof_Profile_intern_string(profile, 
                {functionNames[i].c_str(), functionNames[i].length()});
            if (functionNameResult.tag != DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID) {
                std::cout << "CreateRealLocationIds: Failed to intern function name '" << functionNames[i] 
                          << "' (tag: " << functionNameResult.tag << ")" << std::endl;
                continue; // Skip this one if it fails
            }

            // Create empty string for system name and filename
            auto emptyStringId = ddog_prof_Profile_interned_empty_string();

            // Intern function
            auto functionResult = ddog_prof_Profile_intern_function(profile, 
                functionNameResult.ok, emptyStringId, emptyStringId);
            if (functionResult.tag != DDOG_PROF_FUNCTION_ID_RESULT_OK_GENERATIONAL_ID_FUNCTION_ID) {
                std::cout << "CreateRealLocationIds: Failed to intern function (tag: " << functionResult.tag << ")" << std::endl;
                continue; // Skip this one if it fails
            }

            // Create location with realistic address (use a static function as base)
            uint64_t address = reinterpret_cast<uint64_t>(GlobalTestFunction) + (i * 0x100);
            auto locationResult = ddog_prof_Profile_intern_location(profile, 
                functionResult.ok, address, static_cast<int64_t>(i + 1));
            if (locationResult.tag == DDOG_PROF_LOCATION_ID_RESULT_OK_GENERATIONAL_ID_LOCATION_ID) {
                locationIds.push_back(locationResult.ok);
                std::cout << "CreateRealLocationIds: Successfully created location ID for '" << functionNames[i] 
                          << "' at address 0x" << std::hex << address << std::dec << std::endl;
            } else {
                std::cout << "CreateRealLocationIds: Failed to intern location (tag: " << locationResult.tag << ")" << std::endl;
            }
        }
        
        std::cout << "CreateRealLocationIds: Created " << locationIds.size() << " location IDs" << std::endl;
        return locationIds;
    }

    // Helper method to create sample values matching the sample types
    std::vector<int64_t> CreateSampleValues(std::span<const SampleValueType> types) {
        std::vector<int64_t> values;
        for (const auto& type : types) {
            if (type.Name == "cpu-samples" && type.Unit == "count") {
                values.push_back(1); // 1 sample
            } else if (type.Name == "cpu-time" && type.Unit == "nanoseconds") {
                values.push_back(1000000); // 1ms in nanoseconds
            } else if (type.Name == "wall-time" && type.Unit == "nanoseconds") {
                values.push_back(2000000); // 2ms in nanoseconds
            } else if (type.Name == "alloc-samples" && type.Unit == "count") {
                values.push_back(5); // 5 allocations
            } else if (type.Name == "alloc-space" && type.Unit == "bytes") {
                values.push_back(1024); // 1KB allocated
            } else {
                values.push_back(100); // Default value for unknown types
            }
        }
        return values;
    }

    // Helper method to create an empty labelset for testing
    ddog_prof_LabelSetId CreateEmptyLabelSet(PprofAggregator& aggregator) {
        auto* profile = aggregator.GetProfile();
        if (!profile) {
            std::cout << "CreateEmptyLabelSet: Failed to get profile" << std::endl;
            return ddog_prof_LabelSetId{};
        }
        
        // Create empty label set
        ddog_prof_Slice_LabelId empty_label_slice = {
            .ptr = nullptr,
            .len = 0
        };
        
        auto labelset_result = ddog_prof_Profile_intern_labelset(profile, empty_label_slice);
        if (labelset_result.tag != DDOG_PROF_LABEL_SET_ID_RESULT_OK_GENERATIONAL_ID_LABEL_SET_ID) {
            std::cout << "CreateEmptyLabelSet: Failed to intern empty labelset (tag: " << labelset_result.tag << ")" << std::endl;
            return ddog_prof_LabelSetId{};
        }
        
        return labelset_result.ok;
    }
};

TEST_F(PprofAggregatorTest, InitializationWithValidSampleTypes) {
    // Arrange
    auto sampleTypes = CreateDefaultSampleTypes();
    auto stringStorage = CreateStringStorage();

    // Act
    PprofAggregator aggregator(sampleTypes, stringStorage);

    // Assert
    EXPECT_TRUE(aggregator.IsInitialized()) << "Aggregator should be initialized with valid sample types";
    EXPECT_TRUE(aggregator.GetLastError().empty()) << "No error should be present after successful initialization";
    EXPECT_NE(aggregator.GetProfile(), nullptr) << "Profile should be accessible after initialization";
}

TEST_F(PprofAggregatorTest, InitializationWithMinimalSampleTypes) {
    // Arrange
    auto sampleTypes = CreateMinimalSampleTypes();
    auto stringStorage = CreateStringStorage();

    // Act
    PprofAggregator aggregator(sampleTypes, stringStorage);

    // Assert
    EXPECT_TRUE(aggregator.IsInitialized()) << "Aggregator should be initialized with minimal sample types";
    EXPECT_TRUE(aggregator.GetLastError().empty()) << "No error should be present after successful initialization";
}

TEST_F(PprofAggregatorTest, InitializationWithEmptySampleTypes) {
    // Arrange
    std::vector<SampleValueType> emptySampleTypes;
    auto stringStorage = CreateStringStorage();

    // Act
    PprofAggregator aggregator(emptySampleTypes, stringStorage);

    // Assert
    EXPECT_FALSE(aggregator.IsInitialized()) << "Aggregator should not be initialized with empty sample types";
    EXPECT_FALSE(aggregator.GetLastError().empty()) << "Error should be present when initialization fails";
    EXPECT_EQ(aggregator.GetProfile(), nullptr) << "Profile should not be accessible when initialization fails";
}

TEST_F(PprofAggregatorTest, InitializationWithPeriod) {
    // Arrange
    auto sampleTypes = CreateDefaultSampleTypes();
    auto stringStorage = CreateStringStorage();
    int periodMs = 1000; // 1 second in milliseconds

    // Act
    PprofAggregator aggregator(sampleTypes, stringStorage, periodMs);

    // Assert
    EXPECT_TRUE(aggregator.IsInitialized()) << "Aggregator should be initialized with valid sample types and period";
    EXPECT_TRUE(aggregator.GetLastError().empty()) << "No error should be present after successful initialization";
}

TEST_F(PprofAggregatorTest, AddSampleWithRealLocationIds) {
    // Arrange
    auto sampleTypes = CreateMinimalSampleTypes();
    auto stringStorage = CreateStringStorage();
    PprofAggregator aggregator(sampleTypes, stringStorage);
    ASSERT_TRUE(aggregator.IsInitialized()) << "Precondition: aggregator must be initialized";

    auto locationIds = CreateRealLocationIds(aggregator, 3); // Stack with 3 frames
    ASSERT_GT(locationIds.size(), 0) << "Should have created at least one location ID";
    
    auto values = CreateSampleValues(sampleTypes);
    int64_t timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Act
    auto labelsetId = CreateEmptyLabelSet(aggregator);
    bool result = aggregator.AddSample(locationIds, values, timestamp, labelsetId);
    std::cout << "AddSampleWithRealLocationIds: AddSample result: " << result << std::endl;
    std::cout << "Last error: " << aggregator.GetLastError() << std::endl;
    // Assert
    EXPECT_TRUE(result) << "AddSample should succeed with real location IDs";
    EXPECT_TRUE(aggregator.GetLastError().empty()) << "No error should be present after successful AddSample";
}

TEST_F(PprofAggregatorTest, AddSampleWithMismatchedValueCount) {
    // Arrange
    auto sampleTypes = CreateDefaultSampleTypes(); // 3 sample types
    auto stringStorage = CreateStringStorage();
    PprofAggregator aggregator(sampleTypes, stringStorage);
    ASSERT_TRUE(aggregator.IsInitialized()) << "Precondition: aggregator must be initialized";

    auto locationIds = CreateRealLocationIds(aggregator, 2);
    std::vector<int64_t> wrongValues = {100}; // Only 1 value instead of 3
    int64_t timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Act
    auto labelsetId = CreateEmptyLabelSet(aggregator);
    bool result = aggregator.AddSample(locationIds, wrongValues, timestamp, labelsetId);

    // Assert
    EXPECT_FALSE(result) << "AddSample should fail with mismatched value count";
    EXPECT_FALSE(aggregator.GetLastError().empty()) << "Error should be present when value count is wrong";
}

TEST_F(PprofAggregatorTest, AddSampleToUninitializedAggregator) {
    // Arrange
    std::vector<SampleValueType> emptySampleTypes;
    auto stringStorage = CreateStringStorage();
    PprofAggregator aggregator(emptySampleTypes, stringStorage);
    ASSERT_FALSE(aggregator.IsInitialized()) << "Precondition: aggregator must not be initialized";

    // Create a temporary initialized aggregator just to get location IDs
    auto tempSampleTypes = CreateMinimalSampleTypes();
    auto tempStringStorage = CreateStringStorage();
    PprofAggregator tempAggregator(tempSampleTypes, tempStringStorage);
    auto locationIds = CreateRealLocationIds(tempAggregator, 2);
    
    std::vector<int64_t> values = {100};
    int64_t timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Act
    auto labelsetId = CreateEmptyLabelSet(aggregator);
    bool result = aggregator.AddSample(locationIds, values, timestamp, labelsetId);

    // Assert
    EXPECT_FALSE(result) << "AddSample should fail on uninitialized aggregator";
    EXPECT_FALSE(aggregator.GetLastError().empty()) << "Error should be present when aggregator is not initialized";
}

TEST_F(PprofAggregatorTest, AddMultipleSamples) {
    // Arrange
    auto sampleTypes = CreateMinimalSampleTypes();
    auto stringStorage = CreateStringStorage();
    PprofAggregator aggregator(sampleTypes, stringStorage);
    ASSERT_TRUE(aggregator.IsInitialized()) << "Precondition: aggregator must be initialized";

    auto locationIds1 = CreateRealLocationIds(aggregator, 2);
    auto locationIds2 = CreateRealLocationIds(aggregator, 3);
    auto values = CreateSampleValues(sampleTypes);
    int64_t baseTimestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Act
    auto labelsetId = CreateEmptyLabelSet(aggregator);
    bool result1 = aggregator.AddSample(locationIds1, values, baseTimestamp, labelsetId);
    bool result2 = aggregator.AddSample(locationIds2, values, baseTimestamp + 1000000, labelsetId); // 1ms later

    // Assert
    EXPECT_TRUE(result1) << "First AddSample should succeed";
    EXPECT_TRUE(result2) << "Second AddSample should succeed";
    EXPECT_TRUE(aggregator.GetLastError().empty()) << "No error should be present after multiple successful AddSample calls";
}

TEST_F(PprofAggregatorTest, IntegrationWithProfileExporterAndDebugFileWriting) {
    // Arrange
    auto sampleValueTypeDefs = CreateMinimalSampleValueTypeDefinitions();
    Sample::SetValuesCount(sampleValueTypeDefs.size());
    ProfileExporter exporter(config.get(), sampleValueTypeDefs);
    ASSERT_TRUE(exporter.Initialize()) << "ProfileExporter should initialize successfully";
    
    // Configure debug file writing (OFF by default when no output directory is configured)
    EXPECT_FALSE(exporter.IsDebugPprofFileWritingEnabled()) << "Debug file writing should be OFF by default";
    
    // Enable debug file writing with a test prefix (using current directory)
    exporter.SetDebugPprofFileWritingEnabled(true);
    exporter.SetDebugPprofPrefix(".\\test_profile_");
    
    // todo: add some settings on this to avoid flooding the disk
    EXPECT_TRUE(exporter.IsDebugPprofFileWritingEnabled()) << "Debug file writing should be enabled";
    EXPECT_EQ(exporter.GetDebugPprofPrefix(), ".\\test_profile_") << "Debug prefix should be set correctly";

    // Create a Sample object and add it to the ProfileExporter
    // This will go into the ProfileExporter's internal aggregator
    int64_t timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::chrono::nanoseconds sampleTimestamp(timestamp);
    
    // Create a valid ThreadInfo with a real handle
    HANDLE hThread;
    ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &hThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    auto threadInfo = std::make_shared<ThreadInfo>(::GetCurrentThreadId(), hThread);
    
    // weird addresses, todo: make these more realistic
    // uint64_t callstack[] = {0x1234567890ABCDEF, 0xFEDCBA0987654321, 0x0000000012345678};
    uint64_t callstack[] = { reinterpret_cast<uint64_t>(&GlobalTestFunction)};
    auto sample = std::make_shared<Sample>(sampleTimestamp, threadInfo, callstack, 1);
    sample->AddValue(10000000, 0);  // cpu-time in nanoseconds
    sample->AddValue(1, 1);         // cpu-samples count
    
    // Add the sample to the ProfileExporter - this uses the exporter's internal aggregator
    bool addToExporterResult = exporter.Add(sample);
    EXPECT_TRUE(addToExporterResult) << "Should be able to add sample to ProfileExporter";

    // Test ProfileExporter export functionality with debug file writing
    // This will serialize the profile (with the samples we just added) and write debug files if enabled
    bool exportResult = exporter.Export();
    EXPECT_TRUE(exportResult) << "Export should succeed (debug file writing failure is non-fatal)";

    // Disable debug file writing and test again
    exporter.SetDebugPprofFileWritingEnabled(false);
    EXPECT_FALSE(exporter.IsDebugPprofFileWritingEnabled()) << "Debug file writing should be disabled";
    
}

TEST_F(PprofAggregatorTest, CreateEmptyPprof) {
    // Arrange
    auto sampleTypes = CreateDefaultSampleTypes();
    auto stringStorage = CreateStringStorage();
    PprofAggregator aggregator(sampleTypes, stringStorage);
    ASSERT_TRUE(aggregator.IsInitialized()) << "Precondition: aggregator must be initialized";

    // Act
    auto startMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 60000; // 1 minute ago
    auto endMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto encodedProfile = aggregator.Serialize(startMs, endMs);

    // Assert
    EXPECT_NE(encodedProfile, nullptr) << "Serialize should return a valid encoded profile";
    EXPECT_TRUE(aggregator.GetLastError().empty()) << "No error should be present after successful serialization";

    // Cleanup
    SafeCleanupEncodedProfile(encodedProfile);
}

TEST_F(PprofAggregatorTest, SerializeProfileWithSamples) {
    // Arrange
    auto sampleTypes = CreateMinimalSampleTypes();
    auto stringStorage = CreateStringStorage();
    PprofAggregator aggregator(sampleTypes, stringStorage);
    ASSERT_TRUE(aggregator.IsInitialized()) << "Precondition: aggregator must be initialized";

    // Add some samples
    auto locationIds = CreateRealLocationIds(aggregator, 2);
    auto values = CreateSampleValues(sampleTypes);
    int64_t timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    auto labelsetId = CreateEmptyLabelSet(aggregator);
    ASSERT_TRUE(aggregator.AddSample(locationIds, values, timestamp, labelsetId)) << "Precondition: sample must be added";

    // Act
    auto startMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 60000;
    auto endMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto encodedProfile = aggregator.Serialize(startMs, endMs);

    // Assert
    EXPECT_NE(encodedProfile, nullptr) << "Serialize should return a valid encoded profile with samples";
    EXPECT_TRUE(aggregator.GetLastError().empty()) << "No error should be present after successful serialization";

    // Cleanup
    SafeCleanupEncodedProfile(encodedProfile);
}

TEST_F(PprofAggregatorTest, CreateEmptyPprofWithCustomTimes) {
    // Arrange
    auto sampleTypes = CreateMinimalSampleTypes();
    auto stringStorage = CreateStringStorage();
    PprofAggregator aggregator(sampleTypes, stringStorage);
    ASSERT_TRUE(aggregator.IsInitialized()) << "Precondition: aggregator must be initialized";

    auto startMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 60000; // 1 minute ago
    auto endMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Act
    auto encodedProfile = aggregator.Serialize(startMs, endMs);

    // Assert
    EXPECT_NE(encodedProfile, nullptr) << "Serialize should return a valid encoded profile with custom times";
    EXPECT_TRUE(aggregator.GetLastError().empty()) << "No error should be present after successful serialization";

    // Cleanup
    SafeCleanupEncodedProfile(encodedProfile);
}

TEST_F(PprofAggregatorTest, SerializeUninitializedAggregator) {
    // Arrange
    std::vector<SampleValueType> emptySampleTypes;
    auto stringStorage = CreateStringStorage();
    PprofAggregator aggregator(emptySampleTypes, stringStorage);
    ASSERT_FALSE(aggregator.IsInitialized()) << "Precondition: aggregator must not be initialized";

    // Act
    auto startMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 60000;
    auto endMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto encodedProfile = aggregator.Serialize(startMs, endMs);

    // Assert
    EXPECT_EQ(encodedProfile, nullptr) << "Serialize should return nullptr for uninitialized aggregator";
    EXPECT_FALSE(aggregator.GetLastError().empty()) << "Error should be present when serializing uninitialized aggregator";

    // No cleanup needed since encodedProfile is nullptr
}

TEST_F(PprofAggregatorTest, ResetProfile) {
    // Arrange
    auto sampleTypes = CreateDefaultSampleTypes();
    auto stringStorage = CreateStringStorage();
    PprofAggregator aggregator(sampleTypes, stringStorage);
    ASSERT_TRUE(aggregator.IsInitialized()) << "Precondition: aggregator must be initialized";

    // Add a sample before reset
    auto locationIds = CreateRealLocationIds(aggregator, 2);
    auto values = CreateSampleValues(sampleTypes);
    int64_t timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto labelsetId = CreateEmptyLabelSet(aggregator);
    aggregator.AddSample(locationIds, values, timestamp, labelsetId);

    // Act
    aggregator.Reset();

    // Assert
    EXPECT_TRUE(aggregator.IsInitialized()) << "Aggregator should remain initialized after reset";
    EXPECT_TRUE(aggregator.GetLastError().empty()) << "No error should be present after successful reset";
    
    // Verify we can still add samples after reset - need to create NEW location IDs since old ones are invalid
    auto newLocationIds = CreateRealLocationIds(aggregator, 2);
    auto newLabelsetId = CreateEmptyLabelSet(aggregator);
    bool addResult = aggregator.AddSample(newLocationIds, values, timestamp + 1000000, newLabelsetId);
    EXPECT_TRUE(addResult) << "Should be able to add samples after reset";
}

TEST_F(PprofAggregatorTest, ResetUninitializedAggregator) {
    // Arrange
    std::vector<SampleValueType> emptySampleTypes;
    auto stringStorage = CreateStringStorage();
    PprofAggregator aggregator(emptySampleTypes, stringStorage);
    ASSERT_FALSE(aggregator.IsInitialized()) << "Precondition: aggregator must not be initialized";

    // Act
    aggregator.Reset();

    // Assert
    EXPECT_FALSE(aggregator.IsInitialized()) << "Uninitialized aggregator should remain uninitialized after reset";
}

TEST_F(PprofAggregatorTest, MultipleSerializations) {
    // Arrange
    auto sampleTypes = CreateDefaultSampleTypes();
    auto stringStorage = CreateStringStorage();
    PprofAggregator aggregator(sampleTypes, stringStorage);
    ASSERT_TRUE(aggregator.IsInitialized()) << "Precondition: aggregator must be initialized";

    // Act & Assert - First serialization
    auto startMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 60000;
    auto endMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto encodedProfile1 = aggregator.Serialize(startMs, endMs);
    EXPECT_NE(encodedProfile1, nullptr) << "First serialization should succeed";

    // Act & Assert - Second serialization (should work after reset during serialization)
    auto encodedProfile2 = aggregator.Serialize(startMs, endMs);
    EXPECT_NE(encodedProfile2, nullptr) << "Second serialization should succeed";

    // Cleanup
    SafeCleanupEncodedProfile(encodedProfile1);
    SafeCleanupEncodedProfile(encodedProfile2);
}

// Test to ensure the aggregator handles different sample types
TEST_F(PprofAggregatorTest, InitializationWithAllocationSampleTypes) {
    // Arrange
    std::vector<SampleValueType> allocationTypes;
    allocationTypes.push_back({"alloc-samples", "count"});
    allocationTypes.push_back({"alloc-space", "bytes"});

    // Act
    auto stringStorage = CreateStringStorage();
    PprofAggregator aggregator(allocationTypes, stringStorage);

    // Assert
    EXPECT_TRUE(aggregator.IsInitialized()) << "Aggregator should be initialized with allocation sample types";
    EXPECT_TRUE(aggregator.GetLastError().empty()) << "No error should be present with valid allocation sample types";
}

TEST_F(PprofAggregatorTest, AddAllocationSamples) {
    // Arrange
    std::vector<SampleValueType> allocationTypes;
    allocationTypes.push_back({"alloc-samples", "count"});
    allocationTypes.push_back({"alloc-space", "bytes"});
    
    auto stringStorage = CreateStringStorage();
    PprofAggregator aggregator(allocationTypes, stringStorage);
    ASSERT_TRUE(aggregator.IsInitialized()) << "Precondition: aggregator must be initialized";

    auto locationIds = CreateRealLocationIds(aggregator, 3);
    auto values = CreateSampleValues(allocationTypes);
    int64_t timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Act
    auto labelsetId = CreateEmptyLabelSet(aggregator);
    bool result = aggregator.AddSample(locationIds, values, timestamp, labelsetId);

    // Assert
    EXPECT_TRUE(result) << "AddSample should succeed with allocation sample types";
    EXPECT_TRUE(aggregator.GetLastError().empty()) << "No error should be present after successful AddSample";
}

TEST_F(PprofAggregatorTest, SerializeAfterAddingSamples) {
    // Arrange
    auto sampleTypes = CreateDefaultSampleTypes();
    auto stringStorage = CreateStringStorage();
    PprofAggregator aggregator(sampleTypes, stringStorage);
    ASSERT_TRUE(aggregator.IsInitialized()) << "Precondition: aggregator must be initialized";

    // Add multiple samples
    auto locationIds1 = CreateRealLocationIds(aggregator, 2);
    auto locationIds2 = CreateRealLocationIds(aggregator, 4);
    auto values = CreateSampleValues(sampleTypes);
    int64_t baseTimestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto labelsetId = CreateEmptyLabelSet(aggregator);
    ASSERT_TRUE(aggregator.AddSample(locationIds1, values, baseTimestamp, labelsetId));
    ASSERT_TRUE(aggregator.AddSample(locationIds2, values, baseTimestamp + 1000000, labelsetId));

    // Act
    auto startMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 60000;
    auto endMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto encodedProfile = aggregator.Serialize(startMs, endMs);

    // Assert
    EXPECT_NE(encodedProfile, nullptr) << "Serialize should succeed after adding samples";
    EXPECT_TRUE(aggregator.GetLastError().empty()) << "No error should be present after successful serialization";

    // Cleanup
    SafeCleanupEncodedProfile(encodedProfile);
}

TEST_F(PprofAggregatorTest, AddSampleWithEmptyLocations) {
    // Initialize with minimal sample types
    auto sampleTypes = CreateMinimalSampleTypes();
    auto stringStorage = CreateStringStorage();
    PprofAggregator aggregator(sampleTypes, stringStorage);
    ASSERT_TRUE(aggregator.IsInitialized()) << "Precondition: aggregator must be initialized";

    // Create empty locations vector
    std::vector<ddog_prof_LocationId> emptyLocations;

    // Create sample values (2 values to match sample types)
    std::vector<int64_t> values = {100, 1};  // Simple values for CPU time and samples
    
    // Use a simple timestamp
    int64_t timestamp = 1000000;  // 1ms in nanoseconds

    // Try to add the sample
    std::cout << "Attempting to add sample with empty locations" << std::endl;
    auto labelsetId = CreateEmptyLabelSet(aggregator);
    bool result = aggregator.AddSample(emptyLocations, values, timestamp, labelsetId);
    
    // We expect this to succeed since empty locations should be valid
    EXPECT_TRUE(result) << "AddSample should succeed with empty locations";
    EXPECT_TRUE(aggregator.GetLastError().empty()) << "No error should be present after successful AddSample";
} 
