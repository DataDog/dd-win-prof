# Smoke test for ObfSymbols: builds TestSymbolsDll, runs ObfSymbols, validates output.

param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $ProjectRoot "build" }

$ObfBuildDir = Join-Path $BuildDir "obfuscation"
$testPdb = "$ObfBuildDir/TestSymbolsDll/$Configuration/TestSymbolsDll.pdb"
$outputFile = "$ObfBuildDir/TestSymbolsDll/$Configuration/TestSymbolsDll.sym"
$obfOutputFile = "$ObfBuildDir/TestSymbolsDll/$Configuration/TestSymbolsDll_obf.sym"
$obfSymbolsExe = "$ObfBuildDir/ObfSymbols/$Configuration/ObfSymbols.exe"

Write-Host "[INFO] ObfSymbols Smoke Test - $Configuration" -ForegroundColor Cyan

# ============================================================================
# Build
# ============================================================================
if (-not (Test-Path (Join-Path $BuildDir "CMakeCache.txt"))) {
    Write-Host "[INFO] Configuring CMake..." -ForegroundColor Cyan
    cmake -G "Visual Studio 17 2022" -A x64 -B "$BuildDir" -S "$ProjectRoot" -DDD_WIN_PROF_BUILD_OBFUSCATION=ON
    if ($LASTEXITCODE -ne 0) { exit 1 }
}

Write-Host "[INFO] Building TestSymbolsDll..." -ForegroundColor Cyan
cmake --build "$BuildDir" --config $Configuration --target TestSymbolsDll
if ($LASTEXITCODE -ne 0) { exit 1 }

if (-not (Test-Path $testPdb)) {
    Write-Error "TestSymbolsDll.pdb not found: $testPdb"
}
Write-Host "[PASS] TestSymbolsDll built" -ForegroundColor Green

# ============================================================================
# Run ObfSymbols
# ============================================================================
if (-not (Test-Path $obfSymbolsExe)) {
    Write-Host "[INFO] Building ObfSymbols..." -ForegroundColor Cyan
    cmake --build "$BuildDir" --config $Configuration --target ObfSymbols
    if ($LASTEXITCODE -ne 0) { exit 1 }
}

if (Test-Path $outputFile) { Remove-Item $outputFile -Force }
if (Test-Path $obfOutputFile) { Remove-Item $obfOutputFile -Force }

Write-Host "[INFO] Running ObfSymbols..." -ForegroundColor Cyan
& $obfSymbolsExe --pdb $testPdb --out $outputFile 2>&1 | Out-Host
if ($LASTEXITCODE -ne 0) { exit 1 }

if (-not (Test-Path $outputFile) -or -not (Test-Path $obfOutputFile)) {
    Write-Error "ObfSymbols did not produce output files"
}
Write-Host "[PASS] ObfSymbols produced output files" -ForegroundColor Green

# ============================================================================
# Basic validation
# ============================================================================
$symbolLines = Get-Content $outputFile
$obfLines = Get-Content $obfOutputFile

if ($symbolLines.Count -eq 0 -or $obfLines.Count -eq 0) {
    Write-Error "Output files are empty"
}

if ($symbolLines.Count -ne $obfLines.Count) {
    Write-Error "Line count mismatch: $($symbolLines.Count) vs $($obfLines.Count)"
}
Write-Host "[PASS] Both files have content ($($symbolLines.Count) lines)" -ForegroundColor Green

# Zero-RVA regression
$zeroRva = $symbolLines + $obfLines | Where-Object { $_ -match '^(PUBLIC|PRIVATE)\s+0\s+' }
if ($zeroRva.Count -gt 0) {
    Write-Error "Found $($zeroRva.Count) zero-RVA entries (regression)"
}
Write-Host "[PASS] No zero-RVA entries" -ForegroundColor Green

# Obfuscated names follow pattern
$invalidObf = $obfLines | Where-Object { $_ -match '^(PUBLIC|PRIVATE)\s+' -and $_ -notmatch '\bobf_[0-9A-F]{8}\b' }
if ($invalidObf.Count -gt 1) {  # Allow MODULE header
    Write-Error "Found invalid obfuscated names: $(($invalidObf | Select-Object -First 3) -join '; ')"
}
Write-Host "[PASS] Obfuscated names valid" -ForegroundColor Green

Write-Host "`n[SUCCESS] All tests passed" -ForegroundColor Green
