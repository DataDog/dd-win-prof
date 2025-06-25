// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "Helpers.h"

#include <stdio.h>
#include "Windows.h"

void Spin(int durationMS)
{
    auto start = ::GetTickCount64();
    auto current = ::GetTickCount64();
    do {
        current = ::GetTickCount64();
    } while ((current - start) < durationMS);
}
