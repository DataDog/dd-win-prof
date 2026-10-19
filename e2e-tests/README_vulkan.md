# Vulkan example builds

The quick-start script builds Vulkan examples with profiler integration and
prepares launchers. It does not run the examples, including in CI.

## Build

After [configuring CMake](../README.md#build), run from the repository root:

```powershell
cmake --build build --config Release --target dd-win-prof ProfilerInjector
.\e2e-tests\vulcan_quickstart.ps1 -EnableProfiler
```

The script copies `e2e-tests/.env` beside the examples if it exists. For local
profiles without upload, use:

```env
DD_SERVICE=vulkan-example
DD_INTERNAL_PROFILING_EXPORT_ENABLED=0
DD_INTERNAL_PROFILING_OUTPUT_DIR=C:\temp\vulkan-pprof
```

Use `-BuildTargets @("triangle", "gears")` to select examples. `-Clean` removes
the Vulkan build directory and dependency markers, then exits. Run the command
again with `-EnableProfiler` to rebuild.

## Run

From `e2e-tests\vulkan_examples\build\bin\Release\`:

```powershell
.\triangle_with_profiler.bat
.\ProfilerInjector.exe gears.exe
.\computeheadless.exe
```

The last command runs without injection. Windowed examples require a GPU and
display. See [ProfilerInjector](../src/ProfilerInjector/README.md) for environment
file discovery and configuration.
