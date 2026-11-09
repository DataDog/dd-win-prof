# Experimental native profile aggregation (PROF-16455)

Portable C11 aggregation and progressive pprof encoding. Windows capture and symbolication stay in the existing C++ platform layer. No Ruby, Rust, Windows headers or libdatadog in the production core.

## Build and test

```sh
cmake -S packages/profile-aggregation -B /tmp/profile-core -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/profile-core --parallel
ctest --test-dir /tmp/profile-core --output-on-failure
/tmp/profile-core/aggregation_tests /tmp/native.zst.pprof
/tmp/profile-core/aggregation_bench 100000
```

Core tests use an independent Zstd decoder: pkg-config libzstd >= 1.5.7 if available, otherwise a SHA256-pinned **test-only** Zstd 1.5.7 download. It is excluded from installs. Default SDK/production builds do not enable these tests, fetch that decoder or fetch/link libdatadog. Windows root behavioral Tests use the production core, not an FFI runtime.

Windows validation: `e2e-tests/validate-c-aggregation.ps1`. Use the win-sync worker bridge, not a build from the SMB drive. See [validation](docs/validation.md) for evidence and remaining gates.

## API and ownership

`include/profile_aggregation.h` is the public C boundary. Create an opaque profile, intern strings/mappings/functions/locations/stacks/label sets, record values, serialize into a caller sink, then reset. Caller supplies resolved native metadata. No global state, VM types or callback into a symbol resolver.

- One thread at a time per instance. Caller serializes access, including sinks; sinks must not reenter the instance.
- Input is borrowed only for each call; successful interning copies it. IDs are instance- and generation-scoped. Do not exchange them between instances. Zero denotes empty string/absent metadata, not a stack or label set.
- `pa_reset` invalidates **all** IDs and empties the profile while retaining normal metadata, observation, hash, arena and compression capacity. String **contents**, not public IDs, remain cached until three consecutive completed exports without use. Exceptional spare capacity is eventually released. Sample types and period remain. Generation exhaustion returns `PA_LIMIT` rather than reviving old IDs.
- Serialization does not reset/consume samples, including on failure. Sink chunks are borrowed only during the sink call. A sink error invalidates the entire partially emitted frame. Caller owns stored output and its allocator. Destroy frees all core-owned storage with the allocator used at create.
- Timestamp presence is explicit. Present epoch zero is a real observation. Timed observations never merge. Absent timestamps sum matching stack/ordered-label-set values with int64 saturation. Labels are ordered for identity; callers should use a consistent order.
- dd-win-prof preserves its old `timestamp == 0` means untimed convention in its C++ adapter. Windows timestamps/flush bounds use fixed-width Unix nanoseconds; callback bounds remain Unix milliseconds.
- Failures are explicit (`PA_INVALID`, `PA_NOMEM`, `PA_OUTPUT`, `PA_COMPRESSION`, `PA_LIMIT`). No silent sample dropping. Failed recording leaves sample contents unchanged. Metadata growth may retain a successfully grown vector after failure, but failed table insertions roll back their arena payload. Optional cache compaction is failure-safe and deferred until after reset; allocation failure retains the old cache. Serialization can intern configuration strings before failing; observations remain intact for retry.

The current package is a static library. `PA_SHARED`/`PA_BUILD` provide matching Windows import/export annotations for a shared build. Allocator ownership never crosses CRT boundaries without the caller-supplied allocator pair.

## Implementation

Separate Verstable maps index transient metadata and persistent string contents by kind + byte slice, with rapidhash and **full equality**, not hash identity. Interned payloads live in 16 KiB amortized arena blocks (larger payloads get a fitted block). Struct keys contain explicit fixed-width fields; label keys are canonicalized without hashing caller padding. A separate hash map indexes untimed sums. Timed observations use an amortized contiguous array, not per-sample allocations. Known stack/label-set lookup requires no temporary heap key. No compression is done during recording.

