#pragma once
#include "pch.h"

#include "ThreadInfo.h"
#include "SampleValueType.h"

class Sample
{
public:
    static size_t ValuesCount;

public:
    Sample(std::chrono::nanoseconds timestamp, std::shared_ptr<ThreadInfo> threadInfo, uint64_t* pFrames, size_t framesCount);

    // let compiler generating the move and copy ctors/assignment operators
    Sample(const Sample&) = default;
    Sample& operator=(const Sample& sample) = default;
    Sample(Sample&& sample) noexcept = default;
    Sample& operator=(Sample&& other) noexcept = default;

    void AddValue(std::int64_t value, size_t index);

    // Static setter for ValuesCount
    static void SetValuesCount(size_t count) { ValuesCount = count; }

    inline std::chrono::nanoseconds GetTimestamp() { return _timestamp; }
    inline std::span<const uint64_t> GetFrames() { return _callstack; }
    inline std::span<const int64_t> GetValues() { return _values; }
    inline std::shared_ptr<ThreadInfo> GetThreadInfo() { return _threadInfo; }

private:
    std::chrono::nanoseconds _timestamp;
    std::vector<uint64_t> _callstack;
    std::vector<int64_t> _values;
    std::shared_ptr<ThreadInfo> _threadInfo;
};

