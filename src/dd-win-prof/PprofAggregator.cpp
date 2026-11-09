// Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0
#include "PprofAggregator.h"

#include <limits>

#include "Log.h"
namespace dd_win_prof {
PprofAggregator::PprofAggregator(
    std::span<const SampleValueType> types, int periodMs, ProfileEncodingApi api
)
    : m_api(api) {
  std::vector<pa_value_type> config;
  for (const auto& type : types)
    config.push_back({type.Name.c_str(), type.Unit.c_str()});
  if (pa_create(
          config.data(),
          config.size(),
          static_cast<int64_t>(periodMs) * 1000000,
          nullptr,
          &m_profile
      ) != PA_OK)
    Log::Error("C profile initialization failed");
}
PprofAggregator::~PprofAggregator() { pa_destroy(m_profile); }
bool PprofAggregator::Serialize(
    int64_t start, int64_t end, std::vector<uint8_t>& bytes
) {
  bytes.clear();
  constexpr auto maxMs = std::numeric_limits<int64_t>::max() / 1000000;
  constexpr auto minMs = std::numeric_limits<int64_t>::min() / 1000000;
  if (start > maxMs || start < minMs || end > maxMs || end < minMs) return false;
  auto sink = [](void* ctx, const uint8_t* data, size_t n) -> pa_status {
    try {
      auto& out = *static_cast<std::vector<uint8_t>*>(ctx);
      out.insert(out.end(), data, data + n);
      return PA_OK;
    } catch (...) {
      return PA_OUTPUT;
    }
  };
  auto status =
      m_api.serialize(m_profile, start * 1000000, end * 1000000, sink, &bytes);
  if (status != PA_OK) {
    bytes.clear();
    Log::Error("C profile serialization failed: ", status);
    return false;
  }
  return true;
}
bool PprofAggregator::Reset() { return m_api.reset(m_profile) == PA_OK; }
bool PprofAggregator::AddSample(
    std::span<const pa_id> locations,
    std::span<const int64_t> values,
    int64_t timestamp,
    pa_id labels
) {
  pa_id stack = 0;
  return pa_intern_stack(m_profile, locations.data(), locations.size(), &stack) ==
             PA_OK &&
         pa_record(
             m_profile,
             stack,
             labels,
             values.data(),
             values.size(),
             timestamp != 0,
             timestamp
         ) == PA_OK;
}
}  // namespace dd_win_prof
