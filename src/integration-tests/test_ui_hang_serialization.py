"""Wire isolation across ordinary provider exports and reset boundaries."""

import os
import unittest

from pprof_utils import extract_labels
from test_profile_serialization import exported_profiles, type_pairs


@unittest.skipUnless(os.name == "nt", "Native serialization runs on Windows")
class UiHangSerializationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.profiles = exported_profiles(
            "UiHangProfileTest.CallbackKeepsWaitAndHangCustomSlotsAcrossTwoFlushes:"
            "UiHangProfileTest.HttpKeepsWaitAndHangCustomSlotsAcrossTwoFlushes:"
            "UiHangProfileTest.PartialEndpointsAcrossFlushesDoNotInventDurations"
        )

    def samples(self, name):
        profile = self.profiles[name]
        self.assertEqual(
            type_pairs(profile),
            [
                ("cpu-time", "nanoseconds"),
                ("cpu-samples", "count"),
                ("wall-time", "nanoseconds"),
                ("wait-time", "nanoseconds"),
                ("ui-hang-time", "nanoseconds"),
            ],
        )
        self.assertEqual(profile.period, 10_000_000)
        strings = profile.string_table
        self.assertEqual(
            (strings[profile.period_type.type], strings[profile.period_type.unit]),
            ("cpu-time", "nanoseconds"),
        )
        labels = [entry["labels"] for entry in extract_labels(profile)]
        identities = {(entry["process_id"], entry["thread id"]) for entry in labels}
        self.assertEqual(len(identities), 1)
        self.assertTrue(
            all(entry["process_id"] and entry["thread id"] > 0 for entry in labels)
        )
        self.assertTrue(
            all(entry["thread_name"] == "HangFixture_Main" for entry in labels)
        )
        return list(zip(profile.sample, labels))

    def check_sample(self, sample, labels, values, cycle, phase=None):
        self.assertEqual(list(sample.value), values)
        self.assertEqual(labels.get("UIHang"), phase)
        timestamp = 1_700_000_000_000_000_000 + cycle
        view = "ordinary-view"
        if phase is not None:
            timestamp += 100_000_000
            if phase == "false":
                timestamp += 1_410_000_000 * cycle
            view = "detected-view" if phase == "true" else "recovered-view"
        self.assertEqual(labels["end_timestamp_ns"], timestamp)
        self.assertEqual(labels["rum.view_id"], view)
        self.assertEqual(labels["trace endpoint"], view)

    def check_cycles(self, mode):
        for cycle in (1, 2):
            samples = self.samples(f"{mode}-{cycle}")
            self.assertEqual(len(samples), 4)
            ordinary = [
                (sample, labels) for sample, labels in samples if "UIHang" not in labels
            ]
            self.assertEqual(len(ordinary), 2)
            self.assertCountEqual(
                [list(sample.value) for sample, _ in ordinary],
                [
                    [1_000_000 * cycle, 1, 0, 0, 0],
                    [0, 0, 2_000_000 * cycle, 3_000_000 * cycle, 0],
                ],
            )
            for sample, labels in ordinary:
                values = (
                    [1_000_000 * cycle, 1, 0, 0, 0]
                    if sample.value[0]
                    else [0, 0, 2_000_000 * cycle, 3_000_000 * cycle, 0]
                )
                self.check_sample(sample, labels, values, cycle)
            for phase, duration in (("true", 32_000_000), ("false", 1_410_000_000)):
                matches = [
                    (sample, labels)
                    for sample, labels in samples
                    if labels.get("UIHang") == phase
                ]
                self.assertEqual(len(matches), 1)
                self.check_sample(
                    *matches[0], [0, 0, 0, 0, duration * cycle], cycle, phase
                )

    def test_callback_values_survive_two_flushes(self):
        self.check_cycles("hang")

    def test_http_values_survive_two_flushes(self):
        self.check_cycles("http")

    def test_partial_flushes_do_not_duplicate_samples_or_duration(self):
        expected = [
            ([0, 0, 0, 0, 32_000_000], "true"),
            ([0, 0, 2_000_000, 3_000_000, 0], None),
            ([0, 0, 0, 0, 1_410_000_000], "false"),
        ]
        total = 0
        for index, (values, phase) in enumerate(expected, 1):
            samples = self.samples(f"partial-{index}")
            self.assertEqual(len(samples), 1)
            self.check_sample(*samples[0], values, 1, phase)
            total += samples[0][0].value[4]
        self.assertEqual(total, 1_442_000_000)
