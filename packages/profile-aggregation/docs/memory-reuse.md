# Memory reuse constraints (PROF-16455)

Agreed behavior for the experimental C aggregation core:

- Allocate a modest starting capacity for normal profiles. Keep the sizes internal; do not add public tuning options.
- Reset empties the profile but keeps normal working storage. Reuse sample buffers, metadata arrays, hash tables, arena blocks and compression workspace across exports.
- Do not repeatedly shrink and grow storage during normal exports. Release excess capacity after an exceptional spike, not merely because one profile is smaller.
- Keep string contents cached across exports. Evict a string after three consecutive completed exports without use. Using it again restarts the count. Use includes references through mappings, functions and labels.
- Failed serialization/output attempts and retries do not advance the eviction clock. Repeated serialization of one profile is not multiple export cycles.
- Evicted string storage must become reusable or reclaimable; removing only hash entries must not allow memory to grow forever.
- Preserve the current public ID lifetime: reset invalidates all IDs, even when string contents and storage are retained privately.
- Keep ownership/error behavior, timestamped observations, untimed aggregation and bounded streaming intact. Do not drop samples to meet a capacity target.
- Destroy frees all owned storage. No custom OS allocator or allocator redesign is required by these constraints.

## Private storage policy

Creation reserves the normal working floors, including the compression workspace, so the first sampled record does not pay for vector/hash setup:

| Storage | Initial floor |
| --- | --- |
| Observations | 8192 records for one or two value types. For larger strides, the largest power of two not exceeding both 8192 and 384 KiB / stride; at least one record. |
| Generation-local entries | 1024 entries, including current strings and metadata |
| Metadata intern map | Reserve 1024 keys |
| Untimed sum map | Reserve 256 keys |
| Persistent string map | Reserve 256 strings |
| Metadata arena and string arena | One 16 KiB block each |
| Zstd workspace | Fixed estimate for level 1, 128 KiB window; reinitialized on every attempt |

Map reserves denote key counts, not bucket counts. Growth remains geometric for vectors and maps. Arena requests are eight-byte aligned, with 16 KiB blocks or a fitted block for larger payloads. Large value strides can require more than 384 KiB for even one record. These are private policies, not limits or public tuning knobs.

Metadata arena blocks reset their cursors rather than releasing ordinary blocks. String payloads live in separate persistent blocks. Per-block live-byte accounting permits fully dead blocks to be reused immediately after reset; retain at most one empty 16 KiB block and free other empty blocks, including fitted large blocks. A cache hit reuses content but gets a fresh generation-local entry and wire ID. Only current-profile strings enter the serialized string table, not all cached strings. References from mappings, functions and labels therefore continue to address strings in the current profile.

## Export clock and ID lifetime

A completed export cycle is **successful core sink output finalized by `pa_reset`**, not a downstream network acknowledgement. After the final compressed chunk is accepted, serialization marks the strings in that frame used and sets a pending-success flag. At reset:

1. If output succeeded at least once in this generation, consume exactly one cycle. Used strings get age zero; other strings advance one unused-export age. Remove strings reaching age three.
2. Clear pending-success/use state, empty transient metadata and samples, and invalidate every public ID by advancing the generation.

Failed-only serialization followed by reset does not advance the clock. Neither do resets without serialization. Multiple successful serializations before reset consume only one cycle. Failed retries do not mark newly added strings used; a prior successful frame still counts once. A successful empty export counts as a cycle and uses its configuration strings, but does not emit unrelated cached strings. Destroy frees all storage, regardless of cycle state.

## Exceptional capacity and failure-safe maintenance

Transient demand history contains the last **three completed cycles**, including the cycle being finalized. Reset takes the maximum observation count, entry count, untimed key count and metadata arena bytes over that history, each bounded below by its normal floor. It releases a vector only when capacity exceeds four times that demand. An emptied transient map is released only when bucket count exceeds eight times recent key demand. Replacement normal vectors/maps are acquired lazily on subsequent use. Arena reset frees a block larger than four times recent byte demand and drops additional blocks once retained capacity exceeds four times demand; the last retained block can cross that threshold. This is a generous spare-capacity threshold, not an exact byte target. One small export cannot immediately undo a spike; after three smaller completed cycles the spike leaves the history. Reset itself allocates nothing.

Persistent strings follow their independent three-unused-cycle clock. If retained payload capacity exceeds 64 KiB, is more than twice live aligned bytes, and wastes at least 64 KiB, reset schedules compaction. An oversized string map (bucket count above eight times max(256, retained string count)) also schedules a rebuild. On the first subsequent nonempty interning operation, before any generation-local entries exist, copy retained content into replacement blocks and a replacement map. Commit both only after all allocations/inserts succeed. On failure, destroy the replacement and retain the original cache. Maintenance is optional, does not fail an otherwise valid cache hit, and is retried after a later reset if still warranted, not on every call. Map values and relocated payload pointers never escape into a live generation during replacement.

