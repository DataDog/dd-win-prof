// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "pch.h"
#include "CollectorBase.h"
#include "SampleValueTypeProvider.h"

class CpuTimeProvider : public CollectorBase
{
public:
    CpuTimeProvider(SampleValueTypeProvider& valueTypeProvider);

    inline void Add(Sample&& sample, std::chrono::nanoseconds cpuDuration)
    {
        auto offsets = GetValueOffsets();
        sample.AddValue(cpuDuration.count(), offsets[0]);
        sample.AddValue(1, offsets[1]);
        CollectorBase::Add(std::move(sample));
    }

    static std::vector<SampleValueType> SampleTypeDefinitions;
};

