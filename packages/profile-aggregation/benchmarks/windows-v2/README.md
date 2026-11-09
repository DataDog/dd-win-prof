# Windows aggregation: retained storage and string cache

## Result

Normal exports now reuse working storage instead of freeing and rebuilding it. In the Windows churn workload (100 exports of 1,000 records), counted ingestion allocations fall from **2,300 to 13** for timestamped records and **3,200 to 15** for untimed records. These totals include warmup. Timestamped ingestion falls from 54.933 ms to 20.817 ms; equivalent flush falls from 82.249 ms to 60.952 ms. Cross-cohort timings are observations, not isolated causal estimates.

This deliberately retains memory. The timestamped churn case peaks at **1.84 MiB requested core storage**, versus 1.41 MiB before, and retains **1.79 MiB** after reset. Destroy returns requested live bytes to zero. The old 307-byte reset target no longer applies.

The [packed-stack report](packed.md) and [original baseline](baseline.md) remain historical evidence for the earlier storage policy. Their source snapshots and results were not changed.

## Policy

- Modest initial capacities and a reusable compression workspace are allocated at creation. There are no public reserve/capacity knobs.
- Normal reset reuses observations, metadata, maps and arena blocks. It makes no allocation.
- Recent demand from three completed exports controls exceptional-spike trimming. A smaller export does not immediately shrink everything.
- String contents survive reset, but public IDs still expire. Strings unused for three consecutive completed exports are evicted. Mapping, function and label references count as use.
- Failed serialization and retries do not advance the clock. Repeated successful serialization of one profile counts once, when reset finalizes it.
- Eviction reclaims/reuses payload storage, not just map entries. Compaction is transactional on allocation failure.

The exact floors, trimming bounds and lifetime rules are in [memory-reuse.md](../../docs/memory-reuse.md).

## Matched Windows matrix

Job `20261109T145706Z-run-script-dd-win-prof-a5b0acf5`, [run run-20261109T145716Z](run-20261109T145716Z/): **312 complete cases**, seven primary and three latency repetitions, mask 4, shuffle seed 16457, `/MT`, the same machine and Balanced power scheme. Only core source and public-header hashes differ from the prior packed matrix; driver, build/runner inputs, vendor and v19 baseline hashes match. The header changes document retention semantics; no API signatures changed.

Every case passed phase/percentile/allocation/affinity/CRT acceptance. All **20 groups / 60 fixtures** passed independent decode, recipe checks and comparison with public v19 CFFI. Normal and instrumented C emitted identical fixture bytes. C uses Zstd; v19 uses LZ4.

Median milliseconds for 100,000 input records. C flush is serialize+reset; v19 serialization already resets, so its redundant explicit reset is excluded. Churn includes all 100 flushes. Output destruction and setup/reseed are separate.

| Timestamped workload | Ingest C / v19 (ms) | Flush C before / after / v19 (ms) |
| --- | ---: | ---: |
| repeated | 12.060 / 42.352 | 32.959 / 31.693 / 57.915 |
| unique | 55.987 / 60.334 | 61.097 / 57.266 / 77.122 |
| deep256 | 49.701 / 175.803 | 179.231 / 177.941 / 328.730 |
| labels | 54.794 / 93.245 | 52.191 / 54.739 / 74.776 |
| churn | 20.817 / 58.096 | 82.249 / 60.952 / 99.235 |

Untimed unique ingestion remains slightly slower than v19: **67.198 / 66.254 ms** (+1.4%). No universal speedup or production parity is claimed. Raw ranges and quartiles are in the snapshot.

## Allocation and memory cost

Separate counted-allocator processes, not throughput processes. Requested bytes exclude output/scaffolding, CRT and OS allocations.

| Workload | Timestamped | Ingest allocations before / after | Peak requested core memory before / after (MiB) | Retained after reset (MiB) |
| --- | --- | ---: | ---: | ---: |
| churn | False | 3200 / 15 | 1.46 / 1.85 | 1.83 |
| churn | True | 2300 / 13 | 1.41 / 1.84 | 1.79 |
| deep256 | False | 2 / 0 | 0.98 / 1.47 | 1.47 |
| deep256 | True | 14 / 4 | 9.05 / 10.10 | 7.10 |
| labels | False | 389 / 376 | 32.27 / 35.21 | 34.07 |
| labels | True | 374 / 368 | 29.02 / 31.97 | 30.83 |
| repeated | False | 2 / 0 | 0.98 / 1.47 | 1.47 |
| repeated | True | 14 / 4 | 9.05 / 10.10 | 7.10 |
| unique | False | 1027 / 1007 | 42.04 / 42.09 | 42.09 |
| unique | True | 1012 / 999 | 38.79 / 38.85 | 38.85 |

The 100k-record stress cases can retain tens of MiB after their first reset. This is intentional recent-demand retention, not a claim that all capacity is immediately released. Subsequent normal completed cycles are required for trimming. The package regressions exercise both timestamped observations and high-cardinality untimed sums, including sum-map growth failure and atomic retry.

## Repeated normal exports and recovery

A separate Windows recovery run makes 30 exports of 1,000 records, 30 exports of 6,000, then 30 exports of 1,000. Steady checkpoints below exclude the first five cycles of each phase. Each variant is one fresh process; there are no confidence intervals.

| Variant | After-reset small working set / private commit (MiB) | Burst | Recovered small |
| --- | ---: | ---: | ---: |
| Timestamped | 12.535 / 6.793 | 15.113 / 8.844 | 13.293 / 7.633 |
| Untimed | 12.438 / 6.863 | 15.273 / 9.059 | 13.262 / 6.906 |

