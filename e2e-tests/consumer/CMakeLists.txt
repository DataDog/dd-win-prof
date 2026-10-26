cmake_minimum_required(VERSION 3.21)
project(dd_win_prof_consumer LANGUAGES CXX)

# Minimal project that consumes dd-win-prof the way another project would, via
# FetchContent. Built by scripts/test-consumer.ps1, which checks that nothing meant
# only for dd-win-prof developers leaks into the consuming project. Deliberately
# C++17: dd-win-prof itself builds as C++20, and consumers shouldn't be forced to match.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DD_WIN_PROF_SOURCE_DIR)
    message(FATAL_ERROR "Pass -DDD_WIN_PROF_SOURCE_DIR=<path to the dd-win-prof checkout>")
endif()

# FetchContent writes SOURCE_DIR into a generated CMake file unescaped, so Windows
# backslashes must be converted first.
file(TO_CMAKE_PATH "${DD_WIN_PROF_SOURCE_DIR}" DD_WIN_PROF_SOURCE_DIR)

include(FetchContent)
FetchContent_Declare(dd-win-prof SOURCE_DIR "${DD_WIN_PROF_SOURCE_DIR}")
FetchContent_MakeAvailable(dd-win-prof)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE dd-win-prof)
install(TARGETS consumer RUNTIME DESTINATION bin)

# Export dd-win-prof from this project's own install set, as the README documents
# (minus the libdatadog DLL, which isn't what is being tested here). The "downstream"
# project then consumes the installed export; see scripts/test-consumer.ps1.
install(TARGETS dd-win-prof
    EXPORT ConsumerTargets
    RUNTIME DESTINATION bin
    ARCHIVE DESTINATION lib
)
install(FILES "${dd-win-prof_SOURCE_DIR}/src/dd-win-prof/include/dd-win-prof.h" DESTINATION include)
install(EXPORT ConsumerTargets DESTINATION lib/cmake/consumer)
