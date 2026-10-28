// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "Configuration.h"
#include "dd-win-prof.h"

// Applies ProfilerConfig fields to a Configuration object.
// When pSettings->noEnvVars is true, resets to defaults first so that
// only the explicit struct fields take effect.
// HTTP mode requires url and apiKey when noEnvVars is set; callback mode does not.
ProfilingResult InitializeConfiguration(
    Configuration* pConfig,
    const ProfilerConfig* pSettings,
    ProfileDeliveryMode mode = ProfileDeliveryMode::Http
);
