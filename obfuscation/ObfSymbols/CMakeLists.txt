add_executable(ObfSymbols
    ObfSymbols.cpp
    PdbParser.cpp
    ObfSymbols.rc
    PdbParser.h
    resource.h
)

# DIA_SDK_INCLUDE and DIA_SDK_LIB are discovered in the parent CMakeLists.txt.
target_include_directories(ObfSymbols PRIVATE "${DIA_SDK_INCLUDE}")
target_link_libraries(ObfSymbols PRIVATE "${DIA_SDK_LIB}")

# Link-time code generation (whole-program optimisation) in Release builds only.
set_property(TARGET ObfSymbols PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# Install rule used by the release workflow. Lands under dd-win-prof/Obfuscation/
# to match the layout shipped in the release zip (and v0.2.0 by hand).
install(TARGETS ObfSymbols RUNTIME DESTINATION Obfuscation)
