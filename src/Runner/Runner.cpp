// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

// Runner.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <random>
#include <string>
#include <Windows.h>

#include "..\dd-win-prof\dd-win-rum-private.h"
#include "Helpers.h"
#include "Spinner.h"

// scenarios
//  1 : simple C functions
//  2 : C++ calls
//  3 : create CPU-bound threads
//  4 : create threads waiting on different synchronization primitives
//  5 : RUM context (view transitions)

struct RunnerOptions
{
    int scenario = 0;
    int iterations = 1;

    std::string serviceName;
    std::string serviceEnv;
    std::string serviceVersion;

    bool noEnvVars = false;
    bool symbolize = false;
    std::string url;
    std::string apiKey;
    std::string tags;
    std::string pprofDir;

    std::string rumApplicationId;
    std::string rumSessionId;
};

void ShowHelp(const char* programName);
bool ParseCommandLine(int argc, char* argv[], RunnerOptions& opts);

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


// Scenario 5: RUM context view transitions

void SetSession(const std::string& appId, const std::string& sessionId)
{
    RumSessionContext ctx = {};
    ctx.application_id = appId.c_str();
    ctx.session_id = sessionId.c_str();
    SetRumSession(&ctx);
}

// Per-view function chains: each view produces a distinct call stack.
// __declspec(noinline) prevents MSVC from collapsing them during optimization.

// --- HomePage ---
__declspec(noinline) void FetchRecommendations() { Spin(500); }
__declspec(noinline) void RenderDashboard() { Spin(500); }
__declspec(noinline) void ProcessNotifications() { Spin(500); }

__declspec(noinline) void LoadHomeContent()
{
    FetchRecommendations();
    RenderDashboard();
}

__declspec(noinline) void HomePageView()
{
    LoadHomeContent();
    ProcessNotifications();
}

// --- SettingsPage ---
__declspec(noinline) void ReadConfiguration() { Spin(600); }
__declspec(noinline) void ValidateSettings() { Spin(600); }
__declspec(noinline) void ApplyTheme() { Spin(600); }

__declspec(noinline) void LoadSettings()
{
    ReadConfiguration();
}

__declspec(noinline) void SettingsPageView()
{
    LoadSettings();
    ValidateSettings();
    ApplyTheme();
}

// --- ProfilePage ---
__declspec(noinline) void LoadAvatar() { Spin(400); }
__declspec(noinline) void DecodeImage() { Spin(400); }
__declspec(noinline) void ComputeStatistics() { Spin(400); }
__declspec(noinline) void RenderTimeline() { Spin(400); }

__declspec(noinline) void FetchUserProfile()
{
    LoadAvatar();
    DecodeImage();
}

__declspec(noinline) void ProfilePageView()
{
    FetchUserProfile();
    ComputeStatistics();
    RenderTimeline();
}

void RunRumScenario(const std::string& appId, const std::string& sessionId)
{
    static const std::string sessionId2 = "99999999-2222-3333-4444-555555555555";

    SetSession(appId, sessionId);

    // Explicit leave between views
    EnterView("HomePage");
    std::cout << "View 1: HomePage, session S1..." << std::endl;
    HomePageView();

    LeaveCurrentView();
    std::cout << "No active view, session S1 (spinning 1s)..." << std::endl;
    Spin(1000);

    // Stacked view: SettingsPage completes HomePage's record automatically
    EnterView("SettingsPage");
    std::cout << "View 2: SettingsPage, session S1..." << std::endl;
    SettingsPageView();

    EnterView("ProfilePage");
    std::cout << "View 3: ProfilePage, session S1 (stacked, completes SettingsPage)..." << std::endl;
    ProfilePageView();

    LeaveCurrentView();

    // Session rotation: switch from S1 to S2
    SetSession(appId, sessionId2);
    EnterView("HomePage");
    std::cout << "View 4: HomePage, session S2..." << std::endl;
    HomePageView();

    LeaveCurrentView();
}


