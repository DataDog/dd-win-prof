# PROF-16455: source-built libdd-c integration

## Revisions and boundary

- Consumer base: `86dd462ebad979855ac7119017a813fe9748f9ad`, branch
  `r1viollet/prof-16455-c-aggregation`.
- Integration branch: `r1viollet/prof-16455-use-libdd-c`.
- Original producer Windows-tested revision: `c7c8a8520c99fddb87f04f8a1fe76258f2854dfa`, from
  `r1viollet/prof-16455-extract-profile-core`. This includes encoded-stack caching
  from `9e9b88886edc6cc442bb47b12448ba6e3ecdf657`. The later cleanup commit changes
  tests/docs, not the production source, public header or vendors.
- Current string-view API and Windows/host-validated revision:
  `4e37730228b579860e0865cf6b8c3592666dc7ed`. Results for the retention and
  string-view follow-ups are recorded separately below.
- At the user's request, the build temporarily follows that producer branch.
  TODO(PROF-16455): pin its final full SHA in `cmake/libdd-c.cmake` before release.
  A local source override is never reset, fetched or modified by the build.

`dd-win-prof` and its internal native tests privately link `ddc::profiling` and
include `ddc/profiling.h`. The dependency is compiled as C11 PIC source with its
private, namespaced vendors and the enclosing build's MSVC CRT selection. No Rust,
prebuilt libdd-c package, extra third-party runtime DLL or dependency headers are
part of the public profiler interface or installed CMake target.

The copied engine, its `pa_*` header/vendors, and core-only tests are removed.
Windows capture, symbolication, persistent symbol metadata, generation-local ID
caches, context translation, callback ownership/lifecycle, and C/WinHTTP transport
remain here. Dependency tests are opt-in and run from libdd-c, not a duplicate suite.
The release install includes libdd-c notices and all its transitive license texts.

## Approved behavior change

Native CPU/wall and UI-hang stacks are now capped at **256 frames**, previously
512. Capture uses `DDC_PROFILE_MAX_STACK_DEPTH`, so the native sampler truncates
at the dependency's optimized limit rather than recording an oversized stack
that serialization would reject. Direct adapter input above the limit fails.
This reduction was explicitly approved for the stack optimization. It is not a
claim of semantic equivalence for profiles with more than 256 frames.

Apart from the upstream RUM HTTP tag change described below, emitted profile and
callback behavior is unchanged: ordered/custom sample definitions, periods, labels,
module-relative location addresses, generation-local IDs and cache invalidation,
owned callback bytes after `StopProfiler`, stop/final delivery, and RUM/UI hangs.
There is still exactly one reset on the native export path, including failed
serialization, sink/callback or HTTP delivery. No retry, queue or replay is added.
C++ output sinks catch exceptions and discard partial frames.

Existing conventions intentionally retained, not fixed here:

- `ProfileExporter` supplies a 10 ms emitted period even when capture is configured
  differently. The aggregator's explicit nondefault periods still survive resets.
- The adapter uses `timestamp != 0` as presence. The core supports explicit epoch
  zero, but the adapter does not express it.

## String retention follow-up

The updated API at `1efe5ea5be96c36bf90906a1d2efcb20e1742261` retains strings
across ordinary resets until explicitly cleared. The local exporter now calls
`ddc_profile_clear_strings` after every tenth successful reset, before reinterning
sample labels. Failed delivery and empty windows still count; failed resets do
not. A failed clear remains due for the next reset. Native symbol caches remain
intact. There is no new public setting or timer.

Ten resets are roughly ten minutes at the default production upload interval.
The constant carries a TODO to tune retention against memory and reinterning
cost. This is an age policy, not a peak-memory limit. See the
[string retention proposal and validation notes](libdd-c-string-retention-proposal.md).

Against that producer revision, the local host harness passes 2/2 CTest checks
and the 41-run smoke with independent wire decoding using Python 3.15's standard
library Zstd decoder. Removing explicit clearing makes the new tenth-reset
reclamation assertion fail. These checks do not execute the Windows exporter.
Three native policy tests were added, and existing custom/NUL/wide fixtures now
exercise clearing. The later Windows run below covers these changes; the original
pre-main Windows results do not.

## String-view API follow-up

