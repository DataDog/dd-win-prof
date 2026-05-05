cmake_minimum_required(VERSION 3.21)
project(WindowsProfiler LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Store the project root directory for use in subdirectories. This ensures correct
# paths when this project is included via add_subdirectory() or FetchContent.
set(WINDOWSPROFILER_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# Determine if this is the top-level project or included as a dependency
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(DD_WIN_PROF_IS_TOPLEVEL ON)
else()
    set(DD_WIN_PROF_IS_TOPLEVEL OFF)
endif()

# Option to build obfuscation tools (ObfSymbols). These tools require the DIA SDK
# and are only needed for development/symbol processing, not for runtime profiling.
# Disabled by default when used as a dependency.
option(DD_WIN_PROF_BUILD_OBFUSCATION "Build obfuscation tools (requires DIA SDK)" ${DD_WIN_PROF_IS_TOPLEVEL})

add_compile_definitions(UNICODE _UNICODE)

if(MSVC)
    # /Zi: produce PDB in all configs (matches vcxproj GenerateDebugInformation=true
    # for Debug, DebugFull for Release)
    add_compile_options(/W3 /sdl /permissive- /Zi)
    add_compile_options(
        $<$<CONFIG:Release>:/Gy>
        $<$<CONFIG:Release>:/Oi>
    )
    # /DEBUG:FULL for Release configs (matches DebugFull in vcxproj); /DEBUG for Debug.
    # /OPT:REF and /OPT:ICF trim unreferenced code and duplicate COMDATs in Release.
    add_link_options(
        $<$<CONFIG:Debug>:/DEBUG>
        $<$<CONFIG:Release>:/DEBUG:FULL>
        $<$<CONFIG:MinSizeRel>:/DEBUG:FULL>
        $<$<CONFIG:RelWithDebInfo>:/DEBUG:FULL>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()

add_subdirectory(src)

if(DD_WIN_PROF_BUILD_OBFUSCATION)
    add_subdirectory(obfuscation)
endif()

include(cmake/clang-format.cmake)

# Helper function for consuming projects to copy dd-win-prof runtime
# dependencies (DLLs and optionally PDBs) to their output directory.
#
# Usage:
#   dd_win_prof_copy_runtime_deps(target_name [INCLUDE_PDBS])
#
# Arguments:
#   target_name  - Name of the consuming target (e.g., dd-sdk-cpp)
#   INCLUDE_PDBS - Optional flag to also copy .pdb debug files
#
# This function adds a POST_BUILD command that copies:
#   - dd-win-prof.dll (always)
#   - datadog_profiling_ffi.dll (always)
#   - dd-win-prof.pdb (if INCLUDE_PDBS specified)
#   - datadog_profiling_ffi.pdb (if INCLUDE_PDBS specified)
#
function(dd_win_prof_copy_runtime_deps TARGET_NAME)
    if(NOT TARGET dd-win-prof)
        message(FATAL_ERROR "dd_win_prof_copy_runtime_deps: dd-win-prof target not found. "
                            "Did you include dd-win-prof with FetchContent or add_subdirectory?")
    endif()

    # Parse optional INCLUDE_PDBS argument
    set(options INCLUDE_PDBS)
    cmake_parse_arguments(ARG "${options}" "" "" ${ARGN})

    # Determine libdatadog DLL directory based on build configuration
    set(_ld_dyn "${DD_WIN_PROF_LIBDATADOG_DIR}/$<LOWER_CASE:$<CONFIG>>/dynamic")

    # Copy DLLs (always)
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:dd-win-prof>
            $<TARGET_FILE_DIR:${TARGET_NAME}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_ld_dyn}/datadog_profiling_ffi.dll"
            $<TARGET_FILE_DIR:${TARGET_NAME}>
        VERBATIM
        COMMENT "Copying dd-win-prof runtime DLLs to ${TARGET_NAME} output directory"
    )

    # Copy PDBs (optional)
    if(ARG_INCLUDE_PDBS)
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_PDB_FILE:dd-win-prof>
                $<TARGET_FILE_DIR:${TARGET_NAME}>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_ld_dyn}/datadog_profiling_ffi.pdb"
                $<TARGET_FILE_DIR:${TARGET_NAME}>
            VERBATIM
            COMMENT "Copying dd-win-prof debug symbols (PDBs) to ${TARGET_NAME} output directory"
        )
    endif()
endfunction()
