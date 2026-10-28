add_library(dd-win-prof SHARED
    Configuration.cpp
    CpuTimeProvider.cpp
    dd-win-prof.cpp
    dllmain.cpp
    OsSpecificApi.cpp
    OsSysTools.cpp
    Profiler.cpp
    ProfileExporter.cpp
    PprofAggregator.cpp
    Resource.rc
    Sample.cpp
    SamplesCollector.cpp
    SampleValueTypeProvider.cpp
    StackFrameCollector.cpp
    StackSamplerLoop.cpp
    Symbolication.cpp
    TagsHelper.cpp
    ThreadInfo.cpp
    ThreadList.cpp
    Uuid.cpp
    WalltimeProvider.cpp

    # Headers (listed for IDE navigation, not compiled)
    CollectorBase.h
    Configuration.h
    CpuTimeProvider.h
    include/dd-win-prof.h
    include/error.h
    include/internal/dd-win-rum-private.h
    dd-win-prof-internal.h
    EnvironmentVariables.h
    framework.h
    ISamplesProvider.h
    LibDatadogHelper.h
    Log.h
    OpSysTools.h
    OsSpecificApi.h
    pch.h
    PprofAggregator.h
    Profiler.h
    ProfileExporter.h
    ProfilingConstants.h
    RumContext.h
    resource.h
    Sample.h
    SamplesCollector.h
    SampleValueType.h
    SampleValueTypeProvider.h
    ScopedHandle.h
    StackFrameCollector.h
    StackSamplerLoop.h
    Symbolication.h
    TagsHelper.h
    ThreadInfo.h
    ThreadList.h
    Uuid.h
    version.h
    WalltimeProvider.h
)

# pch.cpp is the "Create PCH" translation unit in the MSBuild project; CMake
# generates its own PCH mechanism, so we drive it only through pch.h here.
target_precompile_headers(dd-win-prof PRIVATE pch.h)

# Expose include/, with the SDK bridge under internal/. Implementation headers such
# as Log.h, Uuid.h and version.h remain private.
# Projects exporting this target install the same header tree under <prefix>/include.
target_include_directories(dd-win-prof
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PUBLIC $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(dd-win-prof PRIVATE
    DD_WIN_PROF_EXPORTS
    _WINDOWS
    _USRDLL
    _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING
)

# The libdatadog include directory is propagated automatically via
# dd_win_prof_libdatadog_static's INTERFACE_INCLUDE_DIRECTORIES, and spdlog via
# dd_win_prof_spdlog.

target_link_libraries(dd-win-prof PRIVATE
    dd_win_prof_libdatadog_static
    dd_win_prof_spdlog
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

# UAC elevation prompt is disabled for this DLL (matches EnableUAC=false in the
# vcxproj linker settings).
target_link_options(dd-win-prof PRIVATE /MANIFESTUAC:NO)

# Link-time code generation (whole-program optimisation) in Release builds only.
set_property(TARGET dd-win-prof PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# Stage build outputs into src/reference (in the source tree) for Runner, CI artifact
# upload and the e2e scripts. This writes into the source directory, so it is limited
# to development builds (Runner also needs it) and never happens when dd-win-prof is
# consumed as a dependency.
if(DD_WIN_PROF_DEVELOPMENT OR DD_WIN_PROF_BUILD_RUNNER)
    set(_ref_dir "${WINDOWSPROFILER_ROOT_DIR}/src/reference")
    set(_ld_dyn  "${libdatadog_SOURCE_DIR}/${DD_WIN_PROF_LIBDATADOG_CONFIG_DIR}/dynamic")

    # Preserve the published header tree, including the SDK bridge under internal/.
    add_custom_command(TARGET dd-win-prof PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${_ref_dir}"
        VERBATIM
        COMMENT "Copying profiler headers to src/reference"
    )

    # Publish the DLL, import library, PDB and the libdatadog dynamic DLL/PDB to
    # src/reference so that Runner and developers can copy them as a unit.
    add_custom_command(TARGET dd-win-prof POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:dd-win-prof>"         "${_ref_dir}/dd-win-prof.dll"
        COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_LINKER_FILE:dd-win-prof>"  "${_ref_dir}/dd-win-prof.lib"
        COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_PDB_FILE:dd-win-prof>"     "${_ref_dir}/dd-win-prof.pdb"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${_ld_dyn}/datadog_profiling_ffi.dll" "${_ref_dir}/datadog_profiling_ffi.dll"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${_ld_dyn}/datadog_profiling_ffi.pdb" "${_ref_dir}/datadog_profiling_ffi.pdb"
        VERBATIM
        COMMENT "Copying dd-win-prof binaries to src/reference"
    )
endif()

# Install rules used by the release workflow (cmake --install build --config
# Release --prefix <stage>/dd-win-prof). Mirrors the layout published in the
# release zip: dd-win-prof/Profiler/{dll,lib,pdb,h, datadog_profiling_ffi.{dll,pdb}}.
# Config-aware (see DD_WIN_PROF_LIBDATADOG_CONFIG_DIR) so `--config Debug` would install
# the debug variant; the release workflow only uses Release.
if(DD_WIN_PROF_BUILD_INSTALL)
    install(TARGETS dd-win-prof
        RUNTIME DESTINATION Profiler
        ARCHIVE DESTINATION Profiler
    )
    install(FILES $<TARGET_PDB_FILE:dd-win-prof> DESTINATION Profiler OPTIONAL)
    install(FILES include/dd-win-prof.h include/error.h DESTINATION Profiler)
    install(FILES include/internal/dd-win-rum-private.h DESTINATION Profiler/internal)
    install(FILES
        "${libdatadog_SOURCE_DIR}/${DD_WIN_PROF_LIBDATADOG_CONFIG_DIR}/dynamic/datadog_profiling_ffi.dll"
        "${libdatadog_SOURCE_DIR}/${DD_WIN_PROF_LIBDATADOG_CONFIG_DIR}/dynamic/datadog_profiling_ffi.pdb"
        DESTINATION Profiler
    )
endif()
