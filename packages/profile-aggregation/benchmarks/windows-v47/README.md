# Windows C aggregation versus libdatadog v47

Fresh measurements on main `96d14f8a83f1267830bdb8877f50222aec0b1b84`, not a relabeling of the historical v19 runs.

## Result

- C has lower full-flush cost in these synthetic workloads. V47 has faster unique-stack ingestion; C is also slower on untimed string labels.
- C keeps substantially more memory after a high-cardinality export. Unique timestamped stress retains **44.38 MiB private commit versus v47 12.09 MiB**, after reset and output free.
- The small native app instead uses **1.50 MiB less working set / 4.59 MiB less private commit** with C than v47 during steady work. This is one short fixed-order capture, not a full-SDK result.
- Historical v19 measurements and staging-indexing evidence are unchanged. Neither old CI nor old intake proves the new branch is production-qualified.

## Method

Run `20261112T085300Z`: 312 fresh-process cases, seven throughput and three latency repetitions, 100,000 input records, affinity mask 4, shuffle seed 16457, MSVC `/MT`. Windows 11 Pro 10.0.26200, i9-12900H, Balanced power scheme. The control is the checksum-pinned Windows v47.0.0 static release, public CFFI only, in a separate process. Types, values, stacks, labels, timestamp presence and recipes match.

Both paths use Zstd level 1. C fixes windowLog=17 (128 KiB); v47 keeps the native Zstd default window. Encoder ownership, output growth and reset policy differ. These are complete-path comparisons, not an isolated aggregation-algorithm estimate.

The primary flush metric includes serialization, required reset and output free. V47 serializes by rotating/draining the profile before encoding, so its redundant explicit compatibility reset is excluded and recorded separately. Types/period/custom definitions survive reset; ordinary IDs do not. The tested legacy v47 constructor has no managed-string dictionary/TTL. C retains private capacity and expires cached strings after three unused completed exports.

Independent decode and recipe comparison passed **20 semantic groups / 60 fixtures**. Normal and timer-instrumented C fixtures are byte-identical. Acceptance binds summaries to scheduled workload/mode/timestamp/allocator and recipe-derived sample/flush totals, and requires the complete schedule/replicate multiplicities. Seven regression tests pass; restoring the old checker makes the wrong-workload regression fail.

## Throughput

Median milliseconds per 100,000 inputs; churn includes 100 exports. Ranges/quartiles are in the snapshot. These are separate phase medians, not a median of summed end-to-end times.

| Workload | Timestamped | Ingest C / v47 | Full flush + output free C / v47 |
| --- | --- | ---: | ---: |
| churn | no | 27.545 / 38.806 | 50.185 / 85.648 |
| churn | yes | 20.614 / 32.751 | 64.423 / 131.883 |
| deep256 | no | 48.918 / 70.290 | 0.172 / 0.357 |
| deep256 | yes | 48.801 / 74.296 | 180.356 / 250.524 |
| labels | no | 70.705 / 63.878 | 41.279 / 76.247 |
| labels | yes | 56.546 / 65.697 | 57.195 / 89.214 |
| repeated | no | 11.229 / 16.005 | 0.147 / 0.315 |
| repeated | yes | 11.874 / 21.793 | 33.741 / 58.439 |
| unique | no | 67.174 / 40.270 | 46.138 / 71.935 |
| unique | yes | 54.110 / 40.090 | 60.958 / 84.536 |

Untimed unique ingestion is 66.8% slower in C; timestamped unique is 35.0% slower. Untimed labels ingestion is 10.7% slower. Lower flush cost does not remove the ingestion latency or retained-memory tradeoffs.

## Record pauses and output size

Timestamped latency processes only. Values below are median per-run p99 / median per-run maximum, in microseconds; three repetitions. Timing instrumentation is separate from throughput runs. Cold growth is not eliminated.

| Workload | C p99 / max (us) | v47 p99 / max (us) |
| --- | ---: | ---: |
| repeated | 0.3 / 676.1 | 0.3 / 404.3 |
| unique | 2.4 / 5686.3 | 0.5 / 1638.2 |
| deep256 | 0.7 / 804.8 | 0.9 / 392.6 |
| labels | 2.1 / 2305.3 | 0.8 / 2004.2 |
| churn | 0.4 / 108.6 | 3.3 / 48.8 |

The 6,000-record timestamped deep fixture is 2,475,823 / 2,450,821 raw protobuf bytes and 17,655 / 14,087 compressed bytes (C / v47). C is 25.3% larger compressed here. Window policy and protobuf layout differ; no compression-neutral claim is made.

## Memory and reuse

Timestamped 100,000-input stress, median of seven processes, after required reset and output free. Working set / private commit in MiB. Peaks in the JSON are process-lifetime high-water marks, not phase-local peaks.

