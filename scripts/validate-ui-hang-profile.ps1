#Requires -Version 5.1
<#
.SYNOPSIS
    Validate duration-only UI hang profiles locally, without credentials or intake.
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if ($repo -notmatch '^[A-Za-z]:\\' -or $repo -match '^Z:') {
    throw 'Validation must run from a local Windows drive, not the SMB share'
}
$build = Join-Path $repo 'build'
$results = Join-Path $build 'hang-validation/results'
New-Item -ItemType Directory -Force $results | Out-Null
Set-Location $repo
Get-ChildItem Env: | Where-Object { $_.Name -like 'DD_*' } | ForEach-Object { Remove-Item "Env:$($_.Name)" }
$env:DD_PROFILING_AUTO_START = '0'

function Run {
    Write-Host "> $($args -join ' ')"
    & $args[0] $args[1..($args.Length - 1)]
    if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $($args -join ' ')" }
}

$venv = Join-Path $build 'hang-validation/venv'
if (-not (Test-Path "$venv/Scripts/python.exe")) { Run python -m venv $venv }
$python = Join-Path $venv 'Scripts/python.exe'
Run $python -m pip install -r "$repo/src/integration-tests/requirements.txt"
$env:PATH = "$venv\Scripts;$env:PATH"
Run $python -m unittest discover -s "$repo/src/integration-tests" -p test_pprof_utils.py -v
Run $python -m unittest discover -s "$repo/src/integration-tests" -p test_validate_ui_hangs.py -v
Run cmake -S $repo -B $build -G 'Visual Studio 17 2022' -A x64 -DDD_WIN_PROF_BUILD_OBFUSCATION=OFF
$summary = @()
foreach ($config in @('Debug', 'Release')) {
    Run cmake --build $build --config $config --parallel 4 --target Tests UIApp Runner
    $exe = Join-Path $build "src/Tests/$config/Tests.exe"
    $xml = Join-Path $results "$config-unit.xml"
    $env:GTEST_OUTPUT_FILE = $xml
    try {
        & $exe 2>&1 | Tee-Object -FilePath (Join-Path $results "$config-unit.log")
        if ($LASTEXITCODE -ne 0) { throw "Unit tests failed: $config" }
    } finally { Remove-Item Env:GTEST_OUTPUT_FILE }
    [xml]$unit = Get-Content $xml
    Run $python "$repo/src/integration-tests/validate_ui_hang_profile.py" --tests-exe $exe --output "$results/$config-fixtures"
    Run $python "$repo/src/integration-tests/validate_libdatadog_upgrade.py" --tests-exe $exe --output "$results/$config-legacy-fixtures"
    Run powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$repo/src/integration-tests/test_ui_hangs.ps1" -Config $config -KeepArtifacts -OutputRoot "$results/$config-ui"
    $summary += [ordered]@{ config = $config; tests = $unit.testsuites.tests;
        failures = $unit.testsuites.failures; disabled = $unit.testsuites.disabled;
        skipped = @($unit.SelectNodes("//testcase[@result='skipped']")).Count }
    Write-Host "PASS: $config build, units, dedicated/legacy wire fixtures and actual sleep/wait/CPU/lifecycle"
}
$decode = @'
import json, sys
from pathlib import Path
from google.protobuf.json_format import MessageToJson
sys.path.insert(0, sys.argv[1])
from pprof_utils import parse_pprof
for path in Path(sys.argv[2]).rglob('*.pprof'):
    path.with_suffix('.decoded.json').write_text(MessageToJson(parse_pprof(str(path))), encoding='utf-8')
print('PASS: retained decoded protobuf JSON for all dedicated, legacy and actual profiles')
'@
Run $python -c $decode "$repo/src/integration-tests" $results
$summary | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $results 'summary.json')
Write-Host 'PASS: duration-only UI hang profile validation complete'
