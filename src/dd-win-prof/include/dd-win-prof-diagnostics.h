// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <stdint.h>

#ifndef DD_WIN_PROF_API
#ifdef DD_WIN_PROF_STATIC_LINK
#define DD_WIN_PROF_API
#elif defined(DD_WIN_PROF_EXPORTS)
#define DD_WIN_PROF_API __declspec(dllexport)
#else
#define DD_WIN_PROF_API __declspec(dllimport)
#endif
#endif

#define DD_WIN_PROF_SUCCESS 0
#define DD_WIN_PROF_INFO 1
#define DD_WIN_PROF_WARNING 2
#define DD_WIN_PROF_ERROR 3
#define DD_WIN_PROF_FATAL 4

#define DD_WIN_PROF_CODE_NONE 0u
#define DD_WIN_PROF_CODE_SERIALIZATION_FAILED 1u
#define DD_WIN_PROF_CODE_PROFILE_BYTES_UNAVAILABLE 2u
#define DD_WIN_PROF_CODE_PROFILE_RESET_FAILED 3u
#define DD_WIN_PROF_CODE_SAMPLE_LABELS_UNAVAILABLE 4u
#define DD_WIN_PROF_CODE_PROFILER_UNAVAILABLE 5u
#define DD_WIN_PROF_CODE_ALREADY_RUNNING 6u
#define DD_WIN_PROF_CODE_CALLBACK_COPY_FAILED 7u
#define DD_WIN_PROF_CODE_INVALID_ARGUMENT 8u
#define DD_WIN_PROF_CODE_UNSUPPORTED_CONFIG_SIZE 9u
#define DD_WIN_PROF_CODE_EXPLICITLY_DISABLED 10u
#define DD_WIN_PROF_CODE_INVALID_CONFIGURATION 11u
#define DD_WIN_PROF_CODE_INITIALIZATION_FAILED 12u
#define DD_WIN_PROF_CODE_FINAL_EXPORT_FAILED 13u
#define DD_WIN_PROF_CODE_STOP_FAILED 14u
#define DD_WIN_PROF_CODE_UNEXPECTED_EXCEPTION 15u
#define DD_WIN_PROF_CODE_CALLBACK_FAILED 16u

#ifdef __cplusplus
extern "C" {
#endif

struct _ProfilerConfig;

// Additive lifecycle API. Return values are DD_WIN_PROF_SUCCESS or a severity above.
// Serialize with all lifecycle/registration calls; do not call from callbacks.
// These calls do not log synchronous diagnostics. The caller owns reporting.
DD_WIN_PROF_API int32_t __cdecl SetupProfilerV1(const struct _ProfilerConfig* settings);
DD_WIN_PROF_API int32_t __cdecl StartProfilerV1(void);
// Stops and waits for callbacks, not downstream SDK uploads. Already stopped succeeds.
DD_WIN_PROF_API int32_t __cdecl StopProfilerV1(void);

// Copies the calling thread's most recent lifecycle diagnostic. Success clears it;
// background errors and getter calls do not change it. All outputs are optional.
// Returns required UTF-8 message bytes including NUL, or zero for no diagnostic.
// A null message requires zero capacity. Positive capacity always receives a NUL;
// truncation preserves UTF-8 boundaries. Storage is bounded to 1023 message bytes.
// Querying size does not repeat the operation. No pointers are retained or returned.
DD_WIN_PROF_API uint32_t __cdecl GetLastProfilerDiagnosticV1(
    int32_t* severity, uint32_t* code, char* message, uint32_t message_capacity_bytes
);

#ifdef __cplusplus
}
#endif
