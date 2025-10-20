// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "CollectorBase.h"
#include "SampleValueTypeProvider.h"

class WallTimeProvider : public CollectorBase
{
public:
    WallTimeProvider(SampleValueTypeProvider& valueTypeProvider);

    inline void Add(Sample&& sample, std::chrono::nanoseconds walltimeDuration, std::chrono::nanoseconds waitDuration, ULONG waitingReason)
    {
        auto offsets = GetValueOffsets();
        sample.AddValue(walltimeDuration.count(), offsets[0]);
        sample.AddValue(waitDuration.count(), offsets[1]);

        // TODO: copy waiting reason into a label

        CollectorBase::Add(std::move(sample));
    }

    static std::vector<SampleValueType> SampleTypeDefinitions;
};
