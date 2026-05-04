# Defines two CMake targets when this project is top-level and clang-format is
# resolvable:
#   * format-apply  - rewrites all source files in-place using clang-format
#   * check-format  - verifies formatting without modifying files (what CI runs)
#
# To pin a specific clang-format binary, configure with -DCLANG_FORMAT_BINARY=...
# Otherwise we look for clang-format-20 first, then clang-format on PATH.

if(NOT DD_WIN_PROF_IS_TOPLEVEL)
    return()
endif()

if(NOT CLANG_FORMAT_BINARY)
    find_program(CLANG_FORMAT_BINARY NAMES clang-format-20 clang-format)
endif()

if(NOT CLANG_FORMAT_BINARY)
    message(STATUS "clang-format not found; format-apply / check-format targets are unavailable")
    return()
endif()

set(_FORMAT_DIRECTORIES src obfuscation)
set(_FORMAT_EXTENSIONS h cpp)
set(_FORMAT_SOURCE_FILES "")
foreach(DIR IN LISTS _FORMAT_DIRECTORIES)
    foreach(EXT IN LISTS _FORMAT_EXTENSIONS)
        file(GLOB_RECURSE _DIR_FILES "${WINDOWSPROFILER_ROOT_DIR}/${DIR}/*.${EXT}")
        list(APPEND _FORMAT_SOURCE_FILES ${_DIR_FILES})
    endforeach()
endforeach()

add_custom_target(format-apply
    COMMAND "${CLANG_FORMAT_BINARY}" -i ${_FORMAT_SOURCE_FILES}
    COMMENT "Formatting source code with ${CLANG_FORMAT_BINARY}"
    VERBATIM
)

add_custom_target(check-format
    COMMAND "${CLANG_FORMAT_BINARY}" --dry-run --Werror ${_FORMAT_SOURCE_FILES}
    COMMENT "Checking source code formatting with ${CLANG_FORMAT_BINARY}"
    VERBATIM
)
