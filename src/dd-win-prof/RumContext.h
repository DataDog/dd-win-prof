// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct RumViewContext {
  std::string view_id;
  std::string view_name;
};

enum class ViewVitalKind : uint8_t {
  CpuTime = 0,
  WaitTime = 1,
  Unknown  // sentinel – must remain last
};

inline constexpr size_t MaxViewVitalKind = static_cast<size_t>(ViewVitalKind::Unknown);

struct RumViewRecord {
  int64_t timestamp_ms{0};  // milliseconds since Unix epoch (view start)
  int64_t duration_ms{0};   // view duration in milliseconds
  std::string view_id;
  std::string view_name;
  std::array<int64_t, MaxViewVitalKind>
      vitals_ns{};  // indexed by ViewVitalKind (nanoseconds)
};

struct RumSessionRecord {
  int64_t timestamp_ms{0};  // milliseconds since Unix epoch (session start)
  int64_t duration_ms{0};   // session duration in milliseconds (0 if still ongoing)
  std::string session_id;
};

class IRumViewContextProvider {
 public:
  virtual ~IRumViewContextProvider() = default;

  // Returns true and fills 'context' with a copy of the current view if active.
  // Returns false if no view is currently active.
  virtual bool GetCurrentViewContext(RumViewContext& context) const = 0;
};

class IViewVitalsAccumulator {
 public:
  virtual ~IViewVitalsAccumulator() = default;

  // Called from the sampler hot path (lock-free).
  // Adds 'valueNs' nanoseconds to the running total for 'kind' on the
  // currently active view. Returns false if 'kind' is out of range.
  virtual bool AccumulateViewVitals(ViewVitalKind kind, int64_t valueNs) = 0;
};

class IRumRecordProvider {
 public:
  virtual ~IRumRecordProvider() = default;

  // Swaps the internal completed-view buffer into 'records'.
  // After the call, 'records' holds the accumulated entries and the internal buffer is
  // empty.
  virtual void ConsumeViewRecords(std::vector<RumViewRecord>& records) = 0;

  // Swaps the internal completed-session buffer into 'records'.
  virtual void ConsumeSessionRecords(std::vector<RumSessionRecord>& records) = 0;

  // Returns the currently active session_id (may be empty if no session set yet).
  virtual std::string GetCurrentSessionId() const = 0;
};
