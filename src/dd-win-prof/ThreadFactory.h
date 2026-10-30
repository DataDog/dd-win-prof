// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <functional>
#include <thread>
#include <utility>

namespace dd_win_prof {
inline std::thread MakeThread(std::function<void()> action) {
  return std::thread(std::move(action));
}
using ThreadFactory = decltype(&MakeThread);
}  // namespace dd_win_prof
