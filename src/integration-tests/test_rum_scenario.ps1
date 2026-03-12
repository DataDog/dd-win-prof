# Integration test for Runner scenario 5 (RUM context view transitions).
#
# Validates:
#   1. Runner exits cleanly
#   2. The profiler log contains successful pprof write messages
#   3. The pprof file(s) contain expected per-sample RUM labels
#
# Prerequisites:
#   - Runner.exe built (Debug or Release)
#   - Python 3 available via 'py -3' with lz4, protobuf, grpcio-tools installed
#     (run: py -3 -m pip install -r requirements.txt)
#
# Usage:
#   .\test_rum_scenario.ps1
#   .\test_rum_scenario.ps1 -Config Release
#   .\test_rum_scenario.ps1 -Iterations 2

[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "Auto")]
    [string]$Config = "Auto",
    [int]$Iterations = 2
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$runnerDir = Join-Path $scriptDir "..\Runner"

# ── Helpers ──────────────────────────────────────────────────────────────────

$script:passed = 0
$script:failed = 0
$script:failures = @()

function Assert($condition, [string]$message) {
    if ($condition) {
        Write-Host "  PASS: $message" -ForegroundColor Green
        $script:passed++
    } else {
        Write-Host "  FAIL: $message" -ForegroundColor Red
        $script:failed++
        $script:failures += $message
    }
}

# ── Check prerequisites ─────────────────────────────────────────────────────

Write-Host "`n=== RUM Scenario 5 Integration Test ===" -ForegroundColor Cyan
Write-Host ""

# Python
try {
    $pyVer = & py -3 --version 2>&1
    Write-Host "Python: $pyVer" -ForegroundColor Gray
} catch {
    Write-Host "ERROR: Python 3 not found via 'py -3'. Install Python 3 first." -ForegroundColor Red
    exit 1
}

# Runner.exe
. (Join-Path $runnerDir "find-runner.ps1")
$runner = Find-Runner -ScriptDir $runnerDir -Config $Config

# ── Setup temp directories ───────────────────────────────────────────────────

$testRoot = Join-Path $env:TEMP "dd-rum-integration-test-$(Get-Date -Format 'yyyyMMddHHmmss')"
$pprofDir = Join-Path $testRoot "pprof"
$logDir   = Join-Path $testRoot "logs"

New-Item -ItemType Directory -Force -Path $pprofDir | Out-Null
New-Item -ItemType Directory -Force -Path $logDir   | Out-Null

Write-Host "Pprof dir : $pprofDir" -ForegroundColor Gray
Write-Host "Log dir   : $logDir"   -ForegroundColor Gray
Write-Host ""

# ── Run Runner scenario 5 ───────────────────────────────────────────────────

$rumAppId     = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
$rumSessionId = "11111111-2222-3333-4444-555555555555"

$env:DD_TRACE_LOG_DIRECTORY = $logDir

$runnerArgs = @(
    "--scenario",       5,
    "--iterations",     $Iterations,
    "--noenvvars",
    "--url",            "https://localhost:0/no-upload",
    "--apikey",         "integration-test-key",
    "--pprofdir",       $pprofDir,
    "--rum-app-id",     $rumAppId,
    "--rum-session-id", $rumSessionId
)

Write-Host "Running: $runner $($runnerArgs -join ' ')" -ForegroundColor Yellow
Write-Host ""

& $runner @runnerArgs
$runnerExit = $LASTEXITCODE

Remove-Item Env:\DD_TRACE_LOG_DIRECTORY -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "── Assertions ──" -ForegroundColor Cyan

Assert ($runnerExit -eq 0) "Runner exited with code 0 (got $runnerExit)"

# ── Validate log file ───────────────────────────────────────────────────────

$logFiles = Get-ChildItem -Path $logDir -Filter "DD-InprocProfiler-*.log" -ErrorAction SilentlyContinue
Assert ($logFiles.Count -gt 0) "Profiler log file exists ($($logFiles.Count) found)"

if ($logFiles.Count -gt 0) {
    $logContent = Get-Content $logFiles[0].FullName -Raw

    $wroteMatch = $logContent | Select-String "Successfully wrote .+ bytes to pprof file"
    Assert ($null -ne $wroteMatch) "Log contains 'Successfully wrote ... bytes to pprof file'"

    $lastExportMatch = $logContent | Select-String "Export last profile #"
    Assert ($null -ne $lastExportMatch) "Log contains 'Export last profile #' (final export ran)"
}

# ── Validate pprof content ──────────────────────────────────────────────────

$pprofFiles = Get-ChildItem -Path $pprofDir -Filter "*.pprof" -ErrorAction SilentlyContinue
Assert ($pprofFiles.Count -gt 0) "At least one .pprof file was written ($($pprofFiles.Count) found)"

# Expected views from RunRumScenario in Runner.cpp
$expectedViews = @(
    @{ ViewId = "11111111-1111-1111-1111-111111111111"; ViewName = "HomePage" },
    @{ ViewId = "22222222-2222-2222-2222-222222222222"; ViewName = "SettingsPage" },
    @{ ViewId = "33333333-3333-3333-3333-333333333333"; ViewName = "ProfilePage" }
)

$allSamples = @()

foreach ($pprofFile in $pprofFiles) {
    $jsonOutput = & py -3 (Join-Path $scriptDir "pprof_utils.py") labels $pprofFile.FullName 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  WARN: Failed to parse $($pprofFile.Name)" -ForegroundColor Yellow
        continue
    }
    $samples = $jsonOutput | ConvertFrom-Json
    $allSamples += $samples
}

Assert ($allSamples.Count -gt 0) "Parsed samples from pprof files ($($allSamples.Count) total)"

foreach ($view in $expectedViews) {
    $matching = $allSamples | Where-Object {
        $_.labels.'rum.view_id' -eq $view.ViewId -and
        $_.labels.'trace endpoint' -eq $view.ViewName
    }
    Assert ($matching.Count -gt 0) `
        "Found samples with rum.view_id=$($view.ViewId) and trace endpoint=$($view.ViewName) ($($matching.Count) samples)"
}

$noViewSamples = $allSamples | Where-Object {
    -not $_.labels.PSObject.Properties['rum.view_id']
}
Assert ($noViewSamples.Count -gt 0) `
    "Found samples without rum.view_id (no active view periods) ($($noViewSamples.Count) samples)"

# ── Summary ──────────────────────────────────────────────────────────────────

Write-Host ""
Write-Host "── Results ──" -ForegroundColor Cyan
Write-Host "  Passed: $($script:passed)" -ForegroundColor Green
Write-Host "  Failed: $($script:failed)" -ForegroundColor $(if ($script:failed -gt 0) { "Red" } else { "Green" })

if ($script:failed -gt 0) {
    Write-Host ""
    Write-Host "Failures:" -ForegroundColor Red
    foreach ($f in $script:failures) {
        Write-Host "  - $f" -ForegroundColor Red
    }
}

# ── Cleanup ──────────────────────────────────────────────────────────────────

try {
    Remove-Item -Recurse -Force $testRoot -ErrorAction SilentlyContinue
} catch {
    Write-Host "  WARN: Could not clean up $testRoot" -ForegroundColor Yellow
}

Write-Host ""
if ($script:failed -gt 0) {
    Write-Host "FAILED" -ForegroundColor Red
    exit 1
} else {
    Write-Host "ALL PASSED" -ForegroundColor Green
    exit 0
}
