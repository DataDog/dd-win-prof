add_library(TestSymbolsDll SHARED
    TestSymbolsDll.cpp
)

target_compile_definitions(TestSymbolsDll PRIVATE
    TESTSYMBOLS_DLL_EXPORTS
)

# The vcxproj enables link-time code generation in Release so the DLL's symbol
# table is representative of an optimised binary — which is the scenario
# ObfSymbols is designed to process.
set_property(TARGET TestSymbolsDll PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
