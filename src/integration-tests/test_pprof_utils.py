#!/usr/bin/env python3
# Unless explicitly stated otherwise all files in this repository are licensed under
# the Apache 2 License. This product includes software developed at Datadog
# (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.
"""Decoder compatibility for historical LZ4 and current Zstd callback bytes."""

from pathlib import Path
import tempfile
import unittest

import lz4.block
import lz4.frame
import zstandard

from pprof_utils import _ensure_profile_pb2, extract_labels, parse_pprof

_ensure_profile_pb2()
import profile_pb2


class PprofDecoderTests(unittest.TestCase):
    def setUp(self):
        self.profile = profile_pb2.Profile()
        self.profile.string_table.extend(["", "wait-time", "nanoseconds", "rum.view_id", "view"])
        self.profile.sample_type.add(type=1, unit=2)
        self.profile.period_type.CopyFrom(self.profile.sample_type[0])
        self.profile.period = 10000000
        sample = self.profile.sample.add(value=[1234])
        sample.label.add(key=3, str=4)
        self.raw = self.profile.SerializeToString()

    def parse(self, data, filename="legacy-name.lz4.pprof"):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / filename
            path.write_bytes(data)
            return parse_pprof(str(path))

    def test_historical_lz4_frame(self):
        decoded = self.parse(lz4.frame.compress(self.raw))
        self.assertEqual(decoded, self.profile)
        self.assertEqual(extract_labels(decoded), [{"labels": {"rum.view_id": "view"}}])

    def test_historical_lz4_block(self):
        self.assertEqual(self.parse(lz4.block.compress(self.raw)), self.profile)

    def test_current_zstd_frame(self):
        compressed = zstandard.ZstdCompressor().compress(self.raw)
        for filename in ("profile.zstd.pprof", "profile.lz4.pprof"):
            with self.subTest(filename=filename):
                self.assertEqual(self.parse(compressed, filename), self.profile)

    def test_current_zstd_stream_without_content_size(self):
        for checksum in (False, True):
            with self.subTest(checksum=checksum):
                compressed = zstandard.ZstdCompressor(
                    write_content_size=False, write_checksum=checksum
                ).compress(self.raw)
                self.assertEqual(self.parse(compressed), self.profile)

    def test_truncated_zstd_frame_rejected(self):
        for content_size in (False, True):
            for checksum in (False, True):
                compressed = zstandard.ZstdCompressor(
                    write_content_size=content_size, write_checksum=checksum
                ).compress(self.raw)
                for length in (4, len(compressed) // 2, len(compressed) - 4, len(compressed) - 1):
                    with self.subTest(content_size=content_size, checksum=checksum, length=length):
                        with self.assertRaisesRegex(ValueError, "Incomplete Zstd frame"):
                            self.parse(compressed[:length])

    def test_concatenated_zstd_frames(self):
        compressor = zstandard.ZstdCompressor(write_content_size=False, write_checksum=True)
        midpoint = len(self.raw) // 2
        first = compressor.compress(self.raw[:midpoint])
        second = compressor.compress(self.raw[midpoint:])
        self.assertEqual(self.parse(first + second), self.profile)
        for length in (4, len(second) - 4, len(second) - 1):
            with self.subTest(length=length):
                with self.assertRaisesRegex(ValueError, "Incomplete Zstd frame"):
                    self.parse(first + second[:length])

    def test_corrupt_payload_rejected(self):
        for data in (b"not compressed pprof", b"\x28\xb5\x2f\xfdinvalid"):
            with self.subTest(data=data), self.assertRaises(Exception):
                self.parse(data)


if __name__ == "__main__":
    unittest.main()
