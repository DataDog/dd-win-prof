"""Small, deterministic harness contract tests. No benchmark numbers are asserted.
Use --decode with test-only zstandard installed for independent semantic checks.
"""
import argparse
import csv
import subprocess
import tempfile
from pathlib import Path


def rows(path):
    with path.open(newline="") as f:
        result = list(csv.DictReader(f))
    assert all(None not in r and None not in r.values() for r in result), path
    return result


def reject_missing_function_metadata(raw):
    from compare_fixtures import decode_raw, fields

    def varint(n):
        result = bytearray()
        while n >= 128:
            result.append((n & 127) | 128)
            n >>= 7
        result.append(n)
        return bytes(result)

    def encode(records):
        result = bytearray()
        for tag, wire, value in records:
            result.extend(varint((tag << 3) | wire))
            if wire == 0:
                result.extend(varint(value))
            else:
                assert wire == 2
                result.extend(varint(len(value)))
                result.extend(value)
        return bytes(result)

    mutated = []
    for tag, wire, value in fields(raw):
        if tag == 5:
            continue
        if tag == 4:
            value = encode([record for record in fields(value) if record[0] != 4])
        mutated.append((tag, wire, value))
    try:
        decode_raw(encode(mutated))
    except AssertionError as error:
        assert 'exactly one line' in str(error)
    else:
        raise AssertionError('decoder accepted missing function records and location lines')


