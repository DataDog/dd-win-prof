# Aggregation performance harness v2 (PROF-16455)

Benchmark-only standalone C11/C++20 project. No production SDK dependency, Rust build, download, layout manipulation, or API additions. Existing v1 evidence is preserved. **Do not use v1 relative times/allocation counts as a fair comparison:** its Rust global atomic allocator, per-record clocks, different setup calls, and differently classified churn reset/reconstruction costs bias those comparisons. V2 does not retroactively correct those CSVs. See the [accepted Windows results and evidence](../windows-v2/README.md).

## Build and sanity on the host

```sh
cmake -S packages/profile-aggregation/benchmarks/harness \
  -B /tmp/prof-16455-perf-v2 -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/prof-16455-perf-v2 --parallel 4
ctest --test-dir /tmp/prof-16455-perf-v2 --output-on-failure
# With test-only zstandard==0.25.0 and lz4 installed in an isolated Python:
python packages/profile-aggregation/benchmarks/harness/test_harness.py \
  /tmp/prof-16455-perf-v2/bench_core /tmp/prof-16455-perf-v2/bench_core_timed --decode
```

`test_harness.py` checks phase totals, memory-stage ordering, exact nearest-rank percentiles, max latency/index against raw records, allocation balance and capped-event overflow, and normal/timed/allocator-instrumented C fixture identity. `--decode` independently decompresses and parses pprof wire fields, checks the recipe including untimed aggregation, and checks the timed protobuf byte counter against actual decompressed size. It does not reuse the encoder, generated encoder structs, or production Zstd decoder. A negative mutation removing function records and location lines must be rejected.

## Windows execution

Use the Windows worker bridge. The parent mirrors this package to the isolated `C:\Repos\dd-win-prof-c-aggregation` tree, then executes:

```powershell
C:\Repos\dd-win-prof-c-aggregation\packages\profile-aggregation\benchmarks\harness\run-windows.ps1 -AffinityMask 4 -Seed 16457
```

Defaults use the existing v19 **static** library at `C:\Repos\dd-win-prof\build\_deps\libdatadog-src\release\static\datadog_profiling_ffi.lib`. Build/output writes during measurements stay on C:, outside the mirrored package. Artifacts are copied after execution (including on failure) to `Z:\winshare\artifacts\prof-16455-perf-v2\run-<UTC>`. Nothing changes the Windows worker or power scheme. `-Output`, `-BuildRoot`, `-V19Root`, and `-V19Library` override paths. `-AffinityMask 0` inherits OS affinity, the default; a nonzero decimal mask requests pinning before measurement and fails if unavailable. Every process records the actual process/system masks, application success, and cached QPC frequency in `*-platform.csv`. Metadata records machine/OS/CPU/RAM, VS/toolchain, power scheme before/after, source and baseline SHA256 hashes, deterministic shuffle seed, command order, and exit status. `runtime-config.csv` records the generated MSVC runtime directives for each private library/executable.

Default schedule: seven 100k throughput replicates for both primary backends across five workloads and both timestamp modes; three separate 100k latency repetitions, populated-profile discard/reset, 6k small cases, 6k fixtures for all three targets, and full-count C allocator diagnostic runs. `LatencyReplicates` and `DiagnosticSamples` can override those diagnostics. Seven timed-C replicates each for deep256 and churn quantify compression and hook overhead against normal C. A 90-flush trace per primary backend/timestamp mode uses 30 small repeated, 30 high-cardinality unique (6x batch size), then 30 small repeated batches. It is a bounded synthetic recovery trace, not a long-duration SDK soak. Each scheduled case starts a fresh process; churn/recovery deliberately keep their own process across flushes. There are 312 short-lived processes at defaults. Runtime is machine dependent; the measured Windows matrices are documented in the results report. CLI count overrides support smoke runs.

Standalone configuration with explicit prebuilt paths:

```powershell
cmake -S <harness> -B C:\Repos\dd-win-prof-c-aggregation\build-perf-v2 `
  -G 'Visual Studio 17 2022' -A x64 -DPA_BENCH_V19=ON `
  -DPA_V19_ROOT=C:\Repos\dd-win-prof\build\_deps\libdatadog-src `
  -DPA_V19_LIBRARY=C:\Repos\dd-win-prof\build\_deps\libdatadog-src\release\static\datadog_profiling_ffi.lib