This policy bounds ordinary continuous churn and reclaims sparse retained blocks without a custom OS allocator. OOM can delay optional reclamation; it does not silently discard current samples or valid cached content. A failed insertion can retain normal spare capacity but rolls back its uncommitted payload bytes. An empty oversized block is unlinked and freed immediately on insertion failure, so progressively larger failed requests cannot accumulate fitted blocks while table growth is rejected.

## Host validation

Commands (existing host build directories):

```sh
cmake --build /tmp/prof-16455-core --parallel 4
ctest --test-dir /tmp/prof-16455-core --output-on-failure
cmake --build /tmp/prof-16455-core-ubsan --parallel 4
ctest --test-dir /tmp/prof-16455-core-ubsan --output-on-failure
cmake --build /tmp/prof-16455-perf-v2 --parallel 4
/tmp/prof-16455-validation-py39/bin/python \
  packages/profile-aggregation/benchmarks/harness/test_harness.py \
  /tmp/prof-16455-perf-v2/bench_core /tmp/prof-16455-perf-v2/bench_core_timed --decode
```

Release and UBSan normal/forced-collision tests pass. Added coverage asserts no allocation/free calls during warmed ingestion, serialization and reset; exact three-cycle eviction with real large-payload release; no clock advance on failures/retries/repeated serialization; empty exports; stale IDs; mixed stable/new-string churn; sparse-block compaction and OOM recovery; exceptional spike recovery; allocation-site failure sweeps through vector/map/payload growth; and repeated rejected table growth with progressively larger fitted payloads. Existing timestamp, saturation, packed-stack and bounded streaming decode checks remain. The high-cardinality metadata spike and growth-failure sweep run in the normal suite; forced-collision tests retain smaller collision-heavy fixtures. A focused normal-suite untimed test uses one stack and 4096 distinct numeric-label sets to grow the sum map within the preallocated observation capacity. Failure injection is armed only during recording until map growth fails; decoded output verifies that `PA_NOMEM` appends nothing and leaves prior sums unchanged, then verifies retried and repeated values for every key. After three smaller completed cycles it checks released spike capacity and requires the next untimed record to reacquire sum-map storage, without fixed allocation-count or byte-size assertions.

The warmed-cycle test fails when linked against the prior reset-frees-storage core. The fitted-block regression also fails with oversized-block rollback disabled in a temporary source copy, confirming both checks detect their targeted regressions.

The harness passes independent decoding and its 51-run contract matrix, plus an allocator-instrumented 12-cycle recovery check. A separate host diagnostic using `bench_core --workload recovery --samples 6000 --cycles 12 --timestamped 1 --alloc-stats 1` measured:

- Initial normal requested core bytes: 1,544,308. Warmed normal cycles perform zero allocations/frees in seed, ingestion, serialization and reset.
- Burst requested core bytes: 18,642,844, retained through the first two smaller completed cycles.
- Third smaller reset: 1,114,380 requested bytes after releasing exceptional storage, with zero allocations during reset.
- Following normal cycle: 1,610,004 requested bytes after reacquiring normal floors. Destroy: zero live requested bytes.

These are allocator-requested bytes, **not** process RSS, working set or private commit. Host diagnostic CSVs are temporary under `/tmp/prof-16455-reuse-host-*`; archived Windows evidence/source snapshots were not modified. Windows matched performance, current working set/private commit and full integration checks remain separate acceptance gates. The existing roughly 5–10 MB whole-profiler small-application target still applies. ASan was not run because of the known host initialization hang.

## Acceptance

1. Once warmed, repeated normal profiles reuse core storage with few or no allocation/free calls. Track sample ingestion and serialization separately.
2. Requested core bytes and actual Windows working set/private commit stay roughly stable across normal exports. Report them separately; requested bytes are not RSS.
3. A large spike followed by normal exports eventually returns to a reasonable working capacity. Strings still follow the three-unused-export rule.
4. Tests cover successful export cycles, failed output/retry, repeated serialization, indirect string references, eviction/reuse, stale IDs, reset and destruction.
5. Continuous new-string churn must not cause unbounded retained storage. Allocation failures must leave the profile valid and destroy must release everything.
6. Compare ingestion/serialization/latency against the existing matched Windows matrix and repeat full Windows integration checks.

The former target of 307 requested bytes after each reset is superseded. Implementation, independent core/integration review, host Release/UBSan suites and the 51-run harness passed. Windows passed the 312-run matched matrix with all 60 strict fixtures, then the rebased full integration: 220 behavioral tests, both core suites, 101 RUM and 23 UI-hang assertions. See the [measured retention report](../benchmarks/windows-v2/README.md) for allocation counts, memory cost, recovery and remaining limits.
