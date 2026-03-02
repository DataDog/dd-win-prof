add_executable(Runner
    Runner.cpp
    Helpers.cpp
    Spinner.cpp
)

target_link_libraries(Runner PRIVATE dd-win-prof)

# Link-time code generation (whole-program optimisation) in Release builds only.
set_property(TARGET Runner PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# Copy the full contents of src/reference (dd-win-prof.dll, libdatadog DLLs, etc.)
# to the Runner output directory so that Runner.exe can be launched in-place.
# The vcxproj restricts this to Release; we apply it unconditionally here because
# dd-win-prof's post-build step populates src/reference in all configurations.
add_custom_command(TARGET Runner POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${WINDOWSPROFILER_ROOT_DIR}/src/reference"
        "$<TARGET_FILE_DIR:Runner>"
    VERBATIM
    COMMENT "Copying reference binaries to Runner output directory"
)