cmake --build C:\Repos\dd-win-prof-c-aggregation\build-perf-v2 --config Release --parallel 4
```

All benchmark executables and their private C libraries use the **same CRT**. The default MSVC runtime is `/MT` (`MultiThreaded`), matching the v19 static archive's `LIBCMT` directives and bundled examples. This is a controlled benchmark configuration, not a claim that the embedding SDK uses `/MT`. `PA_BENCH_MSVC_RUNTIME` is an explicit override for a differently built baseline; it must match that baseline's CRT. `PA_V19_DLL` copies an explicitly supplied DLL when using an import library, which may require a matching `/MD` override. Static is the default runner configuration. No baseline is discovered/fetched automatically and no Cargo invocation exists.

## Recipe and lifecycle

One `driver.cpp` is compiled into `bench_core`, `bench_v19`, and diagnostic `bench_core_timed`. Only backend operations differ. All use two types (`cpu-time`/`wall-time`, nanoseconds), 10 ms period, values 10/20 ms, epoch-zero flush start and positive timestamp `i+1` when timestamped. The same recipe also runs **without timestamps**: repeated/deep then genuinely aggregate matching observations. Setup performs 256 empty-function and location interning calls plus identical key/stack/empty-label lookups on both backends; each sample interns its stack and label set. Labels mode formats `worker-i`, then interns the key/value on each sample on both backends. Unique/churn re-intern the empty function and a distinct native address for the first frame. V19's immutable empty-string ID is fetched once per backend instance, not on each record. Default depth is 16; deep256 is 256. Churn flushes every 1,000 records without an extra empty final flush. IDs are copied only using supported C/CFFI value types, never Rust direct-API IDs.

Phases are disjoint and reported in nanoseconds:

- `create` and `seed`, summed in setup. Every post-reset reseed is setup, never ingestion.
- `ingest`: whole matched record path including interning/label formatting. **Throughput has no per-sample clocks or allocation counters.** Batch start/end clocks only. Latency is a separate process with two clock reads around each complete matched record path; raw index/latency and nearest-rank p50/p95/p99/max+index are retained. Latency instrumentation overhead is not subtracted.
- `serialize`: encoding, compression, allocation and output retention. C collects a full output vector, not a count-only sink. V19 retains its supported `EncodedProfile`, without an extra copy. Sink buffer growth is a real adapter cost, not a core allocation count.
- `explicitreset`: C `pa_reset`; v19 `Profile_reset`. **V19 `Profile_serialize` already drains/resets internally**, so that implicit reset cannot be isolated from v19 serialize. Its explicit reset is redundant, reported separately to model the old Windows adapter. Compare C `serialize + explicitreset` with v19 `serialize` for the nonredundant flush lifecycle, and add both explicit resets only when comparing old-adapter behavior. Do not compare the isolated reset values as equivalent populated-profile resets.
- `discard` mode instead populates a fresh process/profile with the equivalent recipe and resets without serialization. This isolates populated-profile reset/discard, separately from successful delivery.
- `output_free` releases the retained compressed output; `destroy` releases the profile. Accessing bytes, fixture file writes, CSV writes and memory snapshots are outside timed phases. All byte fixtures are written locally, only in their dedicated runs.

Each process measures 10k empty clock pairs before profile creation and reports p50/p95/p99/max. Windows QPC frequency is cached once. Portable drivers use `CLOCK_MONOTONIC` nanoseconds. No platform headers or runtime timing API enter the production core.

## Diagnostic C instrumentation

The normal C executable remains the primary comparator. `bench_core_timed` recompiles the same core/vendor sources in a private library with the same vendor symbol prefixes and `PA_BENCH_COMPRESSION_HOOKS`. The compile-time hooks bracket each `ZSTD_compressStream2` call, count calls and the final `pb_ostream_t.bytes_written`, and disappear completely in normal/production builds. No public header/API, core mutable global, compression settings, or wire format changes. Output must be byte-identical to normal C.

`compression_ns` sums clock-bracketed compressor calls, not Zstd setup or workspace allocation. `noncompression_serialization_ns = serialize_ns - compression_ns` includes protobuf **sizing plus emission**, allocations, Zstd setup, sink/vector handling and hook overhead outside the brackets. It is **not pure protobuf time**. Hook timings themselves have overhead; compare whole timed-C against normal-C deep runs before interpreting the breakdown. `protobuf_bytes` quantifies uncompressed size. Current C packs location IDs and caches each interned stack's payload size; sample values remain unpacked. The archived baseline used unpacked location IDs as well. Raw sizes can still differ despite equal semantics. See the before/after results for the serializer change. Decode baseline fixtures offline for its raw byte sizes.

`--alloc-stats 1` is separate, C-only and nonprimary. Its allocator reports per-phase core counts/requested bytes/frees/live/peak and allocation/free events indexed by cycle/sample. `highwater_growth_bytes` means requested-live high-water growth, not a classified internal vector reallocation. When the dropped-event counter is zero, allocation/growth events can be associated with raw record indices in a separate latency run, not assumed identical wall times. Event storage is reserved before creation, capped at 100k (or `--allocation-event-limit`), never grows/throws through C callbacks, and has an explicit dropped-event counter. Sink, driver, C++/CRT and OS allocations are excluded from these **core requested-byte** counters; OS memory includes them. Baseline allocator totals and uninstrumented C counters are `NA`, never invented from scaffold allocations.

## Memory and independent acceptance

`*-memory.csv` records `beforecreate`, `aftersetup`, `beforeflush`, `afterencode`, `afterreset+free` for every flush, plus `afterdestroy`. Windows `WorkingSetSize` and `PeakWorkingSetSize` are current/peak working set; `PrivateUsage` is current private committed bytes; `PeakPagefileUsage` is labeled **peak process commit counter**. These are distinct metrics, not interchangeable RSS/commit estimates. Peaks are process-lifetime counters, not phase-local maxima. Snapshots can miss transient current values. The scaffold/empty-clock storage is present beforecreate; latency vectors and diagnostic event storage also affect their separate process footprints. macOS provides current resident and peak RSS, with Windows commit fields `NA`. No component-residency or full-profiler attribution is claimed.

After the Windows run, independently decode fixtures on the host:

```sh
python packages/profile-aggregation/benchmarks/harness/compare_fixtures.py \
  <copied-run>/fixtures --require-v19 --report <copied-run>/semantic-comparison.json
