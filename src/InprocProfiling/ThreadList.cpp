#include "pch.h"
#include "ThreadList.h"

const std::uint32_t ThreadList::DefaultThreadListSize = 50;


ThreadList::ThreadList()
{
    _threads.reserve(DefaultThreadListSize);
}

ThreadList::~ThreadList()
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    _threads.clear();
}

void ThreadList::AddThread(uint32_t tid, HANDLE hThread)
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    // TODO: check if the thread ID already exists in the list but enough for this POC

    auto info = std::make_shared<ThreadInfo>(tid, hThread);
    _threads.push_back(info);
}

void ThreadList::RemoveThread(uint32_t tid)
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    uint32_t pos = 0;
    for (auto i = _threads.begin(); i != _threads.end(); ++i)
    {
        std::shared_ptr<ThreadInfo> pInfo = *i; // make a copy so it can be moved later
        if (pInfo->GetThreadId() == tid)
        {
            // remove it from the storage
            _threads.erase(i);

            // iterators might need to be updated
            UpdateIterators(pos);

            return;
        }
        pos++;
    }
}

size_t ThreadList::Count()
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    return _threads.size();
}

uint32_t ThreadList::CreateIterator()
{
    uint32_t iterator = static_cast<uint32_t>(_iterators.size());
    _iterators.push_back(0);
    return iterator;
}

std::shared_ptr<ThreadInfo> ThreadList::LoopNext(uint32_t iterator)
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    auto activeThreadCount = _threads.size();
    if (activeThreadCount == 0)
    {
        return nullptr;
    }

    if (iterator >= _iterators.size())
    {
        return nullptr;
    }

    uint32_t pos = _iterators[iterator];
    std::shared_ptr<ThreadInfo> pInfo = nullptr;

    auto startPos = pos;
    do
    {
        pInfo = _threads[pos];
        // move the iterator to the next thread and loop
        // back to the first thread if the end is reached
        pos = (pos + 1) % activeThreadCount;
    } while (startPos != pos &&
        (pInfo->GetOsThreadHandle() == static_cast<HANDLE>(NULL) || pInfo->GetOsThreadHandle() == INVALID_HANDLE_VALUE));

    _iterators[iterator] = pos;

    if (startPos == pos)
    {
        return nullptr;
    }

    return pInfo;
}

// NOTE: we know this is called only by one thread so no need to be thread-safe
void ThreadList::UpdateIterators(uint32_t removalPos)
{
    // Iterators are positions (in the threads vector) pointing to the next thread to return via LoopNext.
    // So, when a thread is removed from the vector at a position BEFORE an iterator position,
    // this iterator needs to be moved left by 1 to keep on pointing to the same thread.
    // There is no need to update iterators pointing to threads before or at the same spot
    // as the removal position because they will point to the same thread
    //
    // In the following example, the thread at position 1 will be removed and an iterator
    // is pointing to ^ the thread in the third position (i.e. at pos = 2).
    //      x
    //  T0  T1  T2  T3
    //          ^ = 2
    // -->          |
    //  T0  T2  T3  v
    //      ^ = 1 =(2 - 1)
    //
    // After the removal, this iterator should now point to the thread at position 1 instead of 2
    //
    // If the new pos is beyond the vector (i.e. the last element was removed),
    // then reset the iterator to the beginning of the vector:
    //          x
    //  T0  T1  T2  (_threads.size() == 2 when this function is called)
    //          ^ = 2
    // -->
    //  T0  T1
    //  ^ = 0  (reset)
    //
    for (auto i = _iterators.begin(); i != _iterators.end(); ++i)
    {
        uint32_t pos = *i;
        if (removalPos < pos)
        {
            pos = pos - 1;
        }

        // reset iterator if needed
        if (pos >= _threads.size())  // the thread has already been removed from the vector
        {
            pos = 0;
        }

        *i = pos;
    }
}
