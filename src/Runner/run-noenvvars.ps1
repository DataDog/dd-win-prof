# Run the profiler Runner in --noenvvars mode.
# All environment variables are ignored; configuration comes entirely from
# the command-line flags (mapped to ProfilerConfig fields).
# -Url and -ApiKey are mandatory in this mode.
#
# Usage:
#   .\run-noenvvars.ps1 -Url https://intake.datadoghq.com -ApiKey <YOUR_KEY>
#   .\run-noenvvars.ps1 -Config Release -Url https://intake.datadoghq.com -ApiKey <YOUR_KEY>
#   .\run-noenvvars.ps1 -Url https://intake.datadoghq.com -ApiKey <YOUR_KEY> `
#       -Name myApp -Env staging -Version 2.0 -Tags "team:backend,region:us"
#   .\run-noenvvars.ps1 -Url https://intake.datadoghq.com -ApiKey <YOUR_KEY> `
#       -PprofDir C:\temp\pprof

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [string]$Url,

    [Parameter(Mandatory=$true)]
    [string]$ApiKey,

    [ValidateSet("Debug", "Release", "Auto")]
    [string]$Config       = "Auto",
    [int]$Scenario        = 1,
    [int]$Iterations      = 3,
    [string]$Name         = "",
    [string]$Env          = "",
    [string]$Version      = "",
    [string]$Tags         = "",
    [string]$PprofDir     = "",
    [string]$RumAppId     = "",
    [string]$RumSessionId = "",
    [switch]$Symbolize
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

. (Join-Path $scriptDir "find-runner.ps1")
$runner = Find-Runner -ScriptDir $scriptDir -Config $Config

$args = @(
    "--scenario",   $Scenario,
    "--iterations", $Iterations,
    "--noenvvars",
    "--url",        $Url,
    "--apikey",     $ApiKey
)

if ($Name)         { $args += "--name",           $Name }
if ($Env)          { $args += "--env",            $Env }
if ($Version)      { $args += "--version",        $Version }
if ($Tags)         { $args += "--tags",           $Tags }
if ($PprofDir)     { $args += "--pprofdir",       $PprofDir }
if ($RumAppId)     { $args += "--rum-app-id",     $RumAppId }
if ($RumSessionId) { $args += "--rum-session-id", $RumSessionId }
if ($Symbolize)    { $args += "--symbolize" }

Write-Host "Running (noenvvars): $runner $($args -join ' ')" -ForegroundColor Yellow
& $runner @args
exit $LASTEXITCODE
