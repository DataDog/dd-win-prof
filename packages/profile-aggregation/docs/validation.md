# Validation and remaining gates

## Current retained-storage validation

The current implementation retains normal working storage and caches strings across exports. Three consecutive unused completed exports expire a string; failed/repeated serialization does not advance multiple cycles. The historical 307-byte reset measurements below predate this policy.

- Parent reran Release and UBSan normal/forced-collision suites: **2/2 each**, plus the **51-run** host harness with independent decode, warmed zero-allocation/free checks and spike recovery. Added untimed numeric-label sum-map growth/OOM/atomic-retry/trimming coverage passes.
- Windows memory-policy matrix `20261109T145706Z-run-script-dd-win-prof-a5b0acf5`: **312 complete runs**, all manifest/phase/percentile/allocation/affinity/CRT checks, **20 groups / 60 strict fixtures** compared with v19 CFFI. Exact measured core/header sources and metadata are archived in `benchmarks/windows-v2/run-20261109T145716Z/`.
- Rebased onto upstream `11eae539`, preserving UI-hang detection and configuration/session reset behavior. Windows integration `20261109T151035Z-run-script-dd-win-prof-ea675e2d`: **220 behavioral tests**, both core suites, callback ownership, source/installed/downstream consumers, consumer isolation/import inventories, **101 RUM assertions** and **23 UI-hang assertions**. UI cases cover sleep/wait/CPU and two-session restart.
- Eight returned core/native/standalone/UI captures, **8,508 observations**, independently decoded and metadata-reference checked on the host. See `windows-memory-reuse-validation.json` for per-file hashes/counts.
- Independent read-only reviews of the core and upstream integration found no blockers. Windows runtime checks subsequently passed.
- Fresh small-app job `20261109T151820Z-run-script-dd-win-prof-4a2f0bbf` covers five sequential 45-second states. Enabled C versus no profiler linked adds **6.18 MiB working set / 3.05 MiB private commit** in the small phase, **6.45 / 3.31 MiB** in recovery. Both enabled variants deliver nine profiles, one final callback and no errors; all 18 captures independently decode. Snapshot: `benchmarks/windows-small-app/run-20261109T151820Z/`.
- A subsequent namespace audit found and prefixed `g_debuglevel` plus two MSVC-emitted Zstd helpers. Windows job `20261109T152926Z-run-script-dd-win-prof-2743f3c2` rebuilt and passed both core suites, all 220 behavioral tests and the 297-definition archive audit. Host audit finds 233 definitions, all `pa_`/`ddpa_`, with no benchmark hooks. The Windows validation script now checks defined archive symbols. Performance/memory cohorts predate these identifier-only changes; their exact measured sources remain preserved.

The [current Windows report](../benchmarks/windows-v2/README.md) records allocation reductions, retained memory, stable short-run recovery and remaining cold-growth stalls. The original baseline and packed-stack matrices remain historical evidence, not measurements of the new policy. Live intake, full-SDK/long-soak residency and Windows ASan remain open.

## Portable core, macOS arm64

Executed on the host with AppleClang 17, C11, Release:

```sh
cmake -S packages/profile-aggregation -B /tmp/prof-16455-core -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/prof-16455-core -j4
ctest --test-dir /tmp/prof-16455-core --output-on-failure
```

**2/2 passed**: normal rapidhash and an alternate build forcing every interned key to the same hash. Tests independently decompress Zstd and decode protobuf, checking populated 64-bit mappings/ranges/offsets/PDB IDs, native addresses/functions/files/lines, numeric label zero/unit, process/RUM labels, configurable types/period/times, repeated timestamped observations (including explicit epoch zero), untimed saturation/sums, resets/stale IDs, strings/observations crossing compression staging boundaries, allocator failures and failed-output retries.

The allocator verifies no leaked requested bytes at destroy, no warmed untimed-record allocation, and no accumulating arena payload on repeated failed table insertion. The failed-insert regression was also run against an isolated `/tmp` copy with only rollback removed: it **failed** on the repeated failed-insert assertion. Production source was not temporarily reverted while Windows builds ran.

