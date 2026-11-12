# MSVC AddressSanitizer plumbing for dd-win-prof.
#
# Provides two public entry points:
#   dd_win_prof_setup_asan()            - call once from the top-level
#                                         CMakeLists after project(); applies
#                                         /fsanitize=address project-wide and
#                                         resolves the runtime DLL location.
#   dd_win_prof_copy_asan_runtime(<tgt>) - post-build copy of the ASan runtime
#                                         DLL next to <tgt>'s output. No-op
#                                         when ASan is OFF.
#
# Both no-op unless DD_WIN_PROF_ENABLE_ASAN is ON and the toolchain is MSVC.

# Strip a flag matched by <regex> from every standard CMAKE_<lang>_FLAGS_<cfg>
# cache variable. Used to remove options that are incompatible with ASan
# (/RTC* for compile flags, /INCREMENTAL for link flags).
function(_dd_strip_msvc_flag REGEX VAR_PREFIX_LIST)
    foreach(_prefix IN LISTS VAR_PREFIX_LIST)
        foreach(_cfg "" _DEBUG _RELEASE _MINSIZEREL _RELWITHDEBINFO)
            set(_var ${_prefix}${_cfg})
            if(DEFINED ${_var})
                string(REGEX REPLACE "${REGEX}" "" _new "${${_var}}")
                set(${_var} "${_new}" CACHE STRING "" FORCE)
            endif()
        endforeach()
    endforeach()
endfunction()

# Append <flag> to every standard CMAKE_<linker_kind>_LINKER_FLAGS_<cfg>.
function(_dd_append_linker_flag FLAG)
    foreach(_kind EXE SHARED MODULE)
        foreach(_cfg "" _DEBUG _RELEASE _MINSIZEREL _RELWITHDEBINFO)
            set(_var CMAKE_${_kind}_LINKER_FLAGS${_cfg})
            if(DEFINED ${_var})
                set(${_var} "${${_var}} ${FLAG}" CACHE STRING "" FORCE)
            endif()
        endforeach()
    endforeach()
endfunction()

# Resolve clang_rt.asan_dynamic-x86_64.dll. Writes the absolute path into the
# parent-scope variable named by OUT_VAR, or "" if not found.
#
# Lookup order:
#   1. $ENV{VCToolsInstallDir}     - set inside a VS Developer Command Prompt.
#   2. vswhere.exe                 - lives at a documented stable path on
#                                    every machine with VS 2017+ installed,
#                                    so this also works from a plain shell.
function(_dd_find_asan_runtime_dll OUT_VAR)
    set(_rel "bin/Hostx64/x64/clang_rt.asan_dynamic-x86_64.dll")
    set(_found "")

    if(DEFINED ENV{VCToolsInstallDir})
        set(_candidate "$ENV{VCToolsInstallDir}/${_rel}")
        if(EXISTS "${_candidate}")
            set(_found "${_candidate}")
        endif()
    endif()

    if(_found STREQUAL "")
        set(_vswhere "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer/vswhere.exe")
        if(NOT EXISTS "${_vswhere}")
            set(_vswhere "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe")
        endif()
        if(EXISTS "${_vswhere}")
            execute_process(
                COMMAND "${_vswhere}" -latest -products *
                        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
                        -property installationPath
                OUTPUT_VARIABLE _vs_install
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            set(_stamp "${_vs_install}/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt")
            if(_vs_install AND EXISTS "${_stamp}")
                file(READ "${_stamp}" _vc_ver)
                string(STRIP "${_vc_ver}" _vc_ver)
                set(_candidate "${_vs_install}/VC/Tools/MSVC/${_vc_ver}/${_rel}")
                if(EXISTS "${_candidate}")
                    set(_found "${_candidate}")
                endif()
            endif()
        endif()
    endif()

    set(${OUT_VAR} "${_found}" PARENT_SCOPE)
endfunction()

# Public: enable ASan project-wide.
function(dd_win_prof_setup_asan)
    if(NOT DD_WIN_PROF_ENABLE_ASAN OR NOT MSVC)
        return()
    endif()

    message(STATUS "dd-win-prof: AddressSanitizer ENABLED (/fsanitize=address)")

    _dd_find_asan_runtime_dll(_dll)
    set(DD_WIN_PROF_ASAN_RUNTIME_DLL "${_dll}" CACHE INTERNAL "")
    if(_dll STREQUAL "")
        message(WARNING "DD_WIN_PROF_ENABLE_ASAN: clang_rt.asan_dynamic-x86_64.dll "
                        "could not be located via VCToolsInstallDir or vswhere. "
                        "Ensure the 'C++ AddressSanitizer' component is installed via "
                        "the Visual Studio Installer, otherwise instrumented binaries "
                        "(Tests.exe, Runner.exe) will fail to start.")
    else()
        message(STATUS "dd-win-prof: ASan runtime DLL = ${_dll}")
    endif()

    # /RTC* and /fsanitize=address are mutually exclusive. CMake injects /RTC1
    # by default in Debug-style configs; strip it from every config.
    _dd_strip_msvc_flag("/RTC[1csu]+" "CMAKE_CXX_FLAGS;CMAKE_C_FLAGS")

    # /INCREMENTAL is also incompatible. Strip any existing form and force NO.
    _dd_strip_msvc_flag("/INCREMENTAL(:NO)?"
        "CMAKE_EXE_LINKER_FLAGS;CMAKE_SHARED_LINKER_FLAGS;CMAKE_MODULE_LINKER_FLAGS")
    _dd_append_linker_flag("/INCREMENTAL:NO")

    add_compile_options(/fsanitize=address)
    add_link_options(/INCREMENTAL:NO)
endfunction()

# Public: copy the ASan runtime DLL next to <target_name>'s output. No-op when
# ASan is OFF or the DLL could not be located.
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
