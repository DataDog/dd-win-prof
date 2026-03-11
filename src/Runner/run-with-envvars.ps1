# Run the profiler Runner using environment variables (default mode).
# Environment variables (DD_AGENT_HOST, DD_API_KEY, DD_ENV, DD_SERVICE, etc.)
# are read by the profiler as usual.
#
# Usage:
#   .\run-with-envvars.ps1                           # defaults: scenario 1, 3 iterations
#   .\run-with-envvars.ps1 -Scenario 3 -Iterations 5
#   .\run-with-envvars.ps1 -Config Release
#   .\run-with-envvars.ps1 -Name dd-win-prof -Env staging -Version 1.0
#   .\run-with-envvars.ps1 -PprofDir C:\temp\pprof -Name dd-win-prof

[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "Auto")]
    [string]$Config    = "Auto",
    [int]$Scenario     = 1,
    [int]$Iterations   = 3,
    [string]$Name      = "",
    [string]$Env       = "",
    [string]$Version   = "",
    [string]$Tags      = "",
    [string]$PprofDir  = "",
    [switch]$Symbolize
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

. (Join-Path $scriptDir "find-runner.ps1")
$runner = Find-Runner -ScriptDir $scriptDir -Config $Config

$args = @("--scenario", $Scenario, "--iterations", $Iterations)

if ($Name)      { $args += "--name",     $Name }
if ($Env)       { $args += "--env",      $Env }
if ($Version)   { $args += "--version",  $Version }
if ($Tags)      { $args += "--tags",     $Tags }
if ($PprofDir)  { $args += "--pprofdir", $PprofDir }
if ($Symbolize) { $args += "--symbolize" }

Write-Host "Running: $runner $($args -join ' ')" -ForegroundColor Cyan
& $runner @args
exit $LASTEXITCODE
