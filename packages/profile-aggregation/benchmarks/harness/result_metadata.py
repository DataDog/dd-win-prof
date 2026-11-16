"""Version identity shared by raw-result tools; unversioned bundles remain v19."""
from collections import Counter
import json
import re
from pathlib import Path


def run_identity(directory):
    directory = Path(directory)
    result = json.loads((directory / 'result.json').read_text(encoding='utf-8-sig'))
    metadata = json.loads((directory / 'metadata.json').read_text(encoding='utf-8-sig'))
    version = metadata.get('resultVersion', 'v2-v19')
    assert version in ('v2-v19', 'v2-v47'), 'unsupported result version'
    baseline = 'v47.0.0' if version == 'v2-v47' else 'v19.0.0'
    if version == 'v2-v47':
        assert result.get('resultVersion') == version, 'mismatched result version'
        assert result.get('baselineVersion') == metadata.get('baselineVersion') == baseline
        assert metadata.get('baselineSourceCommit') == 'ef6371243c020319e74e8531035f0a5a2fcb8858'
        assert metadata.get('recipe') == 'shared-driver-v2'
        assert metadata.get('hashes') and metadata.get('executableHashes'), 'missing binary/source hashes'
        assert metadata.get('baselineCrtDirectives'), 'missing baseline CRT audit'
        assert metadata.get('msvcRuntime') in ('MultiThreaded', 'MultiThreadedDLL')
    else:
        assert metadata.get('baselineVersion', baseline) == baseline
    return version, baseline, metadata


def check_row(row, job, version):
    number = '47' if version == 'v2-v47' else '19'
    assert job['backend'] in ('core', 'core_timed', 'v' + number), 'wrong baseline in manifest'
    expected = {'core': 'c', 'core_timed': 'c_timed', 'v' + number: 'v' + number + '_cffi'}
    assert row['backend'] == expected[job['backend']], 'backend differs from manifest'
    for measured, planned in [('workload', 'workload'), ('mode', 'mode'),
                              ('timestamped', 'timestamped'), ('allocator_instrumented', 'allocator')]:
        assert planned in job, f'missing manifest {planned}'
        assert row.get(measured) == job[planned], f'{measured} differs from manifest'
    requested = int(job['samples'])
    options = dict(re.findall(r'(?:^|\s)--(cycles|flush-every)\s+(\d+)(?=\s|$)', job.get('command', '')))
    cycles = int(options.get('cycles', 90))
    flush_every = int(options.get('flush-every', 1000))
    assert requested > 0 and 0 < cycles <= 1000 and flush_every > 0, 'invalid manifest counts'
    if job['workload'] == 'recovery':
        samples = requested * sum(6 if cycle * 3 // cycles == 1 else 1 for cycle in range(cycles))
        flushes = cycles
    else:
        samples = requested
        flushes = (requested + flush_every - 1) // flush_every if job['workload'] == 'churn' else 1
    assert int(row['samples']) == samples, 'sample count differs from manifest recipe'
    assert int(row['flushes']) == flushes, 'flush count differs from manifest recipe'
    if version == 'v2-v47':
        assert row.get('result_version') == version and row.get('baseline_version') == number, 'unversioned or mislabeled new result'
    else:
        assert row.get('result_version', version) == version
        assert row.get('baseline_version', number) == number
    return samples, flushes


def expected_cases(metadata, version):
    parameters = ('replicates', 'samples', 'latencySamples', 'latencyReplicates',
                  'diagnosticSamples', 'fixtureSamples')
    assert all(key in metadata for key in parameters), 'missing scheduled recipe parameters'
    counts = {key: int(metadata[key]) for key in parameters}
    assert all(value > 0 for value in counts.values()), 'invalid scheduled recipe parameters'
    baseline = 'v47' if version == 'v2-v47' else 'v19'
    workloads = ('repeated', 'unique', 'deep256', 'labels', 'churn')
    for timestamped in ('0', '1'):
        for workload in workloads:
            for backend in ('core', baseline):
                for replicate in range(1, counts['replicates'] + 1):
                    yield ('primary', backend, workload, timestamped, str(counts['samples']),
                           str(replicate), 'throughput', '0')
                yield ('small', backend, workload, timestamped, '6000', '1', 'throughput', '0')
                for replicate in range(1, counts['latencyReplicates'] + 1):
                    yield ('latency', backend, workload, timestamped, str(counts['latencySamples']),
                           str(replicate), 'latency', '0')
                yield ('discard', backend, workload, timestamped, str(counts['samples']),
                       '1', 'discard', '0')
            for backend in ('core', 'core_timed', baseline):
                yield ('fixture', backend, workload, timestamped, str(counts['fixtureSamples']),
                       '1', 'throughput', '0')
            yield ('allocator', 'core', workload, timestamped, str(counts['diagnosticSamples']),
                   '1', 'throughput', '1')
        for workload in ('deep256', 'churn'):
            for replicate in range(1, counts['replicates'] + 1):
                yield ('compression', 'core_timed', workload, timestamped, str(counts['samples']),
                       str(replicate), 'throughput', '0')
        for backend in ('core', baseline):
            yield ('recovery', backend, 'recovery', timestamped, '1000', '1', 'throughput', '0')


def check_manifest(jobs, metadata, version):
    assert len({job['id'] for job in jobs}) == len(jobs), 'duplicated scheduled output IDs'
    fields = ('group', 'backend', 'workload', 'timestamped', 'samples', 'replicate', 'mode', 'allocator')
    actual = Counter(tuple(job[field] for field in fields) for job in jobs)
    expected = Counter(expected_cases(metadata, version))
    assert actual == expected, 'missing, duplicated or unexpected scheduled cases'
