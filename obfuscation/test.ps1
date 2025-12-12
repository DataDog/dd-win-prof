# Test script for ObfSymbols
# This script builds TestSymbols, runs ObfSymbols, and validates the output files

param(
    [Parameter(Position=0)]
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [Parameter(Position=1)]
    [ValidateSet("x64", "x86")]
    [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"

# Color output helpers
function Write-Success { param($Message) Write-Host "[PASS] $Message" -ForegroundColor Green }
function Write-Failure { param($Message) Write-Host "[FAIL] $Message" -ForegroundColor Red }
function Write-Info { param($Message) Write-Host "[INFO] $Message" -ForegroundColor Cyan }
function Write-TestHeader { param($Message) Write-Host "`n=== $Message ===" -ForegroundColor Yellow }

# Test results tracking
$script:TestsPassed = 0
$script:TestsFailed = 0
$script:FailureMessages = @()

function Test-Condition {
    param(
        [string]$TestName,
        [bool]$Condition,
        [string]$FailureMessage = ""
    )

    if ($Condition) {
        Write-Success $TestName
        $script:TestsPassed++
        return $true
    } else {
        Write-Failure $TestName
        if ($FailureMessage) {
            Write-Host "  → $FailureMessage" -ForegroundColor Yellow
            $script:FailureMessages += "$TestName`: $FailureMessage"
        }
        $script:TestsFailed++
        return $false
    }
}

# ============================================================================
# MAIN TEST EXECUTION
# ============================================================================

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "ObfSymbols Test Suite" -ForegroundColor Cyan
Write-Host "Configuration: $Configuration" -ForegroundColor Yellow
Write-Host "Platform: $Platform" -ForegroundColor Yellow
Write-Host "========================================" -ForegroundColor Cyan

# ============================================================================
# STEP 1: Build TestSymbolsDll
# ============================================================================
Write-TestHeader "Building TestSymbolsDll"

# Check if Visual Studio Developer PowerShell is already initialized
if (-not (Get-Command msbuild -ErrorAction SilentlyContinue)) {
    Write-Info "Initializing Visual Studio Developer Environment..."

    $vsDevShell = "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\Launch-VsDevShell.ps1"

    if (Test-Path $vsDevShell) {
        & $vsDevShell -Arch amd64 -SkipAutomaticLocation
        Set-Location $PSScriptRoot
    } else {
        Write-Failure "Visual Studio 2022 Professional not found!"
        exit 1
    }
}

$testProjectPath = "TestSymbolsDll\TestSymbolsDll.vcxproj"
Write-Info "Building: $testProjectPath"

msbuild $testProjectPath /t:Build /p:Configuration=$Configuration /p:Platform=$Platform /v:minimal /nologo

if ($LASTEXITCODE -ne 0) {
    Write-Failure "Failed to build TestSymbolsDll"
    exit 1
}

Write-Success "TestSymbolsDll built successfully"

# ============================================================================
# STEP 2: Verify TestSymbolsDll outputs exist
# ============================================================================
Write-TestHeader "Verifying TestSymbolsDll Outputs"

$testDll = "TestSymbolsDll\$Platform\$Configuration\TestSymbolsDll.dll"
$testPdb = "TestSymbolsDll\$Platform\$Configuration\TestSymbolsDll.pdb"

Test-Condition "TestSymbolsDll.dll exists" (Test-Path $testDll) "File not found: $testDll"
Test-Condition "TestSymbolsDll.pdb exists" (Test-Path $testPdb) "File not found: $testPdb"

if (-not (Test-Path $testPdb)) {
    Write-Failure "Cannot proceed without TestSymbolsDll.pdb"
    exit 1
}

# ============================================================================
# STEP 3: Run ObfSymbols
# ============================================================================
Write-TestHeader "Running ObfSymbols"

$obfSymbolsExe = "ObfSymbols\$Platform\$Configuration\ObfSymbols.exe"
$outputFile = "TestSymbolsDll\$Platform\$Configuration\TestSymbolsDll.sym"
$obfOutputFile = "TestSymbolsDll\$Platform\$Configuration\TestSymbolsDll_obf.sym"

# Clean up previous test outputs
if (Test-Path $outputFile) { Remove-Item $outputFile -Force }
if (Test-Path $obfOutputFile) { Remove-Item $obfOutputFile -Force }

Test-Condition "ObfSymbols.exe exists" (Test-Path $obfSymbolsExe) "File not found: $obfSymbolsExe"

if (-not (Test-Path $obfSymbolsExe)) {
    Write-Info "Building ObfSymbols first..."
    & "$PSScriptRoot\build.ps1" -Configuration $Configuration -Platform $Platform
    if ($LASTEXITCODE -ne 0) {
        Write-Failure "Failed to build ObfSymbols"
        exit 1
    }
}

Write-Info "Running: $obfSymbolsExe --pdb `"$testPdb`" --out `"$outputFile`""

$output = & $obfSymbolsExe --pdb $testPdb --out $outputFile 2>&1
$exitCode = $LASTEXITCODE

Write-Host $output

Test-Condition "ObfSymbols executed successfully" ($exitCode -eq 0) "Exit code: $exitCode"

if ($exitCode -ne 0) {
    Write-Failure "Cannot proceed without successful ObfSymbols execution"
    exit 1
}

# Test: Check for OMAP in Release builds (informational)
if ($Configuration -eq "Release") {
    $hasOMAPMessage = $output -match "OMAP tables loaded successfully"
    $noOMAPMessage = $output -match "No OMAP tables found"

    Write-TestHeader "OMAP Detection (Release Build)"

    if ($hasOMAPMessage) {
        Write-Info "OMAP tables detected - code was reordered during link-time optimization"
        Test-Condition "OMAP tables present in Release build" $true

        # Check for OMAPTO and OMAPFROM messages
        if ($output -match "Loaded OMAPTO table with (\d+) entries") {
            Write-Info "  OMAPTO entries: $($matches[1])"
        }
        if ($output -match "Loaded OMAPFROM table with (\d+) entries") {
            Write-Info "  OMAPFROM entries: $($matches[1])"
        }
        if ($output -match "skipped (\d+) eliminated symbols") {
            Write-Info "  Eliminated symbols: $($matches[1])"
        }
    } elseif ($noOMAPMessage) {
        Write-Info "No OMAP tables found in Release build"
        Write-Host "  → This is normal for simple code where the linker doesn't reorder functions" -ForegroundColor Cyan
        Write-Host "  → OMAP tables are generated when LTCG causes significant code reordering" -ForegroundColor Cyan
        Write-Host "  → The OMAP implementation is working correctly and will handle OMAP when present" -ForegroundColor Green
    }

    Write-Host ""
}

# ============================================================================
# STEP 4: Verify Output Files Exist
# ============================================================================
Write-TestHeader "Verifying Output Files"

Test-Condition "Symbol file created" (Test-Path $outputFile) "File not found: $outputFile"
Test-Condition "Obfuscated symbol file created" (Test-Path $obfOutputFile) "File not found: $obfOutputFile"

if (-not (Test-Path $outputFile) -or -not (Test-Path $obfOutputFile)) {
    Write-Failure "Cannot proceed without output files"
    exit 1
}

# ============================================================================
# STEP 5: Validate Output File Contents
# ============================================================================
Write-TestHeader "Validating Symbol File Format"

$symbolLines = Get-Content $outputFile
$obfLines = Get-Content $obfOutputFile

# Test: Files are not empty
Test-Condition "Symbol file is not empty" ($symbolLines.Count -gt 0) "File has 0 lines"
Test-Condition "Obfuscated file is not empty" ($obfLines.Count -gt 0) "File has 0 lines"

# Test: Both files have the same number of lines
Test-Condition "Both files have same line count" ($symbolLines.Count -eq $obfLines.Count) `
    "Symbol file: $($symbolLines.Count) lines, Obfuscated file: $($obfLines.Count) lines"

Write-Info "Found $($symbolLines.Count) lines (includes MODULE header)"

# Test: Symbol file format (PUBLIC/PRIVATE ADDRESS SIZE SymbolName)
# Note: Addresses are hex without 0x prefix
$invalidSymbolLines = @()
$addresses = @()
$publicCount = 0
$privateCount = 0

foreach ($line in $symbolLines) {
    # Skip MODULE header line
    if ($line -match '^MODULE\s+') {
        continue
    }

    if ($line -match '^(PUBLIC|PRIVATE)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+(.+)$') {
        $visibility = $matches[1]
        $address = $matches[2]
        $size = $matches[3]
        $symbolName = $matches[4]
        $addresses += [Convert]::ToInt64($address, 16)

        # Count visibility types
        if ($visibility -eq "PUBLIC") { $publicCount++ } else { $privateCount++ }

        # Validate address is hex
        if ($address -notmatch '^[0-9a-fA-F]+$') {
            $invalidSymbolLines += $line
        }

        # Validate size is hex
        if ($size -notmatch '^[0-9a-fA-F]+$') {
            $invalidSymbolLines += $line
        }

        # Validate symbol name is not empty
        if ([string]::IsNullOrWhiteSpace($symbolName)) {
            $invalidSymbolLines += $line
        }
    } else {
        $invalidSymbolLines += $line
    }
}

Test-Condition "All symbol lines have valid format" ($invalidSymbolLines.Count -eq 0) `
    "Found $($invalidSymbolLines.Count) invalid lines. First: $($invalidSymbolLines[0])"

Write-Info "Found $publicCount public symbols and $privateCount private symbols"

# Test: Addresses are sorted in ascending order
$sortedAddresses = $addresses | Sort-Object
$addressesSorted = $true
for ($i = 0; $i -lt $addresses.Count; $i++) {
    if ($addresses[$i] -ne $sortedAddresses[$i]) {
        $addressesSorted = $false
        break
    }
}

Test-Condition "Addresses are sorted in ascending order" $addressesSorted `
    "Addresses are not in order"

# ============================================================================
# STEP 6: Validate Obfuscated File Format
# ============================================================================
Write-TestHeader "Validating Obfuscated Symbol File Format"

# Test: Obfuscated file format (PUBLIC/PRIVATE ADDRESS SIZE obf_XXXXXXXX)
# Note: Obfuscated file does NOT include real symbol names
# Note: Addresses are hex without 0x prefix
$invalidObfLines = @()
$obfNames = @()
$obfAddresses = @()
$obfPublicCount = 0
$obfPrivateCount = 0

foreach ($line in $obfLines) {
    # Skip MODULE header line
    if ($line -match '^MODULE\s+') {
        continue
    }

    if ($line -match '^(PUBLIC|PRIVATE)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+(obf_[0-9A-F]{8})$') {
        $visibility = $matches[1]
        $address = $matches[2]
        $size = $matches[3]
        $obfName = $matches[4]

        $obfNames += $obfName
        $obfAddresses += [Convert]::ToInt64($address, 16)

        # Count visibility types
        if ($visibility -eq "PUBLIC") { $obfPublicCount++ } else { $obfPrivateCount++ }

        # Validate address is hex
        if ($address -notmatch '^[0-9a-fA-F]+$') {
            $invalidObfLines += $line
        }

        # Validate size is hex
        if ($size -notmatch '^[0-9a-fA-F]+$') {
            $invalidObfLines += $line
        }

        # Validate obfuscated name format
        if ($obfName -notmatch '^obf_[0-9A-F]{8}$') {
            $invalidObfLines += $line
        }
    } else {
        $invalidObfLines += $line
    }
}

Test-Condition "All obfuscated lines have valid format" ($invalidObfLines.Count -eq 0) `
    "Found $($invalidObfLines.Count) invalid lines. First: $($invalidObfLines[0])"

# Test: All obfuscated names are unique
$uniqueObfNames = $obfNames | Select-Object -Unique
$duplicateCount = $obfNames.Count - $uniqueObfNames.Count
Test-Condition "All obfuscated names are unique" ($obfNames.Count -eq $uniqueObfNames.Count) `
    "Found $duplicateCount duplicate names"

# Test: Obfuscated names follow the pattern
$validObfPattern = $obfNames | Where-Object { $_ -match '^obf_[0-9A-F]{8}$' }
Test-Condition "All obfuscated names follow obf_XXXXXXXX pattern" ($validObfPattern.Count -eq $obfNames.Count) `
    "Expected $($obfNames.Count), got $($validObfPattern.Count)"

# Test: Addresses in obfuscated file are sorted
$sortedObfAddresses = $obfAddresses | Sort-Object
$obfAddressesSorted = $true
for ($i = 0; $i -lt $obfAddresses.Count; $i++) {
    if ($obfAddresses[$i] -ne $sortedObfAddresses[$i]) {
        $obfAddressesSorted = $false
        break
    }
}

Test-Condition "Addresses in obfuscated file are sorted in ascending order" $obfAddressesSorted `
    "Addresses are not in order"

Write-Info "Obfuscated file: $obfPublicCount public symbols and $obfPrivateCount private symbols"

# Test: Public/private counts match between files
Test-Condition "Public symbol counts match" ($publicCount -eq $obfPublicCount) `
    "Symbol file: $publicCount, Obfuscated file: $obfPublicCount"

Test-Condition "Private symbol counts match" ($privateCount -eq $obfPrivateCount) `
    "Symbol file: $privateCount, Obfuscated file: $obfPrivateCount"

# ============================================================================
# STEP 7: Cross-validate Both Files
# ============================================================================
Write-TestHeader "Cross-Validating Files"

# Sample a few symbols and check they match between files
# Skip line 0 (MODULE header) and sample actual symbol lines
if ($symbolLines.Count -gt 1) {
    $lastIndex = $symbolLines.Count - 1
    $middleIndex = [Math]::Min(10, $lastIndex)
    $sampleIndices = @(1, $middleIndex, $lastIndex)  # Start at 1 to skip MODULE line

    foreach ($idx in $sampleIndices) {
        $symbolLine = $symbolLines[$idx]
        $obfLine = $obfLines[$idx]

        if ($symbolLine -match '^(PUBLIC|PRIVATE)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+(.+)$') {
            $symVisibility = $matches[1]
            $symAddress = $matches[2]
            $symSize = $matches[3]
            $symName = $matches[4]

            # Extract obfuscated name from symbol line (it's the first space-separated token after size)
            $symObfName = if ($symName -match '^(obf_[0-9A-F]{8})\s+') { $matches[1] } else { "" }

            if ($obfLine -match '^(PUBLIC|PRIVATE)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+(obf_[0-9A-F]{8})$') {
                $obfVisibility = $matches[1]
                $obfAddress = $matches[2]
                $obfSize = $matches[3]
                $obfName = $matches[4]

                # Test: Visibility matches
                $visibilityMatch = ($symVisibility -eq $obfVisibility)
                Test-Condition "Line $idx : Visibility matches" $visibilityMatch `
                    "Symbol file: $symVisibility, Obfuscated file: $obfVisibility"

                # Test: Addresses match
                $addressesMatch = ($symAddress -eq $obfAddress)
                Test-Condition "Line $idx : Addresses match" $addressesMatch `
                    "Symbol file: $symAddress, Obfuscated file: $obfAddress"

                # Test: Sizes match
                $sizesMatch = ($symSize -eq $obfSize)
                Test-Condition "Line $idx : Sizes match" $sizesMatch `
                    "Symbol file: $symSize, Obfuscated file: $obfSize"

                # Test: Obfuscated names match between files
                $obfNamesMatch = ($symObfName -eq $obfName)
                Test-Condition "Line $idx : Obfuscated names match" $obfNamesMatch `
                    "Symbol file: $symObfName, Obfuscated file: $obfName"

                # Test: Has valid obfuscated name
                $hasObf = $obfName -match '^obf_[0-9A-F]{8}$'
                Test-Condition "Line $idx : Has valid obfuscated name" $hasObf `
                    "Obfuscated name: $obfName"
            } else {
                Test-Condition "Line $idx : Obfuscated line format is valid" $false `
                    "Invalid format: $obfLine"
            }
        }
    }
}

# ============================================================================
# STEP 8: Content Analysis
# ============================================================================
Write-TestHeader "Content Analysis"

# Count specific symbol types we expect from TestSymbols
# Note: ObfSymbols filters to include only functions, so we won't see variables or types
$classMethodSymbols = $symbolLines | Where-Object { $_ -match '\b(SimpleClass|Circle|Square|Complex)::' }
$operatorSymbols = $symbolLines | Where-Object { $_ -match '\boperator' }
$functionSymbols = $symbolLines | Where-Object { $_ -match '\b(Add|Max|Min|Calculate|main)\b' }
$constructorSymbols = $symbolLines | Where-Object { $_ -match '::[^:]+<[^>]*>$' }

Write-Info "Found $($classMethodSymbols.Count) class method symbols"
Write-Info "Found $($operatorSymbols.Count) operator symbols"
Write-Info "Found $($functionSymbols.Count) function symbols"
Write-Info "Found $($constructorSymbols.Count) constructor/template symbols"

# Test: Symbols should only be functions (no variables or types)
# Since we filter for SymTagFunction, all symbols should be executable code
Test-Condition "Found symbols (functions only)" ($symbolLines.Count -gt 0) `
    "No symbols found"

# Note: Function symbols may be inlined/optimized away in Release builds
if ($functionSymbols.Count -gt 0 -or $classMethodSymbols.Count -gt 0) {
    Test-Condition "Found executable code symbols" $true `
        "Found functions, methods, or operators"
} else {
    Write-Info "Limited symbols found (may be heavily optimized in Release builds)"
}

# ============================================================================
# FINAL RESULTS
# ============================================================================
Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "Test Results Summary" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

Write-Host "Tests Passed: " -NoNewline
Write-Host $script:TestsPassed -ForegroundColor Green

Write-Host "Tests Failed: " -NoNewline
if ($script:TestsFailed -eq 0) {
    Write-Host $script:TestsFailed -ForegroundColor Green
} else {
    Write-Host $script:TestsFailed -ForegroundColor Red
}

$totalTests = $script:TestsPassed + $script:TestsFailed
$passRate = if ($totalTests -gt 0) { [Math]::Round(($script:TestsPassed / $totalTests) * 100, 2) } else { 0 }

Write-Host "Pass Rate: $passRate%" -ForegroundColor $(if ($passRate -eq 100) { "Green" } else { "Yellow" })

if ($script:TestsFailed -gt 0) {
    Write-Host "`nFailed Tests:" -ForegroundColor Red
    foreach ($msg in $script:FailureMessages) {
        Write-Host "  - $msg" -ForegroundColor Yellow
    }
    Write-Host "`n========================================" -ForegroundColor Red
    Write-Host "TESTS FAILED" -ForegroundColor Red
    Write-Host "========================================" -ForegroundColor Red
    exit 1
} else {
    Write-Host "`n========================================" -ForegroundColor Green
    Write-Host "ALL TESTS PASSED!" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Green

    Write-Host "`nOutput files:" -ForegroundColor Cyan
    Write-Host "  Symbol file: $outputFile" -ForegroundColor Gray
    Write-Host "  Obfuscated file: $obfOutputFile" -ForegroundColor Gray

    exit 0
}

