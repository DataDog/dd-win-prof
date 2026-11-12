"""Independent pprof wire decoder, not linked to either encoder.

Usage: python compare_fixtures.py <run-directory>/fixtures [--require-v19]
Requires test-only zstandard and lz4 wheels. Validates each decoded observation
against the shared recipe, compares normalized backends, and checks timed C bytes.
"""
import argparse
import hashlib
import json
import re
from collections import Counter
from pathlib import Path


def varint(data, pos):
    n = shift = 0
    while True:
        assert pos < len(data), "truncated varint"
        byte = data[pos]
        pos += 1
        n |= (byte & 127) << shift
        if byte < 128:
            return n, pos
        shift += 7
        assert shift < 70, "oversized varint"


def fields(data):
    result, pos = [], 0
    while pos < len(data):
        key, pos = varint(data, pos)
        tag, wire = key >> 3, key & 7
        assert tag, "zero field tag"
        if wire == 0:
            value, pos = varint(data, pos)
        elif wire == 2:
            length, pos = varint(data, pos)
            assert pos + length <= len(data), "truncated field"
            value, pos = data[pos:pos + length], pos + length
        else:
            raise AssertionError("unexpected wire type: %d" % wire)
        result.append((tag, wire, value))
    return result


def items(data, tag):
    return [v for t, _, v in fields(data) if t == tag]


def one(data, tag, default=0):
    values = items(data, tag)
    assert len(values) <= 1, "duplicate singular field"
    return values[0] if values else default


def integers(data, tag):
    result = []
    for t, wire, value in fields(data):
        if t != tag:
            continue
        if wire == 0:
            result.append(value)
        else:
            pos = 0
            while pos < len(value):
                n, pos = varint(value, pos)
                result.append(n)
    return tuple(result)


def decompress(path):
    data = path.read_bytes()
    if data.startswith(bytes.fromhex("28b52ffd")):
        import zstandard
        frames = []
        while data:
            decoder = zstandard.ZstdDecompressor().decompressobj()
            frames.append(decoder.decompress(data))
            assert decoder.eof, "incomplete Zstd frame: %s" % path
            data = decoder.unused_data
        return b"".join(frames)
    if data.startswith(bytes.fromhex("04224d18")):
        import lz4.frame
        return lz4.frame.decompress(data)
    raise AssertionError("fixture must be Zstd or LZ4: %s" % path)


def decode(path):
    return decode_raw(decompress(path))


def decode_raw(data):
    strings = [s.decode("utf-8") for s in items(data, 6)]
    assert strings and strings[0] == ""
    value_type = lambda b: (strings[one(b, 1)], strings[one(b, 2)])
    assert [value_type(t) for t in items(data, 1)] == [("cpu-time", "nanoseconds"), ("wall-time", "nanoseconds")]
    assert value_type(one(data, 11)) == ("cpu-time", "nanoseconds")
    assert one(data, 12) == 10000000
    assert one(data, 9) == 0
    assert not items(data, 3), "no mapping in this shared workload"
    functions = {}
    for f in items(data, 5):
        ident = one(f, 1)
        assert ident and ident not in functions
        functions[ident] = tuple(strings[one(f, tag)] for tag in (2, 3, 4))
        assert functions[ident] == ("", "", "") and one(f, 5) == 0
    locations = {}
    for loc in items(data, 4):
        ident = one(loc, 1)
        assert ident and ident not in locations and one(loc, 2) == 0
        lines = items(loc, 4)
        assert len(lines) == 1, "recipe locations require exactly one line"
        line = lines[0]
        assert functions[one(line, 1)] == ("", "", "") and one(line, 2) == 0
        locations[ident] = one(loc, 3)
    samples = Counter()
    for sample in items(data, 2):
        stack = tuple(locations[i] for i in integers(sample, 1))
        labels = []
        for label in items(sample, 3):
            key = strings[one(label, 1)]
            if items(label, 2):
                assert not items(label, 3) and not items(label, 4)
                value = strings[one(label, 2)]
            else:
                value = one(label, 3)
                assert strings[one(label, 4)] == ""
            labels.append((key, value))
        samples[(stack, integers(sample, 2), tuple(sorted(labels)))] += 1
    return samples, one(data, 10), len(data)