At producer `4e37730228b579860e0865cf6b8c3592666dc7ed`, string interning takes a
`ddc_profile_string_view` rather than separate pointer/length arguments. Native
`std::string_view` inputs retain explicit lengths; no `strlen` conversion is used
for these or for custom type/unit definitions. Embedded NUL support is preserved.
The harness and native test literals use `ddc/profiling_helpers.h`'s borrowed
`ddc_profile_string_view_from_cstr` helper for NUL-terminated strings.

The producer now links its private vendors through a separate
`ddc_private_dependencies` archive. Consumers continue linking only the public
`ddc::profiling` target, which supplies that dependency. License installation now
copies `vendor/licenses` into `licenses/libdd-c/vendor`. Windows validation audits
both static archives and both dependency projects' CRT settings. No new public SDK
surface or runtime dependency DLL is added.

Using a local producer snapshot, host configure/build and CTest pass 9/9, including
the dependency's namespace audit and C/C++ examples. The 41-run smoke also passes
with independent wire decoding. The reset-only negative variant still fails at
the tenth-reset string reclamation assertion. All seven installed license/notice
files match the source bytes. These host checks are separate from the later
Windows SDK and PowerShell validation recorded below.

## Main synchronization

The validated pre-sync integration is commit
`ea20ab1e92ce0c2fa1f6815c2848b0661d5046c7`. Main commit
`cf7aa2beaaa61956a83b0fda3d830ee699cd2a20` adds repeated `rum.session_id` and
`rum.view_id` HTTP tags. Its exporter changes are adapted to the C/WinHTTP path,
retaining all nine upstream tests, including empty-ID handling, uncapped tag
lists and no stale IDs on the next export. Callback delivery still discards
these HTTP-only records.

The original 234-test results apply before that merge. The later 246-test run
includes its nine upstream tests plus three retention tests. Newer main changes,
including `3d62818` (stable C callback API) and `f2b8b52` (UI-hang duration profiles),
are not merged into this branch.

## Build

libdd-c is currently internal. Normal fetching requires authorized GitHub
credentials, including cross-repository access in CI. The installed consumer
does not fetch it.

Normal source fetch (Visual Studio 2022 x64):

```powershell
cmake -S . -B build-libdd-c-release -G 'Visual Studio 17 2022' -A x64 `
  -DDD_WIN_PROF_DEVELOPMENT=OFF
cmake --build build-libdd-c-release --config Release --parallel 4
cmake --install build-libdd-c-release --config Release --prefix stage-libdd-c
```

Explicit local-source build, also used for isolated Windows mirrors:

```powershell
cmake -S . -B build-libdd-c-local -G 'Visual Studio 17 2022' -A x64 `
  -DDD_WIN_PROF_DEVELOPMENT=OFF `
  -DDD_WIN_PROF_LIBDD_C_SOURCE_DIR=C:/Repos/my-libdd-c-source
cmake --build build-libdd-c-local --config Release --parallel 4
```

The override names the libdd-c **repository root**, not its `profiling` directory.
It must supply `profiling/include/ddc/profiling.h`, `ddc/profiling_helpers.h` in
the same public include tree, and `ddc::profiling`; adapters compile directly
against that API. Neither an implicit sibling path nor an
author-machine absolute path is part of the normal build. An installed consumer
needs only the profiler's installed headers, import library and DLL.

## Windows validation

Use the same `prof-correctness v1.0.0` as this repository's CI. With Go >= 1.25.1,
build its analyzer and the small consumer-contract checker:

```powershell
New-Item build-libdd-c-tools -ItemType Directory -Force | Out-Null
Push-Location e2e-tests/profile-check
go build -o ../../build-libdd-c-tools/prof-analyze.exe github.com/DataDog/prof-correctness/cmd/prof-analyze
go build -o ../../build-libdd-c-tools/libdd-c-profile-check.exe .
Pop-Location
$env:PATH = (Join-Path $PWD 'build-libdd-c-tools') + ';' + $env:PATH
```

These are test tools, not profiler dependencies. They can also be cross-built on
macOS with `GOOS=windows GOARCH=amd64 CGO_ENABLED=0` and copied to the worker.
Run on a fresh isolated mirror under `C:\Repos` via the win-sync SMB worker:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File e2e-tests/validate-c-aggregation.ps1 `
  -LibddCSourceDir C:/Repos/my-libdd-c-source `
  -TestZstdSourceDir C:/Repos/zstd-1.5.7
```

