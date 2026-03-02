include(FetchContent)

# Use NEW behavior for DOWNLOAD_EXTRACT_TIMESTAMP to avoid warnings
cmake_policy(SET CMP0135 NEW)

set(_LD_ROOT "${CMAKE_SOURCE_DIR}/third-party/libdatadog-x64-windows")

# libdatadog static import library — used by dd-win-prof.dll at link time. Paths
# use lowercase debug/release to match the directory layout produced by the
# download script. MinSizeRel and RelWithDebInfo map to release.
add_library(libdatadog_static STATIC IMPORTED GLOBAL)
set_target_properties(libdatadog_static PROPERTIES
    IMPORTED_LOCATION_DEBUG          "${_LD_ROOT}/debug/static/datadog_profiling_ffi.lib"
    IMPORTED_LOCATION_RELEASE        "${_LD_ROOT}/release/static/datadog_profiling_ffi.lib"
    IMPORTED_LOCATION_MINSIZEREL     "${_LD_ROOT}/release/static/datadog_profiling_ffi.lib"
    IMPORTED_LOCATION_RELWITHDEBINFO "${_LD_ROOT}/release/static/datadog_profiling_ffi.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${_LD_ROOT}/include"
)

# libdatadog dynamic import library — used by Tests.exe, which loads the DLL at
# runtime so it can export the profiler DLL's symbols without statically linking
# the whole FFI. MinSizeRel and RelWithDebInfo map to release.
add_library(libdatadog_dynamic SHARED IMPORTED GLOBAL)
set_target_properties(libdatadog_dynamic PROPERTIES
    IMPORTED_LOCATION_DEBUG          "${_LD_ROOT}/debug/dynamic/datadog_profiling_ffi.dll"
    IMPORTED_IMPLIB_DEBUG            "${_LD_ROOT}/debug/dynamic/datadog_profiling_ffi.lib"
    IMPORTED_LOCATION_RELEASE        "${_LD_ROOT}/release/dynamic/datadog_profiling_ffi.dll"
    IMPORTED_IMPLIB_RELEASE          "${_LD_ROOT}/release/dynamic/datadog_profiling_ffi.lib"
    IMPORTED_LOCATION_MINSIZEREL     "${_LD_ROOT}/release/dynamic/datadog_profiling_ffi.dll"
    IMPORTED_IMPLIB_MINSIZEREL       "${_LD_ROOT}/release/dynamic/datadog_profiling_ffi.lib"
    IMPORTED_LOCATION_RELWITHDEBINFO "${_LD_ROOT}/release/dynamic/datadog_profiling_ffi.dll"
    IMPORTED_IMPLIB_RELWITHDEBINFO   "${_LD_ROOT}/release/dynamic/datadog_profiling_ffi.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${_LD_ROOT}/include"
)

# spdlog is header-only; expose its include directory via an INTERFACE target so
# consumers can simply link against spdlog_includes.
add_library(spdlog_includes INTERFACE)
target_include_directories(spdlog_includes INTERFACE
    "${CMAKE_SOURCE_DIR}/third-party/spdlog/include"
)

# Google Test v1.14.0 fetched at configure time. gtest_force_shared_crt ensures
# GTest links the same CRT variant as the rest of the project (/MD vs /MDd).
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_subdirectory(dd-win-prof)
add_subdirectory(Runner)
add_subdirectory(Tests)
add_subdirectory(ProfilerInjector)
