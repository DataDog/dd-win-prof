# ObfSymbols

Extracts function symbols from PDBs and generates a private name mapping and an
obfuscated symbol file for Datadog profiling.

## Build and test

Requires the [main build prerequisites](../README.md#build) and Visual Studio's
DIA SDK headers/libraries. ATL is not required.

From the repository root:

```powershell
cmake -G "Visual Studio 17 2022" -A x64 -B build -DDD_WIN_PROF_BUILD_OBFUSCATION=ON
cmake --build build --config Release --target ObfSymbols
.\obfuscation\test.ps1 Release
```

The executable is `build\obfuscation\ObfSymbols\Release\ObfSymbols.exe`.
Use `Debug` in both commands for a Debug build/test. `test.ps1` accepts
`-BuildDir` for an alternate configured build directory.

The test builds `TestSymbolsDll`, extracts its PDB, and checks symbol formats,
function signatures, obfuscated names, address ordering, and uniqueness.
[CI](../.github/workflows/obfuscation.yml) runs Debug and Release. For validation
against the Runner PDB, see [Runner symbol validation](../e2e-tests/README.md#runner-symbol-validation).

## Extract symbols

```powershell
.\build\obfuscation\ObfSymbols\Release\ObfSymbols.exe --pdb MyApp.pdb --out MyApp.sym
```

This produces:

- `MyApp.sym`: obfuscated names plus real signatures. Keep this mapping private.
- `MyApp_obf.sym`: obfuscated names only. Use this for obfuscated backend symbols.

An optional `--obf` argument selects the obfuscated output path. Both files
start with a `MODULE` line; its fourth field is the build ID. Function entries
use `FUNC <address> <size> <param_size> <obf_name>`, with the real signature
appended only in the private `.sym` mapping.

## Upload symbols

Upload the specific obfuscated file with
[`datadog-ci`](https://github.com/DataDog/datadog-ci#installation):

```powershell
datadog-ci pe-symbols upload MyApp_obf.sym
```

Do not scan a directory containing PDBs, PE binaries, or the private `.sym`
mapping when the intent is obfuscated symbols only: the CLI can prefer richer
debug information for the same build ID.

The backend matches profiles by build ID. Load the private `.sym` mapping in
the Datadog frontend to recover real names locally. To replace an incorrect
upload, add `--replace-existing`; processing can take a few minutes.
