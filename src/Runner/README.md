# Test Runner

An example C++ application for testing the Datadog Windows profiler. It exercises the profiler through four scenarios with configurable iterations, and supports both environment-variable and code-only (`noEnvVars`) configuration modes.

## Building

```cmd
msbuild src\Runner\Runner.vcxproj /p:Configuration=Debug /p:Platform=x64
```

Or with CMake:

```cmd
cmake --build build --target Runner --config Debug
```

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
| `--noenvvars` | Ignore all environment variables; use only explicit flags |
| `--url <url>` | Datadog intake URL (mandatory with `--noenvvars`) |
| `--apikey <key>` | Datadog API key (mandatory with `--noenvvars`) |

### RUM Context Options (scenario 5)

| Flag | Description |
|------|-------------|
| `--rum-app-id <uuid>` | RUM application ID (required for scenario 5) |
| `--rum-session-id <uuid>` | RUM session ID (required for scenario 5) |

Scenario 5 walks through three views across two sessions: views 1-2 use the session ID passed via `--rum-session-id` (S1), then a session rotation occurs and view 3 uses a hardcoded second session ID (S2: `99999999-2222-3333-4444-555555555555`).

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
Runner.exe --scenario 2 --iterations 10 --noenvvars --url https://intake.datadoghq.com --apikey DD_API_KEY --name testApp
Runner.exe --scenario 4 --iterations 2 --noenvvars --url https://intake.datadoghq.com --apikey DD_API_KEY --pprofdir C:\temp\pprof
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

All configuration comes from flags. `-Url` and `-ApiKey` are mandatory.

```powershell
.\run-noenvvars.ps1 -Url https://intake.datadoghq.com -ApiKey YOUR_KEY
.\run-noenvvars.ps1 -Url https://intake.datadoghq.com -ApiKey YOUR_KEY -Name myApp -Env prod -Symbolize
```

### `run-local-debug.ps1` -- Local debugging

Uses `--noenvvars` with a placeholder URL/apikey. Pprof files are written locally (default: `%TEMP%\dd-profiler-debug\pprof`). No backend upload.

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
