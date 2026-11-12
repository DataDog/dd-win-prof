# Aggregation performance harness v2 (PROF-16455)

Benchmark-only standalone C11/C++20 project. No production SDK dependency, Rust build, download, layout manipulation, or API additions. Existing v1 evidence is preserved. **Do not use v1 relative times/allocation counts as a fair comparison:** its Rust global atomic allocator, per-record clocks, different setup calls, and differently classified churn reset/reconstruction costs bias those comparisons. V2 does not retroactively correct those CSVs. See the [historical v19 Windows results and evidence](../windows-v2/README.md).

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

Current runs default to the checksum-verified **v47.0.0 static release** at `C:\Repos\libdatadog-v47.0.0\libdatadog-x64-windows`. No `_deps` baseline is discovered. The runner verifies SHA256 of the actual release headers/archive member, audits `/DEFAULTLIB` directives with dumpbin, and records source/library/executable hashes and actual compiler/CRT directives. The verified v47 archive uses `LIBCMT` (`/MT`), matching the unchanged benchmark CRT. Use a fresh `build-perf-v2-v47`, never the old v19 build cache.

Measurement/log/fixture writes remain on C:, copied afterwards to `Z:\winshare\artifacts\prof-16455-perf-v2-v47\run-<UTC>`. `-Output`, `-BuildRoot`, `-BaselineRoot`, `-BaselineLibrary`, and `-MsvcRuntime` override paths/runtime. The v47 runner deliberately accepts only the pinned Windows release members. `-BaselineVersion 19` plus explicit historical paths enables the legacy adapter in a separate build directory; it does not relabel previous data. No Windows worker or power-scheme changes are made.

`resultVersion=v2-v47`, `baselineVersion=v47.0.0`, pinned source commit `ef6371243c020319e74e8531035f0a5a2fcb8858`, library hash, executable hashes, runtime and compression/lifecycle notes are written to metadata. Every new summary row has `result_version` and `baseline_version`; the baseline backend is `v47_cffi`. The summarizer groups versions separately, and the inspector rejects mismatched manifests/rows. Unversioned archived bundles are interpreted only as v19. Historical benchmark directories are unchanged.

`-AffinityMask 0` inherits OS affinity; matched accepted-recipe runs use `-AffinityMask 4 -Seed 16457`. Nonzero affinity failure is fatal. Each process records actual masks/application success and cached QPC frequency. Metadata retains machine/OS/CPU/RAM, toolchain, power scheme before/after, shuffle seed, command order and exit status. `runtime-config.csv` contains generated MSVC runtime directives; `baseline-library-directives.txt` records the library audit.

Default schedule: seven 100k throughput replicates for both primary backends across five workloads and both timestamp modes; three separate 100k latency repetitions, populated-profile discard/reset, 6k small cases, 6k fixtures for all three targets, and full-count C allocator diagnostic runs. `LatencyReplicates` and `DiagnosticSamples` can override those diagnostics. Seven timed-C replicates each for deep256 and churn quantify compression and hook overhead against normal C. A 90-flush trace per primary backend/timestamp mode uses 30 small repeated, 30 high-cardinality unique (6x batch size), then 30 small repeated batches. It is a bounded synthetic recovery trace, not a long-duration SDK soak. Each scheduled case starts a fresh process; churn/recovery deliberately keep their own process across flushes. There are 312 short-lived processes at defaults. Runtime is machine dependent; the measured Windows matrices are documented in the results report. CLI count overrides support smoke runs.

Standalone configuration with explicit prebuilt paths:

```powershell
cmake -S <harness> -B C:\Repos\dd-win-prof-c-aggregation\build-perf-v2-v47 `
  -G 'Visual Studio 17 2022' -A x64 -DPA_BENCH_V47=ON `
  -DPA_V47_ROOT=C:\Repos\libdatadog-v47.0.0\libdatadog-x64-windows `
  -DPA_V47_LIBRARY=C:\Repos\libdatadog-v47.0.0\libdatadog-x64-windows\release\static\datadog_profiling_ffi.lib `
  -DPA_BENCH_MSVC_RUNTIME=MultiThreaded
cmake --build C:\Repos\dd-win-prof-c-aggregation\build-perf-v2-v47 --config Release --parallel 4
```

