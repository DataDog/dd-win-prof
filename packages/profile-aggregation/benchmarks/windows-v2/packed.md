# Windows aggregation performance v2: packed location IDs

[Chonk report and evidence links](sharing.md) (AppGate required).

## Result

Packed location IDs plus cached stack payload sizes reduce timestamped deep-stack equivalent flushing from **449.059 ms to 179.231 ms** per 100k input records, about **60% less time**. V19 CFFI remains stable at 329.112 ms before / 329.025 ms after; C is now about **46% faster** for this case. The previous untimed-churn flush regression no longer reproduces: 66.836 ms C versus 68.498 ms v19, with overlapping ranges. This is synthetic Windows evidence, not general production parity.

The [pre-optimization report](baseline.md) and its [312-case data](run-20261109T110202Z/) are preserved. The [new 312-case data](run-20261109T111821Z/) use the same seven primary repetitions, three latency repetitions, affinity mask 4, shuffle seed 16457, `/MT`, Windows machine and Balanced power scheme. Driver, harness CMake/runner/hooks, public header, vendor source and baseline library/header hashes are identical; only the measured core source changed. Both source versions are archived beside their results and verified against metadata SHA256s. Before/after are separate cohorts, not alternating A/B processes.

## Implementation and scope

`src/profile_aggregation.c` now emits `Sample.location_id` as one length-delimited packed field. The payload size is calculated lazily once per interned stack and reused by sizing streams and subsequent samples/serializations. Real output still streams location varints directly through the existing bounded compressor. Sample values remain unpacked.

A private tagged union shares the former wire-ID storage with a stack-size cache. Stack entries have no emitted wire ID; other metadata keeps its existing wire ID. This preserves entry size on measured Windows x64 and introduces **no allocations, encoded-stack byte buffers, public API changes or compression setting changes**. New stacks initialize an invalid-size sentinel; reset frees the entries. Immutable location wire IDs keep the cache valid across later interning and entry-vector relocation. Output failure/retry does not consume observations or invalidate cached sizes.

Packing and size memoization are one combined experiment here. Their individual CPU contributions were not measured separately. Raw byte savings below can be attributed directly to packing.

## Equivalent flush timings

Median milliseconds for 100,000 input records, seven processes per cell. C includes serialize+reset; v19 serialize already drains/resets, so its redundant explicit reset remains excluded. Output retention is equivalent; output destruction and setup/reseed are separately reported. Churn totals 100 flushes of 1,000 records.

### Timestamped observations

| Workload | C before | C packed/cache | v19 control after | C change |
| --- | ---: | ---: | ---: | ---: |
| repeated | 48.900 | 32.959 | 58.307 | -32.6% |
| unique | 74.960 | 61.097 | 77.151 | -18.5% |
| deep256 | 449.059 | 179.231 | 329.025 | -60.1% |
| labels | 67.839 | 52.191 | 75.067 | -23.1% |
| churn | 98.199 | 82.249 | 98.886 | -16.2% |

New deep C IQR: **179.077–179.298 ms**; full range 179.027–182.457 ms. Before C IQR: 448.475–449.603 ms. The v19 control did not show a corresponding improvement.

### Untimed aggregation

| Workload | C before | C packed/cache | v19 control after | C change |
| --- | ---: | ---: | ---: | ---: |
| repeated | 0.200 | 0.198 | 0.218 | -0.8% |
| unique | 61.120 | 47.313 | 66.889 | -22.6% |
| deep256 | 0.233 | 0.238 | 0.239 | +1.9% |
| labels | 52.919 | 36.859 | 76.226 | -30.3% |
| churn | 81.633 | 66.836 | 68.498 | -18.1% |

Repeated/deep aggregate into one observation, so their sub-millisecond flush differences are not meaningful optimization evidence. Untimed churn now has C IQR 66.811–68.158 ms and full range 66.731–69.409 ms. Treat its small difference from v19 as approximate parity, not an established win.

## Ingestion and latency are separate

Timestamped ingestion, median milliseconds for 100k records:

| Workload | C before | C packed/cache | v19 after |
| --- | ---: | ---: | ---: |
| repeated | 11.912 | 11.820 | 42.698 |
| unique | 52.211 | 53.890 | 59.429 |
| deep256 | 48.732 | 48.834 | 175.236 |
| labels | 51.437 | 53.602 | 92.655 |
| churn | 55.234 | 54.933 | 57.225 |

There is no universal unchanged-ingestion claim: unique and labels medians rose about 3% and 4% between cohorts, respectively. Those variations are not isolated from process-layout/machine effects or the cache-initialization branch. They remain faster than v19 in this matrix. All per-case ranges are in `summary.json`; no significance or causal claim is made for those small cross-cohort changes.

