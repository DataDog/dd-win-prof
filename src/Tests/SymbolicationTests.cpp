// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "../dd-win-prof/Symbolication.h"
#include "pch.h"

// Include libdatadog headers for string storage
#include <algorithm>
#include <iomanip>
#include <sstream>

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
  void LogSymbolInfo(
      const std::string& description, uint64_t address, const CachedSymbolInfo& info
  ) {
    std::cout << description << " @ " << address << ": " << info.FunctionName << " "
              << info.FileName << std::endl;
  }
  bool ContainsExpectedFunctionName(
      const CachedSymbolInfo& info, const std::string& expected
  ) {
    std::string actual(info.FunctionName), lower = expected;
    std::transform(actual.begin(), actual.end(), actual.begin(), ::tolower);
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return info.isValid && actual.find(lower) != std::string::npos;
  }
  std::string GetFunctionName(const CachedSymbolInfo& info) {
    return std::string(info.FunctionName);
  }
};

TEST_F(SymbolicationTest, TestInitialization) {
  std::cout << "=== Testing Symbolication Initialization ===" << std::endl;

  Symbolication symbolication;

  // Test initial state
  EXPECT_FALSE(symbolication.IsInitialized()) << "Should not be initialized initially";
  std::cout << "[OK] Initial state: Not initialized" << std::endl;

  // Test initialization
  std::cout << "Attempting to initialize symbolication..." << std::endl;
  bool initResult = symbolication.Initialize(true);
  EXPECT_TRUE(initResult) << "Initialization should succeed";
  EXPECT_TRUE(symbolication.IsInitialized())
      << "Should be initialized after Initialize()";
  std::cout << "[OK] Initialization: SUCCESS" << std::endl;

  // Test cleanup
  std::cout << "Testing cleanup..." << std::endl;
  symbolication.Cleanup();
  EXPECT_FALSE(symbolication.IsInitialized())
      << "Should not be initialized after Cleanup()";
  std::cout << "[OK] Cleanup: SUCCESS" << std::endl;
}

TEST_F(SymbolicationTest, TestBasicSymbolication) {
  std::cout << "=== Testing Basic Address Symbolication ===" << std::endl;

  Symbolication symbolication;
  ASSERT_TRUE(symbolication.Initialize(true)) << "Initialization should succeed";

  // Get address of the global test function
  uint64_t testAddress = reinterpret_cast<uint64_t>(&GlobalTestFunction);

  std::cout << "Testing with GlobalTestFunction at address: 0x" << std::hex
            << std::uppercase << testAddress << std::endl;

  // Symbolicate the address using the new API
  auto symbolInfoOpt = symbolication.SymbolicateAndIntern(testAddress);
  ASSERT_TRUE(symbolInfoOpt.has_value()) << "Symbolication should return a value";
  CachedSymbolInfo symbolInfo = symbolInfoOpt.value();

  LogSymbolInfo("GlobalTestFunction Symbolication", testAddress, symbolInfo);

  EXPECT_TRUE(symbolInfo.isValid) << "Symbolication should succeed for valid address";
  if (!symbolInfo.BuildId.empty())
    EXPECT_EQ(symbolInfo.BuildId.find('-'), std::string::npos);

  // Verify we got the correct function name (allowing for name mangling)
  if (symbolInfo.isValid) {
    bool foundCorrectName =
        ContainsExpectedFunctionName(symbolInfo, "GlobalTestFunction");
    if (foundCorrectName) {
      std::cout << "[OK] Function name contains expected 'GlobalTestFunction'"
                << std::endl;
    } else {
      std::string actualName = GetFunctionName(symbolInfo);
      std::cout << "[WARN] Function name '" << actualName
                << "' does not contain 'GlobalTestFunction'" << std::endl;
    }
    EXPECT_TRUE(foundCorrectName)
        << "Function name should contain 'GlobalTestFunction'";
  }
}

