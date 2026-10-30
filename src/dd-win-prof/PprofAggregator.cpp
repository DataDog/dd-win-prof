// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "PprofAggregator.h"

#include <datadog/common.h>
#include <datadog/profiling.h>

#include <cassert>
#include <cstring>
#include <iostream>
#include <string_view>

#include "LibDatadogHelper.h"
#include "Log.h"
#include "pch.h"

namespace dd_win_prof {

PprofAggregator::PprofAggregator(
    std::span<const SampleValueType> sampleValueTypes,
    ddog_prof_ManagedStringStorage stringStorage,
    int periodMs,
    ProfileEncodingApi encodingApi
)
    : m_encodingApi(encodingApi), m_initialized(false), m_periodMs(periodMs) {
  // Initialize with a default/empty profile state
  memset(&m_profile, 0, sizeof(m_profile));  // probably not needed

  // Convert sample value types and store them in member variable
  m_valueTypes = ConvertSampleValueTypes(sampleValueTypes);

  if (m_valueTypes.empty()) {
    Log::Error("No valid sample value types provided");
    return;
  }

  // Create slice for sample types
  ddog_prof_Slice_ValueType sample_types_slice = {
      m_valueTypes.data(), m_valueTypes.size()
  };

  // Create period - use the first sample type for period type, or default to cpu-time
  ddog_prof_ValueType periodType = m_valueTypes.empty()
                                       ? CreateValueType("cpu-time", "nanoseconds")
                                       : m_valueTypes[0];

  ddog_prof_Period period;
  period.type_ = periodType;
  period.value = static_cast<int64_t>(periodMs) * 1000000;  // Convert ms to ns

  // Create the profile with shared string storage
  auto result = m_encodingApi.newProfile(sample_types_slice, &period, stringStorage);

  if (result.tag == DDOG_PROF_PROFILE_NEW_RESULT_OK) {
    m_profile = result.ok;
    m_initialized = true;
  } else {
    const auto message = ddog_Error_message(&result.err);
    Log::Error(
        "Failed to create profile with shared string storage: ",
        std::string_view(message.ptr, message.len)
    );
    ddog_Error_drop(&result.err);
  }
}

PprofAggregator::~PprofAggregator() { Cleanup(); }

bool PprofAggregator::IsInitialized() const {
  return m_initialized && m_profile.inner != nullptr;
}

ddog_prof_EncodedProfile* PprofAggregator::Serialize(
    int64_t startTimestampMs, int64_t endTimestampMs
) {
  if (!IsInitialized()) {
    Log::Error("Cannot serialize an uninitialized profile");
    return nullptr;
  }

  auto encoded = std::make_unique<ddog_prof_EncodedProfile>();

  // Create timestamps - convert from milliseconds to seconds and nanoseconds
  ddog_Timespec start_ts;
  start_ts.seconds = startTimestampMs / 1000;
  start_ts.nanoseconds = (uint32_t)((startTimestampMs % 1000) * 1000000);

  ddog_Timespec end_ts;
  end_ts.seconds = endTimestampMs / 1000;
  end_ts.nanoseconds = (uint32_t)((endTimestampMs % 1000) * 1000000);

  // Serialize the profile
  auto result = m_encodingApi.serialize(&m_profile, &start_ts, &end_ts);

  if (result.tag == DDOG_PROF_PROFILE_SERIALIZE_RESULT_OK) {
    *encoded = result.ok;
    return encoded.release();
  }

  const auto message = ddog_Error_message(&result.err);
  Log::Error(
      "libdatadog profile serialization/compression failed: ",
      std::string_view(message.ptr, message.len)
  );
  ddog_Error_drop(&result.err);
  return nullptr;
}

bool PprofAggregator::Reset() {
  if (!m_initialized) {
    Log::Error("Cannot reset an uninitialized profile");
    return false;
  }

  auto result = m_encodingApi.reset(&m_profile);
  if (result.tag == DDOG_PROF_PROFILE_RESULT_OK) {
    return true;
  }

  const auto message = ddog_Error_message(&result.err);
  Log::Error(
      "libdatadog failed to reset profile: ", std::string_view(message.ptr, message.len)
  );
  ddog_Error_drop(&result.err);
  return false;
}

bool PprofAggregator::AddSample(
    std::span<const ddog_prof_LocationId> locations,
    std::span<const int64_t> values,
    int64_t timestamp,
    ddog_prof_LabelSetId labelsetId
) {
  if (!m_initialized) {
    Log::Error("AddSample failed: PprofAggregator not initialized");
    return false;
  }

  if (values.size() != m_valueTypes.size()) {
    Log::Error(
        "AddSample failed: values count doesn't match configured sample types "
        "(values: ",
        values.size(),
        ", types: ",
        m_valueTypes.size(),
        ")"
    );
    return false;
  }

  ddog_prof_Slice_LocationId locations_slice = {
      .ptr = locations.data(), .len = locations.size()
  };

  auto stacktrace_result =
      ddog_prof_Profile_intern_stacktrace(&m_profile, locations_slice);
  if (stacktrace_result.tag !=
      DDOG_PROF_STACK_TRACE_ID_RESULT_OK_GENERATIONAL_ID_STACK_TRACE_ID) {
    Log::Error(
        "AddSample failed: failed to intern stacktrace (tag: ",
        stacktrace_result.tag,
        ")"
    );
    return false;
  }

  ddog_Slice_I64 values_slice = {.ptr = values.data(), .len = values.size()};

  auto sample_result = ddog_prof_Profile_intern_sample(
      &m_profile, stacktrace_result.ok, values_slice, labelsetId, timestamp
  );
  if (sample_result.tag != DDOG_VOID_RESULT_OK) {
    Log::Error(
        "AddSample failed: failed to intern sample (tag: ", sample_result.tag, ")"
    );
    return false;
  }

  return true;
}

std::vector<ddog_prof_ValueType> PprofAggregator::ConvertSampleValueTypes(
    std::span<const SampleValueType> sampleValueTypes
) {
  std::vector<ddog_prof_ValueType> result;
  result.reserve(sampleValueTypes.size());

  for (const auto& sampleValueType : sampleValueTypes) {
    ddog_prof_ValueType valueType =
        CreateValueType(sampleValueType.Name, sampleValueType.Unit);
    result.push_back(valueType);
  }

  return result;
}

void PprofAggregator::Cleanup() {
  if (m_initialized && m_profile.inner != nullptr) {
    ddog_prof_Profile_drop(&m_profile);
    m_initialized = false;
  }
}

}  // namespace dd_win_prof