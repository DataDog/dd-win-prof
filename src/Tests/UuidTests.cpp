// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"
#include <gtest/gtest.h>

#include "../InprocProfiling/Uuid.h"

class UuidTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Common setup for all tests
    }

    void TearDown() override {
        // Common cleanup for all tests
    }
};

TEST_F(UuidTest, CanCreateUuid) {
    // Arrange & Act
    ddprof::Uuid uuid;

    // Assert
    EXPECT_EQ(uuid.get_version(), 4) << "UUID should be version 4";
}

TEST_F(UuidTest, UuidToStringHasCorrectFormat) {
    // Arrange
    ddprof::Uuid uuid;

    // Act
    std::string uuidStr = uuid.to_string();

    // Assert
    EXPECT_EQ(uuidStr.length(), 36) << "UUID string should be 36 characters long";
    EXPECT_EQ(uuidStr[8], '-') << "UUID should have dash at position 8";
    EXPECT_EQ(uuidStr[13], '-') << "UUID should have dash at position 13";
    EXPECT_EQ(uuidStr[18], '-') << "UUID should have dash at position 18";
    EXPECT_EQ(uuidStr[23], '-') << "UUID should have dash at position 23";

    // Check that it contains only valid hex characters and dashes
    for (size_t i = 0; i < uuidStr.length(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            EXPECT_EQ(uuidStr[i], '-') << "Expected dash at position " << i;
        } else {
            EXPECT_TRUE(std::isxdigit(uuidStr[i])) << "Expected hex digit at position " << i << ", got '" << uuidStr[i] << "'";
        }
    }
}

TEST_F(UuidTest, UuidsAreUnique) {
    // Arrange & Act
    ddprof::Uuid uuid1;
    ddprof::Uuid uuid2;

    std::string str1 = uuid1.to_string();
    std::string str2 = uuid2.to_string();

    // Assert
    EXPECT_NE(str1, str2) << "Two UUIDs should be different";
    EXPECT_EQ(str1.length(), 36) << "First UUID should be 36 characters";
    EXPECT_EQ(str2.length(), 36) << "Second UUID should be 36 characters";
}

TEST_F(UuidTest, UuidVersionAndVariantAreCorrect) {
    // Arrange
    ddprof::Uuid uuid;

    // Act
    int version = uuid.get_version();

    // Assert
    EXPECT_EQ(version, 4) << "UUID version should be 4 (random)";

    // Check that the variant bits are correct (should be 10xx in binary, which is 8-B in hex)
    uint8_t variantNibble = uuid.data[ddprof::Uuid::k_variant_position];
    EXPECT_GE(variantNibble, 0x8) << "Variant should be >= 8";
    EXPECT_LE(variantNibble, 0xB) << "Variant should be <= B";
}