int main(int argc, char* argv[])
{
    RunnerOptions opts;
    if (!ParseCommandLine(argc, argv, opts))
    {
        return -1;
    }

    std::cout << "\nStarting Datadog Windows Profiler Test\n";
    std::cout << "======================================\n";
    std::cout << "Scenario: " << opts.scenario << " (" <<
        (opts.scenario == 1 ? "Simple C function calls" :
         opts.scenario == 2 ? "C++ class method calls" :
         opts.scenario == 3 ? "Multi-threaded execution" :
         opts.scenario == 4 ? "Waiting threads (mutex, semaphore, critical section, sleep)" :
         opts.scenario == 5 ? "RUM context (view transitions)" : "Unknown") << ")\n";
    std::cout << "Iterations: " << opts.iterations << "\n";
    std::cout << "NoEnvVars:  " << (opts.noEnvVars ? "true" : "false") << "\n\n";

    ProfilerConfig config;
    ::ZeroMemory(&config, sizeof(ProfilerConfig));
    config.size = sizeof(ProfilerConfig);

    config.noEnvVars           = opts.noEnvVars;
    config.serviceEnvironment  = opts.serviceEnv.empty()     ? nullptr : opts.serviceEnv.c_str();
    config.serviceName         = opts.serviceName.empty()    ? nullptr : opts.serviceName.c_str();
    config.serviceVersion      = opts.serviceVersion.empty() ? nullptr : opts.serviceVersion.c_str();
    config.url                 = opts.url.empty()            ? nullptr : opts.url.c_str();
    config.apiKey              = opts.apiKey.empty()          ? nullptr : opts.apiKey.c_str();
    config.tags                = opts.tags.empty()            ? nullptr : opts.tags.c_str();
    config.pprofOutputDirectory = opts.pprofDir.empty()       ? nullptr : opts.pprofDir.c_str();
    config.symbolizeCallstacks  = opts.symbolize;

    if (!SetupProfiler(&config))
    {
        std::cout << "SetupProfiler failed. Check that mandatory fields are set (url, apiKey when --noenvvars)." << std::endl;
        return -2;
    }

    if (!StartProfiler())
    {
        std::cout << "Failed to start profiling..." << std::endl;
        return -1;
    }

    for (size_t i = 0; i < static_cast<size_t>(opts.iterations); i++)
    {
        switch (opts.scenario)
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

            case 5:
                RunRumScenario(opts.rumApplicationId, opts.rumSessionId);
            break;

            default:
            break;
        }
    }

    StopProfiler();

    std::cout << "\nProfiling completed successfully!\n";

    // Show where output files can be found
    // Log directory is set at DLL load time (env var or default), not via ProfilerConfig
    std::string logDir;
    {
        char buf[MAX_PATH] = {};
        if (::GetEnvironmentVariableA("DD_TRACE_LOG_DIRECTORY", buf, MAX_PATH) > 0)
            logDir = buf;
    }
    if (logDir.empty())
    {
        char buf[MAX_PATH] = {};
        if (::ExpandEnvironmentStringsA("%PROGRAMDATA%\\Datadog Tracer\\logs", buf, MAX_PATH) > 0)
            logDir = buf;
    }

    std::string pprofDir = opts.pprofDir;
    if (pprofDir.empty() && !opts.noEnvVars)
    {
        char buf[MAX_PATH] = {};
        if (::GetEnvironmentVariableA("DD_INTERNAL_PROFILING_OUTPUT_DIR", buf, MAX_PATH) > 0)
            pprofDir = buf;
    }

    std::cout << "Log files       : " << logDir
              << "  (set via DD_TRACE_LOG_DIRECTORY env var)\n";
    if (!pprofDir.empty())
        std::cout << "Pprof files     : " << pprofDir << "\n";
    else
        std::cout << "Pprof files     : (not saved locally -- profiles uploaded to backend)\n";

    return 0;
}

