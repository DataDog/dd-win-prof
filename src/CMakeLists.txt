include(FetchContent)

# Use NEW behavior for DOWNLOAD_EXTRACT_TIMESTAMP to avoid warnings
cmake_policy(SET CMP0135 NEW)

# libdatadog v19.0.0 pre-built binaries for Windows x64. These binaries are
# downloaded automatically at configure time and extracted to the build directory.
FetchContent_Declare(
    libdatadog
    URL https://github.com/DataDog/libdatadog/releases/download/v19.0.0/libdatadog-x64-windows.zip
    URL_HASH SHA512=548bfe9761625aaa1ce54f42979e75f3a435ca0c1a4280a91b325fff402c4264fb593d203e0f758b68313ff098fa609ed8cef800bbf6a18be7bfed9c489626bd
)
FetchContent_MakeAvailable(libdatadog)

# Expose libdatadog source directory to parent scope so helper function can
# access DLL paths
set(DD_WIN_PROF_LIBDATADOG_DIR "${libdatadog_SOURCE_DIR}" PARENT_SCOPE)

set(_LD_ROOT "${libdatadog_SOURCE_DIR}")

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

# spdlog v1.14.1 downloaded and built from source at configure time. We use it as
# header-only to match the previous configuration.
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
    GIT_SHALLOW TRUE
)
# Disable building examples, tests, and benchmarks
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spdlog)

# Create an alias target for backwards compatibility with the previous setup
add_library(spdlog_includes INTERFACE)
target_link_libraries(spdlog_includes INTERFACE spdlog::spdlog_header_only)

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
