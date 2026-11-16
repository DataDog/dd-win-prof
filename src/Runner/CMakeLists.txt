add_executable(Runner
    Runner.cpp
    Helpers.cpp
    Spinner.cpp
    Helpers.h
    Spinner.h
    ../dd-win-prof/Uuid.cpp
    ../dd-win-prof/Uuid.h
)

target_link_libraries(Runner PRIVATE dd-win-prof)

# Link-time code generation (whole-program optimisation) in Release builds only.
set_property(TARGET Runner PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# Copy only the profiler DLL/PDB, not stale third-party binaries. The stamp
# follows the profiler DLL timestamp so DLL-only rebuilds refresh the copy.
# In-use destination files are renamed aside before replacement.
set(_runner_runtime_deps_stamp "${CMAKE_CURRENT_BINARY_DIR}/runtime_deps_$<CONFIG>.stamp")
add_custom_command(
    OUTPUT "${_runner_runtime_deps_stamp}"
    COMMAND ${CMAKE_COMMAND}
        "-DSRC=$<TARGET_FILE_DIR:dd-win-prof>"
        "-DFILES=$<TARGET_FILE_NAME:dd-win-prof>;$<TARGET_PDB_FILE_NAME:dd-win-prof>"
        "-DDST=$<TARGET_FILE_DIR:Runner>"
        -P "${WINDOWSPROFILER_ROOT_DIR}/cmake/copy_directory_replace.cmake"
    COMMAND ${CMAKE_COMMAND} -E touch "${_runner_runtime_deps_stamp}"
    DEPENDS $<TARGET_FILE:dd-win-prof>
    COMMENT "Copying profiler runtime binaries to Runner output directory"
    VERBATIM
)
add_custom_target(Runner_runtime_deps DEPENDS "${_runner_runtime_deps_stamp}")
add_dependencies(Runner_runtime_deps dd-win-prof)
add_dependencies(Runner Runner_runtime_deps)

# When ASan is on, Runner.exe (and the dd-win-prof.dll it loads) are both
# instrumented and need the MSVC ASan runtime DLL alongside the executable.
# No-op when DD_WIN_PROF_ENABLE_ASAN is OFF.
dd_win_prof_copy_asan_runtime(Runner)
