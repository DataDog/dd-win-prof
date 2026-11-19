// Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <ddc/profiling.h>

#include <cstdint>
#include <span>
#include <vector>

#include "SampleValueType.h"
namespace dd_win_prof {
// Native failure seam used by lifecycle tests.
struct ProfileEncodingApi {
  decltype(&ddc_profile_serialize) serialize = ddc_profile_serialize;
  decltype(&ddc_profile_reset) reset = ddc_profile_reset;
  decltype(&ddc_profile_clear_strings) clearStrings = ddc_profile_clear_strings;
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
  bool ClearStrings();
  bool AddSample(
      std::span<const ddc_profile_id> locations,
      std::span<const int64_t> values,
      int64_t timestamp,
      ddc_profile_id labels
  );
  ddc_profile* GetProfile() { return m_profile; }

 private:
  ddc_profile* m_profile = nullptr;
  ProfileEncodingApi m_api;
};
}  // namespace dd_win_prof
