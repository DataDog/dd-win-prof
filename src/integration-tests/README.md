# Integration Tests

End-to-end integration tests that run the **Runner** executable and validate the profiler's output (log files and pprof content).

Unlike the unit tests in `src/Tests/` (C++ / Google Test), these tests exercise the full profiling pipeline: profiler setup, sample collection, export, and file output. A Python helper is used to decompress and parse the LZ4-compressed pprof files produced by libdatadog.

## Prerequisites

- **Runner.exe** built (Debug or Release) -- see [`src/Runner/README.md`](../Runner/README.md)
- **UIApp.exe** built (only for the UI-hang test) -- `cmake --build build --config Debug --target UIApp`
- **PowerShell 7.0+** (`pwsh`, only for the UI-hang test); Windows PowerShell 5.1 is not supported by that test
- **Python 3.9+** with pip, accessible via `python` or `py -3` (Windows launcher)

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
pip install -r requirements.txt
```

## Running the tests

### Local test (no API key required)

Validates local output only: pprof files, RUM labels, view/session JSON records.
Uses a dummy URL so no network access or API key is needed. **This is what CI runs.**

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
ordinary wait sample overlaps a detected hang. **This is what CI runs.**

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

Validates everything in the local test **plus** that the profile was successfully sent to the backend (HTTP 200). Requires a real Datadog agent URL and API key.

```powershell
.\test_rum_scenario_send.ps1 -Url http://localhost:8126 -ApiKey <your-key>
.\test_rum_scenario_send.ps1 -Url http://localhost:8126 -ApiKey <your-key> -Config Release
```

| Parameter | Default | Description |
|-----------|---------|-------------|
| `-Url` | *(required)* | Datadog agent or intake URL |
| `-ApiKey` | *(required)* | Datadog API key |
| `-Config` | `Auto` | Build config: `Debug`, `Release`, or `Auto` |
| `-Iterations` | `3` | Number of Runner iterations |

## Test inventory

| Script | Scenario | What it validates |
|--------|----------|-------------------|
| `test_rum_scenario.ps1` | Runner scenario 5 (local) | Log messages; pprof samples carry expected `rum.view_id` / `trace endpoint` labels; samples without RUM labels exist during "no active view" gaps; `.rum-views.json` files contain view and session records with valid timestamps and durations |
| `test_rum_scenario_send.ps1` | Runner scenario 5 (with send) | Everything above **plus** successful profile upload to the backend (HTTP 200, no send errors) |
| `test_ui_hangs.ps1` | UIApp Sleep / Wait / CPU hangs (local) | Per kind: UIApp exits cleanly, final export logged, and `UIHang=true/false` pairs on `UIApp_Main` have the expected reason stack, `wall-time == 0`, `wait-time > 0`, ordered timestamps, no ordinary wait overlapping a detected hang, and (CPU) positive `cpu-time` during the hang |

## Utilities

### `pprof_utils.py`

Python helper for reading LZ4-compressed pprof files.

```
python pprof_utils.py labels <file.lz4.pprof>
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
  requirements.txt             # Python packages: lz4, protobuf, grpcio-tools
  profile.proto                # Google pprof protobuf definition
  pprof_utils.py               # LZ4 decompress + protobuf parse + JSON output
  validate_ui_hangs.py         # UI-hang pprof validator (values, labels, stacks, timing)
  test_rum_scenario.ps1        # RUM scenario 5 integration test (local, no API key)
  test_rum_scenario_send.ps1   # RUM scenario 5 integration test (with backend send)
  test_ui_hangs.ps1            # UIApp UI-hang integration test (local, no API key)
  README.md                    # This file
```

UIApp discovery lives next to the app: [`src/UIApp/find-uiapp.ps1`](../UIApp/find-uiapp.ps1).
