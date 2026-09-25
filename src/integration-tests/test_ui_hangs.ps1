#Requires -Version 7.0

# Integration test for UIApp UI-hang detection (Sleep / Wait / CPU).
# Does NOT require an API key or network access -- validates local pprof output only.
#
# For each hang kind it launches UIApp.exe in headless automation mode, waits
# for it to exit, then validates the produced pprof with validate_ui_hangs.py:
#   1. UIApp exits cleanly (exit code 0)
#   2. Profiler log contains the final export message
#   3. At least one .pprof file was written
#   4. UIHang=true/false pairs exist with the expected reason stack and
#      durations, and no ordinary wait sample overlaps a detected hang
#
# Prerequisites:
#   - UIApp.exe built (Debug or Release):
#       cmake --build build --config Debug --target UIApp
#   - Python 3 with lz4, protobuf, grpcio-tools installed:
#       pip install -r src/integration-tests/requirements.txt
#
# Usage (local):
#   pwsh src/integration-tests/test_ui_hangs.ps1 -Config Debug -KeepArtifacts
#   pwsh src/integration-tests/test_ui_hangs.ps1 -Config Release
#
# Tip: for a quick visual smoke run of the automation without validation:
#   build/src/UIApp/Debug/UIApp.exe --auto-hang all --hang-duration-ms 1500 `
#     --hang-cycles 1 --pprofdir "$env:TEMP/ui-hangs" --symbolize

[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "Auto")]
    [string]$Config = "Auto",
    [int]$HangDurationMs = 1500,
    [int]$Cycles = 2,
    [string]$OutputRoot = "",
    [switch]$KeepArtifacts
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$uiAppDir  = Join-Path $scriptDir "..\UIApp"

. (Join-Path $scriptDir "rum_test_helpers.ps1")
. (Join-Path $uiAppDir "find-uiapp.ps1")

# Wall-time sampling period (ms). Passed to the profiler and reused as the
# validator's boundary slack unit.
$SamplingMs = 5

Write-Host "`n=== UIApp UI-Hang Integration Test (local) ===" -ForegroundColor Cyan
Write-Host ""

$pythonCmd = Find-Python
$uiApp = Find-UIApp -ScriptDir $scriptDir -Config $Config

# OutputRoot is the parent folder; each run gets its own date_time subfolder so
# repeated runs never clobber one another.
if (-not $OutputRoot) {
    $OutputRoot = Join-Path $env:TEMP "dd-ui-hang-test"
}
$runDir = Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd_HHmmss')
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
Write-Host "Output root : $OutputRoot" -ForegroundColor Gray
Write-Host "Run folder  : $runDir" -ForegroundColor Gray
Write-Host ""

# Runs validate_ui_hangs.py, transparently handling the "py -3" launcher form.
function Invoke-Validator([string]$pprofDir, [string]$kind) {
    $scriptPath = Join-Path $scriptDir "validate_ui_hangs.py"
    $validatorArgs = @(
        $scriptPath,
        "--pprof-dir", $pprofDir,
        "--kind", $kind,
        "--cycles", "$Cycles",
        "--duration-ms", "$HangDurationMs",
        "--sampling-ms", "$SamplingMs"
    )
    if ($pythonCmd -eq "py -3") {
        $output = & py -3 @validatorArgs 2>&1
    } else {
        $output = & $pythonCmd @validatorArgs 2>&1
    }
    $code = $LASTEXITCODE
    $output | ForEach-Object { Write-Host "    $_" }
    return $code
}

$kinds = @("sleep", "wait", "cpu")

# Generous per-process budget: each kind runs Cycles hangs of HangDurationMs
# plus a ~500ms recovery gap, followed by profiler shutdown/export.
$processTimeoutMs = ($Cycles * ($HangDurationMs + 1000)) + 30000

foreach ($kind in $kinds) {
    Write-Host "=== UIApp hang: $kind ===" -ForegroundColor Cyan

    $caseRoot = Join-Path $runDir $kind
    $pprofDir = Join-Path $caseRoot "pprof"
    $logDir   = Join-Path $caseRoot "logs"
    New-Item -ItemType Directory -Force -Path $pprofDir, $logDir | Out-Null

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $uiApp
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = Split-Path -Parent $uiApp

    foreach ($arg in @(
        "--auto-hang", $kind,
        "--hang-duration-ms", "$HangDurationMs",
        "--hang-cycles", "$Cycles",
        "--pprofdir", $pprofDir,
        "--symbolize"
    )) { $psi.ArgumentList.Add($arg) }

    # No backend upload; local debug pprof only. Small sampling period so short
    # hangs still yield samples. CPU profiling only for the CPU kind, so that
    # CPU sampling does not independently reset wait state for Sleep/Wait and
    # make the no-overlap check flaky.
    $psi.Environment["DD_INTERNAL_PROFILING_EXPORT_ENABLED"] = "0"
    $psi.Environment["DD_PROFILING_WALLTIME_ENABLED"] = "1"
    $psi.Environment["DD_INTERNAL_PROFILING_SAMPLING_RATE"] = "$SamplingMs"
    $psi.Environment["DD_INTERNAL_PROFILING_WALLTIME_THREADS_THRESHOLD"] = "64"
    $psi.Environment["DD_TRACE_LOG_DIRECTORY"] = $logDir
    $psi.Environment["DD_PROFILING_CPU_ENABLED"] = if ($kind -eq "cpu") { "1" } else { "0" }

    $proc = [System.Diagnostics.Process]::Start($psi)
    $exited = $proc.WaitForExit($processTimeoutMs)
    if (-not $exited) {
        try { $proc.Kill($true) } catch {}
        Assert $false "[$kind] UIApp exited within ${processTimeoutMs}ms (timed out)"
        continue
    }
    $exitCode = $proc.ExitCode

    Assert ($exitCode -eq 0) "[$kind] UIApp exited with code 0 (got $exitCode)"

    # -- Validate profiler log --------------------------------------------------
    $logFiles = Get-ChildItem -Path $logDir -Filter "DD-InprocProfiler-*.log" -ErrorAction SilentlyContinue
    Assert ($logFiles.Count -gt 0) "[$kind] Profiler log file exists ($($logFiles.Count) found)"
    if ($logFiles.Count -gt 0) {
        $logContent = Get-Content $logFiles[0].FullName -Raw
        $lastExportMatch = $logContent | Select-String "Export last profile #"
        Assert ($null -ne $lastExportMatch) "[$kind] Log contains 'Export last profile #' (final export ran)"
    }

    # -- Validate pprof content -------------------------------------------------
    $pprofFiles = Get-ChildItem -Path $pprofDir -Filter "*.pprof" -ErrorAction SilentlyContinue
    Assert ($pprofFiles.Count -gt 0) "[$kind] At least one .pprof file was written ($($pprofFiles.Count) found)"

    if ($pprofFiles.Count -gt 0) {
        $validatorExit = Invoke-Validator $pprofDir $kind
        Assert ($validatorExit -eq 0) "[$kind] validate_ui_hangs.py passed (exit $validatorExit)"
    }
}

# -- Summary & cleanup --------------------------------------------------------

$failCount = Show-TestSummary

if ($KeepArtifacts -or $failCount -gt 0) {
    Write-Host ""
    Write-Host "Artifacts kept at: $runDir" -ForegroundColor Cyan
} else {
    try {
        Remove-Item -Recurse -Force $runDir -ErrorAction SilentlyContinue
    } catch {
        Write-Host "  WARN: Could not clean up $runDir" -ForegroundColor Yellow
    }
}

exit $failCount
