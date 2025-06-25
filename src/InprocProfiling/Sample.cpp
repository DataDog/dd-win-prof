#include "pch.h"
#include "Sample.h"

// TODO: update the values vector size if more than 16 slots are needed
size_t Sample::ValuesCount = 16;  // should be set BEFORE any sample gets created

Sample::Sample(std::chrono::nanoseconds timestamp, std::shared_ptr<ThreadInfo> threadInfo, uint64_t* pFrames, size_t framesCount)
    :
    _timestamp(timestamp),
    _callstack(pFrames, pFrames + framesCount),
    _values(ValuesCount, 0), // Initialize with zeros for sane defaults
    _threadInfo(std::move(threadInfo))
{
}

void Sample::AddValue(std::int64_t value, size_t index)
{
    if (index >= ValuesCount)
    {
        throw std::invalid_argument("index");
    }

    _values[index] = value;
}
