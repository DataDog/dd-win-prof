// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"
#include "../InprocProfiling/Symbolication.h"

// Include libdatadog headers for string storage
#include "datadog/profiling.h"
#include "datadog/common.h"

// Test fixture for dynamic module management
class DynamicModuleTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize managed string storage for tests
        auto storageResult = ddog_prof_ManagedStringStorage_new();
        if (storageResult.tag == DDOG_PROF_MANAGED_STRING_STORAGE_NEW_RESULT_OK) {
            _stringStorage = storageResult.ok;
            _hasStringStorage = true;
        } else {
            _hasStringStorage = false;
        }

        // Initialize symbolication for each test
        ASSERT_TRUE(symbolication.Initialize()) << "Symbolication should initialize successfully";
        ASSERT_TRUE(_hasStringStorage) << "String storage should be initialized";
    }

    void TearDown() override {
        // Cleanup after each test
        symbolication.Cleanup();

        if (_hasStringStorage) {
            ddog_prof_ManagedStringStorage_drop(_stringStorage);
            _hasStringStorage = false;
        }
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

    Symbolication symbolication;
    ddog_prof_ManagedStringStorage _stringStorage;
    bool _hasStringStorage;
};

TEST_F(DynamicModuleTest, TestRefreshModules) {
    std::cout << "=== Testing RefreshModules ===" << std::endl;

    // Test that RefreshModules works
    bool refreshResult = symbolication.RefreshModules();
    EXPECT_TRUE(refreshResult) << "RefreshModules should succeed";
    std::cout << "[OK] RefreshModules completed successfully" << std::endl;

    // Test multiple calls to RefreshModules
    for (int i = 0; i < 3; i++) {
        bool result = symbolication.RefreshModules();
        EXPECT_TRUE(result) << "RefreshModules should succeed on call " << (i + 1);
    }

    std::cout << "[OK] Multiple RefreshModules calls completed successfully" << std::endl;
}

TEST_F(DynamicModuleTest, TestActualDynamicLoading) {
    std::cout << "=== Testing Actual Dynamic Loading ===" << std::endl;

    // Choose a system DLL that's likely not loaded yet
    const wchar_t* dllName = L"winmm.dll"; // Windows Multimedia API
    const char* functionName = "PlaySoundW";
    std::string dllDisplayName = "winmm.dll";

    // Check if it's already loaded (it shouldn't be)
    HMODULE existingModule = GetModuleHandle(dllName);
    if (existingModule != nullptr) {
        std::cout << "WARNING: " << dllDisplayName << " is already loaded, trying different DLL..." << std::endl;
        dllName = L"winspool.drv"; // Print spooler - less likely to be loaded
        functionName = "OpenPrinterW";
        dllDisplayName = "winspool.drv";
        existingModule = GetModuleHandle(dllName);
    }

    ASSERT_EQ(existingModule, nullptr) << "Both test DLLs (" << dllDisplayName << " and winmm.dll) are already loaded - cannot test dynamic loading properly. This indicates unusual system state.";

    std::cout << "Testing with DLL: " << dllDisplayName << std::endl;

    // Load the DLL dynamically
    std::cout << "Loading DLL dynamically..." << std::endl;
    HMODULE dynamicModule = LoadLibrary(dllName);
    ASSERT_NE(dynamicModule, nullptr) << "Should be able to load the test DLL";

    // Get the address of a function in the newly loaded DLL
    FARPROC funcAddress = GetProcAddress(dynamicModule, functionName);
    ASSERT_NE(funcAddress, nullptr) << "Should be able to get function address";

    uint64_t testAddress = reinterpret_cast<uint64_t>(funcAddress);
    std::cout << "Function address: 0x" << std::hex << std::uppercase << testAddress << std::endl;

    // Try symbolication before refresh (might not work since module was loaded after init)
    auto beforeRefreshOpt = symbolication.SymbolicateAndIntern(testAddress, _stringStorage);
    CachedSymbolInfo beforeRefresh;
    if (!beforeRefreshOpt.has_value()) {
        std::cout << "Before refresh - No value returned" << std::endl;
        beforeRefresh.isValid = false;
        beforeRefresh.Address = testAddress;
    } else {
        beforeRefresh = beforeRefreshOpt.value();
        std::cout << "Before refresh - Valid: " << (beforeRefresh.isValid ? "YES" : "NO") << std::endl;
        if (beforeRefresh.isValid) {
            std::string funcName = GetFunctionName(beforeRefresh);
            std::cout << "  Function: " << funcName << std::endl;
        }
    }

    // Now refresh the module list
    std::cout << "Refreshing module list..." << std::endl;
    bool refreshResult = symbolication.RefreshModules();
    EXPECT_TRUE(refreshResult) << "RefreshModules should succeed";

    // Try symbolication after refresh (should work now)
    auto afterRefreshOpt = symbolication.SymbolicateAndIntern(testAddress, _stringStorage);
    CachedSymbolInfo afterRefresh;
    if (!afterRefreshOpt.has_value()) {
        std::cout << "After refresh - No value returned" << std::endl;
        afterRefresh.isValid = false;
        afterRefresh.Address = testAddress;
    } else {
        afterRefresh = afterRefreshOpt.value();
        std::cout << "After refresh - Valid: " << (afterRefresh.isValid ? "YES" : "NO") << std::endl;
        if (afterRefresh.isValid) {
            std::string funcName = GetFunctionName(afterRefresh);
            std::cout << "  Function: " << funcName << std::endl;
            std::cout << "  Expected: " << functionName << std::endl;

            // Check if the function name matches (might have decorations)
            EXPECT_TRUE(funcName.find(functionName) != std::string::npos ||
                       funcName == functionName)
                       << "Function name should contain or match expected name";
        }
    }

    // The key test: symbolication should work better after refresh
    bool improvementDetected = (!beforeRefresh.isValid && afterRefresh.isValid) ||
                              (beforeRefresh.isValid && afterRefresh.isValid);

    EXPECT_TRUE(improvementDetected) << "RefreshModules should enable or maintain symbolication for dynamically loaded modules";

    // Cleanup
    FreeLibrary(dynamicModule);

    std::cout << "[OK] Dynamic loading test completed" << std::endl;
}

