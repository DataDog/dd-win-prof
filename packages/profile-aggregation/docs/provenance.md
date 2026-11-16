# Source provenance and modifications

All production vendor source was copied from the verified local snapshot of DataDog/dd-trace-rb commit **a86f6905b5b420177228cd7f7852636c22e0e6cd**, directory `ext/datadog_profiling_native_extension/miteruby/`. This is a pin, not a moving branch dependency.

| Destination | Pinned source | Version / selected license |
| --- | --- | --- |
| `vendor/verstable.h` | `hashtable/verstable.h` | 2.2.1 / MIT, Jackson L. Allan 2023–2025 |
| `vendor/rapidhash.h` | `hashtable/rapidhash.h` | V3 / MIT, Nicolas De Carli 2025 |
| `vendor/pb{,_common,_encode}.{h,c}` (existing files only) | `pprof/` | nanopb 0.4.9.2 / zlib, Petteri Aimonen |
| `vendor/zstd.c` | `pprof/zstd.c` | Zstd 1.5.7 / BSD-3-Clause, Meta and embedded component notices |
| `vendor/zstd.h` | Header sections embedded in the same `zstd.c` | Zstd 1.5.7 / BSD-3-Clause |
| `schema/profile.proto` | Existing dd-win-prof `src/integration-tests/profile.proto` | Google pprof / Apache-2.0, Google 2016 |

Local vendor changes:

1. `pb.h`: disabled the prototype's `PB_BUFFER_ONLY` so nanopb supports the custom compression output stream. Kept nanopb dynamic allocation disabled.
2. Prefixed all 221 external nanopb/Zstd definitions with `ddpa_` to prevent symbol collisions with embedding SDKs. `vendor/symbols.cmake` records the original-to-prefixed names, obtained from `nm -gU` on the pinned source objects. The source/header replacements are mechanical token replacements (except retaining the actual `pb_encode.h` include filename). Internal Zstd inlining aliases use the same prefixed spelling; no generic Zstd or nanopb symbols are exported by the production static archive. The C encoder uses the mapping as private compile definitions.
3. Replaced the prototype's small one-shot-only `zstd.h` shim with the **verbatim embedded `zstd_errors.h` and `zstd.h` sections** from the pinned amalgamation, then applied the same mechanical external-symbol prefix. No new Zstd source version was mixed in.
4. Kept the prototype's compression-only, level-1 strategy restrictions, static-workspace allocation policy, assembly/threading exclusions and embedded component notices. Streaming compression functions already exist in that amalgamation. The production implementation adds no OS-specific allocator or decompressor.

First-party `src/profile_aggregation.c` is a new neutral implementation, not Ruby consumer/sampler/GC/allocator code. It uses progressive fragment encoding ideas, but neither the 100 MiB prototype profile buffer nor its private Rust EncodedProfile workaround. Google's full native pprof schema preserves mapping/address/function/line/period/label fields omitted by the Ruby fragment subset. No iOS implementation source/runtime was copied.

## Notices shipped with binaries

`licenses/` preserves the nanopb license and the Zstd BSD license from **outside** the prototype subtree (`tools/zstd-update/LICENSE` at the pinned commit). Separate Verstable and rapidhash MIT notices are extracted from their source headers; embedded notices remain intact. dd-trace-rb's license declaration and NOTICE are retained; Apache-2.0 is selected where first-party attribution applies, and the repository's root LICENSE contains that full license. Root installation includes this entire license directory. Root `LICENSE-3rdparty.csv` describes actual profiler dependencies, not a guessed count of Rust crates or source files.

## Test-only sources

The optional independent decoder is system `libzstd >= 1.5.7`, or upstream Zstd **1.5.7** from `https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz`, SHA256 **eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3**. Its targets/install rules are excluded from the profiler distribution.

`tests/baseline/` is a separate Rust executable fixture/benchmark harness against an unmodified libdatadog **v19.0.0**, commit **0478a4e78d011404763c13787b369aca93e40625**. Its Cargo.lock is the original v19 workspace lock with only the harness root package added. It is not a production or SDK test link dependency. No libdatadog source or binary is vendored here; the source location is supplied to `tests/prepare_baseline.py`. Test-only Cargo dependencies retain their own upstream licenses and are not shipped in the profiler package.

Python `protobuf`, `grpcio-tools`, `lz4`, `zstandard` are decoder/regeneration test tools only. They are not linked or packaged by production CMake targets.
