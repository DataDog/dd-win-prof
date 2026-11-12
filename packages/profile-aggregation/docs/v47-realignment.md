# PROF-16455 v47 realignment checks

## Source and boundaries

Four experimental commits were rebased from parent `11eae539` onto
`96d14f8a83f1267830bdb8877f50222aec0b1b84` with SSH signing enabled
(`git rebase --gpg-sign`, `GIT_EDITOR=true`). Resulting commits:

- `3c38730`: reusable C core
- `dc27d31`: vendor symbol isolation
- `6311051`: retained-storage evidence
- `b605bb2`: independent C/WinHTTP exporter

`git log --format='%h %G? %s' --max-count=4` reports `G` for all four, with the
existing SSH signing key. No signing settings were disabled. Follow-up adaptation,
review fixes and fresh evidence are separate from these four rewritten commits.
The published head `b14586a` is preserved by local backup branch
`r1viollet/prof-16455-before-v47`. Rewriting the remote requires explicit
force-with-lease approval. Historical benchmark bundles and evidence files are
unchanged from the pre-rebase experimental tip.

Upstream codec/native-wire, callback ownership and loopback HTTP coverage remains.
Rust-specific opaque-handle/interner tests use C IDs instead. C has no unconfigured
custom slot or five-slot ceiling. Invalid UTF-8 still fails initialization. Exact
ordered UTF-8 type/unit definitions, including embedded NUL, use the added
`pa_create_with_types` constructor; the old NUL-terminated constructor is retained
as a compatibility wrapper over the same implementation. Callback ABI and
ownership are unchanged. Both constructors copy input and preserve it across reset.
The internal definition table adds two `size_t` lengths per column, not a per-sample
allocation. The independent exporter keeps tags/internal RUM and the upstream
`profile.pprof` name; no event sample-type field was invented.

The public v47 headers came from the parent-verified Windows release ZIP, SHA512
`168d2733c616b5b6422963194c66333cd5b3745a83ed9b8104e69f1fe271728cb8c9f4ee3c456f30d26923e7972c876439c7c18e26117e9435c74bcd908cda25`.
The runner pins these release-member SHA256s:

- `profiling.h`: `7f90cc0670fbc9da7c4d56b09c5db60fa5f1619a0486dff33bf470bb72b22a2f`
- `common.h`: `273d994bcc8e5c1905f81b4f4023ec6de8dd79690f804a6360f826efce331a41`
- release/static archive: `27f6577e54d74dc32c8ce97c903786249a48045cf2bfa929ce6963527aa5f34f`

Pinned source inspection used v47.0.0 commit
`ef6371243c020319e74e8531035f0a5a2fcb8858`. The legacy public Profile API resets
before serialization and reconstructs ordinary interners. It does not exercise
ProfilesDictionary or a dictionary TTL. The empty-string ID is immortal; all
other driver IDs are reacquired. Timestamp zero means absent. Native v47 uses
Zstd level 1 with default window; C uses level 1/windowLog 17. These differences
are documented, not normalized by private-layout access or Rust instrumentation.
The primary whole-flush metric includes output release and excludes the redundant
baseline explicit reset. Separate latency, allocator and compression processes,
recipe/call counts, affinity, CRT and replicate defaults are retained.

## Host validation

All host builds below use AppleClang 17 on macOS and output under `/tmp`, not the
source tree. Python decoder checks use the existing isolated
`/tmp/prof16112-zstd-frame-tests/bin/python` environment. These are correctness
checks, not fresh Windows performance evidence.

