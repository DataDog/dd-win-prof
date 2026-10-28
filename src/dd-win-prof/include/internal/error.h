// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <optional>
#include <string>

enum class ProfilingSeverity {
  Success = 0,
  Info = 1,
  Warning = 2,
  Error = 3,
  Fatal = 4,
};

enum class ProfilingErrorCode {
  None = 0,
  SerializationFailed = 1,
  ProfileBytesUnavailable = 2,
  ProfileResetFailed = 3,
  SampleLabelsUnavailable = 4,
  ProfilerUnavailable = 5,
  ProfilerAlreadyRunning = 6,
  CallbackCopyFailed = 7,
  UnsupportedConfigSize = 9,
  ExplicitlyDisabled = 10,
  InvalidConfiguration = 11,
  InitializationFailed = 12,
  ExportFailed = 13,
  UnexpectedException = 15,
  CallbackFailed = 16,
};

struct ProfilingError {
  ProfilingSeverity severity;
  ProfilingErrorCode code;
  std::string message;
  // Private adapters can avoid reporting a final-export error twice.
  bool reportedToCallback = false;
};

// No error means success.
using ProfilingResult = std::optional<ProfilingError>;
