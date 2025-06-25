// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"
#include "CpuTimeProvider.h"

std::vector<SampleValueType> CpuTimeProvider::SampleTypeDefinitions(
    {
        {"cpu-time", "nanoseconds"},
        {"cpu-samples", "count"}
    }
);

CpuTimeProvider::CpuTimeProvider(SampleValueTypeProvider& valueTypeProvider)
    : CollectorBase("CpuTimeProvider", valueTypeProvider.GetOrRegister(SampleTypeDefinitions))
{
}
