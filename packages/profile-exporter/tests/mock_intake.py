#!/usr/bin/env python3
# Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0
"""Run on Windows with Python 3 and exporter_probe.exe. No third-party Python modules."""
import argparse
import ctypes
import ctypes.util
import email.parser
import email.policy
import http.server
import json
import pathlib
import re
import socket
import subprocess
import threading
import time


def fixture():
    header = pathlib.Path(__file__).with_name("fixture.h").read_text()
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", header))


def validate_profile(path):
    data = pathlib.Path(path).read_bytes()
    assert data == fixture(), "attachment changed"
    try:
        import zstandard
    except ImportError:
        name = ctypes.util.find_library("zstd")
        if not name:
            raise RuntimeError("Install libzstd or the zstandard Python module on the decoding host")
        zstd = ctypes.CDLL(name)
        zstd.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
        zstd.ZSTD_decompress.restype = ctypes.c_size_t
        out = ctypes.create_string_buffer(4096)
        size = zstd.ZSTD_decompress(out, len(out), data, len(data))
        assert size < len(out), f"Zstd decode failed: {size}"
        pprof = out.raw[:size]
    else:
        pprof = zstandard.ZstdDecompressor().decompress(data, max_output_size=4096)
    size = len(pprof)
    # Known fixture protobuf: sample_type(type=1, unit=2), string table, sample(value=42).
    assert pprof.startswith(b"\x0a\x04\x08\x01\x10\x02")
    assert b"\x32\x08cpu-time" in pprof and b"\x32\x0bnanoseconds" in pprof
    assert pprof.endswith(b"\x12\x02\x10\x2a")
    print(f"decoded attachment: {len(data)} Zstd bytes, {size} protobuf bytes, cpu-time sample=42")


def validate_request(handler, body, output):
    assert handler.path == "/prefix/profiling/v1/input", handler.path
    assert handler.headers["DD-EVP-ORIGIN"] == "dd-win-prof"
    assert handler.headers["DD-EVP-ORIGIN-VERSION"] == "test"
    assert handler.headers["DD-API-KEY"] is None
    content_type = handler.headers["Content-Type"]
    message = email.parser.BytesParser(policy=email.policy.default).parsebytes(
        f"Content-Type: {content_type}\r\nMIME-Version: 1.0\r\n\r\n".encode() + body
    )
    assert message.is_multipart() and not message.defects
    parts = list(message.iter_parts())
    assert [p.get_filename() for p in parts] == ["event.json", "extra.json", "auto.pprof"]
    event = json.loads(parts[0].get_payload(decode=True))
    assert event == {
        "attachments": ["auto.pprof", "extra.json"],
        "tags_profiler": "service:exporter-mock,env:test",
        "start": "2023-11-14T22:13:20.000000000Z",
        "end": "2023-11-14T22:13:21.000000000Z",
        "family": "windows", "version": "4", "internal": {"views": [], "sessions": []},
    }, event
    assert parts[1].get_payload(decode=True) == b'{"unmodified":true}'
    compressed = parts[2].get_payload(decode=True)
    assert compressed == fixture()
    output.write_bytes(compressed)


def run_mock(probe, output):
    errors = []
    seen = []
    mode = [200]

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            try:
                seen.append(self.path)
                length = int(self.headers["Content-Length"])
                body = self.rfile.read(length)
                assert len(body) == length
                validate_request(self, body, output)
                if mode[0] == "timeout":
                    time.sleep(1.5)
                    return
                self.send_response(mode[0])
                if mode[0] == 307:
                    self.send_header("Location", f"http://127.0.0.1:{self.server.server_port}/redirect-target")
                self.send_header("Content-Length", "0")
                self.end_headers()
            except Exception as error:
                errors.append(str(error))

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    endpoint = f"http://127.0.0.1:{server.server_port}/prefix/"
    try:
        for response, expected, timeout in [(200, 0, 3000), (403, 5, 3000), (307, 5, 3000), ("timeout", 4, 200)]:
            mode[0] = response
            subprocess.run([str(probe), endpoint, str(expected), str(timeout)], check=True, timeout=15)
        assert seen == ["/prefix/profiling/v1/input"] * 4, seen
        assert not errors, errors
        # Bound but not listening, so another process cannot steal the refusal port.
        with socket.socket() as closed:
            closed.bind(("127.0.0.1", 0))
            refused = f"http://127.0.0.1:{closed.getsockname()[1]}"
            subprocess.run([str(probe), refused, "4", "200"], check=True, timeout=15)
        print("WinHTTP mock passed: multipart/metadata/pprof, 403, redirect blocked, receive timeout, connection refusal")
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path("received.zst.pprof"))
    parser.add_argument("--decode", type=pathlib.Path, help="Decode captured fixture on a host with libzstd")
    args = parser.parse_args()
    if args.decode:
        validate_profile(args.decode)
    elif args.probe:
        run_mock(args.probe.resolve(), args.output)
    else:
        parser.error("provide --probe or --decode")


if __name__ == "__main__":
    main()
