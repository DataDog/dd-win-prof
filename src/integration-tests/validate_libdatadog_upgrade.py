#!/usr/bin/env python3
# Unless explicitly stated otherwise all files in this repository are licensed under
# the Apache 2 License. This product includes software developed at Datadog
# (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.
"""Run deterministic C++ fixtures and validate decoded pprof and local HTTP uploads.

Optionally compare with a baseline produced by the same fixtures against v19.
No Datadog credentials or external uploads are used.
"""

import argparse
from email import policy
from email.parser import BytesParser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import socket
import subprocess
import threading
import time

from pprof_utils import extract_labels, parse_pprof

START_NS = 1700000000000000000
TIMESTAMP_NS = START_NS + 100000000
TYPES = [
    ("cpu-time", "nanoseconds"),
    ("cpu-samples", "count"),
    ("wall-time", "nanoseconds"),
    ("wait-time", "nanoseconds"),
]


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def type_pair(profile, value_type):
    return (profile.string_table[value_type.type], profile.string_table[value_type.unit])


def normalized(profile):
    """Resolve IDs; omit only process identity and exporter wall-clock windows."""
    labels = extract_labels(profile)
    functions = {fn.id: fn for fn in profile.function}
    locations = {location.id: location for location in profile.location}
    samples = []
    for sample, resolved in zip(profile.sample, labels):
        sample_labels = resolved["labels"]
        sample_labels.pop("process_id", None)
        sample_labels.pop("process id", None)
        stack = []
        for location_id in sample.location_id:
            location = locations[location_id]
            lines = []
            for line in location.line:
                function = functions[line.function_id]
                lines.append((profile.string_table[function.name],
                              profile.string_table[function.filename], line.line))
            stack.append({"address": location.address, "lines": lines})
        samples.append({"values": list(sample.value), "labels": sample_labels, "stack": stack})
    return {
        "types": [type_pair(profile, item) for item in profile.sample_type],
        "period_type": type_pair(profile, profile.period_type),
        "period": profile.period,
        "samples": sorted(samples, key=lambda item: item["labels"]["end_timestamp_ns"]),
    }


def validate_profile(path, cycle, direct=False):
    profile = parse_pprof(str(path))
    result = normalized(profile)
    require(result["types"] == TYPES, f"{path}: sample names/units/order changed")
    require(result["period_type"] == TYPES[0], f"{path}: period type changed")
    require(result["period"] == 10000000, f"{path}: period changed")
    expected_values = [1000000 * cycle, cycle, 2000000 * cycle, 3000000 * cycle]
    samples = result["samples"]
    require(len(samples) == (1 if direct else 3), f"{path}: stale/missing samples after reset")
    require(samples[0]["values"] == expected_values, f"{path}: ordinary values changed")
    require(samples[0]["labels"]["end_timestamp_ns"] == TIMESTAMP_NS + cycle,
            f"{path}: timestamp changed")
    for sample in samples:
        require(sample["labels"]["rum.view_id"] == "upgrade-view", f"{path}: RUM label lost")
        require(len(sample["stack"]) == 1 and sample["stack"][0]["address"] == 0x1234,
                f"{path}: sample stack lost")
    if direct:
        require(profile.time_nanos == START_NS, f"{path}: start timestamp changed")
        require(profile.duration_nanos == 1000000000, f"{path}: duration changed")
        require(len(profile.location) == 1, f"{path}: stack lost")
        location = profile.location[0]
        require(location.address == 0x1234 and location.line[0].line == 42,
                f"{path}: location changed")
        function = next(fn for fn in profile.function if fn.id == location.line[0].function_id)
        require(profile.string_table[function.name] == "UpgradeFrame", f"{path}: function changed")
        result["time_nanos"] = profile.time_nanos
        result["duration_nanos"] = profile.duration_nanos
    else:
        require("UIHang" not in samples[0]["labels"], f"{path}: ordinary hang marker")
        for index, marker in ((1, "true"), (2, "false")):
            sample = samples[index]
            require(sample["values"] == [0, 0, 0, 4000000 * cycle],
                    f"{path}: existing hang wait-time encoding changed")
            require(sample["labels"]["UIHang"] == marker, f"{path}: hang marker changed")
            require(sample["labels"]["end_timestamp_ns"] == TIMESTAMP_NS + cycle + index,
                    f"{path}: hang timestamp changed")
        for sample in samples:
            require(sample["labels"]["trace endpoint"] == "Upgrade view", f"{path}: endpoint lost")
    return result


def validate(directory, codec):
    results = {}
    magic = {"lz4": b"\x04\x22\x4d\x18", "zstd": b"\x28\xb5\x2f\xfd"}[codec]
    profiles = list(directory.glob("*.pprof"))
    require(len(profiles) == 7, f"Expected seven {codec} profiles, got {len(profiles)}")
    for path in profiles:
        require(path.read_bytes().startswith(magic), f"{path}: expected {codec} frame")
    for cycle in (1, 2):
        for mode in ("direct", "callback", "http"):
            name = f"{mode}-{cycle}.pprof"
            results[name] = validate_profile(directory / name, cycle, mode == "direct")
        require(results[f"callback-{cycle}.pprof"] == results[f"http-{cycle}.pprof"],
                "Callback and HTTP profile semantics differ")
        event = json.loads((directory / f"event-{cycle}.json").read_text())
        require(event["family"] == "windows", "HTTP family changed")
        tags = event["tags_profiler"].split(",")
        for tag in ("language:native", "runtime_os:windows", "service:libdatadog-upgrade-test",
                    f"profile_seq:{cycle - 1}"):
            require(tag in tags, f"HTTP tag missing: {tag}")
        internal = event["internal"]
        require(internal["views"][0]["viewId"] == "upgrade-view", "HTTP view metadata lost")
        require(internal["views"][0]["vitals"] == {"cpuTimeNs": 1000000, "waitTimeNs": 3000000},
                "HTTP vitals changed")
        require(internal["sessions"][0]["sessionId"] == "upgrade-session", "HTTP session lost")
    custom = parse_pprof(str(directory / "custom-period.pprof"))
    require([type_pair(custom, item) for item in custom.sample_type] == [TYPES[3]],
            "Custom period sample type changed")
    require(type_pair(custom, custom.period_type) == TYPES[3] and custom.period == 10000000,
            "Custom period configuration lost")
    require(not custom.sample, "Custom-period fixture should be empty")
    return results


