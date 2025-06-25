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
