# Copy selected FILES from SRC into DST, renaming an in-use destination aside
# if it cannot be overwritten. Without FILES, copy all files in SRC.
# Usage: cmake -DSRC=<dir> -DDST=<dir> "-DFILES=file1;file2" -P <this-script>

if(NOT SRC OR NOT DST)
    message(FATAL_ERROR "copy_directory_replace.cmake requires -DSRC=<dir> and -DDST=<dir>")
endif()

if(NOT IS_DIRECTORY "${SRC}")
    message(FATAL_ERROR "copy_directory_replace: SRC is not a directory: ${SRC}")
endif()

file(MAKE_DIRECTORY "${DST}")

if(DEFINED FILES)
    set(_entries)
    foreach(_name IN LISTS FILES)
        list(APPEND _entries "${SRC}/${_name}")
    endforeach()
else()
    file(GLOB _entries LIST_DIRECTORIES true "${SRC}/*")
endif()
foreach(_src IN LISTS _entries)
    if(IS_DIRECTORY "${_src}")
        continue()
    endif()

    get_filename_component(_name "${_src}" NAME)
    set(_dst "${DST}/${_name}")

    file(COPY_FILE "${_src}" "${_dst}" ONLY_IF_DIFFERENT RESULT _copy_res)
    if(NOT _copy_res)
        continue()
    endif()

    # Overwrite failed (typically ERROR_SHARING_VIOLATION). Rename dest aside
    # with a unique suffix so a leftover .prev from a previous rebuild cannot
    # block the rename.
    string(RANDOM LENGTH 8 _tok)
    set(_bak "${_dst}.${_tok}.prev")
    if(EXISTS "${_dst}")
        file(RENAME "${_dst}" "${_bak}")
    endif()
    file(COPY_FILE "${_src}" "${_dst}" RESULT _copy_res)
    if(_copy_res)
        message(FATAL_ERROR "Failed to copy ${_src} -> ${_dst}: ${_copy_res}")
    endif()
    # The renamed file stays mapped until holders unload; ignore delete failure.
    file(REMOVE "${_bak}")
endforeach()
