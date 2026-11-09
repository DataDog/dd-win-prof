"""Summarize raw v2 measurements without mixing instrumentation modes."""
import argparse
import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('artifacts', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()

def percentile(values, p):
    values = sorted(values)
    position = (len(values) - 1) * p
    low = int(position)
    high = min(low + 1, len(values) - 1)
    return values[low] + (values[high] - values[low]) * (position - low)

def distribution(values):
    return {'median': statistics.median(values), 'q25': percentile(values, .25),
            'q75': percentile(values, .75), 'min': min(values), 'max': max(values)}

manifest = {}
for path in args.artifacts.rglob('jobs.csv'):
    result = json.loads((path.parent / 'result.json').read_text(encoding='utf-8-sig'))
    jobs = list(csv.DictReader(path.open(encoding='utf-8-sig')))
    if result['result'] != 'PASS' or len(jobs) != result['jobs'] or len({j['id'] for j in jobs}) != len(jobs):
        raise SystemExit(f'Incomplete or failed run: {path.parent}')
    for row in jobs:
        if row['exitCode'] != '0':
            raise SystemExit(f"Failed case: {row['id']}")
        manifest[(path.parent, row['id'])] = row
expected = {directory / (name + '-summary.csv') for directory, name in manifest}
actual = set(args.artifacts.rglob('*-summary.csv'))
if expected != actual:
    raise SystemExit('Summary files do not match the completed manifests')
rows = []
for path in sorted(actual):
    measured = list(csv.DictReader(path.open()))
    if len(measured) != 1:
        raise SystemExit(f'Expected one summary row: {path}')
    row = measured[0]
    row['_path'] = str(path.relative_to(args.artifacts))
    row['group'] = manifest[(path.parent, path.name.removesuffix('-summary.csv'))]['group']
    rows.append(row)
if not rows:
    raise SystemExit('No v2 summary CSVs found')

groups = defaultdict(list)
for row in rows:
    key = tuple(row[k] for k in ['group', 'backend', 'workload', 'mode', 'timestamped',
                                 'allocator_instrumented', 'samples'])
    groups[key].append(row)

results = []
for key, items in sorted(groups.items()):
    group, backend, workload, mode, timestamped, instrumented, count = key
    result = {'group': group, 'backend': backend, 'workload': workload, 'mode': mode,
              'timestamped': timestamped == '1', 'allocator_instrumented': instrumented == '1',
              'samples': int(count), 'repetitions': len(items), 'files': [x['_path'] for x in items]}
    for column in ['setup_ns', 'ingest_ns', 'serialize_ns', 'explicit_reset_ns',
                   'output_free_ns', 'destroy_ns', 'compressed_bytes', 'compression_ns',
                   'noncompression_serialization_ns', 'protobuf_bytes',
                   'clock_p50_ns', 'clock_p95_ns', 'clock_p99_ns', 'clock_max_ns',
                   'record_p50_ns', 'record_p95_ns', 'record_p99_ns', 'record_max_ns']:
        values = [int(x[column]) for x in items if x.get(column) not in (None, 'NA', '')]
        if values:
            result[column] = distribution(values)
    if mode != 'discard':
        # v19 serialization includes its reset. Its redundant explicit reset is
        # recorded separately, but is not part of the minimal equivalent flush.
        values = [int(x['serialize_ns']) + (0 if backend == 'v19_cffi' else int(x['explicit_reset_ns'])) for x in items]
        result['equivalent_flush_ns'] = distribution(values)
    results.append(result)

comparisons = []
for result in results:
    if result['backend'] != 'c' or result['allocator_instrumented']:
        continue
    other = next((x for x in results if x['backend'] == 'v19_cffi' and
                  all(x[k] == result[k] for k in ['group', 'workload', 'mode', 'timestamped', 'samples', 'allocator_instrumented'])), None)
    if not other:
        continue
    comparison = {k: result[k] for k in ['group', 'workload', 'mode', 'timestamped', 'samples']}
    for field in ['ingest_ns', 'equivalent_flush_ns', 'explicit_reset_ns']:
        if field in result and field in other:
            c, baseline = result[field]['median'], other[field]['median']
            comparison[field] = {'c_median_ns': c, 'v19_median_ns': baseline,
                                 'c_delta_percent': (c / baseline - 1) * 100 if baseline else None}
    comparisons.append(comparison)

report = {'groups': results, 'comparisons': comparisons,
          'notes': ['Primary throughput excludes per-record clocks and allocator counting.',
                    'Latency, compression hooks and allocation diagnostics are separate groups.',
                    'v19 serialization includes implicit reset; redundant explicit reset is excluded from equivalent flush.',
                    'Reported spreads are observed repetitions, not statistical confidence intervals.']}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2) + '\n')
for row in comparisons:
    if row['group'] != 'primary':
        continue
    ingest = row['ingest_ns']
    flush = row['equivalent_flush_ns']
    print(f"{row['workload']:10} timed={int(row['timestamped'])} n={row['samples']:>7} "
          f"ingest C/v19={ingest['c_median_ns']/1e6:.3f}/{ingest['v19_median_ns']/1e6:.3f}ms "
          f"({ingest['c_delta_percent']:+.1f}%) "
          f"flush C/v19={flush['c_median_ns']/1e6:.3f}/{flush['v19_median_ns']/1e6:.3f}ms "
          f"({flush['c_delta_percent']:+.1f}%)")
print(f'{len(rows)} raw runs, {len(results)} groups; report: {args.output}')
