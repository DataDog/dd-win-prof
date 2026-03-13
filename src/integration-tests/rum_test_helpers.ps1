# Shared helpers for RUM scenario integration tests.
# Dot-source this file from the test scripts.

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

function Find-Python {
    # CI (actions/setup-python) puts python on PATH directly; local dev uses 'py -3'
    try {
        $ver = & python --version 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-Host "Python: $ver" -ForegroundColor Gray
            return "python"
        }
    } catch {}

    try {
        $ver = & py -3 --version 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-Host "Python: $ver" -ForegroundColor Gray
            return "py -3"
        }
    } catch {}

    Write-Host "ERROR: Python 3 not found. Install Python 3 first." -ForegroundColor Red
    exit 1
}

function Invoke-PprofLabels([string]$scriptDir, [string]$pythonCmd, [string]$pprofPath) {
    $args_ = @((Join-Path $scriptDir "pprof_utils.py"), "labels", $pprofPath)
    $jsonOutput = & {
        $ErrorActionPreference = 'SilentlyContinue'
        if ($pythonCmd -eq "py -3") {
            & py -3 @args_ 2>$null
        } else {
            & $pythonCmd @args_ 2>$null
        }
    }
    if ($LASTEXITCODE -ne 0) { return $null }
    return $jsonOutput
}

function Show-TestSummary {
    Write-Host ""
    Write-Host "-- Results --" -ForegroundColor Cyan
    Write-Host "  Passed: $($script:passed)" -ForegroundColor Green
    Write-Host "  Failed: $($script:failed)" -ForegroundColor $(if ($script:failed -gt 0) { "Red" } else { "Green" })

    if ($script:failed -gt 0) {
        Write-Host ""
        Write-Host "Failures:" -ForegroundColor Red
        foreach ($f in $script:failures) {
            Write-Host "  - $f" -ForegroundColor Red
        }
    }

    Write-Host ""
    if ($script:failed -gt 0) {
        Write-Host "FAILED" -ForegroundColor Red
    } else {
        Write-Host "ALL PASSED" -ForegroundColor Green
    }

    return $script:failed
}
