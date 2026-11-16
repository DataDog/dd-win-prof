# A minimal Win32 GUI test application that exercises the dd-win-prof UI hang
# detector. Built as a WIN32 (windowed) executable so it uses wWinMain and does
# not spawn a console window.
add_executable(UIApp WIN32
    UIApp.cpp
)

target_include_directories(UIApp PRIVATE
    "${WINDOWSPROFILER_ROOT_DIR}/src/dd-win-prof"
)
target_link_libraries(UIApp PRIVATE dd-win-prof shell32.lib)

# Link-time code generation (whole-program optimisation) in Release builds only.
set_property(TARGET UIApp PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# Copy only the profiler DLL/PDB, not stale third-party binaries. The stamp
# follows the profiler DLL timestamp so DLL-only rebuilds refresh the copy.
# In-use destination files are renamed aside before replacement.
set(_uiapp_runtime_deps_stamp "${CMAKE_CURRENT_BINARY_DIR}/runtime_deps_$<CONFIG>.stamp")
add_custom_command(
    OUTPUT "${_uiapp_runtime_deps_stamp}"
    COMMAND ${CMAKE_COMMAND}
        "-DSRC=$<TARGET_FILE_DIR:dd-win-prof>"
        "-DFILES=$<TARGET_FILE_NAME:dd-win-prof>;$<TARGET_PDB_FILE_NAME:dd-win-prof>"
        "-DDST=$<TARGET_FILE_DIR:UIApp>"
        -P "${WINDOWSPROFILER_ROOT_DIR}/cmake/copy_directory_replace.cmake"
    COMMAND ${CMAKE_COMMAND} -E touch "${_uiapp_runtime_deps_stamp}"
    DEPENDS $<TARGET_FILE:dd-win-prof>
    COMMENT "Copying profiler runtime binaries to UIApp output directory"
    VERBATIM
)
add_custom_target(UIApp_runtime_deps DEPENDS "${_uiapp_runtime_deps_stamp}")
add_dependencies(UIApp_runtime_deps dd-win-prof)
add_dependencies(UIApp UIApp_runtime_deps)

# When ASan is on, UIApp.exe (and the dd-win-prof.dll it loads) are both
# instrumented and need the MSVC ASan runtime DLL alongside the executable.
# No-op when DD_WIN_PROF_ENABLE_ASAN is OFF.
dd_win_prof_copy_asan_runtime(UIApp)
