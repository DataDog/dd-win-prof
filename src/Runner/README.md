# Test Runner

An example C++ application for testing the Datadog Windows profiler. It exercises the profiler through five scenarios with configurable iterations, and supports both environment-variable and code-only (`noEnvVars`) configuration modes.

## Building

After [configuring CMake](../../CONTRIBUTING.md#development-setup), run from the repository root:

```powershell
cmake --build build --target Runner --config Debug
```

The executable is `build\src\Runner\Debug\Runner.exe`. Run the CLI examples from
that directory, or use its full path. Replace `Debug` with `Release` as needed.

## CLI Reference

```
Runner.exe --scenario <1-5> --iterations <num> [options]
```

### Required

| Flag | Description |
|------|-------------|
| `--scenario <1-5>` | 1 = Simple C calls, 2 = C++ class calls, 3 = Multi-threaded (CPU), 4 = Waiting threads, 5 = RUM context |
| `--iterations <num>` | Number of times to repeat the scenario |

### Service Information

| Flag | Description |
|------|-------------|
| `--name <service>` | Service name |
| `--version <version>` | Service version |
| `--env <environment>` | Service environment |

### No-Environment-Variables Mode

| Flag | Description |
|------|-------------|
| `--noenvvars` | Use explicit flags and defaults for profiler settings; logging remains environment-configured |
| `--url <site>` | Datadog site, such as `datadoghq.com` (mandatory with `--noenvvars`) |
| `--apikey <key>` | Datadog API key (mandatory with `--noenvvars`) |

### RUM Context Options (scenario 5)

| Flag | Description |
|------|-------------|
| `--rum-app-id <uuid>` | RUM application ID (required for scenario 5, or set `DD_RUM_APPLICATION_ID`) |
| `--rum-session-id <uuid>` | Optional session ID; falls back to `DD_RUM_SESSION_ID`, then a generated UUID |

Scenario 5 produces four view records per iteration. HomePage, SettingsPage, and
ProfilePage use the initial session (S1). After session rotation, a fourth view
(HomePage) uses S2: `99999999-2222-3333-4444-555555555555`.

When `--pprofdir` is set, scenario 5 also produces `.rum-views.json` files alongside the `.lz4.pprof` files. Each JSON file contains a `{"views":[...],"sessions":[...]}` object with view and session timeline records.

### Additional Options

| Flag | Description |
|------|-------------|
| `--symbolize` | Enable call stack symbolization (default: obfuscated) |
| `--tags <k:v,...>` | Comma-separated tags to attach to profiles |
| `--pprofdir <path>` | Override pprof debug output directory |
| `--help`, `-h` | Show help message |

### Examples

```cmd
Runner.exe --scenario 1 --iterations 5
Runner.exe --scenario 3 --iterations 1 --name myApp --env staging --symbolize
Runner.exe --scenario 2 --iterations 10 --noenvvars --url datadoghq.com --apikey YOUR_KEY --name testApp
Runner.exe --scenario 4 --iterations 2 --noenvvars --url datadoghq.com --apikey YOUR_KEY --pprofdir C:\temp\pprof
Runner.exe --scenario 5 --iterations 1 --rum-app-id aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee --rum-session-id 11111111-2222-3333-4444-555555555555 --pprofdir C:\temp\rum-test --symbolize
```

## PowerShell Scripts

Helper scripts to launch Runner with the right flags. Run from `src\Runner\`.

All scripts accept `-Config` (`Debug`, `Release`, or `Auto`). Auto mode tries Debug first, then Release. Each script prints the resolved `Runner.exe` path and its last-write timestamp so you can confirm you're running the right build.

### `run-with-envvars.ps1` -- Environment variables mode (default)

Environment variables (`DD_AGENT_HOST`, `DD_API_KEY`, `DD_ENV`, etc.) are read by the profiler as usual. Code-based flags override them.

```powershell
.\run-with-envvars.ps1
.\run-with-envvars.ps1 -Scenario 3 -Iterations 5 -Name dd-win-prof -Env staging
.\run-with-envvars.ps1 -PprofDir C:\temp\pprof -Symbolize
```

### `run-noenvvars.ps1` -- No-env-vars mode

Profiler settings come from flags and defaults. `-Url` (a Datadog site name, not a
full URL) and `-ApiKey` are mandatory. Logging is still configured by environment.

```powershell
.\run-noenvvars.ps1 -Url datadoghq.com -ApiKey YOUR_KEY
.\run-noenvvars.ps1 -Url datadoghq.com -ApiKey YOUR_KEY -Name myApp -Env prod -Symbolize
```

### `run-local-debug.ps1` -- Local debugging

Uses `--noenvvars` with placeholder endpoint credentials and writes pprof files
locally (default: `%TEMP%\dd-profiler-debug\pprof`). It does not disable exports;
failed upload attempts may appear in the logs.

For capture with uploads disabled by default, use
[`e2e-tests/capture-pprof.ps1`](../../e2e-tests/capture-pprof.ps1). See the
[profile correctness guide](../../e2e-tests/README.md#runner-profile-correctness).

```powershell
.\run-local-debug.ps1
.\run-local-debug.ps1 -Scenario 3 -Iterations 5 -Symbolize
.\run-local-debug.ps1 -OutputRoot D:\profiler-output
```

### Common parameters

| Parameter | Type | Description |
|-----------|------|-------------|
| `-Config` | string | `Debug`, `Release`, or `Auto` (default: Auto, tries Debug first) |
| `-Scenario` | int | Scenario number 1-5 (default: 1) |
| `-Iterations` | int | Iteration count (default: 3) |
| `-Name` | string | Service name |
| `-Env` | string | Service environment |
| `-Version` | string | Service version |
| `-Tags` | string | Comma-separated tags |
| `-PprofDir` | string | Local pprof output directory |
| `-Symbolize` | switch | Enable call stack symbolization |

## Notes

- **Log directory** is set at DLL load time via `DD_TRACE_LOG_DIRECTORY` (default: `%PROGRAMDATA%\Datadog Tracer\logs`). It cannot be overridden via `ProfilerConfig` or CLI flags.
- When `--noenvvars` is not used, environment variables are read as usual: `DD_AGENT_HOST`, `DD_API_KEY`, `DD_ENV`, `DD_SERVICE`, `DD_VERSION`, `DD_TRACE_LOG_DIRECTORY`, `DD_INTERNAL_PROFILING_OUTPUT_DIR`, etc.
