#pragma once

#include "pch.h"
#include "ISamplesProvider.h"
#include "OpSysTools.h"
#include "Sample.h"
#include "SampleValueTypeProvider.h"

class CollectorBase : public ISamplesProvider
{
public:
    CollectorBase(const char* name, std::vector<SampleValueTypeProvider::Offset> valueOffsets)
        :
        _name(name),
        _valueOffsets{ std::move(valueOffsets) }
    {
    }

    void Add(Sample&& sample)
    {
        std::lock_guard<std::mutex> lock(_lock);
        _collectedSamples.push_back(std::move(sample));
    }

    // ISamplesProvider interface
    size_t MoveSamples(std::vector<Sample>& destination) override
    {
        std::lock_guard<std::mutex> lock(_lock);

        destination.clear();
        _collectedSamples.swap(destination);

        return destination.size();
    }

    const char* GetName() override
    {
        return _name.c_str();
    }

    std::vector<SampleValueTypeProvider::Offset> const& GetValueOffsets() const
    {
        return _valueOffsets;
    }

private:
    std::mutex _lock;
    std::string _name;
    std::vector<SampleValueTypeProvider::Offset> _valueOffsets;
    std::vector<Sample> _collectedSamples;
};