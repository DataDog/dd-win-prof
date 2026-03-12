// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <string>

struct RumViewContext {
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
