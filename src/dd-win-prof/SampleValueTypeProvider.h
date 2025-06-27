// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "pch.h"
#include "Sample.h"
#include <span>

class SampleValueTypeProvider
{
public:
    using Offset = std::uintptr_t; // Use std::uintptr_t to make it work with with libdatadog which is expected int32 or int64 indices type

    SampleValueTypeProvider();

    std::vector<Offset> GetOrRegister(std::span<const SampleValueType> valueTypes);
    std::vector<SampleValueType> const& GetValueTypes();

private:
    std::int8_t GetOffset(SampleValueType const& valueType);

    std::vector<SampleValueType> _sampleTypeDefinitions;
};
