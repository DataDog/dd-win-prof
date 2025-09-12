// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <set>
#include <filesystem>
#include <algorithm>
#include <cctype>

class ProfilerInjector {
public:
    static bool InjectIntoProcess(DWORD processId, const std::string& dllPath) {
        HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);
        if (!hProcess) {
            std::cerr << "Failed to open process " << processId << ": " << GetLastError() << std::endl;
            return false;
        }

        // Allocate memory in the target process for the DLL path
        size_t pathSize = dllPath.length() + 1;
        LPVOID remotePath = VirtualAllocEx(hProcess, nullptr, pathSize, MEM_COMMIT, PAGE_READWRITE);
        if (!remotePath) {
            std::cerr << "Failed to allocate memory in target process: " << GetLastError() << std::endl;
            CloseHandle(hProcess);
            return false;
        }

        // Write the DLL path to the target process
        if (!WriteProcessMemory(hProcess, remotePath, dllPath.c_str(), pathSize, nullptr)) {
            std::cerr << "Failed to write DLL path to target process: " << GetLastError() << std::endl;
            VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
            CloseHandle(hProcess);
            return false;
        }

        // Get the address of LoadLibraryA
        HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
        if (!hKernel32) {
            std::cerr << "Failed to get kernel32.dll handle: " << GetLastError() << std::endl;
            VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
            CloseHandle(hProcess);
            return false;
        }
        
        LPVOID loadLibraryAddr = GetProcAddress(hKernel32, "LoadLibraryA");
        if (!loadLibraryAddr) {
            std::cerr << "Failed to get LoadLibraryA address: " << GetLastError() << std::endl;
            VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
            CloseHandle(hProcess);
            return false;
        }

        // Create a remote thread to load the DLL
        HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
            (LPTHREAD_START_ROUTINE)loadLibraryAddr, remotePath, 0, nullptr);
        if (!hThread) {
            std::cerr << "Failed to create remote thread: " << GetLastError() << std::endl;
            VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
            CloseHandle(hProcess);
            return false;
        }

        // Wait for the thread to complete
        WaitForSingleObject(hThread, INFINITE);

        // Clean up
        CloseHandle(hThread);
        VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);

        std::cout << "Successfully injected profiler DLL into process " << processId << std::endl;
        return true;
    }

    static bool LoadEnvFile(const std::string& envFilePath) {
        std::ifstream file(envFilePath);
        if (!file.is_open()) {
            return false;
        }

        std::string line;
        int count = 0;
        while (std::getline(file, line)) {
            // Skip empty lines and comments
            if (line.empty() || line[0] == '#') continue;
            
            // Find the '=' separator
            size_t pos = line.find('=');
            if (pos == std::string::npos) continue;
            
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            
            // Remove quotes if present
            if (value.length() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.length() - 2);
            }
            
            if (SetEnvironmentVariableA(key.c_str(), value.c_str())) {
                // Hide sensitive values like API keys
                if (key.find("API_KEY") != std::string::npos || 
                    key.find("TOKEN") != std::string::npos ||
                    key.find("SECRET") != std::string::npos) {
                    std::cout << "Set " << key << "=***HIDDEN***" << std::endl;
                } else {
                    std::cout << "Set " << key << "=" << value << std::endl;
                }
                count++;
            }
        }
        
        std::cout << "Loaded " << count << " environment variables from " << envFilePath << std::endl;
        return count > 0;
    }

    // hardcoded search paths for .env file
    // convenient as I usually remove the full vulkan directory (and keep the root directory with the .env file)
    static bool FindAndLoadEnvFile(const std::string& executable) {
        std::filesystem::path exePath = std::filesystem::absolute(executable);
        std::vector<std::string> searchPaths = {
            // 1. Same directory as executable (highest priority)
            (exePath.parent_path() / ".env").string(),
            // 2. Current working directory
            (std::filesystem::current_path() / ".env").string(),
            // 3. Parent directory (useful for build directories)
            (exePath.parent_path().parent_path() / ".env").string(),
            // 4. Two levels up (for nested build structures like build/bin/Release/)
            (exePath.parent_path().parent_path().parent_path() / ".env").string()
        };
        
        for (const auto& envPath : searchPaths) {
            if (std::filesystem::exists(envPath)) {
                std::cout << "Found .env file at: " << envPath << std::endl;
                return LoadEnvFile(envPath);
            }
        }
        
        std::cout << "No .env file found. Searched locations:" << std::endl;
        std::set<std::string> uniquePaths(searchPaths.begin(), searchPaths.end());
        for (const auto& path : uniquePaths) {
            std::cout << "  " << path << std::endl;
        }
        std::cout << "You can create a .env file in any of these locations." << std::endl;
        return false;
    }

    static bool LaunchWithProfiler(const std::string& executable, const std::string& arguments = "") {
        // Try to find and load .env file from multiple locations
        FindAndLoadEnvFile(executable);
        
        // Always set auto-start (can be overridden by .env file)
        SetEnvironmentVariableA("DD_PROFILING_AUTO_START", "1");
        
        // Auto-set service name from executable if not already set
        char existingService[256];
        DWORD serviceLen = GetEnvironmentVariableA("DD_SERVICE", existingService, sizeof(existingService));
        if (serviceLen == 0) {  // DD_SERVICE not set
            std::filesystem::path exePath(executable);
            std::string serviceName = exePath.stem().string();  // Get filename without extension
            
            // Clean up service name for better readability
            // Convert to lowercase and replace common separators with hyphens
            std::transform(serviceName.begin(), serviceName.end(), serviceName.begin(), ::tolower);
            std::replace(serviceName.begin(), serviceName.end(), '_', '-');
            std::replace(serviceName.begin(), serviceName.end(), ' ', '-');
            
            // Add vulkan prefix for context
            serviceName = "vulkan-" + serviceName;
            
            SetEnvironmentVariableA("DD_SERVICE", serviceName.c_str());
            std::cout << "Auto-set DD_SERVICE=" << serviceName << " (from executable name)" << std::endl;
        } else {
            std::cout << "Using existing DD_SERVICE from environment" << std::endl;
        }
        
        // Get the DLL path
        std::filesystem::path exePath(executable);
        std::string dllPath = (exePath.parent_path() / "dd-win-prof.dll").string();
        
        // Check if profiler DLL exists
        if (!std::filesystem::exists(dllPath)) {
            std::cerr << "Profiler DLL not found at: " << dllPath << std::endl;
            return false;
        }

        // Create the process in suspended state
        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        
        std::string commandLine = executable;
        if (!arguments.empty()) {
            commandLine += " " + arguments;
        }

        if (!CreateProcessA(
            executable.c_str(),
            const_cast<char*>(commandLine.c_str()),
            nullptr, nullptr, FALSE,
            CREATE_SUSPENDED,  // Start suspended so we can inject before main()
            nullptr, nullptr,
            &si, &pi)) {
            std::cerr << "Failed to create process: " << GetLastError() << std::endl;
            return false;
        }

        std::cout << "Created process " << pi.dwProcessId << " in suspended state" << std::endl;

        // Inject the profiler DLL
        bool injected = InjectIntoProcess(pi.dwProcessId, dllPath);
        
        if (injected) {
            std::cout << "Profiler injected successfully, resuming process..." << std::endl;
            ResumeThread(pi.hThread);
        } else {
            std::cerr << "Failed to inject profiler, terminating process..." << std::endl;
            TerminateProcess(pi.hProcess, 1);
        }

        // Clean up handles
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        return injected;
    }
};

