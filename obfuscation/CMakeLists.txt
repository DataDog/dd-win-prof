# The DIA (Debug Interface Access) SDK ships with Visual Studio and is required
# to parse PDB files in ObfSymbols. It is not redistributable, so it must be
# located via the VSINSTALLDIR environment variable that the VS Developer Command
# Prompt sets. Running cmake from an ordinary shell without VS tools on PATH will
# cause these find_* calls to fail with the messages below.
find_path(DIA_SDK_INCLUDE
    NAMES dia2.h
    PATHS "$ENV{VSINSTALLDIR}/DIA SDK/include"
    NO_DEFAULT_PATH
)
if(NOT DIA_SDK_INCLUDE)
    message(FATAL_ERROR
        "DIA SDK headers not found. "
        "Run CMake from a Visual Studio Developer Command Prompt so that "
        "VSINSTALLDIR is set (e.g. C:\\Program Files\\Microsoft Visual Studio\\2022\\Community)."
    )
endif()

find_library(DIA_SDK_LIB
    NAMES diaguids.lib
    PATHS "$ENV{VSINSTALLDIR}/DIA SDK/lib/amd64"
    NO_DEFAULT_PATH
)
if(NOT DIA_SDK_LIB)
    message(FATAL_ERROR
        "DIA SDK import library (diaguids.lib) not found. "
        "Run CMake from a Visual Studio Developer Command Prompt so that "
        "VSINSTALLDIR is set, and ensure the DIA SDK component is installed."
    )
endif()

add_subdirectory(ObfSymbols)
add_subdirectory(TestSymbolsDll)
add_subdirectory(TestSymbols)
