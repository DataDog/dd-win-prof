#include "pch.h"
#include "CpuTimeProvider.h"

std::vector<SampleValueType> CpuTimeProvider::SampleTypeDefinitions(
    {
        {"cpu-time", "nanoseconds"},
        {"cpu-samples", "count"}
    }
);

CpuTimeProvider::CpuTimeProvider(SampleValueTypeProvider& valueTypeProvider)
    : CollectorBase("CpuTimeProvider", valueTypeProvider.GetOrRegister(SampleTypeDefinitions))
{
}
