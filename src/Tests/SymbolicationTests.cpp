// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"
#include "../dd-win-prof/Symbolication.h"

// Include libdatadog headers for string storage
#include "datadog/profiling.h"
#include "datadog/common.h"

#include <iomanip>
#include <sstream>
#include <algorithm>

// Test functions defined outside of any class to avoid confusion
void GlobalTestFunction() {
    // Simple global test function - should be easy to symbolicate
    volatile int dummy = 42;
    dummy++;
    dummy *= 2;
}

void GlobalTestFunctionWithParams(int param1, double param2) {
    // Global function with parameters
    volatile int result = param1 * 2;
    volatile double dresult = param2 * 3.14;
    result += static_cast<int>(dresult);
}

// Static function for testing
static void StaticGlobalFunction() {
    // Static global function
    volatile int counter = 0;
    counter++;
}

// Test fixture class for Symbolication tests
class SymbolicationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup code that runs before each test
        // Initialize managed string storage for tests
        auto storageResult = ddog_prof_ManagedStringStorage_new();
        if (storageResult.tag == DDOG_PROF_MANAGED_STRING_STORAGE_NEW_RESULT_OK) {
            _stringStorage = storageResult.ok;
            _hasStringStorage = true;
        } else {
            _hasStringStorage = false;
        }
    }

    void TearDown() override {
        // Cleanup code that runs after each test
        if (_hasStringStorage) {
            ddog_prof_ManagedStringStorage_drop(_stringStorage);
            _hasStringStorage = false;
        }
    }

    void LogSymbolInfo(const std::string& description, uint64_t address, const CachedSymbolInfo& info) {
        std::cout << "\n=== " << description << " ===" << std::endl;
        std::cout << "Address: 0x" << std::hex << std::uppercase << address << std::endl;
        std::cout << (info.isValid ? "Valid: YES" : "Valid: NO") << std::endl;

        if (info.isValid && _hasStringStorage) {
            // Get the actual string values from the managed storage
            auto functionNameResult = ddog_prof_ManagedStringStorage_get_string(_stringStorage, info.FunctionNameId);
            auto fileNameResult = ddog_prof_ManagedStringStorage_get_string(_stringStorage, info.FileNameId);

            if (functionNameResult.tag == DDOG_STRING_WRAPPER_RESULT_OK) {
                // Access the string data from the Vec_U8
                const char* str_data = reinterpret_cast<const char*>(functionNameResult.ok.message.ptr);
                size_t str_len = functionNameResult.ok.message.len;
                std::string functionName(str_data, str_len);
                std::cout << "Function Name: '" << functionName << "' (length: " << functionName.length() << ")" << std::endl;

                // Check for common name mangling patterns
                if (functionName.find("?") != std::string::npos) {
                    std::cout << "Note: Function name appears to be C++ mangled" << std::endl;
                }
                if (functionName.find("@") != std::string::npos) {
                    std::cout << "Note: Function name contains @ symbol" << std::endl;
                }

                // Clean up the string wrapper
                ddog_StringWrapper_drop(&functionNameResult.ok);
            } else {
                std::cout << "Function Name: <failed to retrieve>" << std::endl;
            }

            if (fileNameResult.tag == DDOG_STRING_WRAPPER_RESULT_OK && fileNameResult.ok.message.len > 0) {
                // Access the string data from the Vec_U8
                const char* str_data = reinterpret_cast<const char*>(fileNameResult.ok.message.ptr);
                size_t str_len = fileNameResult.ok.message.len;
                std::string fileName(str_data, str_len);
                std::cout << "File Name: '" << fileName << "'" << std::endl;
                std::cout << "Line Number: " << std::dec << info.lineNumber << std::endl;

                // Clean up the string wrapper
                ddog_StringWrapper_drop(&fileNameResult.ok);
            } else {
                std::cout << "File Name: <not available>" << std::endl;
            }

            std::cout << "Displacement: " << std::dec << info.displacement << " bytes" << std::endl;
        }

        std::cout << "=========================" << std::endl;
    }

    // Helper function to check if function name contains expected substring
    bool ContainsExpectedFunctionName(const CachedSymbolInfo& symbolInfo, const std::string& expectedSubstring) {
        if (!symbolInfo.isValid || !_hasStringStorage) {
            return false;
        }

        auto functionNameResult = ddog_prof_ManagedStringStorage_get_string(_stringStorage, symbolInfo.FunctionNameId);
        if (functionNameResult.tag != DDOG_STRING_WRAPPER_RESULT_OK) {
            return false;
        }

        // Access the string data from the Vec_U8
        const char* str_data = reinterpret_cast<const char*>(functionNameResult.ok.message.ptr);
        size_t str_len = functionNameResult.ok.message.len;
        std::string actualName(str_data, str_len);

        // Clean up the string wrapper
        ddog_StringWrapper_drop(&functionNameResult.ok);

        std::string lowerActual = actualName;
        std::string lowerExpected = expectedSubstring;

        std::transform(lowerActual.begin(), lowerActual.end(), lowerActual.begin(), ::tolower);
        std::transform(lowerExpected.begin(), lowerExpected.end(), lowerExpected.begin(), ::tolower);

        return lowerActual.find(lowerExpected) != std::string::npos;
    }

    // Helper to get function name as string for debugging
    std::string GetFunctionName(const CachedSymbolInfo& symbolInfo) {
        if (!_hasStringStorage || symbolInfo.FunctionNameId.value == 0)
        {
            return "<unknown>";
        }

        ddog_StringWrapperResult result = ddog_prof_ManagedStringStorage_get_string(_stringStorage, symbolInfo.FunctionNameId);
        if (result.tag != DDOG_STRING_WRAPPER_RESULT_OK)
        {
            return "<error>";
        }

        // Access the string data from the Vec_U8
        const char* str_data = reinterpret_cast<const char*>(result.ok.message.ptr);
        size_t str_len = result.ok.message.len;

        std::string functionName(str_data, str_len);

        // Clean up the string wrapper
        ddog_StringWrapper_drop(&result.ok);

        return functionName;
    }