def read_request_body(handler):
    if handler.headers.get("Transfer-Encoding", "").lower() == "chunked":
        chunks = []
        while True:
            line = handler.rfile.readline()
            size = int(line.split(b";", 1)[0], 16)
            if size == 0:
                while True:
                    trailer = handler.rfile.readline()
                    require(trailer, "Truncated chunk trailer")
                    if trailer == b"\r\n":
                        return b"".join(chunks)
            chunks.append(handler.rfile.read(size))
            require(handler.rfile.read(2) == b"\r\n", "Invalid chunk terminator")
    return handler.rfile.read(int(handler.headers["Content-Length"]))


def run_fixtures(executable, directory):
    directory.mkdir(parents=True, exist_ok=True)
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            body = read_request_body(self)
            requests.append((self.path, self.headers["Content-Type"], body))
            self.send_response(202)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    env = os.environ.copy()
    for key in list(env):
        if key.startswith("DD_"):
            env.pop(key)
    url = f"http://127.0.0.1:{server.server_port}"
    env.update(DD_WIN_PROF_UPGRADE_FIXTURE_DIR=str(directory),
               DD_WIN_PROF_UPGRADE_AGENT_URL=url, DD_TRACE_AGENT_URL=url,
               DD_PROFILING_AUTO_START="0")
    try:
        subprocess.run([str(executable), "--gtest_filter=ProfileUpgradeTest.*-ProfileUpgradeTest.HttpResponseAndTransportPolicy"],
                       env=env, check=True, timeout=180)
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    require(len(requests) == 2, f"Expected two local HTTP uploads, got {len(requests)}")
    for cycle, (path, content_type, body) in enumerate(requests, 1):
        require(path == "/profiling/v1/input", f"HTTP route changed: {path}")
        message = BytesParser(policy=policy.default).parsebytes(
            f"Content-Type: {content_type}\r\nMIME-Version: 1.0\r\n\r\n".encode() + body)
        parts = list(message.iter_parts())
        require(len(parts) == 2, "Unexpected HTTP attachments")
        event = next(part for part in parts if part.get_param("name", header="content-disposition") == "event")
        profile = next(part for part in parts if (part.get_filename() or "").endswith(".pprof"))
        (directory / f"event-{cycle}.json").write_bytes(event.get_payload(decode=True))
        (directory / f"http-{cycle}.pprof").write_bytes(profile.get_payload(decode=True))


def run_http_policy(executable):
    for mode in ("200", "204", "403", "404", "500", "504", "timeout", "refused"):
        requests = []

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self, mode=mode):
                read_request_body(self)
                requests.append(self.path)
                if mode == "timeout" and len(requests) == 1:
                    time.sleep(12)
                self.send_response(202 if mode == "timeout" else int(mode))
                self.send_header("Content-Length", "0")
                try:
                    self.end_headers()
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    pass

            def log_message(self, *_args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        port = server.server_port
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        if mode == "refused":
            server.server_close()
            # Reserve the port without listening, so all three connects are refused.
            reserved = socket.socket()
            reserved.bind(("127.0.0.1", port))
        else:
            thread.start()
        env = {key: value for key, value in os.environ.items() if not key.startswith("DD_")}
        env.update(DD_TRACE_AGENT_URL=f"http://127.0.0.1:{port}",
                   DD_WIN_PROF_UPGRADE_HTTP_MODE=mode, DD_PROFILING_AUTO_START="0")
        try:
            subprocess.run([str(executable),
                            "--gtest_filter=ProfileUpgradeTest.HttpResponseAndTransportPolicy"],
                           env=env, check=True, timeout=60)
        finally:
            if mode == "refused":
                reserved.close()
            else:
                server.shutdown()
                server.server_close()
                thread.join()
        require(len(requests) == (0 if mode == "refused" else 3),
                f"Unexpected request count for {mode}: {len(requests)}")
        print(f"PASS: HTTP {mode}, periodic/final thread transition and cleanup")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tests-exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--codec", choices=("lz4", "zstd"), default="zstd")
    args = parser.parse_args()
    run_fixtures(args.tests_exe.resolve(), args.output.resolve())
    results = validate(args.output, args.codec)
    run_http_policy(args.tests_exe.resolve())
    if args.baseline:
        require(results == validate(args.baseline, "lz4"), "Decoded profile differs from v19 baseline")
    print("PASS: types/order/values/period/labels/timestamps/reset, callback and local HTTP parity")
    if args.baseline:
        print("PASS: decoded v19 baseline parity")


if __name__ == "__main__":
    main()