UBSan:

```sh
cmake -S packages/profile-aggregation -B /tmp/prof-16455-core-ubsan \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_FLAGS='-fsanitize=undefined -fno-omit-frame-pointer -Wall -Wextra' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=undefined'
cmake --build /tmp/prof-16455-core-ubsan -j4
ctest --test-dir /tmp/prof-16455-core-ubsan --output-on-failure
```

**2/2 passed**, no diagnostics. First-party core also compiled with `cc -std=c11 -Wall -Wextra -Werror`.

AddressSanitizer is **not validated** on this host: both AppleClang17 and HomebrewClang20 executables hang before `main`, inside sanitizer malloc/shadow-memory initialization. `sample` showed recursive malloc during sanitizer initialization and a `StaticSpinMutex` wait. No test code executed. A separate Homebrew binutils archive-format mismatch was avoided with a clean PATH; that did not solve the ASan runtime deadlock. Windows ASan remains a separate gate.

`nm -gU /tmp/prof-16455-core/libprofile_aggregation.a` showed only `pa_*` and `ddpa_*` external definitions. No generic Zstd/nanopb symbols or libdatadog references were present.

## Decoded comparison against libdatadog v19

The parent independently built an unmodified v19 source archive using the original v19 Cargo lock offline in Release. The harness and lock are retained under `tests/baseline/`, with the source path supplied by the caller. The test-only Rust executable emits ordinary libdatadog output, not modified opaque bytes.

Reproduction (use a Python with supported binary wheels, e.g. Python 3.9–3.13):

```sh
# Extract libdatadog commit 0478a4e78d011404763c13787b369aca93e40625 to /tmp/v19-source.
python3 packages/profile-aggregation/tests/prepare_baseline.py \
  --libdatadog-source /tmp/v19-source --output /tmp/v19-comparison
(cd /tmp/v19-comparison && cargo run --locked --release --bin fixture)
/tmp/prof-16455-core/aggregation_tests /tmp/core-native.zst.pprof
python3 -m venv /tmp/profile-validation
/tmp/profile-validation/bin/python -m pip install zstandard==0.25.0
/tmp/profile-validation/bin/python packages/profile-aggregation/tests/compare_baseline.py \
  /tmp/core-native.zst.pprof /tmp/v19-comparison/native.pprof \
  --report /tmp/semantic-comparison.json
```

**PASS**, rerun locally with fresh C output. Comparison normalizes IDs and field ordering rather than requiring byte identity. Matches native mapping/PDB/address/function/file/line metadata, types/period/times, two distinct identical-timestamp observations, untimed sums and string/numeric-unit labels. The C API's explicit epoch-zero observation and function start-line extension are separately asserted because v19's interning API cannot express those. Unreferenced metadata is not part of semantic sample comparison. `docs/semantic-comparison.json` records the independent parent result.

The existing generated-protobuf Python decoder was extended to detect Zstd and successfully decoded the fresh fixture's labels as well. Python 3.15 wheel installation failed on this host (toolchain/system-header issue); an isolated `/usr/bin/python3` 3.9.6 venv succeeded. No user Python environment was modified.

## Historical v1 benchmarks and memory

**Historical exploratory data only.** Unequal instrumentation/setup/lifecycle costs bias the relative timings and allocator counts below. Use the [matched Windows v2 results](../benchmarks/windows-v2/README.md) for the current comparison; old CSVs remain for provenance.

Raw results are in `benchmarks/`. Both implementations ingest 100,000 observations with the same two sample types, 10 ms period, 16-frame stacks (256 for deep), per-observation stack/label-set interning and timestamps `i+1`. Unique/churn also intern empty functions and distinct native locations; labels mode interns the label key/value on every observation. Churn flushes/resets every 1,000 observations. Core-only record throughput is not presented as matched end-to-end ingestion.

Median of three Release host runs, milliseconds:

