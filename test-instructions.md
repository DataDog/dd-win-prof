# UI hang validation (Windows)

## Sync, build, and test

Run on Windows with the SMB share mapped to `Z:`:

```powershell
& Z:\winshare\sync\sync-and-build.ps1 -SyncOnly -Project dd-win-prof
Set-Location C:\Repos\dd-win-prof
powershell.exe -NoProfile -ExecutionPolicy Bypass -File src\integration-tests\test_ui_hang_suite.ps1
```

Prerequisites: Visual Studio 2022 C++ build tools, CMake, and Python 3 on PATH.
The suite creates `build\ui-hang-venv` and installs the checked-in Python
requirements there; the global Python environment is unchanged.

The suite configures/builds Release, runs native tests, then runs Sleep, Wait,
and CPU hangs at 1.5 seconds (two cycles) and 6 seconds (one cycle). Profiles export
every two seconds and stay local; no API key is needed. Artifacts are retained under
`$env:TEMP\dd-ui-hang-test` and the test log prints the exact directory.

Native regressions cover probe acknowledgment races, stale probes, wait timestamp
handoff, cached wall accounting, recovery without another suspension, stop during
a hang, profiler restart, and unregister/rebind. Integration validation checks
hang pairs, stacks, wait overlap, wall coverage, and CPU attribution.

To isolate native regressions after building:

```powershell
& .\build\src\Tests\Release\Tests.exe "--gtest_filter=UiHangTests.*:UiHangLifecycleTests.*" --gtest_print_time=1
```

To also build/test Debug:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File src\integration-tests\test_ui_hang_suite.ps1 -Config Debug
```

## Submit through the SMB worker from macOS

From the `win-sync` directory, with the Windows worker running:

```bash
WIN_WORKER_ROOT="$PWD/winshare/worker" \
  winshare/worker/submit-job.sh dd-win-prof run-script \
  --script src/integration-tests/test_ui_hang_suite.ps1
```

The worker syncs before running the suite from `C:\Repos`. Check the returned job ID
in `winshare/worker/results/<id>.json` and `winshare/worker/logs/<id>.log`.

## Validation results (2026-09-25)

Both Release and Debug builds passed on the Windows worker. Each configuration
passed 186 native tests (including 12 UI hang regressions) and all short/long
Sleep, Wait, and CPU scenarios. Short scenarios produced three profiles each;
long scenarios produced four, with wall-time coverage checked across exports.

- Release job: `20260925T081819Z-run-script-dd-win-prof`
- Debug job: `20260925T082020Z-run-script-dd-win-prof`

The corresponding result JSON and full build/test logs are under the worker paths
above. Initial validation exposed missing root CTest discovery, an interactive
keyboard prompt in the test runner, and missing Python dependencies. The suite
now enables discovery, runs without the prompt, and provisions an isolated venv.
