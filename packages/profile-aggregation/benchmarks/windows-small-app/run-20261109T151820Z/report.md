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

Both enabled variants delivered nine profiles, exactly one final callback and no callback errors. All 18 actual captures were independently decompressed and metadata/timestamp-reference checked on the host. The [snapshot](./) preserves every memory checkpoint, callback counts and per-profile hashes/counts. Full profiles remain in the local worker artifacts.

The app does not include the SDK upload queue/transport. Initialization precedes sampling and brief peaks may fall between checkpoints. Shared image pages affect working set; requested-core bytes are separate. This short repeated-export run is not a full-SDK/long-soak or live-intake validation.
