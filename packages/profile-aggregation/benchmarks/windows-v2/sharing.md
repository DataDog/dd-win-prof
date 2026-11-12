# Chonk share: PROF-16455 performance

AppGate required. Uploaded to staging Chonk, folder `profiling`.

These links contain the historical packed-stack experiment, before retained storage and cross-export string caching. The [current retention report](README.md) includes the newer measurements; the original uploads were not changed.

- [Readable HTML report](https://chonk.us1.staging.dog/v2/get/chonk-uploads/profiling/e1c61415-fcf6-48e7-937e-704868d6d400)
- [Before/after evidence and methodology ZIP](https://chonk.us1.staging.dog/v2/get/chonk-uploads/profiling/72ee13e4-8fb4-4e03-8d58-01f95057f7f7)

The ZIP includes both 312-case summary datasets, distributions, recovery checkpoints, independent semantic reports, measured source snapshots, benchmark/analysis tooling and regression tests. Data is synthetic. Full per-record latency/allocation traces and compressed fixtures remain in the local artifact directories, not this compact bundle.

Local upload files: `temp/PROF-16455-libdatadog-dependencies/c-aggregation-perf-v2/share/` under `win-sync`.