TEST_F(SymbolicationTest, TestMultipleAddressTypes) {
  std::cout << "=== Testing Multiple Address Types ===" << std::endl;

  Symbolication symbolication;
  ASSERT_TRUE(symbolication.Initialize(true)) << "Initialization should succeed";

  // Test different types of functions - all global functions to avoid class method
  // confusion
  struct TestCase {
    const char* name;
    uint64_t address;
    const char* expectedSubstring;
  };

  TestCase testCases[] = {
      {"Global Function",
       reinterpret_cast<uint64_t>(&GlobalTestFunction),
       "GlobalTestFunction"},
      {"Function with Parameters",
       reinterpret_cast<uint64_t>(&GlobalTestFunctionWithParams),
       "GlobalTestFunctionWithParams"},
      {"Static Global Function",
       reinterpret_cast<uint64_t>(&StaticGlobalFunction),
       "StaticGlobalFunction"}
  };

  int successCount = 0;
  int correctNameCount = 0;
  int totalCases = sizeof(testCases) / sizeof(testCases[0]);

  for (int i = 0; i < totalCases; i++) {
    const TestCase& testCase = testCases[i];
    auto symbolInfoOpt = symbolication.SymbolicateAndIntern(testCase.address);
    if (!symbolInfoOpt.has_value()) {
      std::cout << "[ERROR] Symbolication returned no value for: " << testCase.name
                << std::endl;
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
        std::cout << "[OK] Function name contains expected '"
                  << testCase.expectedSubstring << "'" << std::endl;
      } else {
        std::string actualName = GetFunctionName(symbolInfo);
        std::cout << "[WARN] Function name '" << actualName << "' does not contain '"
                  << testCase.expectedSubstring << "'" << std::endl;
      }
    } else {
      std::cout << "[WARN] Failed to symbolicate: " << testCase.name << std::endl;
    }
  }

  std::cout << "Summary: " << successCount << "/" << totalCases
            << " addresses symbolicated successfully" << std::endl;
  std::cout << "Correct names: " << correctNameCount << "/" << successCount
            << " symbolicated functions had correct names" << std::endl;

  // At least some should succeed
  EXPECT_GT(
      successCount, 0
  ) << "At least some addresses should be symbolicated successfully";
  // At least some should have correct names
  EXPECT_GT(correctNameCount, 0)
      << "At least some symbolicated functions should have correct names";
}

TEST_F(SymbolicationTest, TestInvalidAddress) {
  std::cout << "=== Testing Invalid Address Symbolication ===" << std::endl;

  Symbolication symbolication;
  ASSERT_TRUE(symbolication.Initialize(true)) << "Initialization should succeed";

  // Try invalid addresses
  uint64_t invalidAddresses[] = {
      0x1234,      // Very low address
      0xDEADBEEF,  // Classic invalid address
  };

  int totalAddresses = sizeof(invalidAddresses) / sizeof(invalidAddresses[0]);

  for (int i = 0; i < totalAddresses; i++) {
    uint64_t invalidAddress = invalidAddresses[i];
    auto symbolInfoOpt = symbolication.SymbolicateAndIntern(invalidAddress);
    if (!symbolInfoOpt.has_value()) {
      std::cout << "[OK] Invalid address correctly returned no value" << std::endl;
      continue;
    }
    CachedSymbolInfo symbolInfo = symbolInfoOpt.value();
    LogSymbolInfo("Invalid Address Test", invalidAddress, symbolInfo);

    EXPECT_TRUE(symbolInfo.isValid)
        << "Should always return valid symbol, even for unknown addresses";

    if (symbolInfo.isValid) {
      std::string actualName = GetFunctionName(symbolInfo);
      if (actualName == "") {
        std::cout << "[OK] Invalid address correctly returned empty function name: "
                  << actualName << std::endl;
      } else {
        std::cout << "[WARN] Unexpectedly got real function name for invalid address 0x"
                  << std::hex << invalidAddress << " (function: " << actualName << ")"
                  << std::endl;
      }
    }
  }
}

TEST_F(SymbolicationTest, TestUnknownAddressSymbolication) {
  std::cout << "=== Testing Unknown Address Symbolication (Fake Address) ==="
            << std::endl;

  Symbolication symbolication;
  ASSERT_TRUE(symbolication.Initialize(true)) << "Initialization should succeed";

  // Use the same fake address that's failing in the ProfileExporter test
  uint64_t fakeAddress = 0x1234567890ABCDEF;

  std::cout << "Testing symbolication of fake address: 0x" << std::hex << std::uppercase
            << fakeAddress << std::endl;

  auto symbolInfoOpt = symbolication.SymbolicateAndIntern(fakeAddress);

  ASSERT_TRUE(symbolInfoOpt.has_value())
      << "Should return a symbol (even if unknown) for fake address";

  CachedSymbolInfo symbolInfo = symbolInfoOpt.value();
  LogSymbolInfo("Fake Address Symbolication", fakeAddress, symbolInfo);

  EXPECT_TRUE(symbolInfo.isValid) << "Unknown symbol should still be marked as valid";

  // Check that we got unknown function and filename
  std::string functionName = GetFunctionName(symbolInfo);
  EXPECT_EQ(functionName, "") << "Function name should be ''";

  std::cout << "[OK] Unknown address correctly returned empty symbol with valid empty "
               "string IDs"
            << std::endl;
}