| Workload | C ingest | v19 ingest | C encode+Zstd | v19 encode+LZ4 |
| --- | ---: | ---: | ---: | ---: |
| repeated | 5.577 | 14.557 | 29.834 | 32.278 |
| mostly unique | 22.097 | 20.125 | 42.043 | 42.738 |
| 256-frame deep | 27.115 | 81.771 | 315.622 | 227.296 |
| label/string churn | 22.469 | 27.626 | 38.397 | 41.604 |
| flush/churn | 17.824 | 17.414 | 45.382 | 45.426 |

**V1 observations, not validated comparative regressions**: unique ingestion, deep encoding, and slight churn ingestion. There is no blanket parity claim. Per-record max latency is in each CSV; rehash/vector growth causes stalls. The C timer is platform-resolution dependent (whole-microsecond values on this host). Rust allocation accounting uses global atomics, adding overhead not present in the C callback allocator. Rust compresses LZ4, C compresses Zstd, so encode times include different compressors. These are host microbenchmarks, not Windows capture measurements.

For 100k repeated/deep observations, core live requested storage before serialization is about **6.34 MB**, peak about **9.49 MB** (observation-vector growth overlaps old/new capacities); reset retains **307 bytes** of configuration. Unique and label workloads are much larger. C aggregation makes about 15 allocations after warm setup over repeated/deep ingestion+serialization, versus roughly 600k/1M respectively for the v19 harness; allocation counts include the different serialization paths and are not per-record allocation promises. Reset releases high-water arrays/hash/arenas rather than retaining an unbounded peak.

`core-small-6000.csv` models a 60-second interval at 100 observations/second. Repeated-stack core live storage is about **0.44 MB**, requested peak about **1.42 MB**, standalone benchmark process peak RSS about **2.9 MB**. This is not total profiler overhead. It excludes Windows stack sampler/thread data, DbgHelp, symbol cache, logger, callback queue, SDK export buffers and transport.

`core-churn-1000000.csv` includes 1,000 churn flush/reset cycles. Churn requested core peak stays about **1.48 MB**, resets retain 307 bytes. The same process previously runs million-sample high-cardinality modes, so its reported peak RSS is a process-lifetime high-water mark and does not demonstrate resident recovery. Requested allocation reclamation is not proof of RSS returning to baseline. Windows measurements below cover a short synthetic application, not a long-running full-SDK soak.

## Performance v2 harness and Windows measurements

The independently configured benchmark-only project at [`benchmarks/harness`](../benchmarks/harness/README.md) supersedes v1 as relative performance evidence; it does not retroactively correct the historical CSVs above. V1's direct-Rust atomic allocator instrumentation, per-record clocks, unequal setup and differently classified churn reset/reconstruction costs bias relative times and allocation counts. Do not treat those numbers as an unbiased C/CFFI comparison.

V2 compiles one shared workload driver against the unchanged C API and the existing v19 CFFI binary package. It separates throughput (batch clocks, default allocators) from latency and allocation diagnostics; runs both timestamped observations and actual untimed aggregation; excludes reset/reseed from ingestion; retains complete compressed output for both backends; reports populated discard/reset separately. V19 serialize implicitly drains/resets, while its optional redundant explicit reset is reported separately. Windows current/peak working set, private committed bytes and peak process commit are distinct columns, captured in fresh processes with per-flush recovery traces.

A third, diagnostic-only C target privately compiles compression hooks and counts uncompressed protobuf bytes. Its non-compression serialization remainder includes sizing/emission/allocations/sink work, not pure encoding. No production API or wire-format changes are introduced. Normal/timed/allocator-instrumented C fixture identity, stage/percentile invariants and independent recipe/pprof semantic checks are host-tested. The final matched Windows matrix passed **312 cases**, seven primary repetitions and three latency repetitions per configuration, with independently checked phase/allocation/percentile/affinity/CRT invariants and **20 fixture groups / 60 files** compared with v19 CFFI. Production core suites still pass with hooks disabled.

