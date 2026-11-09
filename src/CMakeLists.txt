include(FetchContent)

# Use NEW behavior for DOWNLOAD_EXTRACT_TIMESTAMP to avoid warnings
cmake_policy(SET CMP0135 NEW)

# spdlog v1.14.1 downloaded and built from source at configure time. We use it as
# header-only to match the previous configuration.
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
    GIT_SHALLOW TRUE
)
# These are plain (non-cache) variables so they apply to spdlog's option()s (CMP0077)
# without leaking into the cache of a project that consumes dd-win-prof.
# Disable building examples, tests, and benchmarks
set(SPDLOG_BUILD_EXAMPLE OFF)
set(SPDLOG_BUILD_TESTS OFF)
set(SPDLOG_BUILD_BENCH OFF)
# Suppress spdlog's install() rules so `cmake --install` for the release
# package doesn't drop spdlog headers/CMake config into our prefix.
set(SPDLOG_INSTALL OFF)
FetchContent_MakeAvailable(spdlog)

add_library(dd_win_prof_spdlog INTERFACE)
target_link_libraries(dd_win_prof_spdlog INTERFACE spdlog::spdlog_header_only)

if(DD_WIN_PROF_BUILD_TESTS)
    # Google Test v1.14.0 fetched at configure time. gtest_force_shared_crt ensures
    # GTest links the same CRT variant as the rest of the project (/MD vs /MDd).
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
        TIMEOUT 120
        INACTIVITY_TIMEOUT 30
    )
    set(gtest_force_shared_crt ON)
    # Suppress googletest's install() rules. Without this, `cmake --install`
    # for the release package tries to install gtest headers + .lib into our
    # prefix (and fails on the .lib path), and would pollute the zip with
    # test-only artifacts.
    set(INSTALL_GTEST OFF)
    FetchContent_MakeAvailable(googletest)
endif()

add_subdirectory(dd-win-prof)

if(DD_WIN_PROF_BUILD_RUNNER)
    add_subdirectory(Runner)
endif()

if(DD_WIN_PROF_BUILD_UIAPP)
    add_subdirectory(UIApp)
endif()

if(DD_WIN_PROF_BUILD_TESTS)
    add_subdirectory(Tests)
endif()

if(DD_WIN_PROF_BUILD_INJECTOR)
    add_subdirectory(ProfilerInjector)
endif()
