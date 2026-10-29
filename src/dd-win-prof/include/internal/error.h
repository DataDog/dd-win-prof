// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <cstdint>

#include "dd-win-prof.h"

enum class ProfilingErrorCode : uint32_t {
  None = 0,
  SerializationFailed = 1,
  ProfileBytesUnavailable = 2,
  ProfileResetFailed = 3,
  SampleLabelsUnavailable = 4,
};

// Returns static text for a callback result; unknown codes have a fallback message.
// The caller must not free the returned string. No error state is stored.
DD_WIN_PROF_API const char* GetProfilingErrorMessage(uint32_t result) noexcept;
