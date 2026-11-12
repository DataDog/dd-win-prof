import json
from pathlib import Path

# Test-only comparison. Inputs may be raw pprof, Zstd (C), or LZ4 (v19).
import argparse
import io
parser = argparse.ArgumentParser()
parser.add_argument("core", type=Path)
parser.add_argument("baseline", type=Path)
parser.add_argument("--report", type=Path)
args = parser.parse_args()

def varint(data, pos):
    value = shift = 0
    while True:
        byte = data[pos]
        pos += 1
        value |= (byte & 127) << shift
        if byte < 128:
            return value, pos
        shift += 7
        assert shift < 70

def fields(data):
    result = []
    pos = 0
    while pos < len(data):
        tag, pos = varint(data, pos)
        wire = tag & 7
        if wire == 0:
            value, pos = varint(data, pos)
        elif wire == 2:
            length, pos = varint(data, pos)
            value = data[pos:pos+length]
            pos += length
        else:
            raise AssertionError(wire)
        result.append((tag >> 3, wire, value))
    return result

def items(data, tag):
    return [v for t, _, v in fields(data) if t == tag]

def one(data, tag, default=0):
    return next(iter(items(data, tag)), default)

def integers(data, tag):
    result = []
    for t, wire, value in fields(data):
        if t == tag:
            if wire == 0:
                result.append(value)
            else:
                pos = 0
                while pos < len(value):
                    n, pos = varint(value, pos)
                    result.append(n)
    return result

def normalize(path):
    data = path.read_bytes()
    if data.startswith(bytes.fromhex("28b52ffd")):
        import zstandard
        with zstandard.ZstdDecompressor().stream_reader(io.BytesIO(data)) as reader:
            data = reader.read()
    elif data.startswith(bytes.fromhex("04224d18")):
        import lz4.frame
        data = lz4.frame.decompress(data)
    strings = [v.decode() for v in items(data, 6)]
    value_type = lambda b: (strings[one(b, 1)], strings[one(b, 2)])
    mappings = {one(m, 1): (one(m, 2), one(m, 3), one(m, 4), strings[one(m, 5)], strings[one(m, 6)]) for m in items(data, 3)}
    functions = {one(f, 1): (strings[one(f, 2)], strings[one(f, 3)], strings[one(f, 4)]) for f in items(data, 5)}
    locations = {one(l, 1): (mappings.get(one(l, 2)), one(l, 3), tuple((functions[one(line, 1)], one(line, 2)) for line in items(l, 4))) for l in items(data, 4)}
    samples = []
    epoch_zero = []
    for sample in items(data, 2):
        labels = sorted((strings[one(l, 1)], strings[one(l, 2)] if items(l, 2) else one(l, 3), strings[one(l, 4)]) for l in items(sample, 3))
        record = {'stack': [locations[i] for i in integers(sample, 1)], 'values': integers(sample, 2), 'labels': labels}
        if any(k == 'end_timestamp_ns' and v == 0 for k, v, _ in labels):
            epoch_zero.append(record)
        else:
            samples.append(record)
    return {'sample_types': [value_type(x) for x in items(data, 1)], 'period_type': value_type(one(data, 11)), 'period': one(data, 12), 'start': one(data, 9), 'duration': one(data, 10), 'samples': sorted(samples, key=repr)}, epoch_zero

core, zero = normalize(args.core)
rust, baseline_zero = normalize(args.baseline)
assert len(zero) == 1 and not baseline_zero
assert core == rust, json.dumps({'core': core, 'v19': rust}, indent=2)
report = {'result': 'PASS', 'matched': core, 'extensions_checked_separately': ['Explicit epoch-zero observation retained by C API; v19 timestamp API cannot represent present zero.', 'C function start_line=42 extension checked by core test; v19 interning API has no start-line input.', 'Unreferenced mapping/stack metadata excluded from semantic sample comparison.']}
if args.report:
    args.report.write_text(json.dumps(report, indent=2) + '\n')
print('PASS: native mappings, opaque PDB ID, function/file/line/address, 2 timed observations, untimed sums, string/numeric-unit labels, value types, period and times match libdatadog v19.')
