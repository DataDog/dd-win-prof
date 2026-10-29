// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "Configuration.h"
#include "dd-win-prof.h"

// Rebuilds configuration from defaults and, unless noEnvVars is set, the current
// environment, then applies ProfilerConfig fields. Previous API overrides are
// discarded. Returns false if mandatory fields are missing (url, apiKey when
// noEnvVars).
bool InitializeConfiguration(Configuration* pConfig, const ProfilerConfig* pSettings);
