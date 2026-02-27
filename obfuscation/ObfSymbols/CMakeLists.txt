add_executable(ObfSymbols
    ObfSymbols.cpp
    PdbParser.cpp
    ObfSymbols.rc
)

# DIA_SDK_INCLUDE and DIA_SDK_LIB are discovered in the parent CMakeLists.txt.
target_include_directories(ObfSymbols PRIVATE "${DIA_SDK_INCLUDE}")
target_link_libraries(ObfSymbols PRIVATE "${DIA_SDK_LIB}")
