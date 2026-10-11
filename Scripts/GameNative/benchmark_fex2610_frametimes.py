#!/usr/bin/env python3
"""Compare real, non-frame-generated display frame intervals for two FEX WCP builds.

Accepted CSV headers: frame_time_ms, frametime_ms, or present_time_ns.
The timestamp format uses adjacent presentation timestamps in nanoseconds.
No FPS estimates are fabricated when samples are missing.
"""
import argparse
import csv
import json
import math
from pathlib import Path
import statistics
import sys

FIELDS = ("frame_time_ms", "frametime_ms", "present_time_ns")

def read_intervals(path: Path):
    with path.open("r", newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        field = next((f for f in FIELDS if f in (reader.fieldnames or [])), None)
        if field is None:
            raise ValueError(f"{path}: expected a CSV header containing {FIELDS}")
        raw = []
        for line, row in enumerate(reader, 2):
            try:
                value = float(row[field])
            except (ValueError, TypeError):
                raise ValueError(f"{path}:{line}: invalid {field}: {row.get(field)!r}") from None
            if not math.isfinite(value) or value < 0:
                raise ValueError(f"{path}:{line}: nonfinite or negative {field}")
            raw.append(value)
    if field == "present_time_ns":
        if len(raw) < 2:
            raise ValueError(f"{path}: need at least two presentation timestamps")
        if any(b <= a for a, b in zip(raw, raw[1:])):
            raise ValueError(f"{path}: presentation timestamps must strictly increase")
        return [(b - a) / 1_000_000.0 for a, b in zip(raw, raw[1:])]
    if any(t <= 0 for t in raw):
        raise ValueError(f"{path}: frame times must be positive")
    return raw

def trim_warmup(times, seconds):
    elapsed = 0.0
    pos = 0
    while pos < len(times) and elapsed < seconds * 1000:
        elapsed += times[pos]
        pos += 1
    return times[pos:]

def percentile(sorted_times, percent):
    # Nearest rank: deterministic even for non-normal, long-tailed frame data.
    return sorted_times[max(0, math.ceil(len(sorted_times) * percent) - 1)]

def metrics(times, minimum):
    if len(times) < minimum:
        raise ValueError(f"only {len(times)} frames; need at least {minimum} after warmup")
    ordered = sorted(times)
    slow_count = max(1, math.ceil(len(ordered) / 100))
    mean_ms = statistics.fmean(times)
    return {
        "frames": len(times),
        "duration_seconds": round(sum(times) / 1000, 3),
        "mean_fps": round(1000 / mean_ms, 3),
        "one_percent_low_fps": round(1000 / statistics.fmean(ordered[-slow_count:]), 3),
        "p95_frame_ms": round(percentile(ordered, 0.95), 3),
        "p99_frame_ms": round(percentile(ordered, 0.99), 3),
        "frame_stddev_ms": round(statistics.pstdev(times), 3),
        "frames_over_50ms": sum(t > 50 for t in times),
    }

def compare(baseline, candidate):
    def improvement(b, c, higher):
        return round((c / b - 1) * 100 if higher else (1 - c / b) * 100, 2)
    return {
        "mean_fps_percent_change": improvement(baseline["mean_fps"], candidate["mean_fps"], True),
        "one_percent_low_fps_percent_change": improvement(baseline["one_percent_low_fps"], candidate["one_percent_low_fps"], True),
        "p99_frame_time_percent_reduction": improvement(baseline["p99_frame_ms"], candidate["p99_frame_ms"], False),
    }

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--warmup-seconds", type=float, default=0.0)
    parser.add_argument("--min-frames", type=int, default=120)
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args(argv)
    if not math.isfinite(args.warmup_seconds) or args.warmup_seconds < 0 or args.min_frames < 2:
        parser.error("warmup must be nonnegative and min-frames must be at least 2")
    try:
        base = metrics(trim_warmup(read_intervals(args.baseline), args.warmup_seconds), args.min_frames)
        cand = metrics(trim_warmup(read_intervals(args.candidate), args.warmup_seconds), args.min_frames)
    except (ValueError, OSError) as exc:
        parser.error(str(exc))
    output = {"baseline": base, "candidate": cand, "delta": compare(base, cand),
              "warmup_seconds": args.warmup_seconds,
              "disclaimer": "FPS derived from supplied presentation intervals; not a JIT or GPU bottleneck attribution"}
    if args.json_out:
        args.json_out.write_text(json.dumps(output, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(output, indent=2, sort_keys=True))
    return 0

if __name__ == "__main__":
    sys.exit(main())
