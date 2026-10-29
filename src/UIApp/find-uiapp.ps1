# Shared helper: locate UIApp.exe for the requested build configuration.
# Dot-source this file, then call Find-UIApp.
#
# Usage:
#   . .\find-uiapp.ps1
#   $uiApp = Find-UIApp -ScriptDir $PSScriptRoot -Config "Debug"

function Find-UIApp {
    param(
        [Parameter(Mandatory)]
        [string]$ScriptDir,

        [ValidateSet("Debug", "Release", "Auto")]
        [string]$Config = "Auto"
    )

    if ($Config -eq "Auto") {
        $configs = @("Debug", "Release")
    } else {
        $configs = @($Config)
    }

    $candidates = @()
    foreach ($cfg in $configs) {
        $candidates += (Join-Path $ScriptDir "..\..\build\src\UIApp\$cfg\UIApp.exe")  # CMake (multi-config: VS)
    }
    # Single-config generators (Ninja, Make) drop the binary directly under
    # build/src/UIApp/UIApp.exe with no per-config subdirectory.
    $candidates += (Join-Path $ScriptDir "..\..\build\src\UIApp\UIApp.exe")

    foreach ($path in $candidates) {
        if (Test-Path $path) {
            $resolved = (Resolve-Path $path).Path
            Write-Host "Found UIApp.exe: $resolved ($((Get-Item $resolved).LastWriteTime))" -ForegroundColor Green
            return $resolved
        }
    }

    $searchList = ($candidates | ForEach-Object { "  $_" }) -join "`n"
    Write-Error "UIApp.exe not found. Build it first: cmake --build build --config $Config --target UIApp`nSearched:`n$searchList"
    exit 1
}
