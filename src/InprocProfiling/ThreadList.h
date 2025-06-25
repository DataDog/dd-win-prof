// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "pch.h"
#include "ThreadInfo.h"


class ThreadList
{
public:
    ThreadList();
    ~ThreadList();

    void AddThread(uint32_t tid, HANDLE hThread);
    void RemoveThread(uint32_t tid);

    // we can't use a lock in a const method so... don't make it const
    size_t Count();

    uint32_t CreateIterator();
    std::shared_ptr<ThreadInfo> LoopNext(uint32_t iterator);


private:
    void UpdateIterators(uint32_t pos);

private:
    static const std::uint32_t DefaultThreadListSize;

    std::recursive_mutex _mutex;
    std::vector<std::shared_ptr<ThreadInfo>> _threads;

    // An iterator is just a position in the vector corresponding to the next thread to be returned by LoopNext
    // so keep track of them in a vector of positions initialized to 0
    std::vector<uint32_t> _iterators;
};