def check(prefix, mode, timed, allocated, samples, flushes):
    summary, = rows(Path(str(prefix) + "-summary.csv"))
    stages = rows(Path(str(prefix) + "-stages.csv"))
    memory = rows(Path(str(prefix) + "-memory.csv"))
    latency = rows(Path(str(prefix) + "-latency.csv"))
    assert int(summary["samples"]) == samples and int(summary["flushes"]) == flushes
    for name, phases in (("setup_ns", ("create", "seed")), ("ingest_ns", ("ingest",)),
                         ("serialize_ns", ("serialize",)), ("explicit_reset_ns", ("explicitreset",)),
                         ("output_free_ns", ("output_free",)), ("destroy_ns", ("destroy",))):
        assert int(summary[name]) == sum(int(s["ns"]) for s in stages if s["phase"] in phases), name
    assert memory[0]["stage"] == "beforecreate" and memory[-1]["stage"] == "afterdestroy"
    expected_stages = ["aftersetup", "beforeflush"] + ([] if mode == "discard" else ["afterencode"]) + ["afterreset+free"]
    for cycle in range(flushes):
        assert [s["stage"] for s in memory[1:-1] if int(s["cycle"]) == cycle] == expected_stages
    assert int(summary["compressed_bytes"]) == sum(int(s["compressed_bytes"]) for s in stages if s["phase"] == "serialize")
    if timed:
        encodes = [s for s in stages if s["phase"] == "serialize"]
        assert sum(int(s["compression_ns"]) for s in encodes) == int(summary["compression_ns"])
        for s in encodes:
            assert int(s["ns"]) == int(s["compression_ns"]) + int(s["noncompression_serialization_ns"])
            assert int(s["compression_calls"]) > 0 and int(s["protobuf_bytes"]) > 0
        assert sum(int(s["protobuf_bytes"]) for s in encodes) == int(summary["protobuf_bytes"])
    else:
        assert summary["compression_ns"] == summary["protobuf_bytes"] == "NA"
    if allocated:
        events = rows(Path(str(prefix) + "-allocations.csv"))
        assert summary["allocation_events_dropped"] == "0"
        live, peak = 0, 0
        for e in events:
            live += int(e["requested_bytes"]) * (1 if e["operation"] == "alloc" else -1)
            peak = max(peak, live)
            assert live == int(e["live_bytes"]) and peak == int(e["peak_bytes"])
        assert live == 0
        assert len(events) == sum(int(s["core_allocations"]) + int(s["core_frees"]) for s in stages)
    else:
        assert all(s["core_allocations"] == s["core_live_bytes"] == "NA" for s in stages)
    if mode == "latency":
        assert len(latency) == samples
        assert [int(r["sample_index"]) for r in latency] == list(range(samples))
        values = sorted((int(r["ns"]), int(r["sample_index"])) for r in latency)
        for p in (50, 95, 99):
            assert int(summary["record_p%d_ns" % p]) == values[(samples * p + 99) // 100 - 1][0]
        assert (int(summary["record_max_ns"]), int(summary["record_max_index"])) == values[-1]
    else:
        assert not latency and summary["record_p50_ns"] == summary["record_max_index"] == "NA"
    assert [int(summary[k]) for k in ("clock_p50_ns", "clock_p95_ns", "clock_p99_ns", "clock_max_ns")] == sorted(int(summary[k]) for k in ("clock_p50_ns", "clock_p95_ns", "clock_p99_ns", "clock_max_ns"))
    return summary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("core", type=Path)
    parser.add_argument("timed", type=Path)
    parser.add_argument("--decode", action="store_true")
    args = parser.parse_args()
    runs = 0
    with tempfile.TemporaryDirectory(prefix="aggregation-perf-v2-") as tmp:
        root = Path(tmp)
        fixtures = root / "fixtures"
        fixtures.mkdir()
        def run(exe, workload, mode, timestamped, allocated=False, recovery=False):
            nonlocal runs
            prefix = root / ("run-%d" % runs)
            runs += 1
            count, flush = 37, 10
            command = [str(exe.resolve()), "--workload", workload, "--mode", mode, "--samples", str(count),
                       "--flush-every", str(flush), "--timestamped", str(int(timestamped)), "--output", str(prefix),
                       "--alloc-stats", str(int(allocated))]
            if recovery:
                command += ["--cycles", "9"]
            elif mode != "discard" and not allocated:
                backend = "core_timed" if exe == args.timed else "core"
                command += ["--fixture", str(fixtures / ("%s-%s-t%d" % (backend, workload, timestamped)))]
            if allocated:
                command += ["--fixture", str(root / ("alloc-%d" % runs))]
            subprocess.run(command, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if allocated:
                for p in root.glob("alloc-%d-*.pprof" % runs):
                    cycle = p.stem.rsplit("-", 1)[1]
                    reference = fixtures / ("core-%s-t%d-%s.pprof" % (workload, timestamped, cycle))
                    assert p.read_bytes() == reference.read_bytes()
            expected_samples = count * 24 if recovery else count
            expected_flushes = 9 if recovery else (4 if workload == "churn" else 1)
            return prefix, check(prefix, mode, exe == args.timed, allocated, expected_samples, expected_flushes)
        for workload in ("repeated", "unique", "deep256", "labels", "churn"):
            for timestamped in (False, True):
                run(args.core, workload, "throughput", timestamped)
                run(args.timed, workload, "throughput", timestamped)
                # Same recipe, same bytes, despite per-record clocks and allocation counters.
                run(args.core, workload, "latency", timestamped)
                run(args.core, workload, "discard", timestamped)
                run(args.core, workload, "throughput", timestamped, allocated=True)
        run(args.core, "recovery", "throughput", True, recovery=True)
        # Deterministic requested-byte checks, not RSS/commit claims. A larger
        # fixture exceeds the private normal floors and then retires the spike.
        reuse = root / "reuse"
        subprocess.run([str(args.core.resolve()), "--workload", "recovery", "--samples", "6000",
                        "--cycles", "12", "--timestamped", "1", "--alloc-stats", "1",
                        "--output", str(reuse)], check=True, stdout=subprocess.PIPE)
        stages = rows(Path(str(reuse) + "-stages.csv"))
        for stage in stages:
            if int(stage["cycle"]) in (1, 2, 3) and stage["phase"] in ("seed", "ingest", "serialize", "explicitreset"):
                assert int(stage["core_allocations"]) == int(stage["core_frees"]) == 0, stage
            if stage["phase"] == "explicitreset":
                assert int(stage["core_allocations"]) == 0, stage
        retained = {int(s["cycle"]): int(s["core_live_bytes"]) for s in stages if s["phase"] == "explicitreset"}
        assert retained[7] > retained[3] * 2, retained
        assert retained[8] == retained[9] == retained[7], retained
        assert retained[10] < retained[7] and retained[11] < retained[3] + 256 * 1024, retained
        assert stages[-1]["core_live_bytes"] == "0"
        capped = root / "capped"
        subprocess.run([str(args.core.resolve()), "--samples", "37", "--alloc-stats", "1", "--allocation-event-limit", "4", "--output", str(capped)], check=True, stdout=subprocess.PIPE)
        capped_summary, = rows(root / "capped-summary.csv")
        assert int(capped_summary["allocation_events_dropped"]) > 0
        assert len(rows(root / "capped-allocations.csv")) == 4
        assert rows(root / "capped-stages.csv")[-1]["core_live_bytes"] == "0"
        for p in fixtures.glob("core-*.pprof"):
            assert p.read_bytes() == p.with_name(p.name.replace("core-", "core_timed-", 1)).read_bytes()
        if args.decode:
            from compare_fixtures import validate_directory, decompress
            report = validate_directory(fixtures, fixture_samples=37, flush_every=10)
            assert report["groups"] == 16
            reject_missing_function_metadata(decompress(next(fixtures.glob('core-*.pprof'))))
            # Timed byte count matches independent decompressor's actual raw size.
            for summary_path in root.glob("*-summary.csv"):
                summary, = rows(summary_path)
                if summary["backend"] != "c_timed":
                    continue
                workload, ts = summary["workload"], summary["timestamped"]
                raw_size = sum(len(decompress(p)) for p in fixtures.glob("core_timed-%s-t%s-*.pprof" % (workload, ts)))
                assert raw_size == int(summary["protobuf_bytes"])
            print("PASS: independent wire decode, expected observations/aggregation, stacks, labels, times, and raw sizes")
        print("PASS: warmed cycles allocate/free nothing, allocation-free reset, three-cycle spike recovery")
        print("PASS: %d runs; stage/memory lifecycle, allocator accounting, exact percentiles/max index, timed/normal bytes" % runs)


if __name__ == "__main__":
    main()
