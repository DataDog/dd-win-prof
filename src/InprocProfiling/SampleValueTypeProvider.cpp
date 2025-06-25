#include "pch.h"
#include "SampleValueTypeProvider.h"

SampleValueTypeProvider::SampleValueTypeProvider()
{
    _sampleTypeDefinitions.reserve(16);
}

std::vector<SampleValueTypeProvider::Offset> SampleValueTypeProvider::GetOrRegister(std::span<const SampleValueType> valueTypes)
{
    std::vector<Offset> offsets;
    offsets.reserve(valueTypes.size());

    for (auto const& valueType : valueTypes)
    {
        size_t idx = GetOffset(valueType);
        if (idx == -1)
        {
            idx = _sampleTypeDefinitions.size();
            _sampleTypeDefinitions.push_back(valueType);
        }
        offsets.push_back(idx);
    }
    return offsets;
}

std::vector<SampleValueType> const& SampleValueTypeProvider::GetValueTypes()
{
    return _sampleTypeDefinitions;
}

std::int8_t SampleValueTypeProvider::GetOffset(SampleValueType const& valueType)
{
    for (auto i = 0; i < _sampleTypeDefinitions.size(); i++)
    {
        auto const& current = _sampleTypeDefinitions[i];
        if (valueType.Name == current.Name)
        {
            if (valueType.Unit != current.Unit)
            {
                throw std::runtime_error("Cannot have the value type name with different unit: " + valueType.Unit + " != " + current.Unit);
            }
            return i;
        }
    }
    return -1;
}
