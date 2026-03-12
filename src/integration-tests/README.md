# Integration Tests

End-to-end integration tests that run the **Runner** executable and validate the profiler's output (log files and pprof content).

Unlike the unit tests in `src/Tests/` (C++ / Google Test), these tests exercise the full profiling pipeline: profiler setup, sample collection, export, and file output. A Python helper is used to decompress and parse the LZ4-compressed pprof files produced by libdatadog.

## Prerequisites

- **Runner.exe** built (Debug or Release) -- see [`src/Runner/README.md`](../Runner/README.md)
- **Python 3.9+** with pip, accessible via the `py -3` launcher (Windows) or `python`

### Python dependencies

| Package | Purpose |
|---------|---------|
| `lz4` | Decompress LZ4-compressed pprof files |
| `protobuf` | Parse the Google pprof protobuf format |
| `grpcio-tools` | Compile `profile.proto` into Python bindings on first run |

Install all at once:

```powershell
.\install-dependencies.ps1
```

Or manually:

```powershell
py -3 -m pip install -r requirements.txt
```

## Running the tests

From `src\integration-tests\`:

```powershell
.\test_rum_scenario.ps1
```

### Options

| Parameter | Default | Description |
|-----------|---------|-------------|
| `-Config` | `Auto` | Build config: `Debug`, `Release`, or `Auto` (tries Debug first) |
| `-Iterations` | `2` | Number of Runner iterations (more iterations = more samples) |

### Examples

```powershell
.\test_rum_scenario.ps1 -Config Release
.\test_rum_scenario.ps1 -Iterations 3
```

## Test inventory

| Script | Scenario | What it validates |
|--------|----------|-------------------|
| `test_rum_scenario.ps1` | Runner scenario 5 (RUM view transitions) | Log contains successful export messages; pprof samples carry expected `rum.view_id` / `trace endpoint` labels for each view; samples without RUM labels exist during the "no active view" gap; `.rum-views.json` files contain the expected view entries with valid `startClocks`, `duration`, `viewId`, and `viewName` fields |

## Utilities

### `pprof_utils.py`

Python helper for reading LZ4-compressed pprof files.

```
py -3 pprof_utils.py labels <file.lz4.pprof>
```

Outputs a JSON array of samples with their resolved string labels:

```json
[
  { "labels": { "rum.view_id": "111...", "trace endpoint": "HomePage", "thread name": "..." } },
  { "labels": { "thread name": "..." } }
]
```

On first run it auto-generates `profile_pb2.py` from `profile.proto` using `grpcio-tools`.

## File layout

```
src/integration-tests/
  install-dependencies.ps1   # One-step Python dependency installer
  requirements.txt           # Python packages: lz4, protobuf, grpcio-tools
  profile.proto              # Google pprof protobuf definition
  pprof_utils.py             # LZ4 decompress + protobuf parse + JSON output
  test_rum_scenario.ps1      # RUM scenario 5 integration test
  README.md                  # This file
```
