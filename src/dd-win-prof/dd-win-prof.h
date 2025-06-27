// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#ifdef DD_WIN_PROF_EXPORTS
#define DD_WIN_PROF_API __declspec(dllexport)
#else
#define DD_WIN_PROF_API __declspec(dllimport)
#endif

extern "C" {
    DD_WIN_PROF_API bool StartProfiler();
    DD_WIN_PROF_API void StopProfiler();
}