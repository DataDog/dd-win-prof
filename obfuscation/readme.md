# ObfSymbols

Extracts symbols from PDB files and generates obfuscated symbol files for Datadog profiling.

## Usage

```cmd
ObfSymbols.exe --pdb <path_to_pdb> --out <output_file> [--obf <obfuscated_output>]
```

**Example:**
```cmd
ObfSymbols.exe --pdb MyApp.pdb --out MyApp.sym
```

This produces two files:
- `MyApp.sym` — full symbols (obfuscated name + real signature)
- `MyApp_obf.sym` — obfuscated only (no real signatures)

## Output Files

### Symbol file (`.sym`)

Contains the mapping between obfuscated names and real function signatures:

```
MODULE windows x86_64 FDA5BF9510B746CA8882DC3DCFA453C41 Runner.exe
FUNC 1000 c 0 obf_7120BD79 Circle::~Circle()
FUNC 1010 e 0 obf_2BDA4789 Shape::Shape()
FUNC 1050 1c4 0 obf_8C39C417 Shape::operator=(Shape&)
```

- First line: MODULE with build ID (4th field)
- `FUNC <address> <size> <param_size> <obf_name> <signature>`

### Obfuscated symbol file (`_obf.sym`)

Same format but without real signatures — safe to upload:

```
MODULE windows x86_64 FDA5BF9510B746CA8882DC3DCFA453C41 Runner.exe
FUNC 1000 c 0 obf_7120BD79
FUNC 1010 e 0 obf_2BDA4789
FUNC 1050 1c4 0 obf_8C39C417
```

## Uploading Symbols to Datadog

Upload the `_obf.sym` file using [`datadog-ci`](https://github.com/DataDog/datadog-ci#installation):

```bash
datadog-ci pe-symbols upload /path/to/MyApp_obf.sym
```

The backend matches profiles to symbols via the build ID (MODULE line, 4th field).

After upload, profiles display obfuscated function names (`obf_XXXXXXXX`). To see real function names, load the `.sym` file (not `_obf.sym`) in the Datadog frontend.

## Troubleshooting

**Uploaded the wrong symbol file**

Re-upload with `--replace-existing` (this can take a few minutes):

```bash
datadog-ci pe-symbols upload --replace-existing /path/to/MyApp_obf.sym
```

## Development

See [DEVELOPMENT.md](DEVELOPMENT.md) for building, testing, and contributing.
