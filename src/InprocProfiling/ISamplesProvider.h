// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

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