// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

// Runner.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <string>
#include <Windows.h>

#include "..\InprocProfiling\InprocProfiling.h"
#include "Helpers.h"
#include "Spinner.h"

// scenarios
//  1 : simple C functions
//  2 : C++ calls
//  3 : create threads
void ShowHelp(const char* programName);
bool ParseCommandLine(int argc, char* argv[], int& scenario, int& iterations);

void SimpleCall2()
{
    Spin(100);
}

void SimpleCall1()
{
    Spin(200);
    SimpleCall2();
}

void SimpleCalls()
{
    for (int count = 0; count < 3; count++)
    {
        Spin(300);
        SimpleCall1();
    }
}

void ClassCalls()
{
    Spinner spinner;
    spinner.Run(300);
}

const int THREAD_COUNT = 4;
DWORD WINAPI ThreadFunction1(LPVOID lpParam);
DWORD WINAPI ThreadFunction2(LPVOID lpParam);
DWORD WINAPI ThreadFunction3(LPVOID lpParam);
DWORD WINAPI ThreadFunction4(LPVOID lpParam);

void RunThreads()
{
    // Create threads, each calling Spin(), and wait for all to finish
    HANDLE threads[THREAD_COUNT];

    for (int i = 0; i < THREAD_COUNT; ++i)
    {
        LPVOID threadParameter = new int(i + 1);
        switch (i)
        {
            case 0:
                threads[i] = ::CreateThread(nullptr, 0, ThreadFunction1, threadParameter, 0, nullptr);
                ::SetThreadDescription(threads[i], L"Worker 1");
            break;
            case 1:
                threads[i] = ::CreateThread(nullptr, 0,ThreadFunction2, threadParameter, 0, nullptr);
                ::SetThreadDescription(threads[i], L"Worker 2");
            break;
            case 2:
                threads[i] = ::CreateThread(nullptr, 0, ThreadFunction3, threadParameter, 0, nullptr);
                ::SetThreadDescription(threads[i], L"Worker 3");
            break;
            case 3:
                threads[i] = ::CreateThread(nullptr, 0, ThreadFunction4, threadParameter, 0, nullptr);
                ::SetThreadDescription(threads[i], L"Worker 4");
            break;
        }
    }

    // Wait for all threads to finish
    WaitForMultipleObjects(THREAD_COUNT, threads, TRUE, INFINITE);

    // Don't forget to close thread handles
    for (int i = 0; i < THREAD_COUNT; i++)
    {
        ::CloseHandle(threads[i]);
    }
}

DWORD DoWork(LPVOID lpParam)
{
    int count = *(int*)lpParam;
    std::cout << "Thread " << count << " started." << std::endl;
    Spin(count * 200);
    delete lpParam; // Don't forget to clean up allocated memory
    return 0;
}
DWORD WINAPI ThreadFunction1(LPVOID lpParam)
{
    Spin(100);
    return DoWork(lpParam);
}
DWORD WINAPI ThreadFunction2(LPVOID lpParam)
{
    Spin(100);
    return DoWork(lpParam);
}
DWORD WINAPI ThreadFunction3(LPVOID lpParam)
{
    Spin(100);
    return DoWork(lpParam);
}
DWORD WINAPI ThreadFunction4(LPVOID lpParam)
{
    Spin(100);
    return DoWork(lpParam);
}


int main(int argc, char* argv[])
{
    int scenario = 0;
    int iterations = 1;
    if (!ParseCommandLine(argc, argv, scenario, iterations))
    {
        return -1; // Exit if command line parsing fails or help was requested
    }

    std::cout << "\nStarting Datadog Windows Profiler Test\n";
    std::cout << "======================================\n";
    std::cout << "Scenario: " << scenario << " (" <<
        (scenario == 1 ? "Simple C function calls" :
         scenario == 2 ? "C++ class method calls" :
         scenario == 3 ? "Multi-threaded execution" : "Unknown") << ")\n";
    std::cout << "Iterations: " << iterations << "\n\n";

    if (!StartProfiler())
    {
        std::cout << "Failed to start profiling..." << std::endl;
        return -1;
    }

    for (size_t i = 0; i < iterations; i++)
    {
        switch (scenario)
        {
            case 1:
                SimpleCalls();
            break;

            case 2:
                ClassCalls();
            break;

            case 3:
                RunThreads();
            break;

            default:
            break;
        }
    }

    StopProfiler();

    std::cout << "\nProfiling completed successfully!\n";
    std::cout << "Check the output for profile data and any debug files.\n";

    return 0;
}

