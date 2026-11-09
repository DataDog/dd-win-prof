# Windows residency/callback fixture

Synthetic application, not the full C++ SDK. Runs 15 seconds each of repeated work,
short-lived-thread churn and recovery, at 20 ms sampling and 5 s flushes. Saves
100 ms working-set/private-commit observations and compressed callback profiles.
Keeps one owned callback buffer, checks compression magic and exactly one final
callback. No credentials or network.

Run through the Windows worker from a local `C:\Repos` mirror. Example setup:

```powershell
$root = 'C:\Repos\dd-win-prof-c-aggregation'
cmake -S "$root\e2e-tests\c-aggregation-small-app" -B "$root\build-small-app" `
  -G 'Visual Studio 17 2022' -A x64 `
  "-DPROFILER_SOURCE=$root" `
  "-DPROFILER_BINARY=$root\build-c-aggregation\src\dd-win-prof\Release"
cmake --build "$root\build-small-app" --config Release
& "$root\build-small-app\Release\profiling_small_app.exe" 0 1 "$root\build-small-app\disabled"
& "$root\build-small-app\Release\profiling_small_app.exe" 1 1 "$root\build-small-app\enabled"
```

Arguments: enabled flag, expected Zstd flag (`0` for baseline LZ4), output folder.
For baseline, configure a separate build with `PROFILER_BINARY` pointing to the
baseline DLL/import library. Keep the same header/compiler/CRT ABI.

For a no-profiler reference, configure a third build with
`-DSMALL_APP_NO_PROFILER=ON`. It does not link/load the profiler DLL. Use arguments
`0 0 <output-folder>`. This distinguishes library-load residency from the
incremental cost of enabling an already-loaded profiler.

Recorded CSVs and phase summaries live in
`packages/profile-aggregation/benchmarks/windows-small-app/`. See
`packages/profile-aggregation/docs/validation.md` for results and limitations.
