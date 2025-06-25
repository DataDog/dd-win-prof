#pragma once

#include "datadog/common.h"
#include <string_view>

namespace InprocProfiling {

inline ddog_CharSlice to_CharSlice(std::string_view str) {
    return {.ptr = str.data(), .len = str.size()};
}

inline ddog_prof_ValueType CreateValueType(std::string_view type, std::string_view unit)
{
    ddog_prof_ValueType valueType = {};
    valueType.type_ = to_CharSlice(type);
    valueType.unit = to_CharSlice(unit);
    return valueType;
}

} // namespace InprocProfiling 