// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#define DLL_VERSION_MAJOR 0
#define DLL_VERSION_MINOR 1
#define DLL_VERSION_PATCH 0
#define DLL_VERSION_BUILD 0


// Build DLL_VERSION_STRING from the other constants
// Two-level stringification to ensure macros are expanded before stringifying
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define PROFILER_VERSION_STRING "v" TOSTRING(DLL_VERSION_MAJOR) "." TOSTRING(DLL_VERSION_MINOR) "." TOSTRING(DLL_VERSION_PATCH) "w"
#define DLL_VERSION_STRING TOSTRING(DLL_VERSION_MAJOR) "." TOSTRING(DLL_VERSION_MINOR) "." TOSTRING(DLL_VERSION_PATCH) "." TOSTRING(DLL_VERSION_BUILD)
#define DLL_NAME "dd-win-prof"
#define DLL_FILENAME DLL_NAME ".dll"