Memory stabilizes over the final 25 normal exports, but it does **not** return exactly to the initial process baseline. Timestamped private commit stays about 0.84 MiB higher; working set about 0.76 MiB higher. These are whole test-process counters, not profiler overhead relative to an unprofiled app, and allocator page retention can differ from requested-core storage. Do not infer a full-SDK memory budget or long-soak result from them.

Release and UBSan core suites each pass 2/2; the 51-run host harness passes independent decode, zero allocation/free in warmed normal cycles, allocation-free reset and three-cycle spike recovery. Core tests cover mixed hot strings/new-string churn, indirect references, exact three-completed-export expiry, failed-output retries, public-ID expiry, compaction failure, oversized-block rollback and numeric-label sum-map OOM/trim. These checks measure storage reuse and correctness; they are not a long-running SDK residency test.

## Windows integration after upstream changes

The branch was rebased onto `11eae539`, retaining upstream UI-hang detection and configuration/session reset behavior. Job `20261109T151035Z-run-script-dd-win-prof-ea675e2d` passed **220 behavioral tests**, both core suites, callback lifecycle/ownership, consumer isolation, install/downstream/import checks, **101 RUM assertions** and **23 UI-hang assertions**. UI cases cover sleep, wait, CPU and two-session restart with separate RUM views. Runner/UIApp copy only the current profiler DLL/PDB while retaining locked-file replacement.

Eight returned native/core/standalone/UI profiles, totaling **8,508 observations**, were independently decompressed and checked on the host for protobuf types, string indices and metadata references. The [validation record](../../docs/windows-memory-reuse-validation.json) records source commit, job and per-file hashes/counts. No live intake upload was performed.

## Final vendor namespace audit

After the measurement cohorts, the archive audit found three missing private prefixes: `g_debuglevel` and two MSVC-emitted Zstd sequence-option helpers. They are now prefixed, and Windows validation rejects unprefixed archive definitions (excluding compiler-reserved symbols). Job `20261109T152926Z-run-script-dd-win-prof-2743f3c2` passed both core suites, all **220 behavioral tests** and the **297-definition Windows namespace audit**. The host archive has **233 definitions**, all `pa_`/`ddpa_`, and no benchmark hooks.

These identifier-only fixes do not change core algorithms or API/header bytes. The performance and small-app cohorts reported here predate the prefix-map fix; they were not rerun after it. Their measured sources remain archived unchanged.

## Small-application residency with retained storage

Job `20261109T151820Z-run-script-dd-win-prof-4a2f0bbf` reran the 45-second small/churn/recovery app for all five states after upstream integration. Same source/settings as the historical small-app fixture: 20 ms sampling, 5-second flushes, symbol names disabled, one retained callback buffer, checkpoints every 100 ms. The C and no-profiler executable use current profiler sources; the Rust baseline DLL is the existing earlier control, not a rebuilt current-main equivalent. This is one sequential process per state, not randomized repeated trials.

Median **working set / private committed MiB**:

| Build/state | Small | Churn | Recovery |
| --- | ---: | ---: | ---: |
| unloaded-0 | 12.309 / 5.281 | 12.461 / 5.359 | 12.418 / 5.273 |
| baseline-0 | 16.324 / 5.969 | 16.492 / 6.086 | 16.430 / 5.984 |
| baseline-1 | 17.844 / 12.184 | 18.188 / 12.391 | 18.195 / 12.285 |
| c-core-0 | 16.773 / 6.039 | 16.922 / 6.156 | 16.922 / 6.156 |
| c-core-1 | 18.492 / 8.328 | 18.836 / 8.566 | 18.863 / 8.582 |

Enabled C versus no profiler linked adds **6.18 MiB working set / 3.05 MiB private commit** in the small phase; **6.45 / 3.31 MiB** in recovery. This is within the stated roughly 5–10 MB working-set target for this fixture only. Retention costs memory: unlike the earlier policy, C working set is not below the older Rust control here, although private commit remains lower. Do not reuse the historical 3.43/1.46 MiB deltas for the new policy.

Both enabled variants delivered nine profiles, exactly one final callback and no callback errors. All 18 actual captures were independently decompressed and metadata/timestamp-reference checked on the host. The [snapshot](../windows-small-app/run-20261109T151820Z/) preserves every memory checkpoint, callback counts and per-profile hashes/counts. Full profiles remain in the local worker artifacts.

The app does not include the SDK upload queue/transport. Initialization precedes sampling and brief peaks may fall between checkpoints. Shared image pages affect working set; requested-core bytes are separate. This short repeated-export run is not a full-SDK/long-soak or live-intake validation.

## Remaining limits

- Cold 100k-record stress growth still causes multi-millisecond record pauses. Timestamped unique median maximum is **5.826 ms** versus **1.645 ms** for v19; p99 is **2.6 / 0.8 µs**. Preallocation does not eliminate unusually large first-profile growth.
- Current fixtures do not represent realistic symbol/string sizes across all production workloads. The Windows recovery recipe has a small stable string set; continuously changing string-cache pressure is covered by core regressions, not a Windows long-soak trace.
- Live SDK intake, the external dirty SDK install rule, long-soak/full-SDK residency and Windows ASan remain unvalidated.
- Standalone HTTP export remains explicitly unsupported. Callback/local-debug output is the validated integration route.

Full raw latency/allocation CSVs, fixtures and logs remain under `winshare/artifacts/prof-16455-perf-v2/run-20261109T145716Z/`. The compact snapshot includes all summary rows, distributions, recovery checkpoints, manifest/order, runtime metadata, semantic checks and exact measured core/header sources verified against SHA256 metadata.
