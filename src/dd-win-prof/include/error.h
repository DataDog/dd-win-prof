// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

// These values are shared by the C API and the private C++ error type.
typedef enum DDWinProfSeverity {
  DD_WIN_PROF_SUCCESS = 0,
  DD_WIN_PROF_INFO = 1,
  DD_WIN_PROF_WARNING = 2,
  DD_WIN_PROF_ERROR = 3,
  DD_WIN_PROF_FATAL = 4
} DDWinProfSeverity;

typedef enum DDWinProfErrorCode {
  DD_WIN_PROF_CODE_NONE = 0,
  DD_WIN_PROF_CODE_SERIALIZATION_FAILED = 1,
  DD_WIN_PROF_CODE_PROFILE_BYTES_UNAVAILABLE = 2,
  DD_WIN_PROF_CODE_PROFILE_RESET_FAILED = 3,
  DD_WIN_PROF_CODE_SAMPLE_LABELS_UNAVAILABLE = 4,
  DD_WIN_PROF_CODE_PROFILER_UNAVAILABLE = 5,
  DD_WIN_PROF_CODE_ALREADY_RUNNING = 6,
  DD_WIN_PROF_CODE_CALLBACK_COPY_FAILED = 7,
  DD_WIN_PROF_CODE_INVALID_ARGUMENT = 8,
  DD_WIN_PROF_CODE_UNSUPPORTED_CONFIG_SIZE = 9,
  DD_WIN_PROF_CODE_EXPLICITLY_DISABLED = 10,
  DD_WIN_PROF_CODE_INVALID_CONFIGURATION = 11,
  DD_WIN_PROF_CODE_INITIALIZATION_FAILED = 12,
  DD_WIN_PROF_CODE_EXPORT_FAILED = 13,
  DD_WIN_PROF_CODE_UNEXPECTED_EXCEPTION = 15,
  DD_WIN_PROF_CODE_CALLBACK_FAILED = 16
} DDWinProfErrorCode;
