# Windows aggregation performance v2: pre-packing baseline (PROF-16455)

Historical snapshot before packed location IDs and size caching. See the [current before/after report](README.md) for the measured optimization and remaining gates.

## Scope and acceptance

Measure the new C aggregation core against the existing libdatadog v19 **public CFFI** before optimizing it. V1 relative timings/allocation counts were biased and are historical only. This pass changes no public API or aggregation behavior; compression hooks exist only in a private diagnostic build.

Final worker job: `20261109T110151Z-run-script-dd-win-prof-8a05bf37`. **312 fresh-process cases passed**, with seven primary throughput repetitions and three latency repetitions per workload/backend/timestamp mode. The matrix itself took 76.6 seconds, excluding worker mirror/build/copy overhead. All 20 fixture groups / 60 files passed independent decompression, recipe validation and cross-backend semantic comparison; normal/timed C bytes agree.

Machine: Windows 11 Pro build 26200, Intel Core i9-12900H, 14 cores / 20 logical processors, Balanced power scheme unchanged. Every child successfully applied affinity mask **4**, with QPC frequency 10 MHz. All private libraries/executables use `/MT`, matching the v19 static archive. This is a benchmark configuration, not a claim about the embedding SDK's CRT.

The shared driver uses two metrics, fixed values, empty function metadata/no mappings, native addresses and 16-frame stacks (256 for deep). Labels mode uses unique thread-name strings. Unique/churn changes the first frame for every sample. V19's immutable empty-string ID is cached, rather than fetched on every unique record. Throughput uses batch clocks and default allocators; per-record clocks, core allocation counters and compression clocks run separately. Output is fully retained on both backends. Setup/reseed and output destruction are separately reported. Equivalent flush means **C serialize+reset versus v19 serialize**, whose public API already drains/resets the profile. V19's redundant explicit reset is excluded from that comparison.

Completed earlier shuffled matrices (seeds 16455 and 16456) gave the same qualitative primary findings. They are not pooled with final seed 16457; the final baseline also removes the avoidable empty-ID getter calls. No cross-machine or general parity claim is made.

## Primary throughput

Median milliseconds for **100,000 input records**, seven fresh processes per cell. Churn totals 100 flushes of 1,000 records. Flush values exclude output destruction, shown separately in the raw data.

### Timestamped observations

| Workload | C ingest | v19 ingest | C equivalent flush | v19 equivalent flush |
| --- | ---: | ---: | ---: | ---: |
| repeated | 11.912 | 41.846 | 48.900 | 57.578 |
| unique | 52.211 | 58.889 | 74.960 | 77.264 |
| deep256 | 48.732 | 175.573 | 449.059 | 329.112 |
| labels | 51.437 | 92.662 | 67.839 | 74.602 |
| churn | 55.234 | 58.045 | 98.199 | 98.485 |

The prior unique-ingestion regression does **not** reproduce: timestamped unique ingestion is about **11% faster** here. Its C IQR is 51.983–52.737 ms; v19 IQR is 56.810–58.914 ms. This does not establish latency or resident-memory parity.

Timestamped deep equivalent flush is **36% slower**: C IQR 448.475–449.603 ms, v19 328.773–329.162 ms. Faster ingestion does not cancel the serialization regression when assessing flush responsiveness.

### Actual untimed aggregation

| Workload | C ingest | v19 ingest | C equivalent flush | v19 equivalent flush |
| --- | ---: | ---: | ---: | ---: |
| repeated | 11.561 | 38.806 | 0.200 | 0.215 |
| unique | 63.987 | 65.053 | 61.120 | 63.513 |
| deep256 | 48.428 | 172.588 | 0.233 | 0.247 |
| labels | 63.409 | 99.394 | 52.919 | 74.277 |
| churn | 65.910 | 65.463 | 81.633 | 68.050 |

Repeated/deep records collapse into one observation, rather than merely receiving different timestamps. Unique untimed ingestion is close, with overlapping ranges; do not advertise the small median difference as an established improvement. Untimed churn ingestion is also close, while equivalent flushing is a real **20% regression** (C IQR 81.497–82.749 ms; v19 67.904–68.420 ms). There is no blanket parity claim.

