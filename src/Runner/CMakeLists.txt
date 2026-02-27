add_executable(Runner
    Runner.cpp
    Helpers.cpp
    Spinner.cpp
)

target_link_libraries(Runner PRIVATE dd-win-prof)

# Copy the full contents of src/reference (dd-win-prof.dll, libdatadog DLLs, etc.)
# to the Runner output directory so that Runner.exe can be launched in-place.
# The vcxproj restricts this to Release; we apply it unconditionally here because
# dd-win-prof's post-build step populates src/reference in all configurations.
add_custom_command(TARGET Runner POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/src/reference"
        "$<TARGET_FILE_DIR:Runner>"
    VERBATIM
    COMMENT "Copying reference binaries to Runner output directory"
)
