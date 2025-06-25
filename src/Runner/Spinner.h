// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

class Spinner
{
public:
    void Run(int durationMS);

private:
    static void StaticRun(int durationMS);
};

