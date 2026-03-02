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
)

# pch.cpp is the "Create PCH" translation unit in the MSBuild project; CMake
# generates its own PCH mechanism, so we drive it only through pch.h here.
target_precompile_headers(dd-win-prof PRIVATE pch.h)

target_compile_definitions(dd-win-prof PRIVATE
    DD_WIN_PROF_EXPORTS
    _WINDOWS
    _USRDLL
    _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING
)

# The libdatadog include directory is propagated automatically via
# libdatadog_static's INTERFACE_INCLUDE_DIRECTORIES, and spdlog via spdlog_includes.

target_link_libraries(dd-win-prof PRIVATE
    libdatadog_static
    spdlog_includes
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

set(_ref_dir "${WINDOWSPROFILER_ROOT_DIR}/src/reference")
set(_ld_dyn  "${libdatadog_SOURCE_DIR}/$<LOWER_CASE:$<CONFIG>>/dynamic")

# Copy the public header before compilation so that Runner (and any other
# consumer) always has a current copy of the API surface.
add_custom_command(TARGET dd-win-prof PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_SOURCE_DIR}/dd-win-prof.h"
        "${_ref_dir}/dd-win-prof.h"
    VERBATIM
    COMMENT "Copying dd-win-prof.h to src/reference"
)

# Publish the DLL, import library, PDB and the libdatadog dynamic DLL/PDB to
# src/reference so that Runner and external consumers can copy them as a unit.
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
