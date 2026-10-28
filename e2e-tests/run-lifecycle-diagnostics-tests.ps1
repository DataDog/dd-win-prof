$ErrorActionPreference = 'Stop'

cmake --build build --config Release --target Tests LifecycleDllMT LifecycleDllMD
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

ctest --test-dir build/src/Tests -C Release --output-on-failure --no-tests=error -R '^(Tests|LifecycleDllMT|LifecycleDllMD)$'
$result = $LASTEXITCODE
Select-String -Path build/src/Tests/Testing/Temporary/LastTest.log `
    -Pattern '\[ *PASSED *\]|Lifecycle DLL checks:' | ForEach-Object { $_.Line }
exit $result