void ShowHelp(const char* programName)
{
    std::cout << "\nDatadog Windows Profiler Test Runner\n";
    std::cout << "====================================\n\n";
    std::cout << "Usage: " << programName << " --scenario <scenario_number> --iterations <iteration_count>\n\n";
    std::cout << "Required Arguments:\n";
    std::cout << "  --scenario <1-3>     Scenario to run:\n";
    std::cout << "                       1 = Simple C function calls\n";
    std::cout << "                       2 = C++ class method calls\n";
    std::cout << "                       3 = Multi-threaded execution\n";
    std::cout << "  --iterations <num>   Number of times to repeat the scenario\n\n";
    std::cout << "Optional Arguments:\n";
    std::cout << "  --help, -h          Show this help message\n\n";
    std::cout << "Examples:\n";
    std::cout << "  " << programName << " --scenario 1 --iterations 5\n";
    std::cout << "  " << programName << " --scenario 3 --iterations 1\n\n";
    std::cout << "Environment Variables (for debug output):\n";
    std::cout << "  DD_INTERNAL_PROFILING_OUTPUT_DIR  Directory to write debug pprof files\n\n";
}

bool ParseCommandLine(int argc, char* argv[], int& scenario, int& iterations)
{
    // Check for help request first
    for (int i = 1; i < argc; ++i)
    {
        if (_stricmp(argv[i], "--help") == 0 || _stricmp(argv[i], "-h") == 0)
        {
            ShowHelp(argv[0]);
            return false;
        }
    }

    // Check minimum argument count
    if (argc < 5) // program name + 4 arguments (--scenario X --iterations Y)
    {
        std::cout << "Error: Missing required arguments.\n";
        ShowHelp(argv[0]);
        return false;
    }

    bool scenarioFound = false;
    bool iterationsFound = false;

    for (int i = 1; i < argc; ++i)
    {
        if (_stricmp(argv[i], "--scenario") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cout << "Error: Missing value for --scenario argument.\n";
                ShowHelp(argv[0]);
                return false;
            }

            try
            {
                scenario = std::stoi(argv[++i]);
                if (scenario < 1 || scenario > 3)
                {
                    std::cout << "Error: Scenario " << scenario << " is invalid. Must be between 1 and 3.\n";
                    ShowHelp(argv[0]);
                    return false;
                }
                scenarioFound = true;
            }
            catch (const std::exception&)
            {
                std::cout << "Error: Invalid scenario value '" << argv[i] << "'. Must be a number between 1 and 3.\n";
                ShowHelp(argv[0]);
                return false;
            }
        }
        else if (_stricmp(argv[i], "--iterations") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cout << "Error: Missing value for --iterations argument.\n";
                ShowHelp(argv[0]);
                return false;
            }

            try
            {
                iterations = std::stoi(argv[++i]);
                if (iterations < 1)
                {
                    std::cout << "Error: Iterations must be a positive number, got " << iterations << ".\n";
                    ShowHelp(argv[0]);
                    return false;
                }
                iterationsFound = true;
            }
            catch (const std::exception&)
            {
                std::cout << "Error: Invalid iterations value '" << argv[i] << "'. Must be a positive number.\n";
                ShowHelp(argv[0]);
                return false;
            }
        }
        else if (argv[i][0] == '-')
        {
            std::cout << "Error: Unknown argument '" << argv[i] << "'.\n";
            ShowHelp(argv[0]);
            return false;
        }
    }

    // Verify both required arguments were provided
    if (!scenarioFound)
    {
        std::cout << "Error: Missing required argument --scenario.\n";
        ShowHelp(argv[0]);
        return false;
    }

    if (!iterationsFound)
    {
        std::cout << "Error: Missing required argument --iterations.\n";
        ShowHelp(argv[0]);
        return false;
    }

    return true;
}
