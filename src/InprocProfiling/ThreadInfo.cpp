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
