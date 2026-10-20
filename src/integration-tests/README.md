# Integration Tests

End-to-end integration tests that run the **Runner** executable and validate the profiler's output (log files and pprof content).

Unlike the unit tests in `src/Tests/` (C++ / Google Test), these tests exercise the full profiling pipeline: profiler setup, sample collection, export, and file output. A Python helper decompresses current Zstd pprof files and historical LZ4 files. New debug files use the `.zstd.pprof` suffix. Historical files may use `.lz4.pprof`; detect compression from the bytes, not the filename.

## Prerequisites

- **Runner.exe** built (Debug or Release) -- see [`src/Runner/README.md`](../Runner/README.md)
- **UIApp.exe** built (only for the UI-hang test) -- `cmake --build build --config Debug --target UIApp`
- **PowerShell 7.0+** (`pwsh`, only for the UI-hang test); Windows PowerShell 5.1 is not supported by that test
- **Python 3.9+** with pip, accessible via `python` or `py -3` (Windows launcher)

### Python dependencies

| Package | Purpose |
|---------|---------|
| `lz4` | Decompress historical LZ4 pprof files |
| `zstandard` | Decompress current Zstd pprof files |
| `protobuf` | Parse the Google pprof protobuf format |
| `grpcio-tools` | Compile `profile.proto` into Python bindings on first run |

Run these commands from `src\integration-tests`. Install all dependencies at once:

```powershell
.\install-dependencies.ps1
```

Or manually:

```powershell
pip install -r requirements.txt
```

## Running the tests

### Local test (no API key required)

Validates local output only: pprof files, RUM labels, view/session JSON records.
Passes a placeholder endpoint and dummy API key; no working backend is required.
It does not disable exports, so failed upload attempts may appear in the logs.
**This is what CI runs.**

```powershell
.\test_rum_scenario.ps1
.\test_rum_scenario.ps1 -Config Release
.\test_rum_scenario.ps1 -Iterations 3
```

| Parameter | Default | Description |
|-----------|---------|-------------|
| `-Config` | `Auto` | Build config: `Debug`, `Release`, or `Auto` (tries Debug first) |
| `-Iterations` | `3` | Number of Runner iterations (more iterations = more samples) |

### UI-hang test (no API key required)

Exercises the UI hang detector via **UIApp** in headless automation mode. For
each hang kind (Sleep, Wait, CPU) it launches `UIApp.exe`, waits for it to
finish, and validates the pprof with `validate_ui_hangs.py`: `UIHang=true/false`
pairs exist on `UIApp_Main` with the expected reason stack and durations, and no
ordinary wait sample overlaps a detected hang. A dedicated lifecycle case also
runs two start/monitor/stop sessions in the same process and verifies that stop
without start, monitor outside a running session, and repeated start are
rejected. Each session uses a distinct synthetic RUM view, and the validator
requires one complete hang pair under each view. **This is what CI runs.**

```powershell
pwsh .\test_ui_hangs.ps1 -Config Debug -KeepArtifacts
pwsh .\test_ui_hangs.ps1 -Config Release
```

| Parameter | Default | Description |
|-----------|---------|-------------|
| `-Config` | `Auto` | Build config: `Debug`, `Release`, or `Auto` |
| `-HangDurationMs` | `1500` | Freeze duration per hang |
| `-Cycles` | `2` | Number of hangs per kind |
| `-OutputRoot` | `%TEMP%\dd-ui-hang-test` | Parent folder; each run creates a `yyyyMMdd_HHmmss` subfolder holding per-kind `pprof`/`logs` |
| `-KeepArtifacts` | *(off)* | Keep artifacts even on success (kept automatically on failure) |

For a quick visual smoke run without validation:

```powershell
build/src/UIApp/Debug/UIApp.exe --auto-hang all --hang-duration-ms 1500 `
  --hang-cycles 1 --pprofdir "$env:TEMP/ui-hangs" --symbolize
```

### Send test (API key required)

Validates everything in the local test **plus** a successful backend send (HTTP 2xx).
This test uses agentless export: supply a Datadog site name, not an Agent or intake
URL, and a real API key.

```powershell
.\test_rum_scenario_send.ps1 -Url datadoghq.com -ApiKey "<your-key>"
.\test_rum_scenario_send.ps1 -Url datadoghq.com -ApiKey "<your-key>" -Config Release
```

| Parameter | Default | Description |
|-----------|---------|-------------|
| `-Url` | *(required)* | Datadog site, such as `datadoghq.com` |
| `-ApiKey` | *(required)* | Datadog API key |
| `-Config` | `Auto` | Build config: `Debug`, `Release`, or `Auto` |
| `-Iterations` | `3` | Number of Runner iterations |

## Test inventory

| Script | Scenario | What it validates |
|--------|----------|-------------------|
| `test_rum_scenario.ps1` | Runner scenario 5 (local) | Log messages; pprof samples carry expected `rum.view_id` / `trace endpoint` labels; samples without RUM labels exist during "no active view" gaps; `.rum-views.json` files contain view and session records with valid timestamps and durations |
| `test_rum_scenario_send.ps1` | Runner scenario 5 (with send) | Everything above **plus** successful profile upload to the backend (HTTP 2xx, no send errors) |
| `test_ui_hangs.ps1` | UIApp Sleep / Wait / CPU hangs and profiler restart (local) | Per kind: UIApp exits cleanly, final export logged, and `UIHang=true/false` pairs on `UIApp_Main` have the expected reason stack, `wall-time == 0`, `wait-time > 0`, ordered timestamps, no ordinary wait overlapping a detected hang, and (CPU) positive `cpu-time` during the hang. The lifecycle case verifies invalid API calls and one hang pair in each of two session-specific RUM views. |

## Regular regression tests

`Tests.exe` (or `ctest --test-dir build/src/Tests -C Release --output-on-failure`) runs
aggregator custom-type/reset/period, callback ownership/Zstd, and loopback HTTP
policy/thread-transition tests without credentials, opt-in variables or a Python
launcher. HTTP traffic stays on an ephemeral loopback port.

Python discovery also decodes the ordinary native tests' output to check exact
custom names/units, period and post-reset values. On Windows, build Release
`Tests.exe` first; `DD_WIN_PROF_TESTS_EXE` can select another built configuration.
These native wire checks run automatically in the existing Release integration
CI step; non-Windows discovery skips only the native executable cases:

```powershell
python -m unittest discover -s src/integration-tests -p "test_*.py" -v
```

Historical v19/v47 A/B results are archived locally. They are not a committed
migration harness or a release-validation framework.

## Utilities

### `pprof_utils.py`

Python helper for reading historical LZ4 and current Zstd pprof files.

```
python pprof_utils.py labels <file.zstd.pprof>
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
  rum_test_helpers.ps1         # Shared helpers (Assert, Find-Python, Show-TestSummary)
  install-dependencies.ps1     # One-step Python dependency installer
  requirements.txt             # Python packages: lz4, zstandard, protobuf, grpcio-tools
  profile.proto                # Google pprof protobuf definition
  pprof_utils.py               # LZ4/Zstd decompress + protobuf parse + JSON output
  validate_ui_hangs.py         # UI-hang pprof validator (values, labels, stacks, timing)
  test_rum_scenario.ps1        # RUM scenario 5 integration test (local, no API key)
  test_rum_scenario_send.ps1   # RUM scenario 5 integration test (with backend send)
  test_ui_hangs.ps1            # UIApp UI-hang integration test (local, no API key)
  README.md                    # This file
```

UIApp discovery lives next to the app: [`src/UIApp/find-uiapp.ps1`](../UIApp/find-uiapp.ps1).
