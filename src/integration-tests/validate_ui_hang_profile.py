#!/usr/bin/env python3
# Unless explicitly stated otherwise all files in this repository are licensed under
# the Apache 2 License. This product includes software developed at Datadog
# (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.
"""Validate provider-generated UI hang columns via callback and loopback HTTP.

The legacy upgrade fixtures/validator remain a strict four-column v19 replay.
These separate fixtures exercise the production registry and both custom slots.
"""

import argparse
from email import policy
from email.parser import BytesParser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import threading

from pprof_utils import extract_labels, parse_pprof
from validate_libdatadog_upgrade import normalized, read_request_body, require, type_pair

START_NS = 1700000000000000000
DETECTION_NS = START_NS + 100000000
TYPES = [
    ("cpu-time", "nanoseconds"),
    ("cpu-samples", "count"),
    ("wall-time", "nanoseconds"),
    ("wait-time", "nanoseconds"),
    ("ui-hang-time", "nanoseconds"),
]


def load(path):
    require(path.read_bytes().startswith(b"\x28\xb5\x2f\xfd"), f"{path}: expected Zstd")
    profile = parse_pprof(str(path))
    require([type_pair(profile, t) for t in profile.sample_type] == TYPES,
            f"{path}: production columns/units/order changed")
    require(type_pair(profile, profile.period_type) == TYPES[0], f"{path}: period type")
    require(profile.period == 10000000, f"{path}: period")
    identities = set()
    for sample in extract_labels(profile):
        labels = sample["labels"]
        require(labels.get("process_id"), f"{path}: missing process_id")
        require(isinstance(labels.get("thread id"), int) and labels["thread id"] > 0,
                f"{path}: missing numeric thread id")
        require(labels.get("thread_name") == "HangFixture_Main", f"{path}: thread name")
        identities.add((labels["process_id"], labels["thread id"]))
    require(len(identities) == 1, f"{path}: inconsistent process/thread identity")
    return normalized(profile)


def check_sample(sample, values, timestamp, address, view, marker=None):
    require(sample["values"] == values, f"values: {sample}")
    labels = sample["labels"]
    require(labels.get("UIHang") == marker, f"phase marker: {sample}")
    require(labels["end_timestamp_ns"] == timestamp, f"timestamp: {sample}")
    require(labels["rum.view_id"] == view and labels["trace endpoint"] == view,
            f"RUM context: {sample}")
    require(len(sample["stack"]) == 1 and sample["stack"][0]["address"] == address,
            f"phase stack: {sample}")


def check_endpoint(sample, cycle, detected):
    check_sample(sample, [0, 0, 0, 0, (32000000 if detected else 1410000000) * cycle],
                 (DETECTION_NS if detected else DETECTION_NS + 1410000000 * cycle) + cycle,
                 0x2345 if detected else 0x3456,
                 "detected-view" if detected else "recovered-view",
                 "true" if detected else "false")


def validate(directory):
    require(len(list(directory.glob("*.pprof"))) == 7, "Expected seven dedicated profiles")
    for cycle in (1, 2):
        profiles = []
        for mode in ("callback", "http"):
            result = load(directory / f"{mode}-{cycle}.pprof")
            samples = result["samples"]
            require(len(samples) == 4, f"{mode}/{cycle}: stale/missing samples after reset")
            # CPU and wall have the same timestamp but distinct values.
            ordinary = [s for s in samples if "UIHang" not in s["labels"]]
            require(len(ordinary) == 2, "Expected two ordinary samples")
            cpu = next(s for s in ordinary if s["values"][0] > 0)
            wall = next(s for s in ordinary if s["values"][2] > 0)
            check_sample(cpu, [1000000 * cycle, 1, 0, 0, 0], START_NS + cycle,
                         0x1234, "ordinary-view")
            check_sample(wall, [0, 0, 2000000 * cycle, 3000000 * cycle, 0], START_NS + cycle,
                         0x1234, "ordinary-view")
            for detected in (True, False):
                marker = "true" if detected else "false"
                check_endpoint(next(s for s in samples if s["labels"].get("UIHang") == marker),
                               cycle, detected)
            profiles.append(result)
        # Thread/process differ between the callback and HTTP fixture test cases.
        for result in profiles:
            for sample in result["samples"]:
                sample["labels"].pop("thread id")
        require(profiles[0] == profiles[1], "Callback and HTTP semantics differ")
        event = json.loads((directory / f"event-{cycle}.json").read_text())
        require(event["family"] == "windows", "HTTP family changed")
        tags = event["tags_profiler"].split(",")
        for tag in ("language:native", "runtime_os:windows", "service:ui-hang-profile-test",
                    f"profile_seq:{cycle - 1}"):
            require(tag in tags, f"HTTP tag missing: {tag}")

    partial = [load(directory / f"partial-{i}.pprof")["samples"] for i in (1, 2, 3)]
    require(all(len(samples) == 1 for samples in partial), "Partial flush duplicated/invented records")
    check_endpoint(partial[0][0], 1, True)
    check_sample(partial[1][0], [0, 0, 2000000, 3000000, 0], START_NS + 1,
                 0x1234, "ordinary-view")
    check_endpoint(partial[2][0], 1, False)
    require(sum(s["values"][4] for samples in partial for s in samples) == 1442000000,
            "Cross-flush endpoint duration duplicated")
    print("PASS: production five-column vectors, Custom1 wait/Custom2 hang, two flush/reset, "
          "phase stacks/timestamps/process/thread/RUM, callback/HTTP windows/native, "
          "detection-only/ordinary-only/recovery-only exports and disjoint 1442000000ns sum")


def run_fixtures(executable, directory):
    directory.mkdir(parents=True, exist_ok=True)
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            requests.append((self.path, self.headers["Content-Type"], read_request_body(self)))
            self.send_response(202)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    env = {k: v for k, v in os.environ.items() if not k.startswith("DD_")}
    url = f"http://127.0.0.1:{server.server_port}"
    env.update(DD_WIN_PROF_HANG_FIXTURE_DIR=str(directory), DD_WIN_PROF_HANG_AGENT_URL=url,
               DD_TRACE_AGENT_URL=url, DD_PROFILING_AUTO_START="0")
    try:
        subprocess.run([str(executable), "--gtest_filter=UiHangProfileTest.*"],
                       env=env, check=True, timeout=180)
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    require(len(requests) == 2, f"Expected two loopback uploads, got {len(requests)}")
    for cycle, (route, content_type, body) in enumerate(requests, 1):
        require(route == "/profiling/v1/input", f"HTTP route changed: {route}")
        message = BytesParser(policy=policy.default).parsebytes(
            f"Content-Type: {content_type}\r\nMIME-Version: 1.0\r\n\r\n".encode() + body)
        parts = list(message.iter_parts())
        require(len(parts) == 2, "Unexpected HTTP attachments")
        event = next(p for p in parts if p.get_param("name", header="content-disposition") == "event")
        profile = next(p for p in parts if (p.get_filename() or "").endswith(".pprof"))
        (directory / f"event-{cycle}.json").write_bytes(event.get_payload(decode=True))
        (directory / f"http-{cycle}.pprof").write_bytes(profile.get_payload(decode=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tests-exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    run_fixtures(args.tests_exe.resolve(), args.output.resolve())
    validate(args.output)


if __name__ == "__main__":
    main()