All executables/private C libraries use the same CRT. `/MT` is a controlled benchmark setting, not a claim about the embedding SDK. CMake retains explicit `PA_BENCH_V19`, `PA_V19_ROOT/LIBRARY/DLL` for historical tooling and `PA_V47_DLL` for compile/link experiments; do not enable both versions in one build. No baseline fetching or Cargo invocation exists. A DLL baseline requires a separately audited matching CRT; the pinned runner uses the static archive only.

## Recipe and lifecycle

One `driver.cpp` is compiled into `bench_core`, `bench_v47`, and diagnostic `bench_core_timed`. Only backend operations differ. All use two types (`cpu-time`/`wall-time`, nanoseconds), 10 ms period, values 10/20 ms, epoch-zero flush start and positive timestamp `i+1` when timestamped. The same recipe also runs **without timestamps**: repeated/deep then genuinely aggregate matching observations. Setup performs 256 empty-function and location interning calls plus identical key/stack/empty-label lookups on both backends; each sample interns its stack and label set. Labels mode formats `worker-i`, then interns the key/value on each sample on both backends. Unique/churn re-intern the empty function and a distinct native address for the first frame. V47's immutable empty-string ID is fetched once per backend instance, not on each record. Default depth is 16; deep256 is 256. Churn flushes every 1,000 records without an extra empty final flush. IDs are copied only using supported C/CFFI value types, never Rust direct-API IDs.

Phases are disjoint and reported in nanoseconds:

- `create` and `seed`, summed in setup. Every post-reset reseed is setup, never ingestion.
- `ingest`: whole matched record path including interning/label formatting. **Throughput has no per-sample clocks or allocation counters.** Batch start/end clocks only. Latency is a separate process with two clock reads around each complete matched record path; raw index/latency and nearest-rank p50/p95/p99/max+index are retained. Latency instrumentation overhead is not subtracted.
- `serialize`: encoding, compression, allocation and output retention. C collects a full output vector, not a count-only sink. V47 retains its supported `EncodedProfile`, without an extra copy. Sink buffer growth is a real adapter cost, not a core allocation count.
- `explicitreset`: C `pa_reset`; v47 `Profile_reset`. **V47 `Profile_serialize` already drains/resets internally**, so that implicit reset cannot be isolated from v47 serialize. Its explicit reset is redundant, reported separately to model the old Windows adapter. Compare C `serialize + explicitreset` with v47 `serialize` for the nonredundant flush lifecycle, and add both explicit resets only when comparing old-adapter behavior. Do not compare the isolated reset values as equivalent populated-profile resets.
- `discard` mode instead populates a fresh process/profile with the equivalent recipe and resets without serialization. This isolates populated-profile reset/discard, separately from successful delivery.
- `output_free` releases the retained compressed output; `destroy` releases the profile. Accessing bytes, fixture file writes, CSV writes and memory snapshots are outside timed phases. All byte fixtures are written locally, only in their dedicated runs.

The primary whole-flush metric is `whole_flush_and_free_ns`: C serialize + explicit reset + output free versus v47 serialize + output free. `equivalent_flush_ns` remains available without output free for historical interpretation. The separate explicit v47 reset is redundant and never counted in the primary metric.

V47 inspection is pinned to the source commit above: `libdd-profiling-ffi/src/profiles/datatypes.rs` resets before encoding; `internal/profile/mod.rs::reset_and_return_previous` constructs fresh ordinary interning tables with preserved definitions/period. This harness uses public `Profile_new`, not `Profile_with_dictionary` or managed storage. It does not exercise a Rust dictionary TTL. Its 90-flush recovery run compares repeated small, unique burst and small recovery under each backend's actual lifecycle. C's three-unused-successful-export string TTL and storage reclamation remain tested independently. IDs are reacquired after every reset; only the documented immortal empty string ID is retained.

Both native codecs use Zstd level 1 (`internal/profile/mod.rs::COMPRESSION_LEVEL`); C fixes windowLog 17 while v47 uses `zstd::Encoder::new` defaults. These are actual backend configurations, not matched window settings. Compression diagnostics and byte sizes must not be described as an isolated algorithm comparison.

Each process measures 10k empty clock pairs before profile creation and reports p50/p95/p99/max. Windows QPC frequency is cached once. Portable drivers use `CLOCK_MONOTONIC` nanoseconds. No platform headers or runtime timing API enter the production core.

## Diagnostic C instrumentation