int main(int argc, char* argv[]) {
    std::cout << "Datadog Profiler Injector" << std::endl;
    std::cout << "=========================" << std::endl;

    if (argc < 2) {
        std::cout << "Usage:" << std::endl;
        std::cout << "  " << argv[0] << " <executable> [arguments...]" << std::endl;
        std::cout << "  " << argv[0] << " --inject <process_id>" << std::endl;
        std::cout << std::endl;
        std::cout << "Examples:" << std::endl;
        std::cout << "  " << argv[0] << " triangle.exe" << std::endl;
        std::cout << "  " << argv[0] << " myapp.exe --config=debug" << std::endl;
        std::cout << "  " << argv[0] << " --inject 1234" << std::endl;
        return 1;
    }

    if (std::string(argv[1]) == "--inject") {
        if (argc < 3) {
            std::cerr << "Error: Process ID required for injection" << std::endl;
            return 1;
        }

        DWORD processId = std::stoul(argv[2]);
        std::string dllPath = "dd-win-prof.dll";  // Assume DLL is in current directory
        
        if (ProfilerInjector::InjectIntoProcess(processId, dllPath)) {
            std::cout << "Injection completed successfully" << std::endl;
            return 0;
        } else {
            std::cerr << "Injection failed" << std::endl;
            return 1;
        }
    } else {
        // Launch with profiler
        std::string executable = argv[1];
        std::string arguments;
        
        for (int i = 2; i < argc; i++) {
            if (i > 2) arguments += " ";
            arguments += argv[i];
        }

        if (ProfilerInjector::LaunchWithProfiler(executable, arguments)) {
            std::cout << "Process launched with profiler successfully" << std::endl;
            return 0;
        } else {
            std::cerr << "Failed to launch process with profiler" << std::endl;
            return 1;
        }
    }
}
