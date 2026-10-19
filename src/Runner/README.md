# Test Runner

Runner exercises the profiler in an application process. It is a workload
generator; profile assertions live in the [correctness tests](../../e2e-tests/README.md)
and [RUM integration tests](../integration-tests/README.md).

## Build and run

After [configuring CMake](../../README.md#build), run from the repository root:

```powershell
cmake --build build --config Release --target Runner
.\build\src\Runner\Release\Runner.exe --scenario 1 --iterations 3
```

Both `--scenario` and `--iterations` are required. Run `Runner.exe --help` for
the full CLI reference (the current help path returns exit code `-1`).

| Scenario | Workload |
|----------|----------|
| 1 | Simple C function calls |
| 2 | C++ class calls |
| 3 | CPU-bound worker threads |
| 4 | Threads waiting on synchronization primitives |
| 5 | RUM view transitions and session rotation |

## Configuration

By default, the profiler reads environment variables. CLI settings such as
`--name`, `--env`, `--version`, `--tags`, and `--symbolize` override the
corresponding settings. `--noenvvars` resets profiler configuration to defaults
and requires `--url` and `--apikey`.

`--pprofdir` enables local file output, not offline mode. Backend export is a
separate setting. For local captures with upload disabled by default, use
[`capture-pprof.ps1`](../../e2e-tests/README.md#profile-correctness).

Logs are configured at DLL load time through `DD_TRACE_LOG_DIRECTORY`
(default: `%PROGRAMDATA%\Datadog Tracer\logs`), independently of `--noenvvars`.

## RUM scenario

Scenario 5 requires `--rum-app-id` or `DD_RUM_APPLICATION_ID`. The initial
session ID comes from `--rum-session-id`, `DD_RUM_SESSION_ID`, or a generated
UUID. It runs three named views across two sessions, with gaps without an active view.
With local output enabled, it writes `.rum-views.json` timeline records alongside
the pprof files. The capture script loads the scenario's checked-in IDs from
`e2e-tests/runner-scenarios/scenario_5/scenario.env`.

## Launch helpers

Scripts in this directory accept `-Config Debug`, `Release`, or `Auto`.
`Auto` tries Debug first, so select a configuration explicitly when validating
a particular build.

- `run-with-envvars.ps1`: environment configuration with optional CLI overrides.
- `run-noenvvars.ps1`: struct-based configuration; requires `-Url` and `-ApiKey`.
- `run-local-debug.ps1`: local pprof output and a dummy upload endpoint. It does
  not disable export; upload attempts to that endpoint can still fail.
