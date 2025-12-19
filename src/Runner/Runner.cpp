// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

// Runner.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <string>
#include <Windows.h>

#include "..\reference\dd-win-prof.h"
#include "Helpers.h"
#include "Spinner.h"

// scenarios
//  1 : simple C functions
//  2 : C++ calls
//  3 : create CPU-bound threads
//  4 : create threads waiting on different synchronization primitives
void ShowHelp(const char* programName);
bool ParseCommandLine(int argc, char* argv[],
    int& scenario, int& iterations,
    std::string& serviceName, std::string& serviceEnv, std::string& serviceVersion);

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
    // Create threads and wait for all to finish
    HANDLE threads[THREAD_COUNT];

    for (int i = 0; i < THREAD_COUNT; ++i)
    {
        LPVOID threadParameter = new int(i + 1);
        switch (i)
        {
            // first threads are CPU bound
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

// Scenario 4: Waiting threads
// Global synchronization objects for scenario 4
HANDLE g_mutex = nullptr;
HANDLE g_semaphore = nullptr;
CRITICAL_SECTION g_criticalSection;
HANDLE g_masterThreadReady = nullptr;

struct WaitThreadParams
{
    int threadId;
    HANDLE readyEvent;
};

DWORD WINAPI MutexThreadFunction(LPVOID lpParam)
{
    WaitThreadParams* params = (WaitThreadParams*)lpParam;
    std::cout << "Thread " << params->threadId << " (Mutex) started, waiting for mutex..." << std::endl;

    // Signal that we're ready to wait
    ::SetEvent(params->readyEvent);

    // Wait for the mutex (will block until master thread releases it)
    DWORD result = ::WaitForSingleObject(g_mutex, INFINITE);
    if (result == WAIT_OBJECT_0)
    {
        std::cout << "Thread " << params->threadId << " (Mutex) acquired mutex, doing work..." << std::endl;
        Spin(100);
        ::ReleaseMutex(g_mutex);
        std::cout << "Thread " << params->threadId << " (Mutex) released mutex." << std::endl;
    }

    delete params;
    return 0;
}

DWORD WINAPI SemaphoreThreadFunction(LPVOID lpParam)
{
    WaitThreadParams* params = (WaitThreadParams*)lpParam;
    std::cout << "Thread " << params->threadId << " (Semaphore) started, waiting for semaphore..." << std::endl;

    // Signal that we're ready to wait
    ::SetEvent(params->readyEvent);

    // Wait for the semaphore
    DWORD result = ::WaitForSingleObject(g_semaphore, INFINITE);
    if (result == WAIT_OBJECT_0)
    {
        std::cout << "Thread " << params->threadId << " (Semaphore) acquired semaphore, doing work..." << std::endl;
        Spin(100);
        ::ReleaseSemaphore(g_semaphore, 1, nullptr);
        std::cout << "Thread " << params->threadId << " (Semaphore) released semaphore." << std::endl;
    }

    delete params;
    return 0;
}

DWORD WINAPI CriticalSectionThreadFunction(LPVOID lpParam)
{
    WaitThreadParams* params = (WaitThreadParams*)lpParam;
    std::cout << "Thread " << params->threadId << " (CriticalSection) started, waiting for critical section..." << std::endl;

    // Signal that we're ready to wait
    ::SetEvent(params->readyEvent);

    // Small delay to ensure master thread has entered the critical section
    ::Sleep(100);

    // Wait for the critical section
    ::EnterCriticalSection(&g_criticalSection);
    std::cout << "Thread " << params->threadId << " (CriticalSection) entered critical section, doing work..." << std::endl;
    Spin(100);
    ::LeaveCriticalSection(&g_criticalSection);
    std::cout << "Thread " << params->threadId << " (CriticalSection) left critical section." << std::endl;

    delete params;
    return 0;
}

DWORD WINAPI SleepThreadFunction(LPVOID lpParam)
{
    WaitThreadParams* params = (WaitThreadParams*)lpParam;
    std::cout << "Thread " << params->threadId << " (Sleep) started, sleeping..." << std::endl;

    // Signal that we're ready to wait
    ::SetEvent(params->readyEvent);

    // Just sleep for a while
    ::Sleep(2000);
    std::cout << "Thread " << params->threadId << " (Sleep) woke up, doing work..." << std::endl;
    Spin(100);

    delete params;
    return 0;
}

DWORD WINAPI WaitMasterThreadFunction(LPVOID lpParam)
{
    HANDLE* readyEvents = (HANDLE*)lpParam;
    std::cout << "Master thread started, holding synchronization objects..." << std::endl;

    // Wait for all worker threads to be ready
    ::WaitForMultipleObjects(4, readyEvents, TRUE, INFINITE);
    std::cout << "Master thread: All worker threads are ready, holding locks for 3 seconds..." << std::endl;

    // Acquire mutex (blocks MutexThreadFunction)
    ::WaitForSingleObject(g_mutex, INFINITE);

    // Acquire semaphore (blocks SemaphoreThreadFunction since count is 1)
    ::WaitForSingleObject(g_semaphore, INFINITE);

    // Enter critical section (blocks CriticalSectionThreadFunction)
    ::EnterCriticalSection(&g_criticalSection);

    // Hold all locks for a while to create contention
    ::Sleep(3000);

    std::cout << "Master thread: Releasing synchronization objects..." << std::endl;

    // Release in reverse order
    ::LeaveCriticalSection(&g_criticalSection);
    ::ReleaseSemaphore(g_semaphore, 1, nullptr);
    ::ReleaseMutex(g_mutex);

    std::cout << "Master thread: Released all synchronization objects." << std::endl;

    // Clean up ready events
    for (int i = 0; i < 4; i++)
    {
        ::CloseHandle(readyEvents[i]);
    }
    delete[] readyEvents;

    return 0;
}

void RunWaitingThreads()
{
    const int WAIT_THREAD_COUNT = 5; // 4 worker threads + 1 master thread
    HANDLE threads[WAIT_THREAD_COUNT];

    // Initialize synchronization objects
    g_mutex = ::CreateMutex(nullptr, FALSE, nullptr);
    g_semaphore = ::CreateSemaphore(nullptr, 1, 1, nullptr); // Initial count 1, max count 1
    ::InitializeCriticalSection(&g_criticalSection);

    // Create ready events for each worker thread
    HANDLE* readyEvents = new HANDLE[4];
    for (int i = 0; i < 4; i++)
    {
        readyEvents[i] = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);
    }

    std::cout << "\nCreating waiting threads scenario..." << std::endl;

    // Create worker threads that will wait on different synchronization primitives
    WaitThreadParams* mutexParams = new WaitThreadParams{ 1, readyEvents[0] };
    threads[0] = ::CreateThread(nullptr, 0, MutexThreadFunction, mutexParams, 0, nullptr);
    ::SetThreadDescription(threads[0], L"Mutex Waiter");

    WaitThreadParams* semaphoreParams = new WaitThreadParams{ 2, readyEvents[1] };
    threads[1] = ::CreateThread(nullptr, 0, SemaphoreThreadFunction, semaphoreParams, 0, nullptr);
    ::SetThreadDescription(threads[1], L"Semaphore Waiter");

    WaitThreadParams* csParams = new WaitThreadParams{ 3, readyEvents[2] };
    threads[2] = ::CreateThread(nullptr, 0, CriticalSectionThreadFunction, csParams, 0, nullptr);
    ::SetThreadDescription(threads[2], L"CriticalSection Waiter");

    WaitThreadParams* sleepParams = new WaitThreadParams{ 4, readyEvents[3] };
    threads[3] = ::CreateThread(nullptr, 0, SleepThreadFunction, sleepParams, 0, nullptr);
    ::SetThreadDescription(threads[3], L"Sleep Waiter");

    // Create master thread that will control the synchronization objects
    threads[4] = ::CreateThread(nullptr, 0, WaitMasterThreadFunction, readyEvents, 0, nullptr);
    ::SetThreadDescription(threads[4], L"Wait Master");

    // Wait for all threads to finish
    ::WaitForMultipleObjects(WAIT_THREAD_COUNT, threads, TRUE, INFINITE);

    std::cout << "All waiting threads completed." << std::endl;

    // Cleanup
    for (int i = 0; i < WAIT_THREAD_COUNT; i++)
    {
        ::CloseHandle(threads[i]);
    }

    ::CloseHandle(g_mutex);
    ::CloseHandle(g_semaphore);
    ::DeleteCriticalSection(&g_criticalSection);
}


int main(int argc, char* argv[])
{
    int scenario = 0;
    int iterations = 1;
    std::string serviceName;
    std::string serviceEnv;
    std::string serviceVersion;
    if (!ParseCommandLine(argc, argv, scenario, iterations, serviceName, serviceEnv, serviceVersion))
    {
        return -1; // Exit if command line parsing fails or help was requested
    }

    std::cout << "\nStarting Datadog Windows Profiler Test\n";
    std::cout << "======================================\n";
    std::cout << "Scenario: " << scenario << " (" <<
        (scenario == 1 ? "Simple C function calls" :
         scenario == 2 ? "C++ class method calls" :
         scenario == 3 ? "Multi-threaded execution" :
         scenario == 4 ? "Waiting threads (mutex, semaphore, critical section, sleep)" : "Unknown") << ")\n";
    std::cout << "Iterations: " << iterations << "\n\n";

    ProfilerConfig config;
    ::ZeroMemory(&config, sizeof(ProfilerConfig));
    config.size = sizeof(ProfilerConfig);
    config.serviceEnvironment = serviceEnv.empty() ? nullptr : serviceEnv.c_str();
    config.serviceName = serviceName.empty() ? nullptr : serviceName.c_str();
    config.serviceVersion = serviceVersion.empty() ? nullptr : serviceVersion.c_str();

    SetupProfiler(&config);

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

            case 4:
                RunWaitingThreads();
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
    std::cout << "  --scenario <1-4>     Scenario to run:\n";
    std::cout << "                       1 = Simple C function calls\n";
    std::cout << "                       2 = C++ class method calls\n";
    std::cout << "                       3 = Multi-threaded execution (CPU-bound)\n";
    std::cout << "                       4 = Waiting threads (mutex, semaphore, critical section, sleep)\n";
    std::cout << "  --iterations <num>   Number of times to repeat the scenario\n\n";
    std::cout << "  --name <service>     Name of the service to profile\n\n";
    std::cout << "  --version <version>  Version of the service to profile\n\n";
    std::cout << "  --env <environment>  Environment of the service to profile\n\n";
    std::cout << "Optional Arguments:\n";
    std::cout << "  --help, -h          Show this help message\n\n";
    std::cout << "Examples:\n";
    std::cout << "  " << programName << " --scenario 1 --iterations 5\n";
    std::cout << "  " << programName << " --scenario 3 --iterations 1\n";
    std::cout << "  " << programName << " --scenario 2 --iterations 10 --name testApp --version 42 --env local\n";
    std::cout << "  " << programName << " --scenario 4 --iterations 2 --name waitTest --env dev\n\n";
    std::cout << "Environment Variables (for debug output):\n";
    std::cout << "  DD_INTERNAL_PROFILING_OUTPUT_DIR  Directory to write debug pprof files\n\n";
}

bool ParseCommandLine(
    int argc, char* argv[],
    int& scenario, int& iterations,
    std::string& serviceName, std::string& serviceEnv, std::string& serviceVersion)
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
                if (scenario < 1 || scenario > 4)
                {
                    std::cout << "Error: Scenario " << scenario << " is invalid. Must be between 1 and 4.\n";
                    ShowHelp(argv[0]);
                    return false;
                }
                scenarioFound = true;
            }
            catch (const std::exception&)
            {
                std::cout << "Error: Invalid scenario value '" << argv[i] << "'. Must be a number between 1 and 4.\n";
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
        else if (_stricmp(argv[i], "--name") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cout << "Error: Missing value for --name argument.\n";
                ShowHelp(argv[0]);
                return false;
            }

            serviceName = argv[++i];
        }
        else if (_stricmp(argv[i], "--version") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cout << "Error: Missing value for --version argument.\n";
                ShowHelp(argv[0]);
                return false;
            }

            serviceVersion = argv[++i];
        }
        else if (_stricmp(argv[i], "--env") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cout << "Error: Missing value for --env argument.\n";
                ShowHelp(argv[0]);
                return false;
            }

            serviceEnv = argv[++i];
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