protected:
    ddog_prof_ManagedStringStorage _stringStorage;
    bool _hasStringStorage;
};

TEST_F(SymbolicationTest, TestInitialization) {
    std::cout << "=== Testing Symbolication Initialization ===" << std::endl;

    Symbolication symbolication;

    // Test initial state
    EXPECT_FALSE(symbolication.IsInitialized()) << "Should not be initialized initially";
    std::cout << "[OK] Initial state: Not initialized" << std::endl;

    // Test initialization
    std::cout << "Attempting to initialize symbolication..." << std::endl;
    bool initResult = symbolication.Initialize();
    EXPECT_TRUE(initResult) << "Initialization should succeed";
    EXPECT_TRUE(symbolication.IsInitialized()) << "Should be initialized after Initialize()";
    std::cout << "[OK] Initialization: SUCCESS" << std::endl;

    // Test cleanup
    std::cout << "Testing cleanup..." << std::endl;
    symbolication.Cleanup();
    EXPECT_FALSE(symbolication.IsInitialized()) << "Should not be initialized after Cleanup()";
    std::cout << "[OK] Cleanup: SUCCESS" << std::endl;
}

TEST_F(SymbolicationTest, TestBasicSymbolication) {
    std::cout << "=== Testing Basic Address Symbolication ===" << std::endl;

    ASSERT_TRUE(_hasStringStorage) << "String storage should be initialized";

    Symbolication symbolication;
    ASSERT_TRUE(symbolication.Initialize()) << "Initialization should succeed";

    // Get address of the global test function
    uint64_t testAddress = reinterpret_cast<uint64_t>(&GlobalTestFunction);

    std::cout << "Testing with GlobalTestFunction at address: 0x" << std::hex << std::uppercase << testAddress << std::endl;

    // Symbolicate the address using the new API
    auto symbolInfoOpt = symbolication.SymbolicateAndIntern(testAddress, _stringStorage);
    ASSERT_TRUE(symbolInfoOpt.has_value()) << "Symbolication should return a value";
    CachedSymbolInfo symbolInfo = symbolInfoOpt.value();

    LogSymbolInfo("GlobalTestFunction Symbolication", testAddress, symbolInfo);

    EXPECT_TRUE(symbolInfo.isValid) << "Symbolication should succeed for valid address";
    EXPECT_EQ(symbolInfo.Address, testAddress) << "Address should match the input address";

    // Verify we got the correct function name (allowing for name mangling)
    if (symbolInfo.isValid) {
        bool foundCorrectName = ContainsExpectedFunctionName(symbolInfo, "GlobalTestFunction");
        if (foundCorrectName) {
            std::cout << "[OK] Function name contains expected 'GlobalTestFunction'" << std::endl;
        } else {
            std::string actualName = GetFunctionName(symbolInfo);
            std::cout << "[WARN] Function name '" << actualName << "' does not contain 'GlobalTestFunction'" << std::endl;
        }
        EXPECT_TRUE(foundCorrectName) << "Function name should contain 'GlobalTestFunction'";
    }
}

