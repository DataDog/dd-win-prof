// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#define WIN32_LEAN_AND_MEAN  // Exclude rarely-used stuff from Windows headers
#define NOMINMAX  // Prevent min/max macros that conflict with std::min/std::max

#include <stdint.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

// Windows Header Files
#include <windows.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
