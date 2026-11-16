// Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0
#include "PprofAggregator.h"

#include <limits>

#include "Log.h"
namespace dd_win_prof {
PprofAggregator::PprofAggregator(
    std::span<const SampleValueType> types, int periodMs, ProfileEncodingApi api
)
    : m_api(api) {
  std::vector<ddc_profile_value_type> config;
  for (const auto& type : types)
    config.push_back(
        {{type.Name.data(), type.Name.size()}, {type.Unit.data(), type.Unit.size()}}
    );
  if (ddc_profile_create(
          config.data(),
          config.size(),
          static_cast<int64_t>(periodMs) * 1000000,
          nullptr,
          &m_profile
      ) != DDC_PROFILE_OK)
    Log::Error("C profile initialization failed");
}
PprofAggregator::~PprofAggregator() { ddc_profile_destroy(m_profile); }
bool PprofAggregator::Serialize(
    int64_t start, int64_t end, std::vector<uint8_t>& bytes
) {
  bytes.clear();
  constexpr auto maxMs = std::numeric_limits<int64_t>::max() / 1000000;
  constexpr auto minMs = std::numeric_limits<int64_t>::min() / 1000000;
  if (start > maxMs || start < minMs || end > maxMs || end < minMs) return false;
  auto sink = [](void* ctx, const uint8_t* data, size_t n) -> ddc_profile_status {
    try {
      auto& out = *static_cast<std::vector<uint8_t>*>(ctx);
      out.insert(out.end(), data, data + n);
      return DDC_PROFILE_OK;
    } catch (...) {
      return DDC_PROFILE_OUTPUT;
    }
  };
  auto status =
      m_api.serialize(m_profile, start * 1000000, end * 1000000, sink, &bytes);
  if (status != DDC_PROFILE_OK) {
    bytes.clear();
    Log::Error("C profile serialization failed: ", status);
    return false;
  }
  return true;
}
bool PprofAggregator::Reset() { return m_api.reset(m_profile) == DDC_PROFILE_OK; }
bool PprofAggregator::AddSample(
    std::span<const ddc_profile_id> locations,
    std::span<const int64_t> values,
    int64_t timestamp,
    ddc_profile_id labels
) {
  ddc_profile_id stack = 0;
  return ddc_profile_intern_stack(
             m_profile, locations.data(), locations.size(), &stack
         ) == DDC_PROFILE_OK &&
         ddc_profile_record(
             m_profile,
             stack,
             labels,
             values.data(),
             values.size(),
             timestamp != 0,
             timestamp
         ) == DDC_PROFILE_OK;
}
}  // namespace dd_win_prof
