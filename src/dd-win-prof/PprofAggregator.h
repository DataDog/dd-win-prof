// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

// todo: leaking this include is not great. pimpl could be used here.
#include "datadog/profiling.h"
#include "datadog/common.h"

#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <cstdint>
#include <span>

#include "SampleValueType.h"


namespace dd_win_prof {

    /// <summary>
    /// Manages pprof profile creation and aggregation using libdatadog.
    /// This class provides a C++ wrapper around the libdatadog profiling API.
    /// </summary>
    class PprofAggregator {
    public:
        /// <summary>
        /// Constructs a new PprofAggregator with the specified sample value types and string storage.
        /// </summary>
        /// <param name="sampleValueTypes">Span of sample value types to include in the profile</param>
        /// <param name="stringStorage">String storage to use for the profile</param>
        /// <param name="periodMs">Optional period in milliseconds (defaults to 60000)</param>
        explicit PprofAggregator(std::span<const SampleValueType> sampleValueTypes,
                                ddog_prof_ManagedStringStorage stringStorage,
                                int periodMs = 60000);

        /// <summary>
        /// Destructor - ensures proper cleanup of libdatadog resources
        /// </summary>
        ~PprofAggregator();

        // Disable copy constructor and assignment operator
        PprofAggregator(const PprofAggregator&) = delete;
        PprofAggregator& operator=(const PprofAggregator&) = delete;

        /// <summary>
        /// Checks if the aggregator was successfully initialized
        /// </summary>
        /// <returns>True if initialized successfully, false otherwise</returns>
        bool IsInitialized() const;

        /// <summary>
        /// Gets the last error message if initialization failed
        /// </summary>
        /// <returns>Error message string, empty if no error</returns>
        const std::string& GetLastError() const;

        /// <summary>
        /// Serializes the current profile to pprof format.
        /// This operation resets the profile for future use.
        /// Note: Caller takes ownership of the returned pointer and must call ddog_prof_EncodedProfile_drop() when done.
        /// </summary>
        /// <param name="startTimestampMs">Optional start timestamp in milliseconds (uses current time if not specified)</param>
        /// <param name="endTimestampMs">Optional end timestamp in milliseconds (uses current time if not specified)</param>
        /// <returns>Encoded profile data, or nullptr on failure</returns>
        ddog_prof_EncodedProfile* Serialize(int64_t startTimestampMs, int64_t endTimestampMs) const;

        /// <summary>
        /// Resets the profile, clearing all accumulated data while preserving configuration
        /// </summary>
        /// <returns>True if reset was successful, false otherwise</returns>
        void Reset();

        /// <summary>
        /// Adds a sample to the profile using interned location IDs and labels
        /// </summary>
        /// <param name="locationIds">Vector of location IDs representing the call stack</param>
        /// <param name="values">Vector of sample values corresponding to the configured sample types</param>
        /// <param name="timestamp">Timestamp for the sample in nanoseconds since epoch</param>
        /// <param name="labelsetId">Interned labelset ID for the sample</param>
        /// <returns>True if sample was added successfully, false otherwise</returns>
        bool AddSample(std::span<const ddog_prof_LocationId> locations, std::span<const int64_t> values, int64_t timestamp, ddog_prof_LabelSetId labelsetId);

        /// <summary>
        /// Gets access to the internal profile for advanced operations like string interning
        /// </summary>
        /// <returns>Pointer to the internal profile, or nullptr if not initialized</returns>
        ddog_prof_Profile* GetProfile() { return m_initialized ? &m_profile : nullptr; }

        // TODO: Future methods for adding samples will be added here
        // - AddSample(const StackTrace& stackTrace, const std::vector<int64_t>& values, ...)
        // - AddEndpoint(uint64_t localRootSpanId, const std::string& endpoint)
        // - AddEndpointCount(const std::string& endpoint, int64_t value)

    private:
        /// <summary>
        /// Converts SampleValueType to libdatadog value types
        /// </summary>
        /// <param name="sampleValueTypes">Input sample value types</param>
        /// <returns>Vector of libdatadog value types</returns>
        std::vector<ddog_prof_ValueType> ConvertSampleValueTypes(std::span<const SampleValueType> sampleValueTypes);

        /// <summary>
        /// Cleans up libdatadog resources
        /// </summary>
        void Cleanup();

    private:
        ddog_prof_Profile m_profile;          // libdatadog profile object
        std::string m_lastError;               // Last error message
        bool m_initialized;                    // Initialization status
        int m_periodMs;                        // Configured period in milliseconds
        std::vector<ddog_prof_ValueType> m_valueTypes; // Storage for value types during profile lifetime
    };

} // namespace dd_win_prof