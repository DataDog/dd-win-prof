#pragma once

#include "pch.h"

class ScopedHandle
{
public:
    explicit ScopedHandle(HANDLE hnd)
        :
        _handle(hnd)
    {
    }

    ~ScopedHandle()
    {
        if (IsValid())
        {
            ::CloseHandle(_handle);
        }
    }

    // Make it non copyable
    ScopedHandle(ScopedHandle&) = delete;
    ScopedHandle& operator=(ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept
    {
        // set the other handle to NULL and store its value in _handle
        _handle = std::exchange(other._handle, static_cast<HANDLE>(NULL));
    }

    ScopedHandle& operator=(ScopedHandle&& other) noexcept
    {
        if (this != &other)
        {
            // set the other handle to NULL and store its value in _handle
            _handle = std::exchange(other._handle, static_cast<HANDLE>(NULL));
        }
        return *this;
    }

    operator HANDLE() const
    {
        return _handle;
    }

    bool IsValid()
    {
        return (_handle != INVALID_HANDLE_VALUE) && (_handle != NULL);
    }

private:
    HANDLE _handle;
};