TEST_F(SymbolicationTest, TestMultipleAddressTypes) {
    std::cout << "=== Testing Multiple Address Types ===" << std::endl;

    ASSERT_TRUE(_hasStringStorage) << "String storage should be initialized";

    Symbolication symbolication;
    ASSERT_TRUE(symbolication.Initialize()) << "Initialization should succeed";

    // Test different types of functions - all global functions to avoid class method confusion
    struct TestCase {
        const char* name;
        uint64_t address;
        const char* expectedSubstring;
    };

    TestCase testCases[] = {
        {"Global Function", reinterpret_cast<uint64_t>(&GlobalTestFunction), "GlobalTestFunction"},
        {"Function with Parameters", reinterpret_cast<uint64_t>(&GlobalTestFunctionWithParams), "GlobalTestFunctionWithParams"},
        {"Static Global Function", reinterpret_cast<uint64_t>(&StaticGlobalFunction), "StaticGlobalFunction"}
    };

    int successCount = 0;
    int correctNameCount = 0;
    int totalCases = sizeof(testCases) / sizeof(testCases[0]);

    for (int i = 0; i < totalCases; i++) {
        const TestCase& testCase = testCases[i];
        auto symbolInfoOpt = symbolication.SymbolicateAndIntern(testCase.address, _stringStorage);
        if (!symbolInfoOpt.has_value()) {
            std::cout << "[ERROR] Symbolication returned no value for: " << testCase.name << std::endl;
            continue;
        }
        CachedSymbolInfo symbolInfo = symbolInfoOpt.value();
        LogSymbolInfo(testCase.name, testCase.address, symbolInfo);

        if (symbolInfo.isValid) {
            successCount++;
            std::cout << "[OK] Successfully symbolicated: " << testCase.name << std::endl;

            // Check if we got the correct function name
            if (ContainsExpectedFunctionName(symbolInfo, testCase.expectedSubstring)) {
                correctNameCount++;
                std::cout << "[OK] Function name contains expected '" << testCase.expectedSubstring << "'" << std::endl;
            } else {
                std::string actualName = GetFunctionName(symbolInfo);
                std::cout << "[WARN] Function name '" << actualName << "' does not contain '" << testCase.expectedSubstring << "'" << std::endl;
            }
        } else {
            std::cout << "[WARN] Failed to symbolicate: " << testCase.name << std::endl;
        }
    }

    std::cout << "Summary: " << successCount << "/" << totalCases << " addresses symbolicated successfully" << std::endl;
    std::cout << "Correct names: " << correctNameCount << "/" << successCount << " symbolicated functions had correct names" << std::endl;

    // At least some should succeed
    EXPECT_GT(successCount, 0) << "At least some addresses should be symbolicated successfully";
    // At least some should have correct names
    EXPECT_GT(correctNameCount, 0) << "At least some symbolicated functions should have correct names";
}

TEST_F(SymbolicationTest, TestInvalidAddress) {
    std::cout << "=== Testing Invalid Address Symbolication ===" << std::endl;

    ASSERT_TRUE(_hasStringStorage) << "String storage should be initialized";

    Symbolication symbolication;
    ASSERT_TRUE(symbolication.Initialize()) << "Initialization should succeed";

    // Try invalid addresses
    uint64_t invalidAddresses[] = {
        0x1234,           // Very low address
        0xDEADBEEF,       // Classic invalid address
    };

    int totalAddresses = sizeof(invalidAddresses) / sizeof(invalidAddresses[0]);

    for (int i = 0; i < totalAddresses; i++) {
        uint64_t invalidAddress = invalidAddresses[i];
        auto symbolInfoOpt = symbolication.SymbolicateAndIntern(invalidAddress, _stringStorage);
        if (!symbolInfoOpt.has_value()) {
            std::cout << "[OK] Invalid address correctly returned no value" << std::endl;
            continue;
        }
        CachedSymbolInfo symbolInfo = symbolInfoOpt.value();
        LogSymbolInfo("Invalid Address Test", invalidAddress, symbolInfo);

        EXPECT_TRUE(symbolInfo.isValid) << "Should always return valid symbol, even for unknown addresses";

        if (symbolInfo.isValid) {
            std::string actualName = GetFunctionName(symbolInfo);
            if (actualName == "<unknown>") {
                std::cout << "[OK] Invalid address correctly returned unknown function: " << actualName << std::endl;
            } else {
                std::cout << "[WARN] Unexpectedly got real function name for invalid address 0x" << std::hex << invalidAddress
                          << " (function: " << actualName << ")" << std::endl;
            }
        }
    }
}