## Deep serialization: size and compression

Separate diagnostic C runs, medians in milliseconds. Churn values sum all 100 flushes. Independently computed medians need not add exactly; each run's arithmetic was checked.

| Workload/timestamps | C diagnostic serialize | Inside Zstd calls | Remaining serialization | Raw protobuf bytes |
| --- | ---: | ---: | ---: | ---: |
| deep256/1 | 468.390 | 8.016 | 460.330 | 66,587,441 |
| churn/0 | 75.316 | 16.898 | 58.453 | 6,578,956 |
| churn/1 | 92.490 | 20.392 | 72.143 | 7,562,446 |

Diagnostic deep serialization is about 468 ms versus about 449 ms for normal C serialization+reset. Hooks add visible overhead. Only about **8 ms** is inside `ZSTD_compressStream2` calls; most time lies outside those calls. The remainder includes protobuf sizing/emission, allocations, Zstd setup/workspace, output-vector handling and unbracketed hook overhead. It is **not pure protobuf time**, nor a separately measured baseline encoder time.

The independently decoded **6,000-observation deep fixtures** contain:

| Backend | Raw protobuf bytes | Compressed bytes | Location-ID representation |
| --- | ---: | ---: | --- |
| C | 3,993,823 | 22,966 | 1,536,000 separate varint fields |
| v19 | 2,450,821 | 53,750 | 6,000 packed fields |

C raw size is about **63% larger**, despite smaller Zstd output than v19's LZ4 output. Wire-size accounting shows that packing just C location IDs would remove **1,518,000 bytes** from this fixture (253 bytes per sample), leaving 2,475,823 bytes before other changes. That is a byte-count hypothesis, **not an implemented optimization or measured speedup**. The 100k C diagnostic raw counter is 66,587,441 bytes; no matching 100k baseline raw-byte measurement was taken.

Packed scalar emission and repeated stack sizing/emission are justified next targets. Changing the compressor alone is not supported as the main deep-stack fix. Churn has a materially larger compression share and should be remeasured after any encoding change.

## Latency tails and growth

Timestamped runs: median of three per-process p99s and maxima. These are instrumented complete-record paths, not primary throughput timings. QPC granularity is 100 ns; the empty-pair p99 is 100 ns, so sub-microsecond values should not be overinterpreted. Instrumentation overhead is not subtracted.

| Workload | C p99 (µs) | v19 p99 (µs) | C maximum (ms) | v19 maximum (ms) |
| --- | ---: | ---: | ---: | ---: |
| repeated | 0.3 | 0.5 | 0.669 | 0.066 |
| unique | 2.6 | 0.8 | 5.612 | 1.574 |
| deep256 | 0.7 | 2.1 | 0.729 | 0.076 |
| labels | 2.1 | 1.1 | 4.409 | 1.630 |
| churn | 2.5 | 3.6 | 0.137 | 0.051 |

All three C repeated/deep maxima occur at zero-based record **65,536**, matching a **6,291,456-byte** observation-vector allocation in the separate same-recipe allocator run. All three unique/labels maxima occur at record **58,852**, matching an **8,912,904-byte** intern-table expansion. Timestamped C unique maxima range from **5.554 to 5.917 ms**; labels from **4.383 to 4.476 ms**. Growth association is reproducible, but timing and allocator events were deliberately recorded in different processes. It does not isolate allocator latency from rehash/copy/page-fault work.

C unique/labels p99 and maxima remain worse despite better average ingestion. Growth-tail work is separate from the deep serialization problem.

## Memory and bounded recovery

These are **benchmark-process** measurements, not the whole profiler/SDK. Median absolute **MiB** across seven timestamped primary processes; each pair is current working set / current private commit. The scaffold is already present before creation.

