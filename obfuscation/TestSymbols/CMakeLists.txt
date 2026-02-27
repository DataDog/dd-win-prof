add_executable(TestSymbols
    TestSymbols.cpp
)

target_link_libraries(TestSymbols PRIVATE TestSymbolsDll)

# On Windows, DLLs must reside in the same directory as the executable (or on
# PATH) to be found at launch. Copy TestSymbolsDll.dll next to TestSymbols.exe
# after each build.
add_custom_command(TARGET TestSymbols POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:TestSymbolsDll>"
        "$<TARGET_FILE_DIR:TestSymbols>/TestSymbolsDll.dll"
    VERBATIM
    COMMENT "Copying TestSymbolsDll.dll to TestSymbols output directory"
)
