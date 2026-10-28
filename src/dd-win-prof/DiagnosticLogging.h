// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

namespace dd_win_prof {
inline thread_local bool diagnosticLoggingEnabled = true;

class ScopedDiagnosticLogging {
 public:
  explicit ScopedDiagnosticLogging(bool enabled) : _previous(diagnosticLoggingEnabled) {
    diagnosticLoggingEnabled = enabled;
  }
  ~ScopedDiagnosticLogging() { diagnosticLoggingEnabled = _previous; }
  ScopedDiagnosticLogging(const ScopedDiagnosticLogging&) = delete;
  ScopedDiagnosticLogging& operator=(const ScopedDiagnosticLogging&) = delete;

 private:
  bool _previous;
};
}  // namespace dd_win_prof
