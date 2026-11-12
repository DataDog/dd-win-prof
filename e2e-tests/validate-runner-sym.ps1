# Validate Runner.sym: checks MODULE header format and zero-RVA regression.
# Run after ObfSymbols to catch symbolication issues before E2E scenarios.

param([Parameter(Mandatory)][string]$SymFile)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $SymFile)) {
    Write-Error "Symbol file not found: $SymFile"
}

$lines = Get-Content $SymFile
if ($lines.Count -eq 0) {
    Write-Error "Symbol file is empty"
}

Write-Host "[INFO] Validating $SymFile ($($lines.Count) lines)" -ForegroundColor Cyan

# ============================================================================
# MODULE header format
# ============================================================================
$header = $lines[0]
if ($header -notmatch '^MODULE\s+windows\s+x86_64\s+[0-9A-F]{32,}\s+Runner\.exe$') {
    Write-Error "Invalid MODULE header: $header"
}
Write-Host "[PASS] MODULE header format" -ForegroundColor Green

# ============================================================================
# Zero-RVA regression: symbols at RVA 0 act as catch-all in symbolizer
# ============================================================================
$zeroRva = $lines | Where-Object { $_ -match '^(PUBLIC|PRIVATE)\s+0\s+' }
if ($zeroRva.Count -gt 0) {
    Write-Error "Found $($zeroRva.Count) zero-RVA entries (these corrupt flamegraphs):`n  $(($zeroRva | Select-Object -First 3) -join "`n  ")"
}
Write-Host "[PASS] No zero-RVA entries" -ForegroundColor Green

# ============================================================================
# Key Runner scenario functions
# ============================================================================
$required = @(
    @('SimpleCalls',        '\bSimpleCalls\(\)'),
    @('Spin',               '\bSpin\(int\)'),
    @('ThreadFunction1',    '\bThreadFunction1\(void\*\)'),
    @('RunWaitingThreads',  '\bRunWaitingThreads\(\)'),
    @('main',               '\bmain\(int')
)

foreach ($fn in $required) {
    $name, $pattern = $fn
    if (-not ($lines | Where-Object { $_ -match $pattern })) {
        Write-Error "Missing symbol: $name (pattern: $pattern)"
    }
}
Write-Host "[PASS] All required symbols present" -ForegroundColor Green

Write-Host "`n[SUCCESS] Validation passed" -ForegroundColor Green