| Workload | C WS / private | v47 WS / private |
| --- | ---: | ---: |
| repeated | 17.355 / 12.520 | 12.516 / 10.484 |
| unique | 47.516 / 44.383 | 12.684 / 12.090 |
| deep256 | 17.297 / 12.523 | 12.859 / 11.246 |
| labels | 40.094 / 37.219 | 12.668 / 11.551 |

Counted C ingestion allocations in separate churn diagnostics are 13 timestamped / 15 untimed, with 1.790 / 1.828 MiB requested storage retained. Requested allocator bytes exclude output, CRT and OS storage. They are not comparable to whole-process RSS/private commit.

Ninety-cycle small/burst/recovery traces return to a stable band rather than baseline immediately. Timestamped C after-free WS/private medians are 12.535/6.871 -> 15.059/8.895 -> 13.324/7.594 MiB; v47 is 12.680/10.738 -> 12.680/10.734 -> 12.707/10.746 MiB. C retains more resident pages after the burst but less private commit in this trace. Retention is not an absolute memory cap.

### Five-state small native app

One process per state, fixed order: unlinked, v47 disabled, C disabled, v47 enabled, C enabled. Each runs 45 seconds: 15 seconds small work, 15 churn, 15 recovery; 20 ms configured sampling and five-second exports. Both native adapters emit unchanged 10 ms period metadata. The latter mismatch is pre-existing, not fixed by this experiment.

| State | Small WS / private (MiB) | Recovery WS / private (MiB) |
| --- | ---: | ---: |
| unloaded-0 | 12.871 / 5.230 | 12.953 / 5.191 |
| baseline-v47-0 | 18.988 / 6.367 | 19.129 / 6.484 |
| c-core-0 | 17.414 / 6.051 | 17.578 / 6.176 |
| baseline-v47-1 | 20.016 / 12.930 | 20.684 / 13.336 |
| c-core-1 | 18.520 / 8.336 | 18.961 / 8.582 |

Enabled C versus unlinked adds 5.648 MiB WS / 3.105 MiB private during small work, and 6.008 / 3.391 MiB in recovery. Both enabled variants return nine profiles, one final callback and zero errors. All **18 profiles / 13,960 observations** independently decode with identical ordered four-column definitions, valid metadata references and recorded hashes. Snapshot medians are not confidence intervals or peak deltas.

## Validation and evidence

- Isolated main/v47 control: 229 native tests, verified source manifest and actual static-CRT directives.
- Candidate Windows Release: 233 native tests, core normal/collision and exporter suites, consumer/source/install/import/no-Rust checks, 101 RUM and 23 UI-hang assertions. Nine returned profiles independently decode.
- Follow-up Release: 233 tests and all 12 Python codec/wire cases, including exact seven-column values across two resets and a 7 ms period.
- Windows native/core/exporter ASan: 233 native tests, core 2/2, exporter 1/1, five independently decoded native wire cases. Runtime import verified; leak detection disabled.
- Host Release and UBSan core/collision 2/2 and exporter 1/1, plus the 51-case correctness harness and independent decoding. Host ASan startup remains inconclusive.
- Final fresh read-only review found no blocker, reproduced all acceptance JSON exactly, repeated the negative control and decoded saved native/ASan/small-app captures.

[Compact measured snapshot](run-20261112T085300Z/) includes distributions, joined measurements, recovery checkpoints, hashes, source snapshot and small-app evidence. It is not the full raw run. Full per-record/allocation traces, fixture binaries, native artifacts and worker logs remain under the win-sync `winshare/artifacts/prof-16455-*` directories. Rerun the inspector/comparator against that full run, not the compact copy.

Measured candidate snapshot: head b605bb2 plus dirty adaptation, SHA256 `a8722bc21a4af835ecab40488914c79ae22ebce2a7e6d30b356bd2d60f562a06`. After measurement, only four postprocessors/tests and two wire-test sources changed; 263 other entries, including all production and driver sources, match. `analysis-changes.json` records both hashes. Historical v19 artifacts were not modified.

Jobs: baseline `20261112T083859Z-run-script-dd-win-prof-00edba3a`; integration `20261112T084604Z-run-script-dd-win-prof-b53121c6`; matrix `20261112T085249Z-run-script-dd-win-prof-3cd07ceb`; small app `20261112T085856Z-run-script-dd-win-prof-b6edd4c2`; ASan/follow-up `20261112T090836Z-run-script-dd-win-prof-ff2b5494`.

Remaining gates: GitHub CI for the new committed/published head, fresh representative v47-realigned intake/indexing, dirty external SDK install-rule migration and full SDK packaging/live delivery, long soak and release pipeline qualification. The prior v19-era staging HTTP202/index proof remains historical. No force-push or PR update is implied by these local results.
