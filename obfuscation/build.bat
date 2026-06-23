@echo off
REM Build script for ObfSymbols using CMake
REM Usage: build.bat [Debug|Release] [clean]

setlocal

set CONFIG=%1
set CLEAN=%2

if "%CONFIG%"=="" set CONFIG=Debug

REM Validate configuration
if /i not "%CONFIG%"=="Debug" if /i not "%CONFIG%"=="Release" (
    echo ERROR: Configuration must be Debug or Release
    echo Usage: build.bat [Debug^|Release] [clean]
    exit /b 1
)

echo ========================================
echo Building ObfSymbols (CMake)
echo Configuration: %CONFIG%
echo ========================================
echo.

set PROJECT_ROOT=%~dp0..
set BUILD_DIR=%PROJECT_ROOT%\build

if /i "%CLEAN%"=="clean" (
    if exist "%BUILD_DIR%" (
        echo Cleaning build directory...
        rmdir /s /q "%BUILD_DIR%"
    )
)

REM Configure if needed
if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo Configuring CMake...
    cmake -G "Visual Studio 17 2022" -A x64 -B "%BUILD_DIR%" -S "%PROJECT_ROOT%" -DDD_WIN_PROF_BUILD_OBFUSCATION=ON
    if errorlevel 1 (
        echo ERROR: CMake configure failed
        exit /b 1
    )
)

echo Building ObfSymbols...
cmake --build "%BUILD_DIR%" --config %CONFIG% --target ObfSymbols

if errorlevel 1 (
    echo.
    echo ========================================
    echo BUILD FAILED!
    echo ========================================
    exit /b 1
)

echo.
echo ========================================
echo BUILD SUCCEEDED!
echo ========================================
echo.
echo Output: %BUILD_DIR%\obfuscation\ObfSymbols\%CONFIG%\ObfSymbols.exe
echo.
echo Usage: %BUILD_DIR%\obfuscation\ObfSymbols\%CONFIG%\ObfSymbols.exe --pdb ^<pdb_file^> --out ^<output_file^> [--obf ^<obfuscated_output_file^>]
echo   If --obf is not specified, the obfuscated file will be auto-generated

endlocal
