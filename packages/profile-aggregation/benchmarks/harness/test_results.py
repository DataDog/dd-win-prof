"""Result-version and strict codec regressions, independent of benchmark timings."""
import csv
import json
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from result_metadata import check_row, run_identity, check_manifest, expected_cases


class ResultVersionTests(unittest.TestCase):
    def make_run(self, root, version):
        directory = root / version
        directory.mkdir()
        modern = version == 'v47'
        metadata = {
            'resultVersion': 'v2-' + version,
            'baselineVersion': version + '.0.0',
            'baselineSourceCommit': 'ef6371243c020319e74e8531035f0a5a2fcb8858',
            'recipe': 'shared-driver-v2', 'hashes': ['test-source-hash'],
            'executableHashes': ['test-executable-hash'],
            'baselineCrtDirectives': ['/DEFAULTLIB:LIBCMT'], 'msvcRuntime': 'MultiThreaded',
        } if modern else {}
        metadata.update(replicates=1, samples=37, latencySamples=37,
                        latencyReplicates=1, diagnosticSamples=37, fixtureSamples=37)
        jobs = []
        fields = ('group', 'backend', 'workload', 'timestamped', 'samples',
                  'replicate', 'mode', 'allocator')
        for index, case in enumerate(expected_cases(metadata, 'v2-' + version)):
            job = dict(zip(fields, case))
            backend = job['backend']
            identifier = backend if (job['group'] == 'primary' and job['workload'] == 'repeated'
                                     and job['timestamped'] == '1') else f'case-{index}'
            job.update(id=identifier, exitCode='0')
            jobs.append(job)
            recovery = job['workload'] == 'recovery'
            count = int(job['samples'])
            flushes = 90 if recovery else ((count + 999) // 1000 if job['workload'] == 'churn' else 1)
            summary = {
                'backend': {'core': 'c', 'core_timed': 'c_timed', version: version + '_cffi'}[backend],
                'workload': job['workload'], 'mode': job['mode'], 'timestamped': job['timestamped'],
                'allocator_instrumented': job['allocator'],
                'samples': str(count * 240 if recovery else count), 'flushes': str(flushes),
                'ingest_ns': '100', 'serialize_ns': '200', 'explicit_reset_ns': '30',
                'output_free_ns': '40',
            }
            if modern:
                summary.update(result_version='v2-v47', baseline_version='47')
            self.csv(directory / (identifier + '-summary.csv'), [summary])
        result = {'result': 'PASS', 'jobs': len(jobs)}
        if modern:
            result.update(resultVersion='v2-v47', baselineVersion='v47.0.0')
        (directory / 'metadata.json').write_text(json.dumps(metadata))
        (directory / 'result.json').write_text(json.dumps(result))
        self.csv(directory / 'jobs.csv', jobs)
        return directory

    @staticmethod
    def csv(path, rows):
        with path.open('w', newline='') as output:
            writer = csv.DictWriter(output, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)

    def test_historical_and_current_never_pool(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make_run(root, 'v19')
            self.make_run(root, 'v47')
            report = root / 'summary.json'
            subprocess.run([sys.executable, str(Path(__file__).with_name('summarize_results.py')),
                            str(root), '--output', str(report)], check=True, capture_output=True)
            data = json.loads(report.read_text())
            self.assertEqual(len(data['groups']), 256)
            self.assertEqual({item['baseline_version'] for item in data['comparisons']}, {'v19.0.0', 'v47.0.0'})
            for item in data['comparisons']:
                if item['mode'] == 'discard':
                    continue
                version = item['baseline_version'].split('.')[0]
                self.assertEqual(item['whole_flush_and_free_ns'][version + '_median_ns'], 240)
                self.assertEqual(item['whole_flush_and_free_ns']['c_median_ns'], 270)

    def test_summary_is_bound_to_manifest_recipe(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = self.make_run(Path(temporary), 'v47')
            with (directory / 'jobs.csv').open() as source:
                job = next(row for row in csv.DictReader(source) if row['id'] == 'core')
            with (directory / 'core-summary.csv').open() as source:
                row = next(csv.DictReader(source))
            self.assertEqual(check_row(row, job, 'v2-v47'), (37, 1))
            mutations = {'workload': 'unique', 'mode': 'latency', 'timestamped': '0',
                         'allocator_instrumented': '1', 'samples': '36', 'flushes': '2'}
            for field, value in mutations.items():
                with self.subTest(field=field), self.assertRaises(AssertionError):
                    check_row({**row, field: value}, job, 'v2-v47')

    def test_multiflush_counts_come_from_requested_recipe(self):
        job = {'backend': 'core', 'workload': 'recovery', 'mode': 'throughput',
               'timestamped': '1', 'allocator': '0', 'samples': '1000'}
        row = {'backend': 'c', 'workload': 'recovery', 'mode': 'throughput',
               'timestamped': '1', 'allocator_instrumented': '0', 'samples': '240000',
               'flushes': '90', 'result_version': 'v2-v47', 'baseline_version': '47'}
        self.assertEqual(check_row(row, job, 'v2-v47'), (240000, 90))
        with self.assertRaises(AssertionError):
            check_row({**row, 'samples': '1000'}, job, 'v2-v47')
        job.update(workload='churn', samples='1001')
        row.update(workload='churn', samples='1001', flushes='2')
        self.assertEqual(check_row(row, job, 'v2-v47'), (1001, 2))
        job['command'] = 'bench_core.exe --flush-every 17'
        row['flushes'] = '59'
        self.assertEqual(check_row(row, job, 'v2-v47'), (1001, 59))
        job.update(workload='recovery', samples='5', command='bench_core.exe --cycles 2')
        row.update(workload='recovery', samples='35', flushes='2')
        self.assertEqual(check_row(row, job, 'v2-v47'), (35, 2))

    def test_summarizer_rejects_swapped_workload(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = self.make_run(Path(temporary), 'v47')
            path = directory / 'core-summary.csv'
            with path.open() as source:
                row = next(csv.DictReader(source))
            self.csv(path, [{**row, 'workload': 'unique'}])
            result = subprocess.run(
                [sys.executable, str(Path(__file__).with_name('summarize_results.py')),
                 str(directory), '--output', str(directory / 'summary.json')], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'workload differs from manifest', result.stderr)

    def test_manifest_rejects_missing_or_duplicated_replicates(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = self.make_run(Path(temporary), 'v47')
            metadata = json.loads((directory / 'metadata.json').read_text())
            with (directory / 'jobs.csv').open() as source:
                jobs = list(csv.DictReader(source))
            check_manifest(jobs, metadata, 'v2-v47')
            for corrupted in (jobs[:-1], jobs + [jobs[0]], jobs[:-1] + [jobs[0]]):
                with self.subTest(count=len(corrupted)), self.assertRaises(AssertionError):
                    check_manifest(corrupted, metadata, 'v2-v47')
            reused_output = [dict(job) for job in jobs]
            reused_output[1]['id'] = reused_output[0]['id']
            with self.assertRaisesRegex(AssertionError, 'duplicated scheduled output IDs'):
                check_manifest(reused_output, metadata, 'v2-v47')
            metadata.update(replicates=7, latencyReplicates=3)
            self.assertEqual(len(list(expected_cases(metadata, 'v2-v47'))), 312)

    def test_current_requires_provenance_and_explicit_rows(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = self.make_run(Path(temporary), 'v47')
            self.assertEqual(run_identity(directory)[:2], ('v2-v47', 'v47.0.0'))
            with self.assertRaises(AssertionError):
                check_row({'backend': 'c'}, {'backend': 'core'}, 'v2-v47')
            with self.assertRaises(AssertionError):
                check_row({'backend': 'v19_cffi'}, {'backend': 'v19'}, 'v2-v47')
            metadata = json.loads((directory / 'metadata.json').read_text())
            del metadata['executableHashes']
            (directory / 'metadata.json').write_text(json.dumps(metadata))
            with self.assertRaises(AssertionError):
                run_identity(directory)


@unittest.skipUnless(importlib.util.find_spec('zstandard'), 'Optional independent Zstd decoder not installed')
class StrictCodecTests(unittest.TestCase):
    def test_incomplete_and_concatenated_frames(self):
        import zstandard
        from compare_fixtures import decompress
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'profile.pprof'
            encoder = zstandard.ZstdCompressor(write_content_size=False, write_checksum=True)
            first, second = encoder.compress(b'first'), encoder.compress(b'second')
            path.write_bytes(first + second)
            self.assertEqual(decompress(path), b'firstsecond')
            for payload in (first[:4], first[:-1], first + second[:-1]):
                path.write_bytes(payload)
                with self.assertRaises(AssertionError):
                    decompress(path)


if __name__ == '__main__':
    unittest.main()