void ShowHelp(const char* programName)
{
    std::cout << "\nDatadog Windows Profiler Test Runner\n";
    std::cout << "====================================\n\n";
    std::cout << "Usage: " << programName << " --scenario <1-5> --iterations <num> [options]\n\n";

    std::cout << "Required Arguments:\n";
    std::cout << "  --scenario <1-5>       Scenario to run:\n";
    std::cout << "                           1 = Simple C function calls\n";
    std::cout << "                           2 = C++ class method calls\n";
    std::cout << "                           3 = Multi-threaded execution (CPU-bound)\n";
    std::cout << "                           4 = Waiting threads (mutex, semaphore, critical section, sleep)\n";
    std::cout << "                           5 = RUM context (view transitions)\n";
    std::cout << "  --iterations <num>     Number of times to repeat the scenario\n\n";

    std::cout << "Service Information:\n";
    std::cout << "  --name <service>       Service name\n";
    std::cout << "  --version <version>    Service version\n";
    std::cout << "  --env <environment>    Service environment\n\n";

    std::cout << "No-Environment-Variables Mode (--noenvvars):\n";
    std::cout << "  --noenvvars            Ignore all environment variables; only use explicit flags\n";
    std::cout << "  --url <url>            Datadog intake URL     (MANDATORY with --noenvvars)\n";
    std::cout << "  --apikey <key>         Datadog API key        (MANDATORY with --noenvvars)\n\n";

    std::cout << "RUM Context Options (scenario 5):\n";
    std::cout << "  --rum-app-id <uuid>    RUM application ID (required for scenario 5)\n";
    std::cout << "  --rum-session-id <uuid> RUM session ID (required for scenario 5)\n\n";

    std::cout << "Additional Options:\n";
    std::cout << "  --symbolize            Enable call stack symbolization (default: obfuscated)\n";
    std::cout << "  --tags <k:v,...>       Comma-separated tags to attach to profiles\n";
    std::cout << "  --pprofdir <path>      Override pprof debug output directory\n";
    std::cout << "  --help, -h             Show this help message\n\n";

    std::cout << "Examples:\n";
    std::cout << "  " << programName << " --scenario 1 --iterations 5\n";
    std::cout << "  " << programName << " --scenario 3 --iterations 1 --name myApp --env staging\n";
    std::cout << "  " << programName << " --scenario 2 --iterations 10 --noenvvars"
                 " --url https://intake.datadoghq.com --apikey DD_API_KEY"
                 " --name testApp --version 42 --env local\n";
    std::cout << "  " << programName << " --scenario 4 --iterations 2 --noenvvars"
                 " --url https://intake.datadoghq.com --apikey DD_API_KEY"
                 " --pprofdir C:\\temp\\pprof\n\n";

    std::cout << "When --noenvvars is NOT used, environment variables are read as usual:\n";
    std::cout << "  DD_AGENT_HOST, DD_API_KEY, DD_ENV, DD_SERVICE, DD_VERSION, etc.\n";
    std::cout << "  DD_TRACE_LOG_DIRECTORY               Log output directory\n";
    std::cout << "  DD_INTERNAL_PROFILING_OUTPUT_DIR      Directory to write debug pprof files\n\n";
}

static bool RequireValue(int i, int argc, char* argv[], const char* flag)
{
    if (i + 1 >= argc)
    {
        std::cout << "Error: Missing value for " << flag << " argument.\n";
        return false;
    }
    return true;
}

