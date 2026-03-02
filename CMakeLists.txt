cmake_minimum_required(VERSION 3.21)
project(WindowsProfiler LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(UNICODE _UNICODE)

if(MSVC)
    # /Zi: produce PDB in all configs (matches vcxproj GenerateDebugInformation=true
    # for Debug, DebugFull for Release)
    add_compile_options(/W3 /sdl /permissive- /Zi)
    add_compile_options(
        $<$<CONFIG:Release>:/Gy>
        $<$<CONFIG:Release>:/Oi>
    )
    # /DEBUG:FULL for Release configs (matches DebugFull in vcxproj); /DEBUG for Debug.
    # /OPT:REF and /OPT:ICF trim unreferenced code and duplicate COMDATs in Release.
    add_link_options(
        $<$<CONFIG:Debug>:/DEBUG>
        $<$<CONFIG:Release>:/DEBUG:FULL>
        $<$<CONFIG:MinSizeRel>:/DEBUG:FULL>
        $<$<CONFIG:RelWithDebInfo>:/DEBUG:FULL>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()

add_subdirectory(src)
add_subdirectory(obfuscation)
