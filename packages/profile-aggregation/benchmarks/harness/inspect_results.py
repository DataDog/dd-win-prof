"""Validate measured CSV invariants and summarize tails, growth and memory."""
import argparse
import csv
import importlib.util
import json
import statistics
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('run', type=Path)
parser.add_argument('--harness', type=Path, default=Path(__file__).resolve().parent)
parser.add_argument('--output', type=Path, required=True)
a = parser.parse_args()

def rows(path):
    with path.open(encoding='utf-8-sig', newline='') as f:
        return list(csv.DictReader(f))

spec = importlib.util.spec_from_file_location('harness_tests', a.harness / 'test_harness.py')
tests = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tests)
result = json.loads((a.run / 'result.json').read_text(encoding='utf-8-sig'))
manifest = rows(a.run / 'jobs.csv')
assert result['result'] == 'PASS' and len(manifest) == result['jobs'], 'incomplete or failed run'
assert len({job['id'] for job in manifest}) == len(manifest), 'duplicate manifest case'
assert [int(job['order']) for job in manifest] == list(range(len(manifest))), 'incomplete case order'
all_runs = []
expected_fixtures = set()
for job in manifest:
    assert job['exitCode'] == '0'
    prefix = a.run / job['id']
    summary, = rows(Path(str(prefix) + '-summary.csv'))
    tests.check(prefix, summary['mode'], summary['backend'] == 'c_timed',
                summary['allocator_instrumented'] == '1', int(summary['samples']), int(summary['flushes']))
    platform, = rows(Path(str(prefix) + '-platform.csv'))
    assert platform['clock'] == 'QPC' and platform['affinity_requested'] == '4'
    assert platform['affinity_apply_success'] == '1' and platform['process_affinity_mask'] == '4'
    all_runs.append((job, summary))
    if job['group'] == 'fixture':
        expected_fixtures.update(f"{job['backend']}-{summary['workload']}-t{summary['timestamped']}-{cycle}.pprof"
                                 for cycle in range(int(summary['flushes'])))
