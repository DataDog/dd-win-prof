#pragma once

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers

#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <stdint.h>
#include <tuple>
#include <vector>
#include <utility>

// Windows Header Files
#include <windows.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