Dependency tests require an independent system libzstd >= 1.5.5 through pkg-config,
or local upstream Zstd sources via `-TestZstdSourceDir` as above. Omit that argument
when system libzstd is available. The producer no longer downloads its test decoder;
the vendored production amalgamation is not an independent test decoder.

This runs dependency correctness/collision/stack-encoding/namespace tests and C/C++
examples, native tests, source and installed consumers with development options
off, independently decoded pprof tests, real busy-thread callback capture retained
after stop, WinHTTP loopback/rejection/redirect/timeout/refusal, standalone capture,
RUM and UI-hang/restart integration. `prof-analyze` uses the consumer's
`expected_profile.json` and the existing Runner scenario expectation to check
workload CPU/wall values, symbolized stacks and thread/process/timestamp labels.
The supplementary checker uses `analysis.ReadPprofFile` from prof-correctness for
exact callback timestamps, ordered definitions/units, period, capture depth and
module/thread identity. It does not implement Zstd or protobuf decoding.
Existing Python wire/transport edge-case tests are retained.

For baseline equivalence and MSVC ASan as well:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File e2e-tests/validate-libdd-c-worker.ps1 `
  -LibddCSourceDir C:/Repos/my-libdd-c-source `
  -BaselineSourceDir C:/Repos/isolated-consumer-base `
  -ArtifactDirectory Z:/winshare/artifacts/my-libdd-c-validation `
  -TestZstdSourceDir C:/Repos/zstd-1.5.7
