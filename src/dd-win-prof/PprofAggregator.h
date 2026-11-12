// Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "SampleValueType.h"
#include "profile_aggregation.h"
namespace dd_win_prof {
// Native failure seam used by lifecycle tests. No FFI types enter the adapter.
struct ProfileEncodingApi {
  decltype(&pa_serialize) serialize = pa_serialize;
  decltype(&pa_reset) reset = pa_reset;
};
class PprofAggregator {
 public:
  explicit PprofAggregator(
      std::span<const SampleValueType> types,
      int periodMs = 60000,
      ProfileEncodingApi api = {}
  );
  ~PprofAggregator();
  PprofAggregator(const PprofAggregator&) = delete;
  PprofAggregator& operator=(const PprofAggregator&) = delete;
  bool IsInitialized() const { return m_profile != nullptr; }
  bool Serialize(int64_t startMs, int64_t endMs, std::vector<uint8_t>& bytes);
  bool Reset();
  bool AddSample(
      std::span<const pa_id> locations,
      std::span<const int64_t> values,
      int64_t timestamp,
      pa_id labels
  );
  pa_profile* GetProfile() { return m_profile; }

 private:
  pa_profile* m_profile = nullptr;
  ProfileEncodingApi m_api;
};
}  // namespace dd_win_prof
