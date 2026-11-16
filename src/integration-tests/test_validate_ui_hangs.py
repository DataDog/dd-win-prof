# Unless explicitly stated otherwise all files in this repository are licensed under
# the Apache 2 License. This product includes software developed at Datadog
# (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.
"""Regression guards for production schema and cross-profile isolation checks."""

from types import SimpleNamespace
import unittest
from unittest.mock import patch

from validate_ui_hangs import EXPECTED_TYPES, Sample, ValidationError, _build_samples, validate


class ValidateUiHangsTest(unittest.TestCase):
    def profile(self):
        strings = [""]
        types = []
        for name, unit in EXPECTED_TYPES:
            strings.extend((name, unit))
            types.append(SimpleNamespace(type=len(strings) - 2, unit=len(strings) - 1))
        return SimpleNamespace(string_table=strings, sample_type=types, function=[], location=[],
                               sample=[SimpleNamespace(value=[0] * 5, label=[], location_id=[])])

    def test_production_schema_accepts_five_columns(self):
        self.assertEqual(_build_samples(self.profile())[0].values["ui-hang-time"], 0)

    def test_legacy_schema_rejected(self):
        profile = self.profile()
        profile.sample_type.pop()
        with self.assertRaises(ValidationError):
            _build_samples(profile)

    def test_wrong_unit_rejected(self):
        profile = self.profile()
        profile.string_table[-1] = "count"
        with self.assertRaises(ValidationError):
            _build_samples(profile)

    def test_truncated_vector_rejected(self):
        profile = self.profile()
        profile.sample[0].value.pop()
        with self.assertRaises(ValidationError):
            _build_samples(profile)

    def reject(self, values, labels, message):
        sample = Sample(dict(zip((name for name, _ in EXPECTED_TYPES), values)), labels, "Sleep")
        with patch("validate_ui_hangs._load_all_samples", return_value=([sample], ["fixture.pprof"])):
            with self.assertRaisesRegex(ValidationError, message):
                validate("unused", "sleep", 1, 1500, 5, 64, [])

    def test_ordinary_hang_value_rejected(self):
        self.reject([0, 0, 10, 10, 1], {}, "ordinary sample has hang duration")

    def test_hang_cross_profile_values_rejected(self):
        for column in range(4):
            with self.subTest(column=column):
                values = [0, 0, 0, 0, 32]
                values[column] = 1
                self.reject(values, {"UIHang": "true"}, "hang contributes to ordinary columns")

    def test_zero_hang_duration_rejected(self):
        self.reject([0] * 5, {"UIHang": "false"}, "hang duration <= 0")

    def test_unknown_phase_rejected(self):
        self.reject([0, 0, 0, 0, 32], {"UIHang": "detected"}, "unexpected UIHang marker")


if __name__ == "__main__":
    unittest.main()
