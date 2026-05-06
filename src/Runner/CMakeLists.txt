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

# Copy the full contents of src/reference (dd-win-prof.dll, libdatadog DLLs, etc.)
# to the Runner output directory so that Runner.exe can be launched in-place.
#
# We use an OUTPUT/DEPENDS custom command (rather than POST_BUILD on Runner)
# so that the copy step is driven by the dd-win-prof DLL's timestamp, not by
# whether Runner.exe itself was rebuilt. Without this, changing only
# dd-win-prof's implementation leaves Runner's import .lib unchanged, the
# Runner target looks "up-to-date" to MSBuild, and its POST_BUILD step is
# skipped -- so the Runner output directory keeps a stale DLL.
#
# A per-config stamp file acts as the command's OUTPUT (target-dependent
# generator expressions aren't allowed in OUTPUT, so we can't use the copied
# DLL's path directly). The stamp is touched after the copy and, being newer
# than the source DLL only while the copy is fresh, drives incremental
# rebuilds correctly. Works identically for Visual Studio, Ninja and Makefile
# generators.
set(_runner_runtime_deps_stamp "${CMAKE_CURRENT_BINARY_DIR}/runtime_deps_$<CONFIG>.stamp")
add_custom_command(
    OUTPUT "${_runner_runtime_deps_stamp}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${WINDOWSPROFILER_ROOT_DIR}/src/reference"
        "$<TARGET_FILE_DIR:Runner>"
    COMMAND ${CMAKE_COMMAND} -E touch "${_runner_runtime_deps_stamp}"
    DEPENDS $<TARGET_FILE:dd-win-prof>
    COMMENT "Copying reference binaries to Runner output directory"
    VERBATIM
)
add_custom_target(Runner_runtime_deps DEPENDS "${_runner_runtime_deps_stamp}")
add_dependencies(Runner_runtime_deps dd-win-prof)
add_dependencies(Runner Runner_runtime_deps)