Historical packed-stack result, before memory reuse: see the [packed report and preserved evidence](../benchmarks/windows-v2/packed.md). Packed location IDs plus cached stack payload sizes reduce timestamped deep equivalent flushing from **449.059 to 179.231 ms** per 100k records, versus **329.025 ms** for v19. Untimed churn flush falls from **81.633 to 66.836 ms**, close to v19's **68.498 ms**. Both 312-case matrices and their exact measured source versions are preserved. Ingestion medians vary slightly between cohorts; no universal unchanged-ingestion claim is made. C still has higher populated working set and reproducible growth-associated multi-millisecond latency tails. Requested core allocation/count/live/peak/reset statistics are unchanged in all ten diagnostic scenarios, retaining 307 bytes after reset. That is not an RSS or full-SDK budget claim.

The new packed-wire assertions fail against an isolated original-serializer build. A 16,384-frame regression checks multi-byte packed IDs/lengths, staging boundaries, repeated serialization and metadata growth after cache initialization. Host Release/UBSan normal and forced-collision suites pass. Full Windows validation after packing, job `20261109T112114Z-run-script-dd-win-prof-75f066a6`, passed 206 behavioral tests, both core suites, consumer/isolation/install/import checks and 101 RUM assertions. No public API, additional allocation or retained encoded-stack byte buffer was introduced.

## Windows / SDK / receiving path gates

`e2e-tests/validate-c-aggregation.ps1` builds the C core, Windows DLL, all migrated behavioral Tests and Runner; exercises busy-thread SDK callback capture and final owned bytes; installs the profiler and a source consumer; builds an installed downstream consumer; inspects imports; runs standalone local-debug capture with HTTP disabled. A narrowly scoped repair removes only the known file artifact from the initial Runner copy bug, never an output directory.

Worker job `20261109T093655Z-run-script-dd-win-prof-ba186db5` passed **206 tests from 20 suites**, both core variants, callback capture/final ownership, source consumer/install/downstream checks, standalone local capture, and DLL/Tests import inspection. No libdatadog artifacts were found in the installed package or imports. The first attempt's Runner copy-before-directory bug was fixed.

Final review also found two existing-CI integration gaps: local RUM capture still enabled unsupported HTTP and expected old log messages; the C package unconditionally generated CTest dashboard targets in production consumers. Local RUM now disables HTTP with environment restoration and checks the final C export; C tests call `enable_testing()` only when explicitly enabled. The validation script now runs the original consumer-isolation and RUM scripts, not just a replacement smoke test. Pre-optimization final worker job `20261109T101116Z-run-script-dd-win-prof-9f386c9b` **passed** after the cache-reclamation and CI-script fixes: 206 behavioral tests, both core suites, existing `scripts/test-consumer.ps1` isolation checks, source/installed/downstream consumer execution, package/import checks and **101 RUM integration assertions** over three scenario iterations. The script also checks actual locally decoded Zstd RUM labels and metadata. Final callback capture (316 samples), symbolized standalone capture (292 samples, 45 functions) and the core fixture were independently decoded again on the host; IDs and native references were valid. Per-file counts are in `docs/windows-final-capture-decoding.json`.

Intermediate retries exposed and fixed a formatter-reordered Windows header dependency (`windows.h` must precede `psapi.h`) and worker-bridge path issues (a copied macOS `.git` pointer, read-only cleanup and short Windows temp paths). One-iteration RUM validation was not a valid test invocation because the existing script expects at least two completed S2 session rotations; the final run uses its standard three iterations. No production RUM assertions were weakened. Full worker logs/results remain under `win-sync/winshare/worker/` using the job IDs above.

The SDK receiver at `dd-sdk-cpp/src/datadog/impl/profiling/profiling.cpp` passes callback bytes through TLV into a `profile.pprof` multipart attachment. Profiling backend `prof-utils/.../io/Decompressor.java` recognizes magic `28 b5 2f fd` as Zstd and its tests include Zstd. This is source evidence, **not live intake/indexing validation**.