def expected(workload, timestamped, count, begin):
    samples = Counter()
    depth = 256 if workload == "deep256" else 16
    # Untimestamped repeated stacks coalesce; timestamped ones must not.
    if not timestamped and workload in ("repeated", "deep256"):
        samples[(tuple(range(depth)), (10000000 * count, 20000000 * count), ())] = 1
        return samples
    for i in range(begin, begin + count):
        stack = tuple(range(depth))
        if workload in ("unique", "churn"):
            stack = (i + 256,) + stack[1:]
        labels = []
        if timestamped:
            labels.append(("end_timestamp_ns", i + 1))
        if workload == "labels":
            labels.append(("thread_name", "worker-%d" % i))
        samples[(stack, (10000000, 20000000), tuple(sorted(labels)))] += 1
    return samples


def validate_directory(directory, require_v19=False, fixture_samples=6000, flush_every=1000, require_v47=False):
    assert not (require_v19 and require_v47), "select one baseline version"
    pattern = re.compile(r"(core|core_timed|v19|v47)-(repeated|unique|deep256|labels|churn)-t([01])-(\d+)\.pprof$")
    grouped, report = {}, []
    for path in sorted(directory.glob("*.pprof")):
        match = pattern.fullmatch(path.name)
        assert match, "unexpected fixture name: %s" % path
        backend, workload, ts, cycle = match.groups()
        cycle, timestamped = int(cycle), bool(int(ts))
        begin = cycle * flush_every if workload == "churn" else 0
        count = min(flush_every, fixture_samples - begin) if workload == "churn" else fixture_samples
        assert count > 0 and (workload == "churn" or cycle == 0)
        samples, duration, raw_size = decode(path)
        assert samples == expected(workload, timestamped, count, begin), "semantic mismatch: %s" % path
        assert duration == begin + count, "duration mismatch: %s" % path
        key = workload, timestamped, cycle
        grouped.setdefault(key, {})[backend] = (samples, path)
        report.append({"file": path.name, "samples": sum(samples.values()), "input_records": count,
                       "compressed_bytes": path.stat().st_size, "protobuf_bytes": raw_size,
                       "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    assert grouped, "no fixtures"
    for key, backends in grouped.items():
        assert {"core", "core_timed"} <= set(backends), "missing normal/timed fixture: %s" % (key,)
        if require_v19 or require_v47:
            baseline = "v47" if require_v47 else "v19"
            assert set(backends) == {"core", "core_timed", baseline}, "missing/wrong baseline fixture: %s" % (key,)
        assert not {"v19", "v47"} <= set(backends), "mixed baseline versions"
        assert backends["core"][1].read_bytes() == backends["core_timed"][1].read_bytes(), "instrumentation changed bytes"
        for samples, _ in backends.values():
            assert samples == backends["core"][0]
    # Require every workload and timestamp setting, not merely one matching pair.
    for workload in ("repeated", "unique", "deep256", "labels", "churn"):
        for ts in (False, True):
            cycles = (fixture_samples + flush_every - 1) // flush_every if workload == "churn" else 1
            for cycle in range(cycles):
                assert (workload, ts, cycle) in grouped, "missing fixture group"
    return {"result": "PASS", "baseline_version": "v47.0.0" if require_v47 else "v19.0.0" if require_v19 else "none", "groups": len(grouped), "files": report}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    baseline = parser.add_mutually_exclusive_group()
    baseline.add_argument("--require-v19", action="store_true")
    baseline.add_argument("--require-v47", action="store_true")
    parser.add_argument("--samples", type=int, default=6000)
    parser.add_argument("--flush-every", type=int, default=1000)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    report = validate_directory(args.directory, args.require_v19, args.samples, args.flush_every, require_v47=args.require_v47)
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    print("PASS: %d fixture groups independently decoded, recipe-checked, and compared" % report["groups"])


if __name__ == "__main__":
    main()
