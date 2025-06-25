#include "pch.h"
#include "PprofAggregator.h"
#include "LibDatadogHelper.h"
#include <datadog/profiling.h>
#include <datadog/common.h>
#include <cstring>
#include <cassert>
#include <iostream>

#include "Log.h"

namespace InprocProfiling {

    PprofAggregator::PprofAggregator(std::span<const SampleValueType> sampleValueTypes,
                                    ddog_prof_ManagedStringStorage stringStorage,
                                    int periodMs)
        : m_initialized(false), m_periodMs(periodMs)
    {
        // Initialize with a default/empty profile state
        memset(&m_profile, 0, sizeof(m_profile)); // probably not needed

        // Convert sample value types and store them in member variable
        m_valueTypes = ConvertSampleValueTypes(sampleValueTypes);

        if (m_valueTypes.empty())
        {
            m_lastError = "No valid sample value types provided";
            return;
        }

        // Create slice for sample types
        ddog_prof_Slice_ValueType sample_types_slice = {
            m_valueTypes.data(),
            m_valueTypes.size()
        };

        // Create period - use the first sample type for period type, or default to cpu-time
        ddog_prof_ValueType periodType = m_valueTypes.empty() ?
            CreateValueType("cpu-time", "nanoseconds") : m_valueTypes[0];

        ddog_prof_Period period;
        period.type_ = periodType;
        period.value = static_cast<int64_t>(periodMs) * 1000000;  // Convert ms to ns

        // Create the profile with shared string storage
        auto result = ddog_prof_Profile_with_string_storage(sample_types_slice, &period, stringStorage);

        if (result.tag == DDOG_PROF_PROFILE_NEW_RESULT_OK)
        {
            m_profile = result.ok;
            m_initialized = true;
        }
        else
        {
            // Extract error message
            m_lastError = "Failed to create profile with shared string storage: ";
            if (result.err.message.ptr && result.err.message.len > 0)
            {
                m_lastError += std::string(reinterpret_cast<const char*>(result.err.message.ptr), result.err.message.len);
            }
        }
    }

    PprofAggregator::~PprofAggregator()
    {
        Cleanup();
    }

    bool PprofAggregator::IsInitialized() const
    {
        return m_initialized && m_profile.inner != nullptr;
    }

    const std::string& PprofAggregator::GetLastError() const
    {
        return m_lastError;
    }

    ddog_prof_EncodedProfile* PprofAggregator::Serialize(int64_t startTimestampMs, int64_t endTimestampMs) const
    {
        if (!IsInitialized())
        {
            return nullptr;
        }

        // Create timestamps - convert from milliseconds to seconds and nanoseconds
        ddog_Timespec start_ts;
        start_ts.seconds = startTimestampMs / 1000;
        start_ts.nanoseconds = (uint32_t)((startTimestampMs % 1000) * 1000000);

        ddog_Timespec end_ts;
        end_ts.seconds = endTimestampMs / 1000;
        end_ts.nanoseconds = (uint32_t)((endTimestampMs % 1000) * 1000000);

        // Serialize the profile
        auto result = ddog_prof_Profile_serialize(const_cast<ddog_prof_Profile*>(&m_profile), &start_ts, &end_ts);

        if (result.tag == DDOG_PROF_PROFILE_SERIALIZE_RESULT_OK)
        {
            // Return a copy of the encoded profile since caller takes ownership
            auto* encoded = new ddog_prof_EncodedProfile;
            *encoded = result.ok;
            return encoded;
        }
        else
        {
            // Error - return nullptr
            return nullptr;
        }
    }

    void PprofAggregator::Reset()
    {
        if (!m_initialized) {
            return;
        }

        auto result = ddog_prof_Profile_reset(&m_profile);
        if (result.tag != DDOG_PROF_PROFILE_RESULT_OK) {
            // Handle error - for now just log
            Log::Error("Failed to reset profile");
        }
    }

    bool PprofAggregator::AddSample(std::span<const ddog_prof_LocationId> locations, std::span<const int64_t> values, int64_t timestamp, ddog_prof_LabelSetId labelsetId)
    {
        if (!m_initialized) {
            m_lastError = "PprofAggregator not initialized";
            Log::Error("AddSample failed: ", m_lastError);
            return false;
        }

        if (values.size() != m_valueTypes.size()) {
            m_lastError = "Values count doesn't match configured sample types";
            Log::Error("AddSample failed: ", m_lastError, " (values: ", values.size(), ", types: ", m_valueTypes.size(), ")");
            return false;
        }

        ddog_prof_Slice_LocationId locations_slice = {
            .ptr = locations.data(),
            .len = locations.size()
        };

        auto stacktrace_result = ddog_prof_Profile_intern_stacktrace(&m_profile, locations_slice);
        if (stacktrace_result.tag != DDOG_PROF_STACK_TRACE_ID_RESULT_OK_GENERATIONAL_ID_STACK_TRACE_ID) {
            m_lastError = "Failed to intern stacktrace";
            Log::Error("AddSample failed: ", m_lastError, " (tag: ", stacktrace_result.tag, ")");
            return false;
        }

        ddog_Slice_I64 values_slice = {
            .ptr = values.data(),
            .len = values.size()
        };

        auto sample_result = ddog_prof_Profile_intern_sample(&m_profile,
                                                            stacktrace_result.ok,
                                                            values_slice,
                                                            labelsetId,
                                                            timestamp);
        if (sample_result.tag != DDOG_VOID_RESULT_OK) {
            m_lastError = "Failed to intern sample";
            Log::Error("AddSample failed: ", m_lastError, " (tag: ", sample_result.tag, ")");
            return false;
        }

        return true;
    }

    std::vector<ddog_prof_ValueType> PprofAggregator::ConvertSampleValueTypes(std::span<const SampleValueType> sampleValueTypes)
    {
        std::vector<ddog_prof_ValueType> result;
        result.reserve(sampleValueTypes.size());

        for (const auto& sampleValueType : sampleValueTypes)
        {
            ddog_prof_ValueType valueType = CreateValueType(sampleValueType.Name, sampleValueType.Unit);
            result.push_back(valueType);
        }

        return result;
    }

    void PprofAggregator::Cleanup()
    {
        if (m_initialized && m_profile.inner != nullptr)
        {
            ddog_prof_Profile_drop(&m_profile);
            m_initialized = false;
        }
    }

} // namespace InprocProfiling