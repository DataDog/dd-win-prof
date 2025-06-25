#include "pch.h"
#include <gtest/gtest.h>
#include <iostream>
#include <cstdlib>
#include <memory>
#include <vector>
#include <string>

int main(int argc, char** argv) {
    // Use _dupenv_s for Windows secure getenv
    char* outputFile = nullptr;
    size_t len = 0;
    _dupenv_s(&outputFile, &len, "GTEST_OUTPUT_FILE");

    std::vector<const char*> new_argv;
    for (int i = 0; i < argc; ++i) {
        new_argv.push_back(argv[i]);
    }
    std::string outputArg;
    if (outputFile) {
        outputArg = "--gtest_output=xml:";
        outputArg += outputFile;
        new_argv.push_back(outputArg.c_str());
        free(outputFile); // Don't forget to free the memory!
    }

    int new_argc = static_cast<int>(new_argv.size());
    ::testing::InitGoogleTest(&new_argc, const_cast<char**>(new_argv.data()));
    
    std::cout << "==================================================" << std::endl;
    std::cout << "  Symbolication Test Suite - Google Test Runner  " << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << "Running All Test Categories:" << std::endl;
    std::cout << "- Symbolication Tests" << std::endl;
    std::cout << "- Dynamic Module Management Tests" << std::endl;
    std::cout << "- Pprof Aggregator Tests" << std::endl;
    std::cout << "=====================================" << std::endl;
    
    // Run all tests
    int result = RUN_ALL_TESTS();
    
    std::cout << "\n==================================================" << std::endl;
    std::cout << "Test suite completed with result: " << (result == 0 ? "SUCCESS" : "FAILURE") << std::endl;
    std::cout << "Press any key to continue..." << std::endl;
    std::cin.get();
    
    return result;
} 