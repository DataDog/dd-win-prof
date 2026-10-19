# Profile correctness and symbols

Run commands from the repository root after [configuring CMake](../README.md#build).

## Profile correctness

The fixtures in `runner-scenarios/` describe expected profiles for Runner
scenarios 1, 4, and 5. CI captures each workload and checks its profile with
[`prof-analyze`](https://github.com/DataDog/prof-correctness).

Build Runner and install the analyzer (Go required; its bin directory must be
on `PATH`):

```powershell
cmake --build build --config Release --target Runner
go install github.com/DataDog/prof-correctness/cmd/prof-analyze@v1.0.0
```

Use a fresh output directory for each run:

```powershell
$pprofDir = Join-Path $env:TEMP "dd-win-prof-$([guid]::NewGuid())"
.\e2e-tests\capture-pprof.ps1 -Scenario 1 -Iterations 5 -Config Release -PprofDir $pprofDir
prof-analyze -expectedJson e2e-tests/runner-scenarios/scenario_1/expected_profile.json -pprofPath $pprofDir
```

Use matching scenario numbers for the capture and fixture. The capture script
enables symbolization and disables upload by default. It then loads optional
`e2e-tests/.env` overrides and the scenario's `scenario.env`, which can override
those defaults. To skip a personal `.env`, pass `-EnvFile` with a nonexistent path.

## Runner symbol validation

With [obfuscation tools enabled](../obfuscation/readme.md#build-and-test):

```powershell
cmake --build build --config Release --target Runner ObfSymbols
.\build\obfuscation\ObfSymbols\Release\ObfSymbols.exe --pdb build/src/Runner/Release/Runner.pdb --out build/src/Runner/Release/Runner.sym
.\e2e-tests\validate-runner-sym.ps1 -SymFile build/src/Runner/Release/Runner.sym
```

[CI](../.github/workflows/test.yml) validates Runner symbols in Debug and Release;
profile correctness checks run in Release. For the extractor's fixture tests,
see [obfuscation tests](../obfuscation/readme.md#build-and-test).

[Vulkan example builds](README_vulkan.md) are a separate build/integration check.