```

This verifies both timestamp modes for every workload/flush, sample multiplicity vs untimed sums, stack order/addresses, labels, values, types/period/times, valid metadata references, and normal/timed C byte identity. This recipe deliberately uses empty functions/no mappings; existing rich-native fixture validation remains necessary and is not replaced. Decoded per-file compressed/protobuf sizes and SHA256s are retained in the report. The C/v19 encoders may order fields/IDs/observations differently; semantic comparison normalizes those, not the bytes across backends. Timing CSV success without this independent comparison is insufficient acceptance evidence. Historical CSVs stay in place. The [accepted Windows report](../windows-v2/README.md) records the measured limitations.

For a **completed raw run**, reproduce distributions and validate every measured case:

```sh
python packages/profile-aggregation/benchmarks/harness/summarize_results.py \
  <copied-run> --output <copied-run>/summary.json
python packages/profile-aggregation/benchmarks/harness/inspect_results.py \
  <copied-run> --output <copied-run>/details.json
```

Both tools require a successful `result.json` and the complete manifest, rejecting partial copies. The inspector checks the full fixture set, exact raw-latency percentiles/max indices, phase totals, allocator balance, memory ordering, affinity mask 4 and the default static CRT. It validates the recorded static-CRT/pinned matrix, not arbitrary alternative DLL/affinity configurations. Full raw files are required; the compact report bundle alone is not sufficient. `summary.json` includes medians, interpolated quartiles and ranges; `details.json` includes latency tails, growth-event association and distinct memory/recovery counters.
