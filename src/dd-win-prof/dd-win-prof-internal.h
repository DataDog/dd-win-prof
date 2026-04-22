// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "Configuration.h"
#include "dd-win-prof.h"

// Applies ProfilerConfig fields to a Configuration object.
// When pSettings->noEnvVars is true, resets to defaults first so that
// only the explicit struct fields take effect.
// Returns false if mandatory fields are missing (url, apiKey when noEnvVars).
bool InitializeConfiguration(Configuration* pConfig, const ProfilerConfig* pSettings);
