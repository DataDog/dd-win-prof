# Capture pprof profiles by running Runner.exe with the chosen scenario, used
# both locally (to bootstrap expected_profile.json files) and in CI (to feed
# the prof-correctness analyzer). Loads DD_* env vars from an optional .env
# file, then invokes Runner with --pprofdir pointing at a local directory.
#
# Switches:
#   -NoExport    Skip the upload path (--noenvvars + fake URL/key), useful in
#                CI where no credentials are present.
#   -Symbolize   Pass --symbolize so frame names land in the captured pprof.
#                Recommended whenever you intend to write or update an
#                expected_profile.json against the result.
#
# Usage:
#   .\capture-pprof.ps1
#   .\capture-pprof.ps1 -Scenario 5 -Iterations 5 -Symbolize
#   .\capture-pprof.ps1 -Config Release -NoExport -Symbolize -PprofDir C:\tmp\scn1

[CmdletBinding()]
param(
    [int]$Scenario    = 1,
    [int]$Iterations  = 3,
    [string]$EnvFile  = (Join-Path $PSScriptRoot ".env"),
    [string]$PprofDir = (Join-Path $PSScriptRoot "temp"),
    [ValidateSet("Debug", "Release", "Auto")]
    [string]$Config   = "Debug",
    [switch]$NoExport,
    [switch]$Symbolize
)

$ErrorActionPreference = "Stop"

# Load .env into the current PowerShell session (KEY=VALUE, skip comments/blank).
if (Test-Path $EnvFile) {
    Write-Host "Loading env from: $EnvFile" -ForegroundColor Cyan
    Get-Content $EnvFile | ForEach-Object {
        if ($_ -match '^\s*([^#=][^=]*)=(.*)$') {
            Set-Item -Path "env:$($matches[1].Trim())" -Value $matches[2].Trim()
        }
    }
} else {
    Write-Host "No .env file at $EnvFile (continuing -- runner will use process env)" -ForegroundColor Yellow
}

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
if ($Symbolize) { $runnerArgs += "--symbolize" }
if ($NoExport)  {
    # --noenvvars forces Runner to ignore env config; --url and --apikey are
    # then mandatory but never actually contacted (intake://0 just no-ops).
    $runnerArgs += @(
        "--noenvvars",
        "--url",    "https://localhost:0/no-upload",
        "--apikey", "correctness-test-key"
    )
}

Write-Host ""
Write-Host "Running: $runner $($runnerArgs -join ' ')" -ForegroundColor Cyan
Write-Host ""

& $runner @runnerArgs
exit $LASTEXITCODE
