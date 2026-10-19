# ProfilerInjector

Test utility that launches an application with the profiler injected, without
changing the application's code. The target must be a 64-bit Windows executable.

## Build and run

After [configuring CMake](../../README.md#build), run from the repository root:

```powershell
cmake --build build --config Release --target ProfilerInjector
.\build\src\ProfilerInjector\Release\ProfilerInjector.exe path\to\myapp.exe --config config.json
```

Place `dd-win-prof.dll` beside the target executable. The injector sets
`DD_PROFILING_AUTO_START=1` before launching the target. See the
[Vulkan guide](../../e2e-tests/README_vulkan.md) for a prepared example directory.

## Configuration

Profiler environment variables can be set in the shell or loaded from a `.env`
file. The first existing file is loaded, in this order:

1. Target executable's directory.
2. Current working directory.
3. Parent of the target executable's directory.
4. Grandparent of the target executable's directory.

For a local capture, place this beside the target executable:

```env
DD_SERVICE=my-service
DD_INTERNAL_PROFILING_EXPORT_ENABLED=0
DD_INTERNAL_PROFILING_OUTPUT_DIR=C:\temp\myapp-pprof
```

See [profiler configuration](../../README.md#configuration) for upload and
logging settings. Do not commit `.env` files containing credentials.
