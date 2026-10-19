# RUM integration tests

These tests run Runner scenario 5 and inspect profiler logs, LZ4-compressed
pprof files, sample labels, and view/session timeline records. They check both
active views and gaps without RUM labels.

## Run locally

Requires [Runner](../Runner/README.md) and Python 3.9+ (`python` or `py -3`).
From the repository root:

```powershell
cmake --build build --config Release --target Runner
python -m pip install -r src/integration-tests/requirements.txt
.\src\integration-tests\test_rum_scenario.ps1 -Config Release
```

No Datadog credentials are needed. The test uses a dummy upload endpoint;
local output does not disable export, so it can still attempt that endpoint.
CI runs this test in Release and in the ASan Debug build.

Options: `-Iterations` (default: 3), `-KeepArtifacts`, and `-Config`
(`Auto`, `Debug`, or `Release`). `Auto` tries Debug first.

## Validate backend upload

The send variant adds checks for successful HTTP 2xx responses and no send
errors. Supply a working endpoint and API key:

```powershell
.\src\integration-tests\test_rum_scenario_send.ps1 -Config Release -Url $url -ApiKey $apiKey
```

It also accepts `-Iterations`. This is not run by CI.

## Inspect pprof labels

```powershell
python src/integration-tests/pprof_utils.py labels path/to/profile.lz4.pprof
```

The helper emits sample labels as JSON. It generates Python bindings from
`profile.proto` on first use; its dependencies are in `requirements.txt`.

For CPU/wall-time profile assertions, see the
[Runner correctness tests](../../e2e-tests/README.md).
