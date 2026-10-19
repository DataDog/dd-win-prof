# Capture Runner profiles for local inspection or prof-analyze validation.
# Symbolization is on and upload is off unless overridden by loaded env files.
# Scenario settings come from runner-scenarios/scenario_<N>/scenario.env.
#
# Usage:
#   .\capture-pprof.ps1                                      # scenario 1, 3 iterations
#   .\capture-pprof.ps1 -Scenario 5 -Iterations 5
#   .\capture-pprof.ps1 -Config Release -PprofDir C:\tmp\out

[CmdletBinding()]
param(
    [int]$Scenario    = 1,
    [int]$Iterations  = 3,
    [string]$EnvFile  = (Join-Path $PSScriptRoot ".env"),
    [string]$PprofDir = (Join-Path $PSScriptRoot "temp"),
    [ValidateSet("Debug", "Release", "Auto")]
    [string]$Config   = "Debug"
)

$ErrorActionPreference = "Stop"

function Load-EnvFile([string]$path) {
    if (-not (Test-Path $path)) { return }
    Write-Host "Loading env from: $path" -ForegroundColor Cyan
    Get-Content $path | ForEach-Object {
        if ($_ -match '^\s*([^#=][^=]*)=(.*)$') {
            Set-Item -Path "env:$($matches[1].Trim())" -Value $matches[2].Trim()
        }
    }
}

# Defaults that suit local dev and CI: never upload, always symbolize.
# Loaded .env files below can override either if a user really needs to.
$env:DD_INTERNAL_PROFILING_EXPORT_ENABLED       = "0"
$env:DD_PROFILING_INTERNAL_SYMBOLIZE_CALLSTACKS = "1"

# Optional user .env (general overrides for local dev).
Load-EnvFile $EnvFile

# Per-scenario env (RUM IDs, scenario-specific tags). Lives next to the
# scenario's expected_profile.json so adding a new scenario means dropping
# a folder, no script change. Named scenario.env (not .env) because the
# repo's .gitignore drops .env / .env.* by default.
Load-EnvFile (Join-Path $PSScriptRoot "runner-scenarios\scenario_$Scenario\scenario.env")

if (-not (Test-Path $PprofDir)) {
    New-Item -ItemType Directory -Force -Path $PprofDir | Out-Null
}
$PprofDir = (Resolve-Path $PprofDir).Path
Write-Host "Pprof dir: $PprofDir" -ForegroundColor Cyan

$runnerDir = Join-Path $PSScriptRoot "..\src\Runner"
. (Join-Path $runnerDir "find-runner.ps1")
$runner = Find-Runner -ScriptDir $runnerDir -Config $Config

$runnerArgs = @(
    "--scenario",   $Scenario,
    "--iterations", $Iterations,
    "--pprofdir",   $PprofDir
)

Write-Host ""
Write-Host "Running: $runner $($runnerArgs -join ' ')" -ForegroundColor Cyan
Write-Host ""

& $runner @runnerArgs
exit $LASTEXITCODE
