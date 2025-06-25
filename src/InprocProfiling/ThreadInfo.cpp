// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"
#include "ThreadInfo.h"

ThreadInfo::ThreadInfo(uint32_t tid, HANDLE hThread)
    :
    _tid(tid),
    _hThread(ScopedHandle(hThread)),
    _lastSampleHighPrecisionTimestamp{ 0ns },
    _cpuConsumption{ 0ms },
    _timestamp{ 0ns }
{
}
