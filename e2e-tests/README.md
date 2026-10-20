# End-to-end test helpers

Run these commands from the repository root after a Release build.
See [CONTRIBUTING.md](../CONTRIBUTING.md#development-setup) for build setup.

## Runner profile correctness

Install `prof-analyze` v1.0.0 with Go and ensure its install directory is on `PATH`
(normally `$(go env GOPATH)\bin`):

```powershell
go install github.com/DataDog/prof-correctness/cmd/prof-analyze@v1.0.0
```

Capture and validate one scenario:

```powershell
$pprofDir = Join-Path $env:TEMP "dd-win-prof-scenario-1"
pwsh e2e-tests/capture-pprof.ps1 -Scenario 1 -Iterations 5 -Config Release -PprofDir $pprofDir
prof-analyze -expectedJson e2e-tests/runner-scenarios/scenario_1/expected_profile.json -pprofPath $pprofDir
```

CI repeats this for every `scenario_*` directory in [runner-scenarios](runner-scenarios/).
`capture-pprof.ps1` enables symbolization and disables upload by default; loaded
`.env` and `scenario.env` files can override those settings.

## Runner symbol validation

With obfuscation tools enabled, generate and validate `Runner.sym`:

```powershell
& .\build\obfuscation\ObfSymbols\Release\ObfSymbols.exe `
  --pdb build/src/Runner/Release/Runner.pdb --out build/src/Runner/Release/Runner.sym
pwsh e2e-tests/validate-runner-sym.ps1 -SymFile build/src/Runner/Release/Runner.sym
```

## Other checks

- CMake consumer/export checks: run `pwsh scripts/test-consumer.ps1` from the repository root.
- [Vulkan tests](README_vulkan.md)
- [RUM integration tests](../src/integration-tests/README.md)

See the [primary CI workflow](../.github/workflows/test.yml) for the full scenario
loop and failure handling.
