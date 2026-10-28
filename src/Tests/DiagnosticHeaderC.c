// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "dd-win-prof-diagnostics.h"

int32_t(__cdecl* diagnostic_c_start)(void) = StartProfilerV1;
uint32_t(__cdecl* diagnostic_c_details)(int32_t*, uint32_t*, char*, uint32_t) =
    GetLastProfilerDiagnosticV1;
