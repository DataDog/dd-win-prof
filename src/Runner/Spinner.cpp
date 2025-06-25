// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "Spinner.h"
#include "Helpers.h"

void Spinner::Run(int durationMS)
{
    Spin(durationMS);
    StaticRun(durationMS);
}

void Spinner::StaticRun(int durationMS)
{
    Spin(durationMS);
}
