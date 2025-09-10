// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#ifdef DD_WIN_PROF_EXPORTS
#define DD_WIN_PROF_API __declspec(dllexport)
#else
#define DD_WIN_PROF_API __declspec(dllimport)
#endif

extern "C" {
    // Start profiling manually (returns false if already started or explicitly disabled)
    DD_WIN_PROF_API bool StartProfiler();
    
    // Stop profiling manually (safe to call even if not started)
    DD_WIN_PROF_API void StopProfiler();
    
    // Environment Variables (independent controls):
    // DD_PROFILING_ENABLED: Controls whether profiler CAN be started
    //   - false/0: Blocks all profiling (security override)
    //   - true/1 or not set: Allows profiling
    // 
    // DD_PROFILING_AUTO_START: Controls whether profiler auto-starts when DLL loads
    //   - true/1: Auto-start when DLL is injected/loaded
    //   - false/0 or not set: Manual control only
    //
    // Zero-code profiling: Use ProfilerInjector.exe to inject the DLL with auto-start
}