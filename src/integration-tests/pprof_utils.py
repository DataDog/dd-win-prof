#!/usr/bin/env python3
"""Utility for reading LZ4-compressed pprof files and extracting sample labels.

Usage:
    python pprof_utils.py labels <file.lz4.pprof>

Outputs a JSON array of samples with their resolved string labels.
"""

import json
import os
import subprocess
import sys


def _ensure_profile_pb2():
    """Auto-generate profile_pb2.py from profile.proto if missing."""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    pb2_path = os.path.join(script_dir, "profile_pb2.py")
    proto_path = os.path.join(script_dir, "profile.proto")

    if os.path.exists(pb2_path):
        return

    if not os.path.exists(proto_path):
        print(f"ERROR: {proto_path} not found", file=sys.stderr)
        sys.exit(1)

    print("Generating profile_pb2.py from profile.proto ...", file=sys.stderr)
    subprocess.check_call([
        sys.executable, "-m", "grpc_tools.protoc",
        f"--proto_path={script_dir}",
        f"--python_out={script_dir}",
        proto_path,
    ])


def decompress_lz4(data: bytes) -> bytes:
    """Decompress LZ4 data, trying frame format first then block format."""
    import lz4.frame
    try:
        return lz4.frame.decompress(data)
    except Exception:
        pass

    import lz4.block
    try:
        return lz4.block.decompress(data, uncompressed_size=-1)
    except Exception:
        pass

    raise RuntimeError("Failed to decompress data as LZ4 frame or block format")


def parse_pprof(path: str):
    """Parse an LZ4-compressed pprof file, return the Profile protobuf object."""
    _ensure_profile_pb2()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    if script_dir not in sys.path:
        sys.path.insert(0, script_dir)

    import profile_pb2  # noqa: E402  (generated file)

    with open(path, "rb") as f:
        raw = f.read()

    decompressed = decompress_lz4(raw)

    profile = profile_pb2.Profile()
    profile.ParseFromString(decompressed)
    return profile


def extract_labels(profile) -> list[dict]:
    """Extract per-sample string labels from a parsed profile.

    Returns a list of dicts, one per sample:
        [{"labels": {"key1": "val1", ...}}, ...]
    """
    st = list(profile.string_table)
    results = []

    for sample in profile.sample:
        labels = {}
        for label in sample.label:
            key = st[label.key] if label.key < len(st) else f"<idx:{label.key}>"
            if label.str:
                val = st[label.str] if label.str < len(st) else f"<idx:{label.str}>"
            else:
                val = label.num
            labels[key] = val
        results.append({"labels": labels})

    return results


def cmd_labels(path: str):
    """Print JSON array of sample labels for the given pprof file."""
    profile = parse_pprof(path)
    labels = extract_labels(profile)
    print(json.dumps(labels, indent=2))


def main():
    if len(sys.argv) < 2:
        print("Usage: pprof_utils.py labels <file.lz4.pprof>", file=sys.stderr)
        sys.exit(1)

    command = sys.argv[1]

    if command == "labels":
        if len(sys.argv) < 3:
            print("Usage: pprof_utils.py labels <file.lz4.pprof>", file=sys.stderr)
            sys.exit(1)
        cmd_labels(sys.argv[2])
    else:
        print(f"Unknown command: {command}", file=sys.stderr)
        print("Available commands: labels", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
