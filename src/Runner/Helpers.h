// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

// Test scenario helpers — declared noinline so they stay observable as
// distinct frames in Release-build profiles. Without this, MSVC inlines
// them into the caller and the prof-correctness analyzer can't assert on
// per-function CPU shares.
__declspec(noinline) void Spin(int durationMS);
