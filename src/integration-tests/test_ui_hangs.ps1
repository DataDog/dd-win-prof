#Requires -Version 7.0

# Integration test for UIApp UI-hang detection (Sleep / Wait / CPU).
# Does NOT require an API key or network access -- validates local pprof output only.
#
# For each hang kind, plus one two-session profiler lifecycle case, it launches
# UIApp.exe in headless automation mode, waits for it to exit, then validates
# the produced pprof with validate_ui_hangs.py:
#   1. UIApp exits cleanly (exit code 0)
#   2. Profiler log contains the final export message
#   3. At least one .pprof file was written
#   4. UIHang=true/false pairs exist with the expected reason stack and
#      durations, and no ordinary wait sample overlaps a detected hang
#   5. Invalid start/monitor/stop combinations are rejected with warnings
#   6. Each lifecycle session's pair carries its own RUM view
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
function Invoke-Validator(
    [string]$pprofDir,
    [string]$kind,
    [int]$expectedCycles,
    [string[]]$expectedViews
) {
    $scriptPath = Join-Path $scriptDir "validate_ui_hangs.py"
    $validatorArgs = @(
        $scriptPath,
        "--pprof-dir", $pprofDir,
        "--kind", $kind,
        "--cycles", "$expectedCycles",
        "--duration-ms", "$HangDurationMs",
        "--sampling-ms", "$SamplingMs"
    )
    foreach ($view in $expectedViews) {
        $validatorArgs += @("--expected-view", $view)
    }
    if ($pythonCmd -eq "py -3") {
        $output = & py -3 @validatorArgs 2>&1
    } else {
        $output = & $pythonCmd @validatorArgs 2>&1
    }
    $code = $LASTEXITCODE
    $output | ForEach-Object { Write-Host "    $_" }
    return $code
}

$cases = @(
    [pscustomobject]@{ Name = "sleep";     Kind = "sleep"; Cycles = $Cycles; Sessions = 1; Lifecycle = $false; Views = @() },
    [pscustomobject]@{ Name = "wait";      Kind = "wait";  Cycles = $Cycles; Sessions = 1; Lifecycle = $false; Views = @() },
    [pscustomobject]@{ Name = "cpu";       Kind = "cpu";   Cycles = $Cycles; Sessions = 1; Lifecycle = $false; Views = @() },
    [pscustomobject]@{
        Name = "lifecycle"
        Kind = "sleep"
        Cycles = 1
        Sessions = 2
        Lifecycle = $true
        Views = @("ProfilerSession1", "ProfilerSession2")
    }
)

foreach ($case in $cases) {
    $caseName = $case.Name
    $kind = $case.Kind
    $caseCycles = $case.Cycles
    $sessions = $case.Sessions
    $expectedCycles = $caseCycles * $sessions
    $expectedViews = $case.Views

    Write-Host "=== UIApp hang: $caseName ===" -ForegroundColor Cyan

    $caseRoot = Join-Path $runDir $caseName
    $pprofDir = Join-Path $caseRoot "pprof"
    $logDir   = Join-Path $caseRoot "logs"
    New-Item -ItemType Directory -Force -Path $pprofDir, $logDir | Out-Null

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $uiApp
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = Split-Path -Parent $uiApp

    $appArgs = @(
        "--auto-hang", $kind,
        "--hang-duration-ms", "$HangDurationMs",
        "--hang-cycles", "$caseCycles",
        "--pprofdir", $pprofDir,
        "--symbolize"
    )
    if ($case.Lifecycle) {
        $appArgs += "--test-profiler-lifecycle"
    }
    foreach ($arg in $appArgs) { $psi.ArgumentList.Add($arg) }

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
    $psi.Environment["DD_PROFILING_AUTO_START"] = "0"
    foreach ($name in @(
        "DD_API_KEY",
        "DD_TRACE_AGENT_URL",
        "DD_AGENT_HOST",
        "DD_TRACE_AGENT_PORT"
    )) {
        $null = $psi.Environment.Remove($name)
    }

    # Generous per-process budget: each session runs its requested hangs plus
    # warmup/recovery time, followed by profiler shutdown/export.
    $processTimeoutMs =
        ($sessions * (($caseCycles * ($HangDurationMs + 1000)) + 1000)) + 30000
    $proc = [System.Diagnostics.Process]::Start($psi)
    $exited = $proc.WaitForExit($processTimeoutMs)
    if (-not $exited) {
        try { $proc.Kill($true) } catch {}
        Assert $false "[$caseName] UIApp exited within ${processTimeoutMs}ms (timed out)"
        continue
    }
    $exitCode = $proc.ExitCode

    Assert ($exitCode -eq 0) "[$caseName] UIApp exited with code 0 (got $exitCode)"

    # -- Validate profiler log --------------------------------------------------
    $logFiles = Get-ChildItem -Path $logDir -Filter "DD-InprocProfiler-*.log" -ErrorAction SilentlyContinue
    Assert ($logFiles.Count -gt 0) "[$caseName] Profiler log file exists ($($logFiles.Count) found)"
    if ($logFiles.Count -gt 0) {
        $logContent = Get-Content $logFiles[0].FullName -Raw
        $lastExportCount = [regex]::Matches($logContent, "Export last profile #").Count
        Assert ($lastExportCount -ge $sessions) "[$caseName] Final export ran for all $sessions session(s)"

        if ($case.Lifecycle) {
            $notRunningCount = [regex]::Matches($logContent, "Profiler is not running\.").Count
            $monitorWithoutStartCount = [regex]::Matches(
                $logContent,
                "Profiler must be started before monitoring window hangs\."
            ).Count
            $alreadyRunningCount = [regex]::Matches(
                $logContent,
                "Profiler is already running\."
            ).Count

            Assert ($notRunningCount -ge 1) "[$caseName] Stop without start emitted a warning"
            Assert ($monitorWithoutStartCount -ge 2) "[$caseName] Monitor without start and after stop were rejected"
            Assert ($alreadyRunningCount -ge 2) "[$caseName] Repeated start was rejected in both sessions"
        }
    }

    # -- Validate pprof content -------------------------------------------------
    $pprofFiles = Get-ChildItem -Path $pprofDir -Filter "*.pprof" -ErrorAction SilentlyContinue
    Assert ($pprofFiles.Count -ge $sessions) "[$caseName] Each session wrote a .pprof file ($($pprofFiles.Count) found)"

    if ($pprofFiles.Count -gt 0) {
        $validatorExit = Invoke-Validator $pprofDir $kind $expectedCycles $expectedViews
        Assert ($validatorExit -eq 0) "[$caseName] validate_ui_hangs.py passed (exit $validatorExit)"
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
