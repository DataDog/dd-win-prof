// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "CollectorBase.h"
#include "SampleValueTypeProvider.h"

class WallTimeProvider : public CollectorBase
{
public:
    WallTimeProvider(SampleValueTypeProvider& valueTypeProvider);

    inline void Add(Sample&& sample, std::chrono::nanoseconds walltimeDuration)
    {
        auto offsets = GetValueOffsets();
        sample.AddValue(walltimeDuration.count(), offsets[0]);
        CollectorBase::Add(std::move(sample));
    }

    static std::vector<SampleValueType> SampleTypeDefinitions;
};
