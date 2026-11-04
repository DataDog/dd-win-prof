// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "PprofAggregator.h"

#include <datadog/common.h>
#include <datadog/profiling.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <iterator>
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

  if (!ConvertSampleValueTypes(sampleValueTypes)) {
    return;
  }

  if (m_valueTypes.empty()) {
    Log::Error("No valid sample value types provided");
    return;
  }

  // Create slice for sample types
  ddog_prof_Slice_SampleType sample_types_slice = {
      m_valueTypes.data(), m_valueTypes.size()
  };

  // Preserve the first column as the period type.
  ddog_prof_Period period;
  period.sample_type = m_valueTypes[0];
  period.value = static_cast<int64_t>(periodMs) * 1000000;  // Convert ms to ns

  // Create the profile with shared string storage
  auto result =
      ddog_prof_Profile_with_string_storage(sample_types_slice, &period, stringStorage);

  if (result.tag == DDOG_PROF_PROFILE_NEW_RESULT_OK) {
    m_profile = result.ok;
    m_initialized = true;
    for (size_t i = 0; i < m_customTypes.size(); ++i) {
      const auto& type = m_customTypes[i];
      auto configured = ddog_prof_Profile_set_custom_sample_type(
          &m_profile,
          static_cast<ddog_prof_SampleType>(DDOG_PROF_SAMPLE_TYPE_CUSTOM1 + i),
          to_CharSlice(type.Name),
          to_CharSlice(type.Unit)
      );
      if (configured.tag != DDOG_PROF_PROFILE_RESULT_OK) {
        const auto message = ddog_Error_message(&configured.err);
        Log::Error(
            "Failed to configure custom sample type: ",
            std::string_view(message.ptr, message.len)
        );
        ddog_Error_drop(&configured.err);
        Cleanup();
        return;
      }
    }
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

bool PprofAggregator::ConvertSampleValueTypes(
    std::span<const SampleValueType> sampleValueTypes
) {
  struct StableType {
    std::string_view name;
    std::string_view unit;
    ddog_prof_SampleType type;
  };
  static constexpr StableType stableTypes[] = {
      {"cpu-time", "nanoseconds", DDOG_PROF_SAMPLE_TYPE_CPU_TIME},
      {"cpu-samples", "count", DDOG_PROF_SAMPLE_TYPE_CPU_SAMPLES},
      {"wall-time", "nanoseconds", DDOG_PROF_SAMPLE_TYPE_WALL_TIME},
      {"alloc-samples", "count", DDOG_PROF_SAMPLE_TYPE_ALLOC_SAMPLES},
      {"alloc-space", "bytes", DDOG_PROF_SAMPLE_TYPE_ALLOC_SPACE},
  };
  m_valueTypes.reserve(sampleValueTypes.size());

  for (const auto& valueType : sampleValueTypes) {
    auto stable = std::find_if(
        std::begin(stableTypes), std::end(stableTypes), [&](const auto& type) {
          return type.name == valueType.Name && type.unit == valueType.Unit;
        }
    );
    if (stable != std::end(stableTypes)) {
      m_valueTypes.push_back(stable->type);
      continue;
    }

    // wait-time is not a stable libdatadog type. Preserve its exact wire pair.
    auto custom =
        std::find_if(m_customTypes.begin(), m_customTypes.end(), [&](const auto& type) {
          return type.Name == valueType.Name && type.Unit == valueType.Unit;
        });
    const auto index = static_cast<size_t>(custom - m_customTypes.begin());
    if (custom == m_customTypes.end()) {
      if (m_customTypes.size() == 5) {
        Log::Error("libdatadog supports at most five custom sample types");
        return false;
      }
      m_customTypes.push_back(valueType);
    }
    m_valueTypes.push_back(
        static_cast<ddog_prof_SampleType>(DDOG_PROF_SAMPLE_TYPE_CUSTOM1 + index)
    );
  }

  return true;
}

void PprofAggregator::Cleanup() {
  if (m_initialized && m_profile.inner != nullptr) {
    ddog_prof_Profile_drop(&m_profile);
    m_initialized = false;
  }
}

}  // namespace dd_win_prof