@echo off
REM Build script for Symbols solution
REM Usage: build.bat [Debug|Release] [x64|x86] [clean]

setlocal

REM Set defaults
set CONFIG=%1
set PLATFORM=%2
set CLEAN=%3

if "%CONFIG%"=="" set CONFIG=Debug
if "%PLATFORM%"=="" set PLATFORM=x64

REM Validate configuration
if /i not "%CONFIG%"=="Debug" if /i not "%CONFIG%"=="Release" (
    echo ERROR: Configuration must be Debug or Release
    echo Usage: build.bat [Debug^|Release] [x64^|x86] [clean]
    exit /b 1
)

REM Validate platform
if /i not "%PLATFORM%"=="x64" if /i not "%PLATFORM%"=="x86" (
    echo ERROR: Platform must be x64 or x86
    echo Usage: build.bat [Debug^|Release] [x64^|x86] [clean]
    exit /b 1
)

echo ========================================
echo Building Symbols Solution
echo Configuration: %CONFIG%
echo Platform: %PLATFORM%
echo ========================================
echo.

REM Initialize Visual Studio environment
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat" -arch=amd64

if errorlevel 1 (
    echo ERROR: Failed to initialize Visual Studio environment
    exit /b 1
)

REM Determine build target
set TARGET=Build
if /i "%CLEAN%"=="clean" set TARGET=Rebuild

echo Running MSBuild (%TARGET%)...
echo.

REM Build the project
set PROJECT=ObfSymbols\ObfSymbols.vcxproj

REM Run MSBuild
msbuild %PROJECT% /t:%TARGET% /p:Configuration=%CONFIG% /p:Platform=%PLATFORM% /v:minimal

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
echo Output: ObfSymbols\%PLATFORM%\%CONFIG%\ObfSymbols.exe
echo.
echo Usage: .\ObfSymbols\%PLATFORM%\%CONFIG%\ObfSymbols.exe --pdb ^<pdb_file^> --out ^<output_file^> [--obf ^<obfuscated_output_file^>]
echo   If --obf is not specified, the obfuscated file will be auto-generated

endlocal