TEST_F(SymbolicationTest, TestUninitializedSymbolication) {
  std::cout << "=== Testing Symbolication Without Initialization ===" << std::endl;

  Symbolication symbolication;

  uint64_t testAddress = reinterpret_cast<uint64_t>(&GlobalTestFunction);
  auto symbolInfoOpt = symbolication.SymbolicateAndIntern(testAddress);
  EXPECT_FALSE(symbolInfoOpt.has_value())
      << "Should return no value when not initialized";
  if (!symbolInfoOpt.has_value()) {
    std::cout << "[OK] Correctly returned no value when not initialized" << std::endl;
    return;
  }
}

TEST_F(SymbolicationTest, TestObfuscation) {
  std::cout << "=== Testing Obfuscation ===" << std::endl;

  Symbolication symbolication;
  ASSERT_TRUE(symbolication.Initialize(false))
      << "Obfuscated initialization should succeed";

  uint64_t testAddress = reinterpret_cast<uint64_t>(&GlobalTestFunction);
  auto symbolInfoOpt = symbolication.SymbolicateAndIntern(testAddress);
  EXPECT_TRUE(symbolInfoOpt.has_value())
      << "Should return a value for an existing function";
  if (symbolInfoOpt.has_value()) {
    CachedSymbolInfo symbolInfo = symbolInfoOpt.value();
    EXPECT_TRUE(symbolInfo.isValid)
        << "Should always return valid symbol, even for obfuscated functions";

    LogSymbolInfo("Obfuscated function", testAddress, symbolInfo);
    if (symbolInfo.isValid) {
      std::string actualName = GetFunctionName(symbolInfo);
      if (actualName == "") {
        std::cout << "[OK] obfuscate function correctly returned empty function name: "
                  << actualName << std::endl;
      } else {
        std::cout
            << "[WARN] Unexpectedly got real function name for obfuscated function 0x"
            << std::hex << testAddress << " (function: " << actualName << ")"
            << std::endl;
      }
    }

    std::cout << "[OK] Correctly returned empty function name when obfuscated"
              << std::endl;
    return;
  }
}

TEST_F(SymbolicationTest, TestStringStorageCaching) {
  std::cout << "=== Testing String Storage Caching ===" << std::endl;

  Symbolication symbolication;
  ASSERT_TRUE(symbolication.Initialize(true)) << "Initialization should succeed";

  uint64_t testAddress = reinterpret_cast<uint64_t>(&GlobalTestFunction);

  // Symbolicate the same address twice
  auto symbolInfoOpt1 = symbolication.SymbolicateAndIntern(testAddress);
  auto symbolInfoOpt2 = symbolication.SymbolicateAndIntern(testAddress);

  ASSERT_TRUE(symbolInfoOpt1.has_value())
      << "First symbolication should return a value";
  ASSERT_TRUE(symbolInfoOpt2.has_value())
      << "Second symbolication should return a value";

  CachedSymbolInfo symbolInfo1 = symbolInfoOpt1.value();
  CachedSymbolInfo symbolInfo2 = symbolInfoOpt2.value();

  LogSymbolInfo("First Symbolication", testAddress, symbolInfo1);
  LogSymbolInfo("Second Symbolication", testAddress, symbolInfo2);

  if (symbolInfo1.isValid && symbolInfo2.isValid) {
    // Both should have the same string IDs if caching is working
    EXPECT_EQ(symbolInfo1.FunctionName, symbolInfo2.FunctionName)
        << "Function name IDs should be the same for repeated symbolication";
    EXPECT_EQ(symbolInfo1.FileName, symbolInfo2.FileName)
        << "File name IDs should be the same for repeated symbolication";

    std::cout << "[OK] String IDs are consistent across multiple symbolications"
              << std::endl;
  }
}