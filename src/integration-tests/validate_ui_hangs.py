#!/usr/bin/env python3
# Unless explicitly stated otherwise all files in this repository are licensed under
# the Apache 2 License. This product includes software developed at Datadog
# (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.
"""Validate UIApp UI-hang profiles produced by the automation mode.

For a directory of .pprof files captured from a single hang kind
(sleep | wait | cpu), this checks that:

  * UIHang=true / UIHang=false samples alternate and form the expected number
    of detected/recovered pairs on the UIApp_Main thread;
  * each detected sample's stack contains the expected blocking frame;
  * hang samples carry wall-time == 0 and wait-time > 0;
  * detected precedes recovered in time;
  * when expected views are provided, each requested profiling session has one
    complete hang pair carrying its distinct RUM view;
  * no ordinary wait sample (no UIHang label, wait-time > 0) overlaps the
    interior of a detected hang interval -- i.e. the profiler suppresses
    ordinary wait accounting while a hang is in progress;
  * ordinary wall-time samples cover at least 90% of each hang interval after
    allowing for one effective per-thread sampling interval at each boundary;
  * for the cpu kind, positive cpu-time is attributed during each hang.

Usage:
    python validate_ui_hangs.py --pprof-dir <dir> --kind sleep|wait|cpu \
        --cycles <n> --duration-ms <n> --sampling-ms <n> \
        --walltime-threads-threshold <n>
"""

import argparse
import glob
import os
import sys

# Reuse the existing pprof reader (LZ4 decompress + protobuf parse).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pprof_utils  # noqa: E402


UI_THREAD_NAME = "UIApp_Main"

# System/user frames that identify each hang kind in a suspended UI-thread
# stack. Sleep and WaitForSingleObject bottom out in ntdll under symbolization,
# so several acceptable frame names are listed per kind.
EXPECTED_FRAMES = {
    "sleep": ("Sleep", "SleepEx", "RtlDelayExecution", "ZwDelayExecution"),
    "wait": (
        "WaitForSingleObject",
        "WaitForSingleObjectEx",
        "NtWaitForSingleObject",
    ),
    "cpu": ("BusySpin",),
}


class ValidationError(Exception):
    """Raised when a profile does not meet the UI-hang expectations."""


class Sample:
    """A single pprof sample with resolved values, labels and folded stack."""

    def __init__(self, values, labels, stack):
        self.values = values  # dict: sample-type name -> int64 value
        self.labels = labels  # dict: label key -> str | int
        self.stack = stack  # folded string "leaf;...;root"

    def label(self, key):
        value = self.labels.get(key)
        return str(value) if value is not None else None

    @property
    def timestamp_ns(self):
        ts = self.labels.get("end_timestamp_ns")
        return int(ts) if ts is not None else None


def _build_samples(profile):
    """Resolve one parsed profile into a list of Sample objects."""
    strings = list(profile.string_table)

    type_names = [strings[vt.type] for vt in profile.sample_type]

    functions = {fn.id: strings[fn.name] for fn in profile.function}

    location_names = {}
    for loc in profile.location:
        names = []
        for line in loc.line:
            name = functions.get(line.function_id)
            if name:
                names.append(name)
        location_names[loc.id] = names

    samples = []
    for sample in profile.sample:
        values = {
            type_names[i]: sample.value[i]
            for i in range(len(type_names))
            if i < len(sample.value)
        }

        labels = {}
        for label in sample.label:
            key = strings[label.key] if label.key < len(strings) else None
            if key is None:
                continue
            if label.str:
                labels[key] = (
                    strings[label.str] if label.str < len(strings) else ""
                )
            else:
                labels[key] = label.num

        frames = []
        for loc_id in sample.location_id:
            frames.extend(location_names.get(loc_id, []))
        stack = ";".join(frames)

        samples.append(Sample(values, labels, stack))

    return samples


def _load_all_samples(pprof_dir):
    """Parse every .pprof under pprof_dir and merge their samples.

    The profiler resets the profile after each export, so distinct files hold
    disjoint time slices; merging reconstructs the full run.
    """
    files = sorted(glob.glob(os.path.join(pprof_dir, "*.pprof")))
    if not files:
        raise ValidationError(f"no .pprof files found in {pprof_dir}")

    all_samples = []
    for path in files:
        profile = pprof_utils.parse_pprof(path)
        all_samples.extend(_build_samples(profile))
    return all_samples, files


def _describe(sample):
    return (
        f"ts={sample.timestamp_ns} values={sample.values} "
        f"labels={sample.labels} stack={sample.stack}"
    )


