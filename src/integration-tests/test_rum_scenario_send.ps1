# Integration test for Runner scenario 5 with backend upload validation.
# Requires a real API key and Datadog agent URL.
#
# Validates everything in test_rum_scenario.ps1 PLUS:
#   - Profile was successfully sent to the backend (HTTP 200 in logs)
#
# Usage:
#   .\test_rum_scenario_send.ps1 -Url http://localhost:8126 -ApiKey <key>
#   .\test_rum_scenario_send.ps1 -Url http://localhost:8126 -ApiKey <key> -Config Release

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Url,

    [Parameter(Mandatory)]
    [string]$ApiKey,

    [ValidateSet("Debug", "Release", "Auto")]
    [string]$Config = "Auto",
    [int]$Iterations = 3
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$runnerDir = Join-Path $scriptDir "..\Runner"

. (Join-Path $scriptDir "rum_test_helpers.ps1")

# -- Check prerequisites ------------------------------------------------------

Write-Host "`n=== RUM Scenario 5 Integration Test (with send) ===" -ForegroundColor Cyan
Write-Host ""

$pythonCmd = Find-Python

. (Join-Path $runnerDir "find-runner.ps1")
$runner = Find-Runner -ScriptDir $runnerDir -Config $Config

# -- Setup temp directories ----------------------------------------------------

$testRoot = Join-Path $env:TEMP "dd-rum-integration-test-send-$(Get-Date -Format 'yyyyMMddHHmmss')"
$pprofDir = Join-Path $testRoot "pprof"
$logDir   = Join-Path $testRoot "logs"

New-Item -ItemType Directory -Force -Path $pprofDir | Out-Null
New-Item -ItemType Directory -Force -Path $logDir   | Out-Null

Write-Host "Pprof dir : $pprofDir" -ForegroundColor Gray
Write-Host "Log dir   : $logDir"   -ForegroundColor Gray
Write-Host ""

# -- Run Runner scenario 5 ----------------------------------------------------

$rumAppId     = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
$rumSessionId = "11111111-2222-3333-4444-555555555555"

$env:DD_TRACE_LOG_DIRECTORY = $logDir

