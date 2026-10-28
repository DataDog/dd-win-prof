// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "dd-win-prof.h"

_Static_assert(sizeof(ProfilerConfig) == 96, "C configuration layout");
_Static_assert(DD_WIN_PROF_SUCCESS == 0, "Zero means success");

int32_t(__cdecl* diagnostic_c_setup)(const ProfilerConfig*) = SetupProfiler;
int32_t(__cdecl* diagnostic_c_start)(void) = StartProfiler;
int32_t(__cdecl* diagnostic_c_stop)(void) = StopProfiler;
uint32_t(__cdecl* diagnostic_c_details)(int32_t*, uint32_t*, char*, uint32_t) =
    GetLastProfilerDiagnostic;