bool ParseCommandLine(int argc, char* argv[], RunnerOptions& opts)
{
    for (int i = 1; i < argc; ++i)
    {
        if (_stricmp(argv[i], "--help") == 0 || _stricmp(argv[i], "-h") == 0)
        {
            ShowHelp(argv[0]);
            return false;
        }
    }

    if (argc < 5)
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
            if (!RequireValue(i, argc, argv, "--scenario")) { ShowHelp(argv[0]); return false; }
            try
            {
                opts.scenario = std::stoi(argv[++i]);
                if (opts.scenario < 1 || opts.scenario > 5)
                {
                    std::cout << "Error: Scenario " << opts.scenario << " is invalid. Must be between 1 and 5.\n";
                    ShowHelp(argv[0]);
                    return false;
                }
                scenarioFound = true;
            }
            catch (const std::exception&)
            {
                std::cout << "Error: Invalid scenario value '" << argv[i] << "'. Must be a number between 1 and 5.\n";
                ShowHelp(argv[0]);
                return false;
            }
        }
        else if (_stricmp(argv[i], "--iterations") == 0)
        {
            if (!RequireValue(i, argc, argv, "--iterations")) { ShowHelp(argv[0]); return false; }
            try
            {
                opts.iterations = std::stoi(argv[++i]);
                if (opts.iterations < 1)
                {
                    std::cout << "Error: Iterations must be a positive number, got " << opts.iterations << ".\n";
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
            if (!RequireValue(i, argc, argv, "--name")) { ShowHelp(argv[0]); return false; }
            opts.serviceName = argv[++i];
        }
        else if (_stricmp(argv[i], "--version") == 0)
        {
            if (!RequireValue(i, argc, argv, "--version")) { ShowHelp(argv[0]); return false; }
            opts.serviceVersion = argv[++i];
        }
        else if (_stricmp(argv[i], "--env") == 0)
        {
            if (!RequireValue(i, argc, argv, "--env")) { ShowHelp(argv[0]); return false; }
            opts.serviceEnv = argv[++i];
        }
        else if (_stricmp(argv[i], "--noenvvars") == 0)
        {
            opts.noEnvVars = true;
        }
        else if (_stricmp(argv[i], "--symbolize") == 0)
        {
            opts.symbolize = true;
        }
        else if (_stricmp(argv[i], "--url") == 0)
        {
            if (!RequireValue(i, argc, argv, "--url")) { ShowHelp(argv[0]); return false; }
            opts.url = argv[++i];
        }
        else if (_stricmp(argv[i], "--apikey") == 0)
        {
            if (!RequireValue(i, argc, argv, "--apikey")) { ShowHelp(argv[0]); return false; }
            opts.apiKey = argv[++i];
        }
        else if (_stricmp(argv[i], "--tags") == 0)
        {
            if (!RequireValue(i, argc, argv, "--tags")) { ShowHelp(argv[0]); return false; }
            opts.tags = argv[++i];
        }
        else if (_stricmp(argv[i], "--pprofdir") == 0)
        {
            if (!RequireValue(i, argc, argv, "--pprofdir")) { ShowHelp(argv[0]); return false; }
            opts.pprofDir = argv[++i];
        }
        else if (_stricmp(argv[i], "--rum-app-id") == 0)
        {
            if (!RequireValue(i, argc, argv, "--rum-app-id")) { ShowHelp(argv[0]); return false; }
            opts.rumApplicationId = argv[++i];
        }
        else if (_stricmp(argv[i], "--rum-session-id") == 0)
        {
            if (!RequireValue(i, argc, argv, "--rum-session-id")) { ShowHelp(argv[0]); return false; }
            opts.rumSessionId = argv[++i];
        }
        else if (argv[i][0] == '-')
        {
            std::cout << "Error: Unknown argument '" << argv[i] << "'.\n";
            ShowHelp(argv[0]);
            return false;
        }
    }

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

    if (opts.noEnvVars)
    {
        if (opts.url.empty())
        {
            std::cout << "Error: --url is mandatory when --noenvvars is used.\n";
            return false;
        }
        if (opts.apiKey.empty())
        {
            std::cout << "Error: --apikey is mandatory when --noenvvars is used.\n";
            return false;
        }
    }

    if (opts.scenario == 5)
    {
        if (opts.rumApplicationId.empty())
        {
            std::cout << "Error: --rum-app-id is mandatory for scenario 5.\n";
            return false;
        }
        if (opts.rumSessionId.empty())
        {
            std::cout << "Error: --rum-session-id is mandatory for scenario 5.\n";
            return false;
        }
    }

    return true;
}
