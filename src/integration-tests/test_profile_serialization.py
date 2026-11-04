"""Wire regressions for the native serializer, run by ordinary unittest discovery."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from pprof_utils import parse_pprof


def exported_profiles(test_filter):
    root = Path(__file__).resolve().parents[2]
    executable = Path(
        os.environ.get(
            "DD_WIN_PROF_TESTS_EXE", root / "build/src/Tests/Release/Tests.exe"
        )
    )
    if not executable.is_file():
        raise AssertionError(
            f"Build Tests.exe before running native wire tests: {executable}"
        )
    with tempfile.TemporaryDirectory() as directory:
        env = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith("DD_")
            and key not in ("GTEST_OUTPUT", "GTEST_OUTPUT_FILE")
        }
        env["DD_WIN_PROF_TEST_PROFILE_DIR"] = directory
        result = subprocess.run(
            [str(executable), f"--gtest_filter={test_filter}"],
            env=env,
            capture_output=True,
            text=True,
            timeout=60,
        )
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return {
            path.stem: parse_pprof(path) for path in Path(directory).glob("*.pprof")
        }


def type_pairs(profile):
    strings = profile.string_table
    return [(strings[value.type], strings[value.unit]) for value in profile.sample_type]


@unittest.skipUnless(os.name == "nt", "Native serialization runs on Windows")
class ProfileSerializationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.profiles = exported_profiles(
            "PprofAggregatorTest.CustomPeriodAndDefinitionsSurviveInputLifetimeAndReset"
        )

    def test_custom_type_pairs_and_period_survive_reset(self):
        expected = [
            ("wait-time", "nanoseconds"),
            ("cpu-time", "nanoseconds"),
            ("cpu-time", "count"),
            ("wait-time", "count"),
            ("prototype-time", "nanoseconds"),
        ]
        self.assertEqual(set(self.profiles), {"custom-1", "custom-2"})
        for profile in self.profiles.values():
            self.assertEqual(type_pairs(profile), expected)
            strings = profile.string_table
            self.assertEqual(
                (strings[profile.period_type.type], strings[profile.period_type.unit]),
                expected[0],
            )
            self.assertEqual(profile.period, 10_000_000)

    def test_sample_values_are_not_replayed_after_reset(self):
        for cycle in (1, 2):
            profile = self.profiles[f"custom-{cycle}"]
            self.assertEqual(len(profile.sample), 1)
            self.assertEqual(
                list(profile.sample[0].value),
                [3000 * cycle, 1000 * cycle, cycle, cycle, 42],
            )
