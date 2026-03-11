# Run the profiler Runner in --noenvvars mode for local debugging.
# Profiles are written to a local directory instead of being uploaded.
# No Datadog backend is contacted.
#
# NOTE: Log files are always written to the location set by DD_TRACE_LOG_DIRECTORY
#       (default: %PROGRAMDATA%\Datadog Tracer\logs). This is determined at DLL load
#       time and cannot be overridden via ProfilerConfig.
#
# Usage:
#   .\run-local-debug.ps1
#   .\run-local-debug.ps1 -Scenario 3 -Iterations 5
#   .\run-local-debug.ps1 -Config Release
#   .\run-local-debug.ps1 -OutputRoot D:\profiler-output
#   .\run-local-debug.ps1 -Name dd-win-prof -Env local -Tags "team:profiling"

[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "Auto")]
    [string]$Config    = "Auto",
    [int]$Scenario     = 1,
    [int]$Iterations   = 3,
    [string]$Name      = "local-debug-runner",
    [string]$Env       = "local",
    [string]$Version   = "0.0.0",
    [string]$Tags      = "",
    [string]$OutputRoot = "",
    [switch]$Symbolize
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

. (Join-Path $scriptDir "find-runner.ps1")
$runner = Find-Runner -ScriptDir $scriptDir -Config $Config

if (-not $OutputRoot) {
    $OutputRoot = Join-Path $env:TEMP "dd-profiler-debug"
}

$pprofDir = Join-Path $OutputRoot "pprof"

New-Item -ItemType Directory -Force -Path $pprofDir | Out-Null

Write-Host "Pprof dir   : $pprofDir"    -ForegroundColor Cyan
Write-Host ""

# Use a placeholder URL/apikey -- SetupProfiler requires them with --noenvvars
# but no upload will happen because pprofOutputDirectory redirects output locally.
$args = @(
    "--scenario",   $Scenario,
    "--iterations", $Iterations,
    "--noenvvars",
    "--url",        "https://localhost:0/no-upload",
    "--apikey",     "local-debug-key",
    "--name",       $Name,
    "--env",        $Env,
    "--version",    $Version,
    "--pprofdir",   $pprofDir
)

if ($Tags)      { $args += "--tags", $Tags }
if ($Symbolize) { $args += "--symbolize" }

Write-Host "Running (local debug): $runner $($args -join ' ')" -ForegroundColor Yellow
& $runner @args
$exitCode = $LASTEXITCODE

Write-Host ""
Write-Host "Pprof files:" -ForegroundColor Cyan
Get-ChildItem -Path $pprofDir -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $_" }

exit $exitCode
