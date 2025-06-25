#pragma once

#include "pch.h"
#include "Sample.h"

class ISamplesProvider
{
public:
    virtual ~ISamplesProvider() = default;
    virtual size_t MoveSamples(std::vector<Sample>& destination) = 0;
    virtual const char* GetName() = 0;
};