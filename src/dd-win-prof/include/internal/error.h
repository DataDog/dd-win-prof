// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <stdint.h>

#include "dd-win-prof.h"

// Zero means success. Nonzero values are opaque implementation-defined errors.
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
