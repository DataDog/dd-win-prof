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