| Workload/backend | Before create WS/private | Populated WS/private | After reset+output free WS/private | After destroy WS/private |
| --- | ---: | ---: | ---: | ---: |
| repeated/c | 12.08 / 5.32 | 18.23 / 12.91 | 12.39 / 5.79 | 12.39 / 5.79 |
| repeated/v19_cffi | 12.20 / 5.30 | 12.99 / 10.48 | 12.90 / 11.73 | 12.37 / 5.83 |
| unique/c | 12.08 / 5.32 | 46.75 / 43.36 | 12.74 / 6.25 | 12.74 / 6.25 |
| unique/v19_cffi | 12.21 / 5.32 | 29.77 / 28.01 | 12.52 / 10.67 | 12.49 / 5.48 |

C has higher populated working set, especially for unique metadata. Do not compare `afterencode` as equal populated states: v19 has already reset there and C has not. Peak working set and peak process commit are separate process-lifetime counters, not current private bytes or phase-local peaks; all are preserved in the data.

Separate counted C runs report 6,341,979 requested bytes live before repeated/deep encoding, peak 9,487,707; unique reports 39,697,371 live, peak 40,673,772. All return to **307 requested bytes** after reset and zero at destruction. These exclude output/driver/CRT/OS allocations, so they cannot be substituted for resident memory or baseline allocator counts.

A separate 90-flush process trace uses 30 small repeated batches, 30 six-times-larger unique bursts, then 30 small repeated batches. The following are medians of **post-reset+output-free** checkpoints, timestamped, current WS/private MiB:

| Backend | Initial small WS/private | Burst WS/private | Recovery small WS/private |
| --- | ---: | ---: | ---: |
| c | 12.21 / 6.28 | 12.61 / 7.32 | 12.29 / 5.49 |
| v19_cffi | 12.43 / 10.56 | 12.67 / 10.92 | 12.73 / 11.03 |

C returned near its initial process working-set level after the burst, with lower current private commit than in the initial small phase. V19 retained more private commit in its empty live profile and released much of it at destruction. This short trace does not establish leak freedom, full-SDK residency, a production budget, or long-soak behavior. Earlier small-app loaded-profiler measurements remain a different experiment.

## Reproduction and evidence

See [harness commands and definitions](../harness/README.md). Dispatch from the `win-sync` root:

```sh
winshare/worker/submit-job.sh dd-win-prof run-script \
  --script e2e-tests/run-c-aggregation-perf.ps1
```

The bridge requests `-AffinityMask 4 -Seed 16457`. All timed work writes to local C: before artifact copy. Full raw CSVs, allocation events, per-record latency traces, fixtures and build logs remain at:

```text
winshare/artifacts/prof-16455-perf-v2/run-20261109T110202Z/
```

The compact [worktree evidence directory](run-20261109T110202Z/) contains all per-case summary rows, job commands/order, metadata/hashes, generated CRT settings, CMake cache, independent semantic/wire reports, recovery checkpoints, and aggregate distributions. It excludes bulky raw latency/allocation files and compressed fixtures; those remain in the full artifact directory above.

To validate a completed raw run and reproduce distributions:

```sh
python packages/profile-aggregation/benchmarks/harness/compare_fixtures.py \
  <run>/fixtures --require-v19 --report <run>/semantic-comparison.json
python packages/profile-aggregation/benchmarks/harness/summarize_results.py \
  <run> --output <run>/summary.json
python packages/profile-aggregation/benchmarks/harness/inspect_results.py \
  <run> --output <run>/details.json
```

The decoder needs isolated test-only `zstandard` and `lz4`. The analysis tools reject incomplete/failed manifests. Inspection additionally checks every phase total, exact raw-record percentiles/max indices, memory-stage ordering, allocation balance, complete fixture set, affinity and static CRT. Host core tests and harness decode/contract tests passed; the missing-function/line mutation fails with the original permissive decoder and passes with the stricter validator.

## Remaining work

1. Trial packed location-ID encoding and avoid repeated stack-size work, keeping APIs unchanged; compare before/after with the same fixtures and primary matrix.
2. Address observation-vector/intern-table growth tails without hiding their memory cost or adding speculative public reserve knobs.
3. Track unique-metadata resident cost and untimed churn flush performance as separate gates.
4. Validate realistic symbol/mapping/string data, full-SDK delivery/residency and a long soak before accepting production overhead claims. Empty-function synthetic stacks are not full symbolization workloads.