Nanopb low-level encoding writes protobuf submessages via sizing streams directly into a 16 KiB staging buffer. It feeds Zstd `compressStream2` with a 128 KiB compression window and emits compressed chunks of at most 16 KiB. There is **no complete uncompressed profile buffer**. The fixed static Zstd workspace is allocated at creation, retained across reset and reinitialized for every serialization attempt. Native mappings, PDB/build strings, module-relative addresses, symbol names/system names/files/lines, numeric label units, value types/period and timestamps retain their pprof field numbers. Schema attribution is in `schema/profile.proto`; this is a native one-line-per-location adapter, not an inline-frame implementation.

See [memory reuse](docs/memory-reuse.md) for exact private floors, retention thresholds, cycle boundaries and host checks. Requested allocator bytes are not process working set or private commit.

At flush the C++ exporter moves compressed bytes into `ProfilingData`, without a second compressed-vector copy. Final delivery/errors/reset recovery use the existing callback contract. Scratch frame IDs are reused. Symbolication caches immutable string views in engine-owned stable string storage rather than copying names/PDB IDs for each address; no Rust managed strings remain. Address caches are cleared when profile IDs reset. C mapping identity includes filename/build ID **and** memory range/file offset, with structural equality.

## Delivery contract and unsupported boundary

`ProfilingData.pprof` is now **one Zstandard frame**, magic `28 b5 2f fd`, not LZ4. Debug files use `.zst.pprof`. Consumers must support Zstd before using this experimental branch. SDK source evidence shows byte pass-through to the `profile.pprof` multipart attachment; backend source auto-detects Zstd. Those observations are not a live SDK intake result.

Both SDK and standalone local-debug modes aggregate with the C core. **Standalone HTTP export is unsupported and explicitly rejected by `ProfileExporter::Initialize` when enabled without a callback.** libdatadog v19 `Exporter_Request_build` requires an opaque Rust-owned `EncodedProfile`; there is bytes-out, but no supported bytes-in constructor/request builder. This branch does not fabricate Rust objects, mutate private Vec layouts or replay samples into Rust. No libdatadog production library is fetched, linked, copied or installed.

Smallest upstream addition needed: a byte-input request builder accepting a borrowed encoded-profile byte slice, explicit compression (`Zstd` here), start/end times, additional tags and optional internal/info metadata JSON. It must document whether it copies bytes synchronously or takes ownership, and provide matching release/cancellation behavior. This would reuse the supported exporter endpoint/send lifecycle. No upstream changes are part of this branch.

The dirty external dd-sdk-cpp checkout is intentionally untouched. Its `cmake/profiler.cmake` install rule still references the removed `dd_win_prof_libdatadog_dynamic` target. Remove the following obsolete rule before SDK installation can succeed:

```cmake
# Remove from dd-sdk-cpp/cmake/profiler.cmake:
install(FILES
    "$<TARGET_FILE:dd_win_prof_libdatadog_dynamic>"
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)
```

Do not replace it with a fake compatibility target. This branch's source consumer/install/export smoke is not full dd-sdk-cpp package validation.

## Dependency and disclosure inventory

Production aggregation dependencies are four components: Verstable 2.2.1 (MIT), rapidhash V3 (MIT), nanopb 0.4.9.2 (zlib), Zstd 1.5.7 (BSD-3-Clause selected). Google's pprof schema carries Apache-2.0 attribution. The overall profiler also uses spdlog and its bundled fmt (MIT); Google Test is test-only. The optional baseline harness uses libdatadog v19 and its locked Cargo dependencies **only in a separate test process**, never linked into SDK tests or installed. Python decoder requirements are test tooling, not runtime dependencies.

See [provenance](docs/provenance.md), `licenses/`, embedded vendor notices and the root `LICENSE-3rdparty.csv`. No disclosure-count or 5–10 MB integrated-residency guarantee is claimed from this inventory.