$runnerArgs = @(
    "--scenario",       5,
    "--iterations",     $Iterations,
    "--noenvvars",
    "--url",            $Url,
    "--apikey",         $ApiKey,
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
Write-Host "-- Assertions --" -ForegroundColor Cyan

Assert ($runnerExit -eq 0) "Runner exited with code 0 (got $runnerExit)"

# -- Validate log file --------------------------------------------------------

$logFiles = Get-ChildItem -Path $logDir -Filter "DD-InprocProfiler-*.log" -ErrorAction SilentlyContinue
Assert ($logFiles.Count -gt 0) "Profiler log file exists ($($logFiles.Count) found)"

if ($logFiles.Count -gt 0) {
    $logContent = Get-Content $logFiles[0].FullName -Raw

    $wroteMatch = $logContent | Select-String "Successfully wrote .+ bytes to pprof file"
    Assert ($null -ne $wroteMatch) "Log contains 'Successfully wrote ... bytes to pprof file'"

    $lastExportMatch = $logContent | Select-String "Export last profile #"
    Assert ($null -ne $lastExportMatch) "Log contains 'Export last profile #' (final export ran)"

    # Send validation: check that the profile was accepted by the backend (2xx)
    $sendSuccessMatch = $logContent | Select-String "Successfully sent profile, HTTP 2\d{2}"
    Assert ($null -ne $sendSuccessMatch) "Log contains successful profile send (HTTP 2xx)"

    $sendErrorMatch = $logContent | Select-String "Send profile failed"
    Assert ($null -eq $sendErrorMatch) "No 'Send profile failed' errors in log"
}

# -- Validate pprof content ----------------------------------------------------

$pprofFiles = Get-ChildItem -Path $pprofDir -Filter "*.pprof" -ErrorAction SilentlyContinue
Assert ($pprofFiles.Count -gt 0) "At least one .pprof file was written ($($pprofFiles.Count) found)"

$expectedViewNames = @("HomePage", "SettingsPage", "ProfilePage")

$allSamples = @()

foreach ($pprofFile in $pprofFiles) {
    $jsonOutput = Invoke-PprofLabels $scriptDir $pythonCmd $pprofFile.FullName
    if ($null -eq $jsonOutput) {
        Write-Host "  WARN: Failed to parse $($pprofFile.Name)" -ForegroundColor Yellow
        continue
    }
    $samples = $jsonOutput | ConvertFrom-Json
    $allSamples += $samples
}

Assert ($allSamples.Count -gt 0) "Parsed samples from pprof files ($($allSamples.Count) total)"

foreach ($viewName in $expectedViewNames) {
    $matching = $allSamples | Where-Object {
        $_.labels.'trace endpoint' -eq $viewName -and
        $_.labels.'rum.view_id' -ne $null -and
        $_.labels.'rum.view_id' -ne ''
    }
    Assert ($matching.Count -gt 0) `
        "Found samples with trace endpoint=$viewName and a non-empty rum.view_id ($($matching.Count) samples)"
}

$noViewSamples = $allSamples | Where-Object {
    -not $_.labels.PSObject.Properties['rum.view_id']
}
Assert ($noViewSamples.Count -gt 0) `
    "Found samples without rum.view_id (no active view periods) ($($noViewSamples.Count) samples)"

# -- Validate .rum-views.json content ------------------------------------------

$rumSessionId2 = "99999999-2222-3333-4444-555555555555"

$jsonFiles = Get-ChildItem -Path $pprofDir -Filter "*.rum-views.json" -ErrorAction SilentlyContinue
Assert ($jsonFiles.Count -gt 0) "At least one .rum-views.json file was written ($($jsonFiles.Count) found)"

$allViewEntries = @()
$allSessionEntries = @()

foreach ($jsonFile in $jsonFiles) {
    $rumData = Get-Content $jsonFile.FullName -Raw | ConvertFrom-Json
    if ($null -ne $rumData.views) {
        $allViewEntries += $rumData.views
    }
    if ($null -ne $rumData.sessions) {
        $allSessionEntries += $rumData.sessions
    }
}

$expectedViewCount = 3 * $Iterations
Assert ($allViewEntries.Count -eq $expectedViewCount) `
    "Total view entries = $expectedViewCount ($($allViewEntries.Count) found)"

foreach ($entry in $allViewEntries) {
    Assert ($entry.startClocks.relative -eq 0) `
        "startClocks.relative == 0 for viewId=$($entry.viewId)"
    Assert ($entry.startClocks.timeStamp -gt 0) `
        "startClocks.timeStamp > 0 for viewId=$($entry.viewId) (got $($entry.startClocks.timeStamp))"
    Assert ($entry.duration -gt 0) `
        "duration > 0 for viewId=$($entry.viewId) (got $($entry.duration))"
    Assert ($null -ne $entry.vitals) `
        "vitals object present for viewId=$($entry.viewId)"
    if ($null -ne $entry.vitals) {
        Assert ($entry.vitals.cpuTimeNs -ge 0) `
            "vitals.cpuTimeNs >= 0 for viewId=$($entry.viewId) (got $($entry.vitals.cpuTimeNs))"
        Assert ($entry.vitals.waitTimeNs -ge 0) `
            "vitals.waitTimeNs >= 0 for viewId=$($entry.viewId) (got $($entry.vitals.waitTimeNs))"
    }
}

# At least one view should have accumulated some CPU time (each view spins for 2s)
$viewsWithCpu = $allViewEntries | Where-Object { $null -ne $_.vitals -and $_.vitals.cpuTimeNs -gt 0 }
Assert ($viewsWithCpu.Count -gt 0) `
    "At least one view has positive vitals.cpuTimeNs ($($viewsWithCpu.Count) found)"

foreach ($viewName in $expectedViewNames) {
    $matching = $allViewEntries | Where-Object {
        $_.viewName -eq $viewName -and
        $_.viewId -ne $null -and $_.viewId -ne ''
    }
    Assert ($matching.Count -ge $Iterations) `
        "Found $($matching.Count) entries for viewName=$viewName with non-empty viewId (expected >= $Iterations)"
}

# -- Validate session records --------------------------------------------------

Assert ($allSessionEntries.Count -gt 0) `
    "At least one session record found ($($allSessionEntries.Count) total)"

$s1Records = $allSessionEntries | Where-Object { $_.sessionId -eq $rumSessionId }
Assert ($s1Records.Count -ge $Iterations) `
    "Session S1 ($rumSessionId) has completed records ($($s1Records.Count) found, expected >= $Iterations)"

foreach ($s1 in $s1Records) {
    Assert ($s1.duration -gt 0) `
        "Session S1 record has positive duration (got $($s1.duration))"
    Assert ($s1.startClocks.timeStamp -gt 0) `
        "Session S1 record has positive timestamp (got $($s1.startClocks.timeStamp))"
}

$s2Records = $allSessionEntries | Where-Object { $_.sessionId -eq $rumSessionId2 }
Assert ($s2Records.Count -ge 2) `
    "Session S2 ($rumSessionId2) has at least 2 completed records ($($s2Records.Count) found)"

foreach ($s2 in $s2Records) {
    Assert ($s2.duration -gt 0) `
        "Session S2 record has positive duration (got $($s2.duration))"
    Assert ($s2.startClocks.timeStamp -gt 0) `
        "Session S2 record has positive timestamp (got $($s2.startClocks.timeStamp))"
}

# -- Summary & cleanup --------------------------------------------------------

$failCount = Show-TestSummary

try {
    Remove-Item -Recurse -Force $testRoot -ErrorAction SilentlyContinue
} catch {
    Write-Host "  WARN: Could not clean up $testRoot" -ForegroundColor Yellow
}

exit $failCount
