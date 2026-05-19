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

# Enable MSVC AddressSanitizer for the whole project (compiler + linker).
# Mostly intended for unit tests in CI; see src/Tests/CMakeLists.txt for the
# runtime DLL deployment that lets Tests.exe actually launch with ASan on.
option(DD_WIN_PROF_ENABLE_ASAN "Build with MSVC AddressSanitizer (/fsanitize=address)" OFF)

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

    if(DD_WIN_PROF_ENABLE_ASAN)
        message(STATUS "dd-win-prof: AddressSanitizer ENABLED (/fsanitize=address)")

        # Resolve the MSVC ASan runtime DLL once, here, so every consumer
        # (Tests.exe, Runner.exe, ...) can ask for it via the helper below.
        # $ENV{VCToolsInstallDir} is exported by the VS Developer shell.
        set(DD_WIN_PROF_ASAN_RUNTIME_DLL "" CACHE INTERNAL "")
        if(DEFINED ENV{VCToolsInstallDir})
            set(_candidate "$ENV{VCToolsInstallDir}/bin/Hostx64/x64/clang_rt.asan_dynamic-x86_64.dll")
            if(EXISTS "${_candidate}")
                set(DD_WIN_PROF_ASAN_RUNTIME_DLL "${_candidate}" CACHE INTERNAL "")
            endif()
        endif()
        if(DD_WIN_PROF_ASAN_RUNTIME_DLL STREQUAL "")
            message(WARNING "DD_WIN_PROF_ENABLE_ASAN: clang_rt.asan_dynamic-x86_64.dll "
                            "could not be located. Configure from a VS Developer Command "
                            "Prompt or set VCToolsInstallDir, otherwise instrumented "
                            "binaries (Tests.exe, Runner.exe) will fail to start.")
        else()
            message(STATUS "dd-win-prof: ASan runtime DLL = ${DD_WIN_PROF_ASAN_RUNTIME_DLL}")
        endif()

        # /RTC* and /fsanitize=address are mutually exclusive. CMake injects
        # /RTC1 by default in Debug-style configs via CMAKE_CXX_FLAGS_<CFG>;
        # strip it from every config to keep the compiler happy.
        foreach(_cfg "" _DEBUG _RELEASE _MINSIZEREL _RELWITHDEBINFO)
            foreach(_lang CXX C)
                set(_var CMAKE_${_lang}_FLAGS${_cfg})
                if(DEFINED ${_var})
                    string(REGEX REPLACE "/RTC[1csu]+" "" ${_var} "${${_var}}")
                    set(${_var} "${${_var}}" CACHE STRING "" FORCE)
                endif()
            endforeach()
        endforeach()

        add_compile_options(/fsanitize=address)

        # ASan is incompatible with incremental linking. Default Debug link
        # flags pass /INCREMENTAL; force it off everywhere.
        add_link_options(/INCREMENTAL:NO)
        foreach(_cfg "" _DEBUG _RELEASE _MINSIZEREL _RELWITHDEBINFO)
            foreach(_kind EXE SHARED MODULE)
                set(_var CMAKE_${_kind}_LINKER_FLAGS${_cfg})
                if(DEFINED ${_var})
                    string(REGEX REPLACE "/INCREMENTAL(:NO)?" "" ${_var} "${${_var}}")
                    set(${_var} "${${_var}} /INCREMENTAL:NO" CACHE STRING "" FORCE)
                endif()
            endforeach()
        endforeach()
    endif()
endif()

# Copy the MSVC AddressSanitizer runtime DLL next to a target's output. No-op
# unless DD_WIN_PROF_ENABLE_ASAN is ON. Used by any executable target that
# needs to launch standalone (Tests.exe, Runner.exe, ...).
function(dd_win_prof_copy_asan_runtime TARGET_NAME)
    if(NOT DD_WIN_PROF_ENABLE_ASAN OR NOT MSVC)
        return()
    endif()
    if(DD_WIN_PROF_ASAN_RUNTIME_DLL STREQUAL "")
        return()
    endif()
    get_filename_component(_dll_name "${DD_WIN_PROF_ASAN_RUNTIME_DLL}" NAME)
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DD_WIN_PROF_ASAN_RUNTIME_DLL}"
            "$<TARGET_FILE_DIR:${TARGET_NAME}>/${_dll_name}"
        VERBATIM
        COMMENT "Copying MSVC ASan runtime next to ${TARGET_NAME}"
    )
endfunction()

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
