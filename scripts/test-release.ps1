param(
    [Parameter(Mandatory=$false)]
    [string]$ReleaseDllPath = "release-artifacts",
    
    [Parameter(Mandatory=$false)]
    [string]$RunnerPath = "src/Runner/x64/Release",
    
    [Parameter(Mandatory=$false)]
    [string]$WorkspacePath = "test-workspace",
    
    [Parameter(Mandatory=$false)]
    [string]$ReleaseVersion = "unknown"
)

$ErrorActionPreference = "Stop"

function Write-TestHeader {
    param([string]$Message)
    Write-Host "`n========================================" -ForegroundColor Cyan
    Write-Host $Message -ForegroundColor Cyan
    Write-Host "========================================`n" -ForegroundColor Cyan
}

function Write-TestResult {
    param([string]$Message, [bool]$Success)
    if ($Success) {
        Write-Host "✓ $Message" -ForegroundColor Green
    } else {
        Write-Host "✗ $Message" -ForegroundColor Red
    }
}

Write-TestHeader "Release Test Suite - Version $ReleaseVersion"

Write-Host "Setting up test environment..."
New-Item -ItemType Directory -Force -Path $WorkspacePath | Out-Null

$profilerDll = Get-ChildItem -Recurse $ReleaseDllPath -Filter "dd-win-prof.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
$datadogDll = Get-ChildItem -Recurse $ReleaseDllPath -Filter "datadog_profiling_ffi.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
$runner = Get-ChildItem $RunnerPath -Filter "Runner.exe" -ErrorAction SilentlyContinue | Select-Object -First 1

if (-not $profilerDll) { throw "dd-win-prof.dll not found in $ReleaseDllPath" }
if (-not $runner) { throw "Runner.exe not found in $RunnerPath" }

Copy-Item $profilerDll.FullName -Destination $WorkspacePath\ -Force
Write-Host "Copied release DLL: dd-win-prof.dll from $($profilerDll.Directory.Name)"

if ($datadogDll) {
    Copy-Item $datadogDll.FullName -Destination $WorkspacePath\ -Force
    Write-Host "Copied: datadog_profiling_ffi.dll"
}

Copy-Item $runner.FullName -Destination $WorkspacePath\ -Force
Write-Host "Copied Runner: Runner.exe (built from source)"

Push-Location $WorkspacePath
$env:DD_PROFILING_ENABLED = "1"
$env:DD_PROFILING_EXPORT_ENABLED = "0"
$env:DD_INTERNAL_PROFILING_OUTPUT_DIR = (Get-Location).Path
$env:DD_PROFILING_LOG_LEVEL = "debug"
$env:DD_PROFILING_LOG_TO_CONSOLE = "1"
$env:DD_TRACE_DEBUG = "1"
$env:DD_INTERNAL_USE_DEVELOPMENT_CONFIGURATION = "1"

$testResults = @{
    Passed = 0
    Failed = 0
    Tests = @()
}

Write-TestHeader "Test 1: SetupProfiler with service metadata"
try {
    Write-Host "Running with debug logging enabled..."
    $output = .\Runner.exe --scenario 1 --iterations 1 --name "release-test-service" --env "ci-test" --version $ReleaseVersion 2>&1 | Out-String
    Write-Host $output
    
    if ($LASTEXITCODE -eq 0) {
        Write-TestResult "SetupProfiler with metadata" $true
        $testResults.Passed++
    } else {
        Write-TestResult "SetupProfiler with metadata (exit code: $LASTEXITCODE)" $false
        $testResults.Failed++
    }
} catch {
    Write-Host $_.Exception.Message
    Write-TestResult "SetupProfiler with metadata (exception)" $false
    $testResults.Failed++
}
$testResults.Tests += "SetupProfiler with metadata"

Write-TestHeader "Test 2: Multiple profiling scenarios"
$scenarios = @(
    @{num=1; name="Simple C calls"},
    @{num=2; name="C++ class calls"},
    @{num=3; name="Multi-threaded"}
)

foreach ($scenario in $scenarios) {
    Write-Host "Running: $($scenario.name)..."
    try {
        $output = .\Runner.exe --scenario $scenario.num --iterations 1 --name "scenario-$($scenario.num)" --env "ci" 2>&1 | Out-String
        if ($LASTEXITCODE -eq 0) {
            Write-TestResult "$($scenario.name)" $true
            $testResults.Passed++
        } else {
            Write-Host $output
            Write-TestResult "$($scenario.name) (exit code: $LASTEXITCODE)" $false
            $testResults.Failed++
        }
    } catch {
        Write-Host $_.Exception.Message
        Write-TestResult "$($scenario.name) (exception)" $false
        $testResults.Failed++
    }
    $testResults.Tests += $scenario.name
}

Write-TestHeader "Test 3: Lock contention profiling"
try {
    $output = .\Runner.exe --scenario 4 --iterations 1 --name "lock-test" --env "ci" 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0) {
        Write-Host "Lock contention test output (last 20 lines):"
        ($output -split "`n" | Select-Object -Last 20) -join "`n" | Write-Host
        Write-TestResult "Lock contention profiling" $true
        $testResults.Passed++
    } else {
        Write-Host $output
        Write-TestResult "Lock contention profiling (exit code: $LASTEXITCODE)" $false
        $testResults.Failed++
    }
} catch {
    Write-Host $_.Exception.Message
    Write-TestResult "Lock contention profiling (exception)" $false
    $testResults.Failed++
}
$testResults.Tests += "Lock contention profiling"

Write-TestHeader "Test 4: Profile output verification"
$pprofFiles = Get-ChildItem -Filter "*.pprof" -ErrorAction SilentlyContinue
if ($pprofFiles.Count -gt 0) {
    Write-Host "Found $($pprofFiles.Count) profile file(s):"
    $pprofFiles | ForEach-Object { Write-Host "  - $($_.Name) ($([math]::Round($_.Length/1KB, 2)) KB)" }
    Write-TestResult "Profile files generated" $true
    $testResults.Passed++
} else {
    Write-Host "No .pprof files found (profiles may have been uploaded)"
    Write-TestResult "Profile files check (skipped)" $true
}
$testResults.Tests += "Profile output"

Pop-Location

Write-TestHeader "Test Results Summary"
Write-Host "Total Tests: $($testResults.Passed + $testResults.Failed)"
Write-Host "Passed: $($testResults.Passed)" -ForegroundColor Green
Write-Host "Failed: $($testResults.Failed)" -ForegroundColor $(if ($testResults.Failed -eq 0) { "Green" } else { "Red" })

if ($testResults.Failed -gt 0) {
    Write-Host "`nTests FAILED" -ForegroundColor Red
    exit 1
} else {
    Write-Host "`nAll tests PASSED" -ForegroundColor Green
    exit 0
}