| Command/check | Result |
| --- | --- |
| `cmake -S packages/profile-aggregation -B /tmp/prof-16455-v47-core-release -DCMAKE_BUILD_TYPE=Release`; build; CTest | 2/2 pass, ordinary and forced-collision suites |
| `aggregation_tests /tmp/prof-16455-v47-core-native.zstd.pprof` | Pass, rich metadata/values/timestamps/reset/allocator and new sized-definition checks |
| `aggregation_bench 100000` | Five workloads finish; not a matched measurement |
| Core Release with `-fsanitize=undefined -fno-sanitize-recover=all` and UBSan linker flag, `/tmp/prof-16455-v47-core-ubsan` | 2/2 pass |
| Exporter Release configure/build/CTest, `/tmp/prof-16455-v47-exporter-release` | 1/1 pass, exact framing and profile attachment name |
| Exporter Release+UBSan configure/build/CTest, `/tmp/prof-16455-v47-exporter-ubsan` | 1/1 pass |
| Harness Release configure/build/CTest, `/tmp/prof-16455-v47-harness-release` | 2/2 pass: result-version checks plus 51 recipe/lifecycle/allocator cases |
| Harness Release+UBSan configure/build/CTest, `/tmp/prof-16455-v47-harness-ubsan` | 2/2 pass |
| `test_harness.py <Release bench_core> <Release bench_core_timed> --decode` | Pass, 51 cases, independent wire semantics, exact sizes, timed/normal identity, three-cycle recovery |
| Same independent `--decode` against UBSan executables | Pass, 51 cases |
| `test_results.py -v` in decoder environment | 7/7 pass: version isolation, manifest-bound recipe/counts and complete schedule, swapped-workload negative regression, strict Zstd frames |
| `python -m unittest discover -s src/integration-tests -p 'test_*.py' -v` | 12 cases: 7 codec passes, 5 Windows-native skips; all 12 pass on Windows |
| `/usr/bin/clang++ -std=c++20 -DPA_BENCH_V47 -DPA_BENCH_BASELINE_VERSION=47 -I<actual-v47-release>/include -fsyntax-only packages/profile-aggregation/benchmarks/harness/driver.cpp` | Pass against actual public release headers, no linking/runtime claim |
| Core `-Wall -Wextra -Werror -fsyntax-only` using actual CMake vendor-remapping definitions | Pass |
| Exporter protocol `-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -fsyntax-only` | Pass |
| Independent Python parse of rich-native fixture | Pass: 4 samples, 1 location, 1 function, 2 mappings, expected type pairs |
| Negative control, temporary external serializer with explicit lengths replaced by `strlen` | Expected failure at sized-definition decoded `wait\0time` length assertion, exit 1 |
| Source/doc `git diff --check`; isolated clang-format 20.1.8 check of changed native files | Clean whitespace and native formatting; exact raw artifact copies retain Windows CRLF/EOF |

Two invocation mistakes were corrected rather than hidden: Homebrew LLVM 17's
bare `clang++` could not find compatible macOS SDK C headers; `/usr/bin/clang++`
passes. A direct standalone core syntax command omitted the required private
vendor symbol remaps; repeating with the actual CMake compile definitions passes.
Neither failure required a source workaround. Raw GitHub source fetching failed
with curl exit 56; the parent supplied the pinned source archive instead.

## Windows results

Parent-owned isolated control passes 229 native tests; the actual archive uses
`/DEFAULTLIB:LIBCMT`. Candidate Release passes 233 native tests, core/collision and
exporter suites, downstream/install/consumer/import inventories, 101 RUM and 23
UI-hang assertions. Nine returned profiles independently decode.

Fresh matrix `run-20261112T085300Z` passes 312 cases, full recipe/schedule checks
and 20 semantic groups / 60 independent fixtures. [The report](../benchmarks/windows-v47/README.md)
includes C's slower unique-stack ingestion, multi-millisecond growth pauses,
higher high-cardinality retention, compression differences and whole-flush costs.
The historical v19 run also passes the strengthened inspector without changing
its artifacts. Replacing the checker with its old body makes the wrong-workload
regression fail because the summarizer returns a false success.

Windows follow-up job `20261112T090836Z-run-script-dd-win-prof-ff2b5494` passes 233
Release tests and all 12 Python cases. The seven-column test now records distinct
values in two generations with a 7 ms period, instead of checking an empty frame.
Independent decoded assertions verify order, repeated definitions, every value
and the period. Windows ASan passes 233 native tests, core 2/2, exporter 1/1 and
five native wire cases; runtime import is verified and leak detection disabled.

Five-state small app `run-20261112T085905Z` returns 18 valid profiles / 13,960
observations, with one final callback and no errors per enabled variant. Sampling
is configured at 20 ms while both unchanged adapters emit 10 ms period metadata.
This pre-existing mismatch is recorded, not repaired in this task.

Final fresh read-only review found no blocker, reproduced acceptance JSON exactly,
repeated the negative control and checked saved ASan/native/small-app profiles.
Only four postprocessing/test files and two wire-test files differ from the
measured 269-file snapshot; all 263 other entries, including production/driver
sources, match. Hashes are in the report's `analysis-changes.json`.

## Remaining gates

New-head GitHub CI after publication; fresh v47-realigned intake/indexing; the
external dirty SDK install-rule migration and full SDK packaging/live delivery;
long-soak/full-profiler residency and release pipeline qualification. Old-head
CI/staging indexing are historical evidence only. Host ASan still does not run.
The remote draft is not force-updated without approval.
