$ErrorActionPreference = 'Stop'

cmake --build build --config Release --target Tests
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

ctest --test-dir build/src/Tests -C Release --output-on-failure --no-tests=error --verbose -R '^(Tests|LifecycleAbiMT|LifecycleAbiMD)$'
exit $LASTEXITCODE