The normal C executable remains the primary comparator. `bench_core_timed` recompiles the same core/vendor sources in a private library with the same vendor symbol prefixes and `PA_BENCH_COMPRESSION_HOOKS`. The compile-time hooks bracket each `ZSTD_compressStream2` call, count calls and the final `pb_ostream_t.bytes_written`, and disappear completely in normal/production builds. No public header/API, core mutable global, compression settings, or wire format changes. Output must be byte-identical to normal C.

`compression_ns` sums clock-bracketed compressor calls, not Zstd setup or workspace allocation. `noncompression_serialization_ns = serialize_ns - compression_ns` includes protobuf **sizing plus emission**, allocations, Zstd setup, sink/vector handling and hook overhead outside the brackets. It is **not pure protobuf time**. Hook timings themselves have overhead; compare whole timed-C against normal-C deep runs before interpreting the breakdown. `protobuf_bytes` quantifies uncompressed size. Current C packs location IDs and caches each interned stack's payload size; sample values remain unpacked. The archived v19 baseline used unpacked location IDs as well. Raw sizes can still differ despite equal semantics. See the before/after results for the serializer change. Decode baseline fixtures offline for its raw byte sizes.

`--alloc-stats 1` is separate, C-only and nonprimary. Its allocator reports per-phase core counts/requested bytes/frees/live/peak and allocation/free events indexed by cycle/sample. `highwater_growth_bytes` means requested-live high-water growth, not a classified internal vector reallocation. When the dropped-event counter is zero, allocation/growth events can be associated with raw record indices in a separate latency run, not assumed identical wall times. Event storage is reserved before creation, capped at 100k (or `--allocation-event-limit`), never grows/throws through C callbacks, and has an explicit dropped-event counter. Sink, driver, C++/CRT and OS allocations are excluded from these **core requested-byte** counters; OS memory includes them. Baseline allocator totals and uninstrumented C counters are `NA`, never invented from scaffold allocations.

## Memory and independent acceptance

`*-memory.csv` records `beforecreate`, `aftersetup`, `beforeflush`, `afterencode`, `afterreset+free` for every flush, plus `afterdestroy`. Windows `WorkingSetSize` and `PeakWorkingSetSize` are current/peak working set; `PrivateUsage` is current private committed bytes; `PeakPagefileUsage` is labeled **peak process commit counter**. These are distinct metrics, not interchangeable RSS/commit estimates. Peaks are process-lifetime counters, not phase-local maxima. Snapshots can miss transient current values. The scaffold/empty-clock storage is present beforecreate; latency vectors and diagnostic event storage also affect their separate process footprints. macOS provides current resident and peak RSS, with Windows commit fields `NA`. No component-residency or full-profiler attribution is claimed.

After the Windows run, independently decode fixtures on the host:

```sh
python packages/profile-aggregation/benchmarks/harness/compare_fixtures.py \
  <copied-run>/fixtures --require-v47 --report <copied-run>/semantic-comparison.json
```

This verifies both timestamp modes for every workload/flush, sample multiplicity vs untimed sums, stack order/addresses, labels, values, types/period/times, valid metadata references, and normal/timed C byte identity. This recipe deliberately uses empty functions/no mappings; existing rich-native fixture validation remains necessary and is not replaced. Decoded per-file compressed/protobuf sizes and SHA256s are retained in the report. The C/v47 encoders may order fields/IDs/observations differently; semantic comparison normalizes those, not the bytes across backends. Timing CSV success without this independent comparison is insufficient acceptance evidence. Historical CSVs stay in place. The [historical v19 Windows report](../windows-v2/README.md) records its measured limitations, not v47 results.

For a **completed raw run**, reproduce distributions and validate every measured case:

```sh
python packages/profile-aggregation/benchmarks/harness/summarize_results.py \
  <copied-run> --output <copied-run>/summary.json
python packages/profile-aggregation/benchmarks/harness/inspect_results.py \
  <copied-run> --output <copied-run>/details.json
```

Both tools require a successful `result.json` and the complete manifest, rejecting partial copies. The inspector checks the full fixture set, exact raw-latency percentiles/max indices, phase totals, allocator balance, memory ordering, affinity mask 4 and the recorded matching CRT. It validates the recorded static-CRT/pinned matrix, not arbitrary alternative DLL/affinity configurations. Full raw files are required; the compact report bundle alone is not sufficient. `summary.json` includes medians, interpolated quartiles and ranges; `details.json` includes latency tails, growth-event association and distinct memory/recovery counters.
