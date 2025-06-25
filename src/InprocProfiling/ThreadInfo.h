// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "pch.h"
#include "OpSysTools.h"
#include "ScopedHandle.h"

// NOTE:
class ThreadInfo
{
public:
    ThreadInfo(uint32_t tid, HANDLE hThread);

    uint32_t GetThreadId() const { return _tid; }
    inline HANDLE GetOsThreadHandle() const { return _hThread; }

    inline std::chrono::nanoseconds SetLastSampleTimestamp(std::chrono::nanoseconds value)
    {
        auto prevValue = _lastSampleHighPrecisionTimestamp;
        _lastSampleHighPrecisionTimestamp = value;
        return prevValue;
    }

    inline std::chrono::milliseconds GetCpuConsumption() const
    {
        return _cpuConsumption;
    }

    inline std::chrono::nanoseconds GetCpuTimestamp() const
    {
        return _timestamp;
    }

    inline std::chrono::milliseconds SetCpuConsumption(std::chrono::milliseconds value, std::chrono::nanoseconds timestamp)
    {
        _timestamp = timestamp;

        auto prevValue = _cpuConsumption;
        _cpuConsumption = value;
        return prevValue;
    }

    inline bool GetThreadName(std::string& name)
    {
        if (_hasThreadName)
        {
            name = _threadName;
            return true;
        }

        if (OpSysTools::GetNativeThreadName(_hThread, _threadName))
        {
            _hasThreadName = true;
            name = _threadName;
            return true;
        }

        return false;
    }

private:
    // we don't handle the case where a thread ID is reused by the OS after a thread has exited
    // so only keep track of the OS thread ID
    uint32_t _tid;

    ScopedHandle _hThread;

    // will be used for walltime
    std::chrono::nanoseconds _lastSampleHighPrecisionTimestamp;

    // last CPU consumption in milliseconds
    std::chrono::milliseconds _cpuConsumption;

    // timestamp of the last CPU consumption sample
    std::chrono::nanoseconds _timestamp;

    // thread name, if available
    bool _hasThreadName = false;
    std::string _threadName;
};

