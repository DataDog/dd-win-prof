# The DIA (Debug Interface Access) SDK ships with Visual Studio and is required
# to parse PDB files in ObfSymbols. It is not redistributable, so it must be
# located via CMAKE_GENERATOR_INSTANCE (set automatically by Visual Studio
# generators) or the VSINSTALLDIR environment variable (set by VS Developer
# Command Prompt).
if(CMAKE_GENERATOR_INSTANCE)
    set(VSINSTALLDIR "${CMAKE_GENERATOR_INSTANCE}")
elseif(DEFINED ENV{VSINSTALLDIR})
    set(VSINSTALLDIR "$ENV{VSINSTALLDIR}")
else()
    message(FATAL_ERROR
        "Visual Studio installation directory not found. "
        "Either use a Visual Studio generator or run CMake from a VS Developer Command Prompt."
    )
endif()

find_path(DIA_SDK_INCLUDE
    NAMES dia2.h
    PATHS "${VSINSTALLDIR}/DIA SDK/include"
    NO_DEFAULT_PATH
)
if(NOT DIA_SDK_INCLUDE)
    message(FATAL_ERROR
        "DIA SDK headers not found at ${VSINSTALLDIR}/DIA SDK/include. "
        "Ensure the DIA SDK component is installed with Visual Studio."
    )
endif()

find_library(DIA_SDK_LIB
    NAMES diaguids.lib
    PATHS "${VSINSTALLDIR}/DIA SDK/lib/amd64"
    NO_DEFAULT_PATH
)
if(NOT DIA_SDK_LIB)
    message(FATAL_ERROR
        "DIA SDK import library (diaguids.lib) not found at ${VSINSTALLDIR}/DIA SDK/lib/amd64. "
        "Ensure the DIA SDK component is installed with Visual Studio."
    )
endif()

add_subdirectory(ObfSymbols)
add_subdirectory(TestSymbolsDll)
add_subdirectory(TestSymbols)
