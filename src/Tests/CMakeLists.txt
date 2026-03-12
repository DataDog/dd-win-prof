add_executable(Tests
    main.cpp
    ConfigurationTests.cpp
    DynamicModuleTests.cpp
    PprofAggregatorTests.cpp
    ProfileExporterTests.cpp
    RumContextTests.cpp
    SymbolicationTests.cpp
    UuidTests.cpp

    # These source files are compiled directly into Tests (rather than linking
    # against dd-win-prof.dll) so that unit tests can exercise internal
    # implementation details. They match the file list in Tests.vcxproj.
    ../dd-win-prof/Configuration.cpp
    ../dd-win-prof/CpuTimeProvider.cpp
    ../dd-win-prof/OsSpecificApi.cpp
    ../dd-win-prof/OsSysTools.cpp
    ../dd-win-prof/PprofAggregator.cpp
    ../dd-win-prof/Profiler.cpp
    ../dd-win-prof/ProfileExporter.cpp
    ../dd-win-prof/Sample.cpp
    ../dd-win-prof/SamplesCollector.cpp
    ../dd-win-prof/SampleValueTypeProvider.cpp
    ../dd-win-prof/StackFrameCollector.cpp
    ../dd-win-prof/StackSamplerLoop.cpp
    ../dd-win-prof/Symbolication.cpp
    ../dd-win-prof/TagsHelper.cpp
    ../dd-win-prof/ThreadInfo.cpp
    ../dd-win-prof/ThreadList.cpp
    ../dd-win-prof/Uuid.cpp
    ../dd-win-prof/WalltimeProvider.cpp
)

# Tests has its own pch.h (includes gtest/gtest.h and framework.h).
# pch.cpp is the "Create PCH" unit in the vcxproj; CMake drives it via pch.h.
target_precompile_headers(Tests PRIVATE pch.h)

# The dd-win-prof shared sources include headers by short name relative to their
# own directory; adding that directory ensures they resolve correctly when
# compiled as part of this target. libdatadog and spdlog headers are propagated
# via libdatadog_dynamic and spdlog_includes INTERFACE_INCLUDE_DIRECTORIES.
target_include_directories(Tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../dd-win-prof"
)

# Tests links the dynamic FFI library so that profiling API calls go through
# the DLL ABI boundary, mirroring how the profiler is consumed at runtime.
target_link_libraries(Tests PRIVATE
    libdatadog_dynamic
    spdlog_includes
    GTest::gtest
    dbghelp.lib
    ws2_32.lib
    userenv.lib
    ntdll.lib
    crypt32.lib
    secur32.lib
    bcrypt.lib
    psapi.lib
    powrprof.lib
    ncrypt.lib
    windowsapp.lib
)

# Link-time code generation (whole-program optimisation) in Release builds only.
set_property(TARGET Tests PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

set(_ld_dyn "${libdatadog_SOURCE_DIR}/$<LOWER_CASE:$<CONFIG>>/dynamic")

# Tests.exe must find the libdatadog DLL at launch; copy it alongside the
# executable (mirrors the CopyLibDatadogDlls MSBuild target in Tests.vcxproj).
add_custom_command(TARGET Tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "${_ld_dyn}/datadog_profiling_ffi.dll"
        "$<TARGET_FILE_DIR:Tests>/datadog_profiling_ffi.dll"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${_ld_dyn}/datadog_profiling_ffi.pdb"
        "$<TARGET_FILE_DIR:Tests>/datadog_profiling_ffi.pdb"
    VERBATIM
    COMMENT "Copying libdatadog DLLs to Tests output directory"
)

include(CTest)
add_test(NAME Tests COMMAND Tests)
