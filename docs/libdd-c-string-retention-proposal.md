# PROF-16455: caller-controlled string retention proposal

Status: implemented with a ten-reset interval and a tuning TODO. Host and Windows
validation pass against producer `4e37730`; results are recorded below.

## Contract read

Producer revision: `1efe5ea5be96c36bf90906a1d2efcb20e1742261`, current head of
`r1viollet/prof-16455-extract-profile-core` when inspected.

Sources:
- [API and ownership](https://github.com/DataDog/libdd-c/blob/1efe5ea5be96c36bf90906a1d2efcb20e1742261/profiling/README.md#api-and-ownership).
- [Reset and clear declarations](https://github.com/DataDog/libdd-c/blob/1efe5ea5be96c36bf90906a1d2efcb20e1742261/profiling/include/ddc/profiling.h#L119-L130).

`ddc_profile_reset` invalidates every ID and discards the current profile, but
retains all interned strings regardless of serialization success. There is no
automatic string eviction. `ddc_profile_clear_strings` explicitly frees retained
string tables/payloads without allocating. It preserves configuration, compression
workspace and other working storage. It must run after create/reset, before
interning metadata or serializing; a nonempty generation returns INVALID.

Before this follow-up the consumer called reset but never clear_strings. With
this producer revision, that retained strings until profile destruction.

## Recommended policy

Start with **10 successful profile resets per clear**, independently of delivery
success. This is an initial trade-off, not a measured optimum.

- The production default upload period is 60 seconds: roughly ten minutes between
  clears. The development default is 20 seconds: roughly three minutes twenty
  seconds. Explicit upload settings change that time window.
- Count existing reset boundaries after failed serialization, failed HTTP delivery
  or callback exceptions too. Count empty export windows. Otherwise failure or
  inactivity could prevent reclamation indefinitely.
- Do not increment on a failed core reset. Set the counter back to zero only when
  clearing succeeds; keep it due if clearing fails, for the next existing reset.
- Scope the counter to the current profile instance. Initialize it to zero when
  creating a new aggregator and reset it during cleanup.
- Final exports follow the same rule. Do not add another clear just for shutdown;
  destruction already releases storage.
- Use a private named constant, not a new public configuration field or timer.
  Keep tracking the producer branch as requested; record the tested revision when
  validation is rerun rather than introducing a release SHA pin now.

This limits string retention age in reset cycles, **not peak memory**. One window
can still contain many unique strings. Allocator behavior, native symbol caches,
other retained core capacity and process RSS are separate concerns. Clearing also
removes hot strings, causing subsequent metadata interning to allocate again;
freeing a large table can add synchronous work under the existing export lock.

## Ownership and ordering

Policy stays in `ProfileExporter`, which owns export windows. A thin
`PprofAggregator::ClearStrings()` wrapper and a `clearStrings` entry in the existing
`ProfileEncodingApi` failure seam expose the operation. `PprofAggregator::Reset()`
remains policy-free. Nothing is exposed through the installed SDK API.

At the current reset point in `ProfileExporter::Export`:

1. Call the existing core reset exactly once.
2. On success, advance the bounded counter and clear strings if the tenth reset
   is due. On clear failure, leave the counter due and use the existing reset/
   recovery error-reporting path, with an operation-specific log message.
3. Invalidate generation-local location IDs as today.
4. Reintern sample-label keys/values with `InternSampleLabels()` as today.

Clearing must happen **before `InternSampleLabels()`**, not beside the later export
ID increment, where the generation already contains metadata. Skip clearing if
reset failed. Preserve the existing reset-failure recovery behavior; add no
second reset, HTTP retry or sample replay.

All profile IDs already become invalid on every ordinary reset. Do not retain
IDs across the ten-window boundary or clear the native symbol caches merely
because core strings were released. Those caches do not borrow core string
storage; their metadata can be interned again into the empty core string table.
Use the existing export lock, without introducing another thread or lock.

## Focused validation

- Verify clears at resets 10 and 20, not 9, 11 or 19, and zeroed cadence for a new
  profile instance. A spy around the real API can check ordering and call counts.
- Extend existing failure-seam tests to cover serialization/delivery failures
  advancing the cadence, reset failure not advancing it, and a failed clear
  remaining due. Verify no clear runs on a failed/nonempty reset path.
- Decode profiles spanning the first clear boundary using the existing
  prof-correctness tooling. Preserve definitions, period, values, labels, symbols
  and callback-owned bytes; ensure the next window records successfully.
- Update the active public-API smoke harness in
  `packages/profile-aggregation/benchmarks/harness`: its backend currently only
  calls reset, and its recovery assertions/message assume automatic three-cycle
  recovery. Make string reclamation explicit and distinguish it from non-string
  spare-capacity recovery; do not keep the old automatic string-eviction claim.
- Rerun native Windows tests and the focused smoke checks against the new producer
  revision. Prior results against `c7c8a8520c99fddb87f04f8a1fe76258f2854dfa` do not
  validate this policy. Keep historical benchmark files unchanged; no new matrix
  or performance campaign is proposed.

## Local validation completed

- Local source snapshot of producer `1efe5ea`, without modifying its original
  checkout or fetching anything. Host configure/build and CTest: 2/2 passed.
- Existing 41-run smoke passes with independent wire assertions and Python 3.15's
  standard-library Zstd decoder in place of the optional zstandard wheel. The
  local runner is `build-libdd-c-string-retention/run-smoke-stdlib-zstd.py`.
- A reset-only variant of the same driver/core fails the new assertion
  `retained[9] < retained[8]`; the clearing variant passes. This is a harness
  regression check, not execution of the Windows policy tests.
- Added three Windows-native policy tests for cadence/reinitialization, failed
  exports and reset/clear failures. Existing wide/custom/NUL fixtures now exercise
  the clear wrapper. The custom callback fixture spans the tenth-reset boundary.
  Those native tests were subsequently compiled and executed on Windows below.

## Subsequent string-view API migration

The local consumer also adopts producer
`4e37730228b579860e0865cf6b8c3592666dc7ed`: explicit byte-length views for native
strings and definitions, and the optional C-string helper for NUL-terminated
harness/test inputs. The ten-reset policy and tuning TODO are unchanged.

At that revision the host passes 9/9 CTest checks and the decoded 41-run smoke;
the reset-only negative variant still fails. The seven installed notice/license
files match their source bytes after adapting to the shared vendor directory.
Both dependency archives pass the Windows CRT/symbol audit. See
`build-libdd-c-string-views` and the integration guide.

## Windows validation completed

Worker job `20261119T104512Z-run-script-dd-win-prof-7d908250` passed 246 native
Release tests, 246 MSVC ASan tests, seven dependency/example checks, protocol and
12 Python serialization/decoder tests. All six decoded baseline/candidate fixtures
matched. Source/installed/downstream consumers, callback capture, WinHTTP failure
cases, RUM, UI hangs and restart also passed. Source/tool hashes and results are in
`winshare/artifacts/prof-16455-libdd-c-api-4e37730-20261119-rerun/`.

The first attempt found a RUM test assumption from the earlier main merge: it
required a comma after the last HTTP tag. The test now checks complete tokens in
the extracted tag field. Only that test changed before the full passing rerun.
Production sources and the producer snapshot were unchanged between attempts.

The interval remains provisional. GitHub CI private-repository authentication and
synchronizing newer main changes remain separate work; neither was changed here.