Remaining integration gates:

- Standalone HTTP is explicitly unsupported until a supported upstream byte-input request builder exists. Enabling it must fail, not silently discard uploads.
- The external dirty dd-sdk-cpp checkout has an obsolete libdatadog target install rule. Required removal is documented in the package README; that checkout was not edited. Source consumer installation is not proof of full SDK package installation.
- Live SDK intake/receiving-path compatibility and backend indexing are not validated by copying opaque bytes.
- Windows capture and lifetime evidence covers the fixtures below, not a full SDK live upload.
- Full-SDK residency/queue/export overlap, cold-start transients, CPU overhead and a long-duration Windows churn soak remain unvalidated. Host ASan did not execute tests.

## Historical Windows small-application residency

These measurements predate retained storage. Do not apply their memory deltas to the current implementation; the [current report](../benchmarks/windows-v2/README.md) records subsequent evidence.

Worker job `20261109T094201Z-run-script-dd-win-prof-68839bbc` ran matched baseline/C-core enabled and disabled applications. Job `20261109T095845Z-run-script-dd-win-prof-3f7053a2` added a third application build that does not link/load the profiler. Source and reproduction are in `e2e-tests/c-aggregation-small-app/`; raw measurements and summaries are in `benchmarks/windows-small-app/`.

MSVC 19.44, Windows 10.0.26200, Release, 20 ms sampling, 5 s flush, symbol names disabled. Each process ran 45 s: 15 s small repeated work, 15 s short-lived-thread churn, 15 s recovery. Working set and private committed bytes were sampled every 100 ms. One owned callback buffer was retained; the C buffer peak was about 13.1 KiB. Baseline and C each delivered nine profiles, exactly one final callback, no callback errors.

Median MiB, values are **working set / private committed bytes**, not interchangeable:

| Build/state | Small | Churn | Recovery |
| --- | ---: | ---: | ---: |
| No profiler linked | 12.879 / 5.336 | 13.023 / 5.418 | 12.973 / 5.324 |
| Baseline loaded, disabled | 16.988 / 5.961 | 17.137 / 6.102 | 17.137 / 6.102 |
| Baseline enabled | 17.906 / 12.215 | 18.250 / 12.398 | 18.242 / 12.305 |
| C loaded, disabled | 15.625 / 5.859 | 15.758 / 5.938 | 15.758 / 5.938 |
| C enabled | 16.305 / 6.793 | 16.629 / 6.961 | 16.672 / 6.977 |

For this small scenario, enabled C versus **no profiler linked** added about **3.43 MiB working set / 1.46 MiB private commit**. Versus the same DLL loaded but disabled, enabling added about **0.68 MiB / 0.93 MiB**. The 2.75 MiB disabled-load working-set contribution includes the profiler image/native dependencies and initialization, not just aggregation objects. Core-only requested-storage figures are separately reported above; no per-component resident attribution was measured. Recovery-phase working-set delta versus no-profiler was 3.70 MiB, not an unbounded short-run increase.

This observation is below the 5–10 MB residency target **for the defined fixture only**. It is one sequential run per state, not a confidence interval or an arbitrary-application guarantee. Initialization precedes sampling, short compression/export peaks may fall between samples, Windows working set includes shared pages, and the disabled executable still loads the DLL unless explicitly built with `SMALL_APP_NO_PROFILER`. The full SDK's storage/upload buffers and standalone transport are absent. A long-duration soak and repeated randomized-order runs remain needed.

All nine real Windows C captures (6,952 samples) and nine baseline LZ4 captures (6,935 samples) were independently decompressed and parsed using generated Google pprof protobuf bindings. Every sample had the configured value count, valid location/mapping/function references and a positive `end_timestamp_ns`. `docs/windows-capture-decoding.json` records per-file counts. Different scheduling means these captures are not sample-by-sample comparison fixtures; the fixed native fixture above provides that semantic comparison. This proves local decoding, not live intake acceptance.
