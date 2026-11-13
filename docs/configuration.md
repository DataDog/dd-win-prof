# Configuration details

For basic integration, see the [README](../README.md). The complete
`ProfilerConfig` field reference is in
[`dd-win-prof.h`](../src/dd-win-prof/include/dd-win-prof.h).

## Code-only configuration

Set `noEnvVars=true` to use struct values and defaults instead of
environment-based profiler settings. For HTTP delivery, `url` and `apiKey` are
mandatory; `SetupProfiler()` returns `false` if either is missing. `url` is a
Datadog site name, not a full intake URL.

```C++
    ProfilerConfig config;
    ::ZeroMemory(&config, sizeof(ProfilerConfig));
    config.size = sizeof(ProfilerConfig);
    config.noEnvVars = true;
    config.url = "datadoghq.com";
    config.apiKey = "your_datadog_api_key_here";
    config.serviceName = "my-windows-app";
    config.serviceEnvironment = "production";
    config.serviceVersion = "1.2.3";
    if (!SetupProfiler(&config)) {
        return -1;
    }
```

The Runner's `--url` flag also takes a site name despite its name. See the
[Runner examples](../src/Runner/README.md#examples).

## Repeated setup calls

Each `SetupProfiler()` call replaces the previous configuration: it starts from
defaults and the current environment (unless `noEnvVars=true`), then applies the
supplied overrides. Omitted fields do not retain previous API overrides.
For example, after setting `serviceVersion` to `"1.2.3"`, a later setup that omits
it uses the current `DD_VERSION` value or the default, not `"1.2.3"`.

`StopProfiler()` preserves configuration for a later `StartProfiler()` call
without another setup.

## Logging and local profiles

The log directory is not configurable through `ProfilerConfig`. Logging is
configured at DLL load time using `DD_TRACE_LOG_DIRECTORY`, even when
`noEnvVars=true`. The default is `%PROGRAMDATA%\Datadog Tracer\logs`.

To write local profiles, set `ProfilerConfig.pprofOutputDirectory` or, with
environment-based configuration, `DD_INTERNAL_PROFILING_OUTPUT_DIR`.
To enable call stack symbolization, set `ProfilerConfig.symbolizeCallstacks=true`
or `DD_PROFILING_INTERNAL_SYMBOLIZE_CALLSTACKS=1`.
