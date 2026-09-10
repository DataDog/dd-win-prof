# A minimal Win32 GUI test application that exercises the dd-win-prof UI hang
# detector. Built as a WIN32 (windowed) executable so it uses wWinMain and does
# not spawn a console window.
add_executable(UIApp WIN32
    UIApp.cpp
)

target_link_libraries(UIApp PRIVATE dd-win-prof shell32.lib)

# Link-time code generation (whole-program optimisation) in Release builds only.
set_property(TARGET UIApp PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# Copy the full contents of src/reference (dd-win-prof.dll, libdatadog DLLs, etc.)
# to the UIApp output directory so that UIApp.exe can be launched in-place.
#
# We use an OUTPUT/DEPENDS custom command (rather than POST_BUILD on UIApp)
# so that the copy step is driven by the dd-win-prof DLL's timestamp, not by
# whether UIApp.exe itself was rebuilt. Without this, changing only
# dd-win-prof's implementation leaves UIApp's import .lib unchanged, the
# UIApp target looks "up-to-date" to MSBuild, and its POST_BUILD step is
# skipped -- so the UIApp output directory keeps a stale DLL.
#
# cmake -E copy_directory is not used: it fails the whole copy when a
# destination DLL is still mapped (running UIApp, or leftover holders of a
# previously installed global hook). copy_directory_replace.cmake renames
# in-use files aside and copies the new ones in.
#
# A per-config stamp file acts as the command's OUTPUT (target-dependent
# generator expressions aren't allowed in OUTPUT, so we can't use the copied
# DLL's path directly). The stamp is touched after the copy and, being newer
# than the source DLL only while the copy is fresh, drives incremental
# rebuilds correctly. Works identically for Visual Studio, Ninja and Makefile
# generators.
set(_uiapp_runtime_deps_stamp "${CMAKE_CURRENT_BINARY_DIR}/runtime_deps_$<CONFIG>.stamp")
add_custom_command(
    OUTPUT "${_uiapp_runtime_deps_stamp}"
    COMMAND ${CMAKE_COMMAND}
        "-DSRC=${WINDOWSPROFILER_ROOT_DIR}/src/reference"
        "-DDST=$<TARGET_FILE_DIR:UIApp>"
        -P "${WINDOWSPROFILER_ROOT_DIR}/cmake/copy_directory_replace.cmake"
    COMMAND ${CMAKE_COMMAND} -E touch "${_uiapp_runtime_deps_stamp}"
    DEPENDS $<TARGET_FILE:dd-win-prof>
    COMMENT "Copying reference binaries to UIApp output directory"
    VERBATIM
)
add_custom_target(UIApp_runtime_deps DEPENDS "${_uiapp_runtime_deps_stamp}")
add_dependencies(UIApp_runtime_deps dd-win-prof)
add_dependencies(UIApp UIApp_runtime_deps)

# When ASan is on, UIApp.exe (and the dd-win-prof.dll it loads) are both
# instrumented and need the MSVC ASan runtime DLL alongside the executable.
# No-op when DD_WIN_PROF_ENABLE_ASAN is OFF.
dd_win_prof_copy_asan_runtime(UIApp)
