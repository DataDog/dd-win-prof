// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <stdint.h>

#include "dd-win-prof.h"

typedef uint32_t ProfilingErrorCode;

#define PROFILING_ERROR_NONE ((ProfilingErrorCode)0)
#define PROFILING_ERROR_SERIALIZATION_FAILED ((ProfilingErrorCode)1)
#define PROFILING_ERROR_PROFILE_BYTES_UNAVAILABLE ((ProfilingErrorCode)2)
#define PROFILING_ERROR_PROFILE_RESET_FAILED ((ProfilingErrorCode)3)
#define PROFILING_ERROR_SAMPLE_LABELS_UNAVAILABLE ((ProfilingErrorCode)4)

// Always returns non-null static text, valid until DLL unload.
#ifdef __cplusplus
extern "C" {
#endif

DD_WIN_PROF_API const char* GetProfilingErrorMessage(uint32_t result)
#ifdef __cplusplus
    noexcept
#endif
    ;

#ifdef __cplusplus
}
#endif