Latency comes from three separate per-record-clock processes, not the throughput runs. QPC is 100 ns granularity and the empty-pair p99 is 100 ns; clock overhead is not subtracted.

| Timestamped workload | C p99 (µs) | v19 p99 (µs) | C median maximum (ms) | v19 median maximum (ms) |
| --- | ---: | ---: | ---: | ---: |
| repeated | 0.3 | 0.5 | 0.657 | 0.069 |
| unique | 2.6 | 0.8 | 5.988 | 1.728 |
| deep256 | 0.7 | 2.0 | 0.697 | 0.077 |
| labels | 2.1 | 1.2 | 4.554 | 1.748 |

The pre-existing growth stalls remain: repeated/deep maxima align with record 65,536 and a 6,291,456-byte observation-vector allocation; unique/labels align with record 58,852 and an 8,912,904-byte intern-table expansion. Allocation events come from separate deterministic-recipe runs, not the timed process. This change does not address rehash/copy/page-fault tails or claim unique/labels latency parity.

## Wire size and serialization diagnostics

Independently decoded 6,000-observation timestamped deep fixtures:

| Backend | Raw protobuf bytes | Compressed bytes |
| --- | ---: | ---: |
| C before | 3,993,823 | 22,966 |
| C packed/cache | 2,475,823 | 17,655 |
| v19, unchanged | 2,450,821 | 53,750 |

Exactly **1,518,000 raw bytes** were removed, matching the pre-change packing calculation: 253 bytes per deep sample. C raw size is now about **1%** above v19 rather than 63%. C uses Zstd and v19 LZ4; compressed sizes are not same-codec comparisons.

At 100k timestamped deep records, the counted C raw size falls from **66,587,441 to 41,287,441 bytes**. Separate timed-C serialization falls from 468.390 to 188.243 ms; bracketed Zstd calls from 8.016 to 5.532 ms; the non-compression remainder from 460.330 to 182.685 ms. Independent medians need not add exactly. Hooks add overhead, so primary normal-C timings remain authoritative. The remainder includes sizing/emission, allocation, Zstd setup, sink work and unbracketed hook overhead; it is not pure protobuf time.

## Memory and validation

Every requested-core allocation/count/live/peak/reset statistic in all **ten** counted workload/timestamp scenarios is exactly unchanged. Repeated/deep retain 6,341,979 requested bytes before encoding, peak 9,487,707; unique retains 39,697,371, peak 40,673,772. Reset retains 307 requested bytes and destroy returns to zero. These counters exclude the sink/scaffold/CRT/OS and cannot replace resident-memory measurements. C's high populated working set and short-recovery limitations remain; no integrated memory-budget improvement is claimed.

Passed:

- Windows matrix job `20261109T111813Z-run-script-dd-win-prof-063e2551`: **312 cases**, complete-manifest/phase/percentile/allocation/affinity/CRT acceptance.
- **20 fixture groups / 60 files** independently decoded, recipe-checked and compared with v19; timed/normal C output remains byte-identical.
- Full Windows integration job `20261109T112114Z-run-script-dd-win-prof-75f066a6`: **206 behavioral tests**, both core suites, consumer-isolation/install/import checks and **101 RUM assertions**. Live SDK intake, the external dirty SDK install rule and integrated memory budget remain outside this validation.
- Host Release and UBSan normal/forced-collision core suites, and the harness's 51-run decode/contract suite.
- Packed-wire assertions fail against an isolated copy of the original serializer; production sources were never temporarily reverted during Windows work.
- A 16,384-frame test covers multi-byte IDs/packed length, input staging boundaries, negative sample value, repeated serialization and metadata growth after cache initialization. Existing empty-stack, reset, allocation-failure and failed-output/retry checks still pass.
- Fresh read-only implementation review found no blockers; actual MSVC execution subsequently passed the matrix and integration jobs above.

Full raw latency/allocation CSVs, fixtures and logs remain in:

```text
winshare/artifacts/prof-16455-perf-v2/run-20261109T111821Z/
```

The [compact evidence bundle](run-20261109T111821Z/) contains all 312 summary rows, aggregate distributions, recovery checkpoints, commands/order, toolchain/runtime metadata, independent semantic reports and exact measured core source. The archived [baseline bundle](run-20261109T110202Z/) contains the pre-change core source. Reproduction and strict acceptance commands are in the [harness README](../harness/README.md); the bridge still pins mask 4 / seed 16457. No commits or pushes are needed to rerun it.

## Remaining gates

1. Address growth-associated record tails without speculative public reserve APIs or hiding memory retention.
2. Measure realistic symbol/mapping/string data and full-SDK/long-soak residency; populated C memory remains higher in the synthetic benchmark.
3. Keep small ingestion differences visible while validating further serializer work. Do not extrapolate these empty-function/native-address stacks to all production profiles or platforms.