```

The baseline checkout must be the exact consumer base above. This builds its
native tests and compares six fixed-time adapter fixtures (custom, seven-column,
embedded-NUL definitions, before/after reset) with the integrated build. Both
profiles are decoded by prof-correctness and serialized by google/pprof before
comparison, avoiding a separate protobuf canonicalizer. Noisy real captures are
not compared byte-for-byte. The MSVC ASan build uses `RelWithDebInfo` and runs the
native suite separately. `-RemainingChecksOnly` resumes this fixture/ASan phase
without repeating primary integration checks; use it only when the production
sources match the previously verified snapshot.

### Results recorded 2026-11-19: retention and string views

Producer `4e37730228b579860e0865cf6b8c3592666dc7ed` was built from a hash-verified
source snapshot, with an independent upstream Zstd 1.5.7 test decoder. Worker job
`20261119T104512Z-run-script-dd-win-prof-7d908250` passed the complete validation:

- 246/246 native Release tests and 246/246 MSVC ASan `RelWithDebInfo` tests, with
  zero failures, errors or disabled tests. This includes the three reset-cadence
  tests and the nine earlier main-merge tests.
- Seven dependency/example CTest checks, C exporter protocol tests and 12 Python
  serialization/decoder tests.
- Source/installed consumers, downstream isolation, both dependency archives'
  namespaces and CRT settings, and DLL import/export audits.
- prof-correctness v1.0.0 callback/Runner checks, WinHTTP loopback and failure
  cases, RUM, UI-hang sleep/wait/CPU and restart checks.
- All six decoded baseline/candidate fixtures matched, including embedded NULs,
  seven columns, custom periods and reset/clear cycles.

The first attempt, `20261119T103958Z-run-script-dd-win-prof-d29fe53f`, passed
245/246 native tests. An earlier RUM test adaptation incorrectly required a comma
after the final tag. The assertion now extracts the tag field and checks complete
comma-delimited tokens, including the last one. Only that test changed between
attempts; production, producer and baseline sources were unchanged. The passing
job reran every phase using the existing isolated build directories, not
`-RemainingChecksOnly`.

Evidence is under `winshare/artifacts/prof-16455-libdd-c-api-4e37730-20261119-rerun/`:
`validation.log`, source/tool manifests, CRT inventories, native/ASan XML and
captures. Worker logs use the job IDs above under `winshare/worker/logs/`.

GitHub CI is not green: its preceding run `43845705637` failed during configuration
because it could not authenticate cloning private `DataDog/libdd-c`, including in
the format job before formatting ran. Cross-repository CI credentials are unchanged.
PR #95 also conflicts with the newer main changes noted above; resolving that merge
is separate from this API update. No live intake or performance result is claimed.

### Results recorded 2026-11-16 (original pre-main integration)

Windows job `20261116T093328Z-run-script-dd-win-prof-97e367a1` used producer
`c7c8a8520c99fddb87f04f8a1fe76258f2854dfa`, verified by SHA256 source manifests before building.
The isolated Release build passed:

- 234 native tests, seven dependency tests and the C exporter protocol tests.
- Source/installed consumers, downstream isolation, CRT and DLL import/export
  audits; 297 dependency archive definitions had permitted namespaces.
- 12 Python serialization/decoder tests and WinHTTP loopback, rejection,
  redirect, timeout and refusal tests, including decoded uploaded bytes.
- prof-correctness v1.0.0 callback workload and existing Runner expectations.
  The callback profile contained 318 samples, including 126 workload samples,
  with one final callback, exact callback/profile timestamps and a 10 ms period.
  Workload CPU/wall totals were 1,898,119,100 / 1,969,796,900 ns for the two-second
  workload. These are correctness observations, not performance measurements.
- RUM sessions/views and UI-hang sleep, wait, CPU and two-session restart checks.

That primary job **failed** afterwards: the baseline fixture writer could not
open its output files because the wrapper had not created the `before`/`after`
directories. After fixing that setup bug, continuation job
`20261116T094729Z-run-script-dd-win-prof-a60fe482` **passed**:

- All six before/after fixtures matched after prof-correctness decoding and
  google/pprof serialization: custom, seven-column and embedded-NUL definitions,
  each before and after reset.
- All 234 native tests passed under MSVC ASan in `RelWithDebInfo`, with zero
  failures, errors or disabled tests.

The continuation reused the primary checks rather than rerunning them. Its
manifests verify that only this guide and the validation wrapper changed between
runs; all production sources, the dependency and baseline were unchanged. No
production regression was identified.

Workspace-relative evidence:

- `winshare/worker/logs/20261116T093328Z-run-script-dd-win-prof-97e367a1.log`
- `winshare/artifacts/prof-16455-libdd-c-integration-20261116-prof-correctness/`
  contains source/tool manifests, `validation.log` and `build-libdd-c-capture/`.
- That capture directory contains `independent-decode.json`, `callback.json`,
  `callback.zst.pprof`, the prof-correctness stack dump `callback.zst.json`, native
  test XML, CRT/import/export audits and standalone/UI-hang captures.
- `winshare/worker/logs/20261116T094729Z-run-script-dd-win-prof-a60fe482.log`
- `winshare/artifacts/prof-16455-libdd-c-integration-20261116-remaining/`
  contains the continuation transcript, primary/current source manifests,
  `primary-source-consistency.json`, ASan cache and
  `build-libdd-c-capture/{before,after,asan-tests.xml}`.

Windows used the explicit source override: a normal Windows network fetch hit
Git Credential Manager authentication. Normal fetching at the recorded revision
passed on macOS, as did the public harness/dependency suite (9 tests), local-source
harness suite (2 tests) and its 41-run public-API smoke with independent decoding.
The dependency still follows the approved temporary branch; pinning a full SHA
remains a release TODO. No live intake/backend-indexing or memory-budget claim is
made.

## Historical evidence and smoke harness

The historical v19/v47 measurement trees, raw profiles, reports and checksum
inventories remain in `packages/profile-aggregation`. All 88 historical docs and
measurement files were verified byte-for-byte against the consumer base. The
diff removes 49,112 lines of copied engine/public header/vendor code, plus the
schema and core-only tests. Historical results describe old commits, not this
head. The old private compression hooks and matrix launcher are retired;
no fake compression-only timings replace them. The small public-API harness can
still be built independently:

```sh
cmake -S packages/profile-aggregation/benchmarks/harness -B build-libdd-c-harness \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-libdd-c-harness --parallel 4
ctest --test-dir build-libdd-c-harness --output-on-failure
```

Do not mix new outputs into historical evidence directories. This migration does
not run the old matrix, download ddprof or launch a Linux profiling campaign.