TEST_F(SymbolicationTest, TestUnknownAddressSymbolication) {
    std::cout << "=== Testing Unknown Address Symbolication (Fake Address) ===" << std::endl;

    ASSERT_TRUE(_hasStringStorage) << "String storage should be initialized";

    Symbolication symbolication;
    ASSERT_TRUE(symbolication.Initialize()) << "Initialization should succeed";

    // Use the same fake address that's failing in the ProfileExporter test
    uint64_t fakeAddress = 0x1234567890ABCDEF;

    std::cout << "Testing symbolication of fake address: 0x" << std::hex << std::uppercase << fakeAddress << std::endl;

    auto symbolInfoOpt = symbolication.SymbolicateAndIntern(fakeAddress, _stringStorage);

    ASSERT_TRUE(symbolInfoOpt.has_value()) << "Should return a symbol (even if unknown) for fake address";

    CachedSymbolInfo symbolInfo = symbolInfoOpt.value();
    LogSymbolInfo("Fake Address Symbolication", fakeAddress, symbolInfo);

    EXPECT_TRUE(symbolInfo.isValid) << "Unknown symbol should still be marked as valid";
    EXPECT_EQ(symbolInfo.Address, fakeAddress) << "Address should match input";

    // Check that we got unknown function and filename
    std::string functionName = GetFunctionName(symbolInfo);
    EXPECT_EQ(functionName, "<unknown>") << "Function name should be '<unknown>'";

    // Check that FileNameId is set (not default 0)
    EXPECT_NE(symbolInfo.FileNameId.value, 0) << "FileNameId should be set for unknown symbols";
    EXPECT_NE(symbolInfo.FunctionNameId.value, 0) << "FunctionNameId should be set for unknown symbols";

    std::cout << "[OK] Unknown address correctly returned unknown symbol with valid string IDs" << std::endl;
}

TEST_F(SymbolicationTest, TestUninitializedSymbolication) {
    std::cout << "=== Testing Symbolication Without Initialization ===" << std::endl;

    ASSERT_TRUE(_hasStringStorage) << "String storage should be initialized";

    Symbolication symbolication;

    uint64_t testAddress = reinterpret_cast<uint64_t>(&GlobalTestFunction);
    auto symbolInfoOpt = symbolication.SymbolicateAndIntern(testAddress, _stringStorage);
    EXPECT_FALSE(symbolInfoOpt.has_value()) << "Should return no value when not initialized";
    if (!symbolInfoOpt.has_value()) {
        std::cout << "[OK] Correctly returned no value when not initialized" << std::endl;
        return;
    }
}

TEST_F(SymbolicationTest, TestStringStorageCaching) {
    std::cout << "=== Testing String Storage Caching ===" << std::endl;

    ASSERT_TRUE(_hasStringStorage) << "String storage should be initialized";

    Symbolication symbolication;
    ASSERT_TRUE(symbolication.Initialize()) << "Initialization should succeed";

    uint64_t testAddress = reinterpret_cast<uint64_t>(&GlobalTestFunction);

    // Symbolicate the same address twice
    auto symbolInfoOpt1 = symbolication.SymbolicateAndIntern(testAddress, _stringStorage);
    auto symbolInfoOpt2 = symbolication.SymbolicateAndIntern(testAddress, _stringStorage);

    ASSERT_TRUE(symbolInfoOpt1.has_value()) << "First symbolication should return a value";
    ASSERT_TRUE(symbolInfoOpt2.has_value()) << "Second symbolication should return a value";

    CachedSymbolInfo symbolInfo1 = symbolInfoOpt1.value();
    CachedSymbolInfo symbolInfo2 = symbolInfoOpt2.value();

    LogSymbolInfo("First Symbolication", testAddress, symbolInfo1);
    LogSymbolInfo("Second Symbolication", testAddress, symbolInfo2);

    if (symbolInfo1.isValid && symbolInfo2.isValid) {
        // Both should have the same string IDs if caching is working
        EXPECT_EQ(symbolInfo1.FunctionNameId.value, symbolInfo2.FunctionNameId.value)
            << "Function name IDs should be the same for repeated symbolication";
        EXPECT_EQ(symbolInfo1.FileNameId.value, symbolInfo2.FileNameId.value)
            << "File name IDs should be the same for repeated symbolication";

        std::cout << "[OK] String IDs are consistent across multiple symbolications" << std::endl;
    }
}