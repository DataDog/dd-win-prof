$ErrorActionPreference = 'Stop'

cmake --build build --config Release --target Tests DiagnosticHeaderC LifecycleAbiMT LifecycleAbiMD
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

ctest --test-dir build/src/Tests -C Release --output-on-failure --no-tests=error -R '^(Tests|LifecycleAbiMT|LifecycleAbiMD)$'
$result = $LASTEXITCODE
Select-String -Path build/src/Tests/Testing/Temporary/LastTest.log `
    -Pattern '\[ *PASSED *\]|Lifecycle DLL ABI checks:' | ForEach-Object { $_.Line }
exit $result
