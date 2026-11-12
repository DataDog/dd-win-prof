# PROF-16455 v47 validation

Source, signatures, release checksums and API differences: [realignment](packages/profile-aggregation/docs/v47-realignment.md).
Fresh matched results, memory costs and limitations: [Windows v47 report](packages/profile-aggregation/benchmarks/windows-v47/README.md).
Historical v19 data remains under `benchmarks/windows-v2/`; do not mix versions.

## Matched Windows run

Use the win-sync SMB worker and build under `C:\Repos`, not `Z:`. No overlapping jobs.
The reference is main `96d14f8a83f1267830bdb8877f50222aec0b1b84` and the pinned v47.0.0 Windows static release.

```powershell
packages\profile-aggregation\benchmarks\harness\run-windows.ps1 `
  -BaselineVersion 47 `
  -BaselineRoot C:\Repos\libdatadog-v47.0.0\libdatadog-x64-windows `
  -BaselineLibrary C:\Repos\libdatadog-v47.0.0\libdatadog-x64-windows\release\static\datadog_profiling_ffi.lib `
  -BuildRoot C:\Repos\dd-win-prof-c-aggregation\build-perf-v2-v47 `
  -MsvcRuntime MultiThreaded -AffinityMask 4 -Seed 16457
```

Validate the full raw artifact directory with an isolated Python environment containing harness decoder requirements:

```sh
python -B packages/profile-aggregation/benchmarks/harness/test_results.py -v
python -B packages/profile-aggregation/benchmarks/harness/compare_fixtures.py "$RUN/fixtures" --require-v47 --report "$RUN/semantic-comparison.json"
python -B packages/profile-aggregation/benchmarks/harness/summarize_results.py "$RUN" --output "$RUN/summary.json"
python -B packages/profile-aggregation/benchmarks/harness/inspect_results.py "$RUN" --output "$RUN/details.json"
```

The accepted run is `run-20261112T085300Z`: 312 cases, 20 semantic groups / 60 fixtures.
The compact committed snapshot does not contain every raw per-record CSV or fixture binary; acceptance requires the full raw directory.

## Integration and ASan

`e2e-tests/validate-c-aggregation.ps1` covers native tests, C suites, source/installed/downstream consumers, package/import isolation, callbacks, loopback HTTP, RUM and UI hangs. Current Windows results are 233 native tests, 101 RUM and 23 UI-hang assertions.

```powershell
$env:DD_WIN_PROF_TESTS_EXE = '<Release-or-ASan Tests.exe>'
python -m unittest discover -s src\integration-tests -p 'test_*.py' -v
```

All 12 Python cases pass on Windows. Five use native output, including populated seven-column definitions, exact values across two resets and a 7 ms period. The seven portable codec cases also run on the host.

Windows native/core/exporter ASan passed in a separate Ninja Debug build with `DD_WIN_PROF_ENABLE_ASAN=ON`; runtime imports were verified. Leak detection was disabled. Host ASan startup remains inconclusive.

The five-state small-app fixture is under `e2e-tests/c-aggregation-small-app/`. Its fresh v47/C captures are single-process, fixed-order snapshots, not full-SDK or soak qualification. Both native adapters retain the pre-existing 20 ms configured sampling / 10 ms emitted period mismatch.

Publication still needs explicit force-with-lease approval and fresh GitHub CI. Fresh v47 intake/indexing, dirty external SDK packaging migration and long-soak qualification remain open.
