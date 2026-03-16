# Shared helper: locate Runner.exe for the requested build configuration.
# Dot-source this file, then call Find-Runner.
#
# Usage:
#   . .\find-runner.ps1
#   $runner = Find-Runner -ScriptDir $PSScriptRoot -Config "Debug"

function Find-Runner {
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
        $candidates += (Join-Path $ScriptDir "x64\$cfg\Runner.exe")          # MSBuild output: src/Runner/x64/<cfg>/
        $candidates += (Join-Path $ScriptDir "..\x64\$cfg\Runner.exe")       # legacy: src/x64/<cfg>/
        $candidates += (Join-Path $ScriptDir "..\..\build\src\Runner\$cfg\Runner.exe")  # CMake output
    }

    foreach ($path in $candidates) {
        if (Test-Path $path) {
            $resolved = (Resolve-Path $path).Path
            Write-Host "Found Runner.exe: $resolved ($((Get-Item $resolved).LastWriteTime))" -ForegroundColor Green
            return $resolved
        }
    }

    $searchList = ($candidates | ForEach-Object { "  $_" }) -join "`n"
    Write-Error "Runner.exe not found. Build the project first.`nSearched:`n$searchList"
    exit 1
}