TEST_F(DynamicModuleTest, TestSymbolicationWithRefresh) {
    std::cout << "=== Testing Symbolication With Refresh ===" << std::endl;

    // Get a function address from our test executable
    extern void GlobalTestFunction(); // Declared in SymbolicationTests.cpp
    uint64_t testAddress = reinterpret_cast<uint64_t>(&GlobalTestFunction);

    std::cout << "Testing symbolication of address: 0x" << std::hex << std::uppercase << testAddress << std::endl;

    // Try symbolication before refresh
    auto beforeRefreshOpt2 = symbolication.SymbolicateAndIntern(testAddress, _stringStorage);
    CachedSymbolInfo beforeRefresh;
    if (!beforeRefreshOpt2.has_value()) {
        std::cout << "Before refresh - No value returned" << std::endl;
        beforeRefresh.isValid = false;
        beforeRefresh.Address = testAddress;
    } else {
        beforeRefresh = beforeRefreshOpt2.value();
        std::cout << "Before refresh - Valid: " << (beforeRefresh.isValid ? "YES" : "NO") << std::endl;
        if (beforeRefresh.isValid) {
            std::string funcName = GetFunctionName(beforeRefresh);
            std::cout << "  Function: " << funcName << std::endl;
        }
    }

    // Refresh modules
    bool refreshResult = symbolication.RefreshModules();
    EXPECT_TRUE(refreshResult) << "RefreshModules should succeed";

    // Try symbolication after refresh
    auto afterRefreshOpt2 = symbolication.SymbolicateAndIntern(testAddress, _stringStorage);
    CachedSymbolInfo afterRefresh;
    if (!afterRefreshOpt2.has_value()) {
        std::cout << "After refresh - No value returned" << std::endl;
        afterRefresh.isValid = false;
        afterRefresh.Address = testAddress;
    } else {
        afterRefresh = afterRefreshOpt2.value();
        std::cout << "After refresh - Valid: " << (afterRefresh.isValid ? "YES" : "NO") << std::endl;
        if (afterRefresh.isValid) {
            std::string funcName = GetFunctionName(afterRefresh);
            std::cout << "  Function: " << funcName << std::endl;
        }
    }

    // At least one of these should work (likely both since we auto-enumerate on init)
    EXPECT_TRUE(beforeRefresh.isValid || afterRefresh.isValid) << "Symbolication should work either before or after refresh";

    std::cout << "[OK] Symbolication with refresh test completed" << std::endl;
}

TEST_F(DynamicModuleTest, TestRefreshAfterInitialization) {
    std::cout << "=== Testing Refresh After Initialization ===" << std::endl;

    // Since we auto-enumerate modules during initialization,
    // RefreshModules should still work and maintain the module list

    bool refreshResult = symbolication.RefreshModules();
    EXPECT_TRUE(refreshResult) << "RefreshModules should work after initialization";

    // Test that symbolication still works after refresh
    extern void GlobalTestFunction();
    uint64_t testAddress = reinterpret_cast<uint64_t>(&GlobalTestFunction);

            auto symbolInfoOpt = symbolication.SymbolicateAndIntern(testAddress, _stringStorage);
        ASSERT_TRUE(symbolInfoOpt.has_value()) << "Symbolication should return a value";
        CachedSymbolInfo symbolInfo = symbolInfoOpt.value();
    std::cout << "Symbolication after refresh - Valid: " << (symbolInfo.isValid ? "YES" : "NO") << std::endl;
    if (symbolInfo.isValid) {
        std::string funcName = GetFunctionName(symbolInfo);
        std::cout << "  Function: " << funcName << std::endl;
    }

    // Assert that symbolication works after refresh
    EXPECT_TRUE(symbolInfo.isValid) << "Symbolication should work after RefreshModules for known function address";

    std::cout << "[OK] Refresh after initialization test completed" << std::endl;
}
