// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct RumViewContext {
    std::string view_id;
    std::string view_name;
};

struct RumViewRecord {
    int64_t timestamp_ms{0};  // milliseconds since Unix epoch (view start)
    int64_t duration_ms{0};   // view duration in milliseconds
    std::string view_id;
    std::string view_name;
};

class IRumViewContextProvider {
public:
    virtual ~IRumViewContextProvider() = default;

    // Returns true and fills 'context' with a copy of the current view if active.
    // Returns false if no view is currently active.
    virtual bool GetCurrentViewContext(RumViewContext& context) const = 0;
};

class IRumViewRecordProvider {
public:
    virtual ~IRumViewRecordProvider() = default;

    // Swaps the internal completed-view buffer into 'records'.
    // After the call, 'records' holds the accumulated entries and the internal buffer is empty.
    virtual void ConsumeViewRecords(std::vector<RumViewRecord>& records) = 0;
};