def validate(
    pprof_dir,
    kind,
    cycles,
    duration_ms,
    sampling_ms,
    walltime_threads_threshold,
    expected_views,
):
    all_samples, files = _load_all_samples(pprof_dir)

    ui_samples = [s for s in all_samples if s.label("thread_name") == UI_THREAD_NAME]
    if not ui_samples:
        raise ValidationError(
            f"no samples on thread '{UI_THREAD_NAME}' across {len(files)} file(s)"
        )

    thread_ids = {
        s.label("thread id")
        for s in all_samples
        if s.label("thread id") is not None
        and s.values.get("wall-time", 0) > 0
    }
    thread_count = len(thread_ids)
    if thread_count == 0:
        raise ValidationError("no thread_id labels found; cannot estimate sampling interval")

    iterations_per_thread = (
        thread_count + walltime_threads_threshold - 1
    ) // walltime_threads_threshold
    effective_sampling_ns = iterations_per_thread * sampling_ms * 1_000_000

    # -- UIHang event pairs ---------------------------------------------------
    events = sorted(
        (s for s in ui_samples if s.label("UIHang") in ("true", "false")),
        key=lambda s: (s.timestamp_ns if s.timestamp_ns is not None else 0),
    )
    for event in events:
        if event.timestamp_ns is None:
            raise ValidationError(f"UIHang sample missing end_timestamp_ns: {_describe(event)}")

    # Events must strictly alternate true/false and end paired.
    observed = [s.label("UIHang") for s in events]
    if observed != ["true", "false"] * (len(observed) // 2):
        raise ValidationError(
            f"UIHang events do not alternate true/false for kind '{kind}': {observed}"
        )

    expected_frames = EXPECTED_FRAMES[kind]
    pairs = list(zip(events[0::2], events[1::2]))

    # The very first paint of the window can itself exceed the hang threshold on
    # a slow/Debug run, producing one extra "startup jank" pair whose detected
    # stack is a paint stack rather than the requested blocking call. Tolerate at
    # most one such non-matching pair; every other pair must match this kind.
    matching = []
    nonmatching = []
    for pair in pairs:
        detected, _ = pair
        if any(f in detected.stack for f in expected_frames):
            matching.append(pair)
        else:
            nonmatching.append(pair)

    if len(matching) != cycles:
        raise ValidationError(
            f"expected {cycles} detected hang(s) with a {kind} stack "
            f"(frames {expected_frames}), found {len(matching)}; "
            f"non-matching detected stacks: "
            f"{[p[0].stack.split(';')[:6] for p in nonmatching]}"
        )
    if len(nonmatching) > 1:
        raise ValidationError(
            f"too many non-{kind} hang pairs ({len(nonmatching)}); "
            f"expected at most one startup-jank pair"
        )

    if expected_views:
        observed_views = []
        observed_view_ids = []
        for detected, recovered in matching:
            detected_view = detected.label("trace endpoint")
            recovered_view = recovered.label("trace endpoint")
            detected_view_id = detected.label("rum.view_id")
            recovered_view_id = recovered.label("rum.view_id")

            if not detected_view_id or detected_view_id != recovered_view_id:
                raise ValidationError(
                    "hang pair does not have one consistent RUM view_id: "
                    f"{_describe(detected)} | {_describe(recovered)}"
                )
            if not detected_view or detected_view != recovered_view:
                raise ValidationError(
                    "hang pair does not have one consistent trace endpoint: "
                    f"{_describe(detected)} | {_describe(recovered)}"
                )

            observed_views.append(detected_view)
            observed_view_ids.append(detected_view_id)

        if observed_views != expected_views:
            raise ValidationError(
                f"expected hang views {expected_views}, found {observed_views}"
            )
        if len(set(observed_view_ids)) != len(expected_views):
            raise ValidationError(
                "expected a distinct rum.view_id for each profiling session, "
                f"found {observed_view_ids}"
            )

    for detected, recovered in matching:
        if detected.values.get("wall-time", -1) != 0:
            raise ValidationError(f"detected hang wall-time != 0: {_describe(detected)}")
        if recovered.values.get("wall-time", -1) != 0:
            raise ValidationError(f"recovered hang wall-time != 0: {_describe(recovered)}")
        if detected.values.get("wait-time", 0) <= 0:
            raise ValidationError(f"detected hang wait-time <= 0: {_describe(detected)}")
        if recovered.values.get("wait-time", 0) <= 0:
            raise ValidationError(f"recovered hang wait-time <= 0: {_describe(recovered)}")
        if not detected.timestamp_ns < recovered.timestamp_ns:
            raise ValidationError(
                "detected timestamp not before recovered timestamp: "
                f"{_describe(detected)} | {_describe(recovered)}"
            )

    # -- No ordinary wait accounting during a detected hang -------------------
    # An ordinary wait sample's interval is [end_timestamp - wait-time, end_timestamp].
    # Allow two sampling periods of slack at each boundary to absorb cross-thread
    # timestamp ordering between the sampler and the hang watchdog.
    grace_ns = 2 * sampling_ms * 1_000_000

    ordinary_waits = [
        s
        for s in ui_samples
        if s.label("UIHang") is None
        and s.values.get("wait-time", 0) > 0
        and s.timestamp_ns is not None
    ]
    if not ordinary_waits:
        raise ValidationError(
            "found no ordinary wait samples on UIApp_Main; the no-overlap check "
            "would be vacuous (expected pre/post-hang wait samples)"
        )

    for wait in ordinary_waits:
        wait_start = wait.timestamp_ns - wait.values["wait-time"]
        for detected, recovered in pairs:
            protected_start = detected.timestamp_ns + grace_ns
            protected_end = recovered.timestamp_ns - grace_ns
            if wait_start < protected_end and wait.timestamp_ns > protected_start:
                raise ValidationError(
                    "ordinary wait sample overlaps a detected hang interval "
                    f"[{protected_start}, {protected_end}]: {_describe(wait)}"
                )

    # -- Wall-time coverage during hangs --------------------------------------
    # Samples are attributed to their end timestamp. The samples crossing the
    # hang boundaries may therefore end just outside the interval. Allow one
    # effective per-thread sampling interval at each boundary, accounting for
    # round-robin sampling when the thread count exceeds the per-iteration limit.
    for detected, recovered in matching:
        hang_duration_ns = recovered.timestamp_ns - detected.timestamp_ns
        boundary_allowance_ns = min(
            hang_duration_ns, 2 * effective_sampling_ns
        )
        expected_walltime_ns = hang_duration_ns - boundary_allowance_ns
        walltime_ns = sum(
            s.values.get("wall-time", 0)
            for s in ui_samples
            if s.label("UIHang") is None
            and s.timestamp_ns is not None
            and detected.timestamp_ns < s.timestamp_ns < recovered.timestamp_ns
        )
        minimum_walltime_ns = 0.90 * expected_walltime_ns
        if walltime_ns < minimum_walltime_ns:
            raise ValidationError(
                "insufficient ordinary wall-time coverage during hang: "
                f"observed={walltime_ns}ns minimum={minimum_walltime_ns:.0f}ns "
                f"hang={hang_duration_ns}ns effective_sampling="
                f"{effective_sampling_ns}ns thread_count={thread_count} "
                f"threshold={walltime_threads_threshold}"
            )

    # -- CPU attribution during CPU hangs -------------------------------------
    if kind == "cpu":
        for detected, recovered in matching:
            cpu_ns = sum(
                s.values.get("cpu-time", 0)
                for s in ui_samples
                if s.timestamp_ns is not None
                and detected.timestamp_ns <= s.timestamp_ns <= recovered.timestamp_ns
            )
            if cpu_ns <= 0:
                raise ValidationError(
                    "no cpu-time attributed during CPU hang interval "
                    f"[{detected.timestamp_ns}, {recovered.timestamp_ns}]"
                )

    print(
        f"OK: kind={kind} files={len(files)} matching_pairs={len(matching)} "
        f"(expected {cycles}) startup_pairs={len(nonmatching)} "
        f"ordinary_waits={len(ordinary_waits)}"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pprof-dir", required=True)
    parser.add_argument("--kind", required=True, choices=("sleep", "wait", "cpu"))
    parser.add_argument("--cycles", required=True, type=int)
    parser.add_argument("--duration-ms", required=True, type=int)
    parser.add_argument("--sampling-ms", required=True, type=int)
    parser.add_argument("--walltime-threads-threshold", required=True, type=int)
    parser.add_argument(
        "--expected-view",
        action="append",
        default=[],
        help="Expected trace endpoint for one hang pair; repeat in session order",
    )
    args = parser.parse_args()

    try:
        validate(
            args.pprof_dir,
            args.kind,
            args.cycles,
            args.duration_ms,
            args.sampling_ms,
            args.walltime_threads_threshold,
            args.expected_view,
        )
    except ValidationError as err:
        print(f"FAILED: {err}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
