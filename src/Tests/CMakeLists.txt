add_executable(Tests
    main.cpp
    ConfigurationTests.cpp
    CpuOverlapTests.cpp
    DynamicModuleTests.cpp
    LifecycleDiagnosticTests.cpp
    PprofAggregatorTests.cpp
    ProfileExporterTests.cpp
    ProfilingDataCallbackTests.cpp
    ProfilingErrorMessageTests.cpp
    RumContextTests.cpp
    SymbolicationTests.cpp
    ThreadListTests.cpp
    UuidTests.cpp
    pch.h
    targetver.h

    # These source files are compiled directly into Tests (rather than linking
    # against dd-win-prof.dll) so that unit tests can exercise internal
    # implementation details.
    ../dd-win-prof/Configuration.cpp
    ../dd-win-prof/CpuTimeProvider.cpp
    ../dd-win-prof/dd-win-prof.cpp
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
target_compile_definitions(Tests PRIVATE DD_WIN_PROF_STATIC_LINK)

# The dd-win-prof shared sources include headers by short name relative to their
# own directory; adding that directory ensures they resolve correctly when
# compiled as part of this target (include/ holds the public dd-win-prof.h).
# libdatadog and spdlog headers are propagated via dd_win_prof_libdatadog_dynamic
# and dd_win_prof_spdlog INTERFACE_INCLUDE_DIRECTORIES.
target_include_directories(Tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../dd-win-prof"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dd-win-prof/include"
)

# Tests links the dynamic FFI library so that profiling API calls go through
# the DLL ABI boundary, mirroring how the profiler is consumed at runtime.
target_link_libraries(Tests PRIVATE
    dd_win_prof_libdatadog_dynamic
    dd_win_prof_spdlog
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

set(_ld_dyn "${libdatadog_SOURCE_DIR}/${DD_WIN_PROF_LIBDATADOG_CONFIG_DIR}/dynamic")

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

# When building with /fsanitize=address, Tests.exe depends on the MSVC ASan
# runtime DLL, which lives inside the Visual Studio install and is not on PATH
# for arbitrary shells. Helper drops it next to the exe (no-op otherwise).
dd_win_prof_copy_asan_runtime(Tests)

include(CTest)
add_test(NAME Tests COMMAND Tests)
set_tests_properties(Tests PROPERTIES TIMEOUT 180)

# Check lifecycle results and static messages with independently selected CRTs.
foreach(_crt IN ITEMS MT MD)
    set(_consumer "LifecycleDll${_crt}")
    add_executable(${_consumer} LifecycleDllTests.cpp)
    target_link_libraries(${_consumer} PRIVATE dd-win-prof)
    set_property(TARGET ${_consumer} PROPERTY RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${_consumer}")
    if(_crt STREQUAL "MT")
        set_property(TARGET ${_consumer} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        set_property(TARGET ${_consumer} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
    add_custom_target(${_consumer}Runtime
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_consumer}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:dd-win-prof>" "$<TARGET_FILE_DIR:${_consumer}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_ld_dyn}/datadog_profiling_ffi.dll" "$<TARGET_FILE_DIR:${_consumer}>"
        DEPENDS dd-win-prof
        VERBATIM
    )
    add_dependencies(${_consumer} ${_consumer}Runtime)
    dd_win_prof_copy_asan_runtime(${_consumer})
    add_test(NAME ${_consumer} COMMAND ${_consumer})
    set_tests_properties(${_consumer} PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "DD_PROFILING_AUTO_START=0;DD_PROFILING_ENABLED=1;DD_INTERNAL_PROFILING_EXPORT_ENABLED=0;DD_API_KEY=;DD_TRACE_AGENT_URL=;DD_AGENT_HOST=localhost;DD_TRACE_AGENT_PORT=8126"
    )
endforeach()
