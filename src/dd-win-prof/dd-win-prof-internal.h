// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "Configuration.h"
#include "dd-win-prof.h"

// Configures the profiler without keeping settings from previous calls.
// When pSettings->noEnvVars is true, resets all fields to defaults
// before overriding with pSettings values.
// HTTP mode requires url and apiKey when noEnvVars is set; callback mode does not.
bool InitializeConfiguration(
    Configuration* pConfig,
    const ProfilerConfig* pSettings,
    ProfileDeliveryMode mode = ProfileDeliveryMode::Http
);
