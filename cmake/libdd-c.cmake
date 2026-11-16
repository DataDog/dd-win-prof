include(FetchContent)

# TODO(PROF-16455): Pin the completed dependency to a full commit SHA before release.
set(DD_WIN_PROF_LIBDD_C_REVISION "r1viollet/prof-16455-extract-profile-core")
set(DD_WIN_PROF_LIBDD_C_SOURCE_DIR "" CACHE PATH "Local libdd-c source root (development override; not fetched or modified)")

if(DD_WIN_PROF_LIBDD_C_SOURCE_DIR)
    get_filename_component(_ddc_source "${DD_WIN_PROF_LIBDD_C_SOURCE_DIR}" ABSOLUTE)
    if(NOT EXISTS "${_ddc_source}/profiling/include/ddc/profiling.h")
        message(FATAL_ERROR "libdd-c source must provide profiling/include/ddc/profiling.h: ${_ddc_source}")
    endif()
    add_subdirectory("${_ddc_source}/profiling" "${CMAKE_CURRENT_BINARY_DIR}/libdd-c-profiling")
else()
    FetchContent_Declare(dd_win_prof_libdd_c
        GIT_REPOSITORY https://github.com/DataDog/libdd-c.git
        GIT_TAG "${DD_WIN_PROF_LIBDD_C_REVISION}"
        GIT_CONFIG core.autocrlf=false
        SOURCE_SUBDIR profiling
        BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/libdd-c-profiling"
    )
    FetchContent_MakeAvailable(dd_win_prof_libdd_c)
    set(_ddc_source "${dd_win_prof_libdd_c_SOURCE_DIR}")
endif()

if(NOT TARGET ddc::profiling OR NOT EXISTS "${_ddc_source}/profiling/include/ddc/profiling.h")
    message(FATAL_ERROR "libdd-c source must provide ddc::profiling and its public ddc/profiling.h API")
endif()
message(STATUS "libdd-c profiling source: ${_ddc_source} (requested revision: ${DD_WIN_PROF_LIBDD_C_REVISION})")
if(DD_WIN_PROF_BUILD_INSTALL)
    install(FILES "${_ddc_source}/LICENSE" "${_ddc_source}/NOTICE"
        "${_ddc_source}/LICENSE-3rdparty.csv" DESTINATION licenses/libdd-c)
    install(DIRECTORY "${_ddc_source}/profiling/licenses/" DESTINATION licenses/libdd-c/profiling)
endif()
