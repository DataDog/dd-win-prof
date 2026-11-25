// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <cstdint>

enum class ProfilingErrorCode : uint32_t {
  None = 0,
  SerializationFailed = 1,
  ProfileBytesUnavailable = 2,
  ProfileResetFailed = 3,
  SampleLabelsUnavailable = 4,
  // Final export aborted by an unexpected exception before the callback ran. Keeps
  // the exactly-one is_last callback per session that SDK consumers rely on.
  InternalError = 5,
};