assert {p.name for p in (a.run / 'fixtures').glob('*.pprof')} == expected_fixtures, 'incomplete fixture set'
crt = rows(a.run / 'runtime-config.csv')
assert crt and all('DLL' not in row['value'].upper() for row in crt)
build_bytes = (a.run / 'build.log').read_bytes()
build_text = build_bytes.decode('utf-16' if build_bytes.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig', errors='replace')
assert 'LNK4098' not in build_text, 'conflicting CRT default-library warning'

latencies = []
growth = []
for workload in ['repeated', 'unique', 'deep256', 'labels', 'churn']:
    for backend in ['c', 'v19_cffi']:
        selected = [(j, s) for j, s in all_runs if j['group'] == 'latency' and s['backend'] == backend and
                    s['timestamped'] == '1' and s['workload'] == workload]
        item = {'backend': backend, 'workload': workload, 'runs': len(selected)}
        for metric in ['record_p50_ns', 'record_p95_ns', 'record_p99_ns', 'record_max_ns']:
            values = [int(s[metric]) for _, s in selected]
            item[metric] = {'median': statistics.median(values), 'min': min(values), 'max': max(values)}
        item['maxima'] = [{'run': j['id'], 'index': int(s['record_max_index']), 'ns': int(s['record_max_ns'])} for j, s in selected]
        latencies.append(item)
        if backend != 'c':
            continue
        diagnostic = next((j for j, s in all_runs if j['group'] == 'allocator' and s['workload'] == workload and s['timestamped'] == '1'), None)
        if diagnostic:
            events = [e for e in rows(a.run / (diagnostic['id'] + '-allocations.csv')) if e['phase'] == 'ingest' and e['operation'] == 'alloc']
            for maximum in item['maxima']:
                matches = [e for e in events if int(e['sample_index']) == maximum['index']]
                growth.append({'workload': workload, 'latency_run': maximum['run'], 'max_index': maximum['index'],
                               'max_ns': maximum['ns'], 'same_recipe_diagnostic_allocations': matches})

recovery = []
for job, summary in all_runs:
    if job['group'] != 'recovery':
        continue
    points = [m for m in rows(a.run / (job['id'] + '-memory.csv')) if m['stage'] == 'afterreset+free']
    item = {'backend': summary['backend'], 'timestamped': summary['timestamped'] == '1', 'phases': {}}
    for section, name in enumerate(['small', 'burst', 'recovery']):
        selected = [p for p in points if int(p['cycle']) * 3 // len(points) == section]
        item['phases'][name] = {metric: {'median': statistics.median(int(p[metric]) for p in selected),
                                                'min': min(int(p[metric]) for p in selected),
                                                'max': max(int(p[metric]) for p in selected)}
                               for metric in ['current_working_set_bytes', 'private_committed_bytes']}
    recovery.append(item)

memory = []
for workload in ['repeated', 'unique', 'deep256', 'labels']:
    for backend in ['c', 'v19_cffi']:
        selected = [j for j, s in all_runs if j['group'] == 'primary' and s['workload'] == workload and
                    s['timestamped'] == '1' and s['backend'] == backend]
        checkpoints = [rows(a.run / (j['id'] + '-memory.csv')) for j in selected]
        item = {'backend': backend, 'workload': workload, 'repetitions': len(selected), 'stages': {}}
        for stage in ['beforecreate', 'aftersetup', 'beforeflush', 'afterencode', 'afterreset+free', 'afterdestroy']:
            points = [next(m for m in measurements if m['stage'] == stage) for measurements in checkpoints]
            item['stages'][stage] = {metric: statistics.median(int(p[metric]) for p in points) for metric in
                                    ['current_working_set_bytes', 'peak_working_set_bytes', 'private_committed_bytes', 'peak_process_commit_bytes']}
        memory.append(item)

allocations = []
for job, summary in all_runs:
    if job['group'] != 'allocator':
        continue
    stages = rows(a.run / (job['id'] + '-stages.csv'))
    allocations.append({'workload': summary['workload'], 'timestamped': summary['timestamped'] == '1',
                        'samples': int(summary['samples']),
                        'peak_requested_core_bytes': max(int(s['core_peak_bytes']) for s in stages),
                        'live_before_final_encode': next(int(s['core_live_bytes']) for s in reversed(stages) if s['phase'] == 'ingest'),
                        'retained_after_reset': next(int(s['core_live_bytes']) for s in reversed(stages) if s['phase'] == 'explicitreset'),
                        'ingest_allocations': sum(int(s['core_allocations']) for s in stages if s['phase'] == 'ingest')})

report = {'validation': {'result': 'PASS', 'runs': len(all_runs), 'crt': crt}, 'latency': latencies,
          'growth_association': growth, 'recovery': recovery, 'memory': memory, 'core_allocator': allocations,
          'limitations': ['Growth events come from separate deterministic-recipe diagnostic runs, not the same timed process.',
                          'Afterencode states differ: v19 has already reset, while C still retains the populated profile.',
                          'Recovery traces are short synthetic runs, not a full-profiler soak.',
                          'Peak working set and peak process commit are process-lifetime counters.']}
a.output.write_text(json.dumps(report, indent=2) + '\n')
print(f'PASS: {len(all_runs)} measured runs; phase totals, memory ordering, exact percentiles, allocation balance, affinity, matching static CRT')
for item in latencies:
    print('LATENCY', item['backend'], item['workload'], 'p99_us', round(item['record_p99_ns']['median']/1000, 3),
          'max_us_median/range', *(round(item['record_max_ns'][k]/1000, 3) for k in ['median', 'min', 'max']))
for item in growth:
    if item['workload'] == 'churn':
        continue
    print('GROWTH', item['workload'], item['max_index'], round(item['max_ns']/1000, 3),
          [int(e['requested_bytes']) for e in item['same_recipe_diagnostic_allocations']])
for item in recovery:
    if item['timestamped']:
        print('RECOVERY', item['backend'], {name: [round(phase[k]['median']/2**20, 3) for k in
              ['current_working_set_bytes', 'private_committed_bytes']] for name, phase in item['phases'].items()})
for item in memory:
    if item['workload'] in ['repeated', 'unique']:
        print('MEMORY',item['backend'],item['workload'], {name: [round(item['stages'][name][k]/2**20,3) for k in
              ['current_working_set_bytes','private_committed_bytes']] for name in ['beforecreate','beforeflush','afterreset+free','afterdestroy']})
