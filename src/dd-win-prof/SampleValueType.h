// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <string>

/// <summary>
/// Represents a sample value type with its name and unit.
/// Used to define what types of values are collected in profiling samples.
/// </summary>
struct SampleValueType
{
    std::string Name;  // e.g., "cpu-time", "cpu-samples", "wall-time"
    std::string Unit;  // e.g., "nanoseconds", "count", "bytes"
};