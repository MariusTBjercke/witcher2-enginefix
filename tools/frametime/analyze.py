#!/usr/bin/env python3
"""Frame pacing report for PresentMon 2.x CSVs (see capture.ps1). Standard library only.

    python tools/frametime/analyze.py build/frametime/baseline-1.csv [more.csv ...]
    python tools/frametime/analyze.py --segments 20 build/frametime/baseline-*.csv

Three things are kept apart on purpose:
  present  MsBetweenPresents        when the game handed over each frame
  display  MsBetweenDisplayChange   how long each frame actually stayed on the screen
  gpu      MsGPUBusy                how long the GPU worked on it

Under DXVK PresentMon sees PresentRuntime=Other and MsInPresentAPI=0, so MsCPUBusy just equals
the frame interval: it says nothing about where the CPU time went. It is not reported.

hitch    a frame longer than 1.5x the rolling median of the 31 frames around it
jitter   mean absolute change from one frame to the next (ms); steady pacing is near 0
period   strongest repeating pattern in the frame times, as cycles per second
"""
import argparse
import cmath
import csv
import glob
import math
import statistics
import sys


def num(value):
    try:
        v = float(value)
    except (TypeError, ValueError):
        return None
    return v if math.isfinite(v) else None


def pct(values, p):
    ordered = sorted(values)
    if not ordered:
        return float("nan")
    k = (len(ordered) - 1) * p / 100
    lo, hi = math.floor(k), math.ceil(k)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (k - lo)


def hitches(values, factor=1.5, half_window=15):
    found = []
    for i, v in enumerate(values):
        window = values[max(0, i - half_window) : i + half_window + 1]
        if v > factor * statistics.median(window):
            found.append(i)
    return found


def jitter(values):
    if len(values) < 2:
        return float("nan")
    return sum(abs(b - a) for a, b in zip(values, values[1:])) / (len(values) - 1)


def strongest_period(times_ms, values, lo_hz=0.2, hi_hz=20.0, step_hz=0.02, rate_hz=100.0):
    """Peak of a DFT over the frame times, sampled at a steady rate (frames are not evenly spaced)."""
    if len(values) < 200:
        return None
    start, end = times_ms[0], times_ms[-1]
    count = int((end - start) / 1000 * rate_hz)
    if count < 200:
        return None
    samples, j = [], 0
    for n in range(count):
        t = start + n * 1000 / rate_hz
        while j + 1 < len(times_ms) and times_ms[j + 1] <= t:
            j += 1
        samples.append(values[j])
    mean = sum(samples) / count
    samples = [s - mean for s in samples]
    best, spectrum = (0.0, 0.0), []
    hz = lo_hz
    while hz <= hi_hz:
        w = -2j * math.pi * hz / rate_hz
        amp = abs(sum(s * cmath.exp(w * n) for n, s in enumerate(samples))) * 2 / count
        spectrum.append(amp)
        if amp > best[1]:
            best = (hz, amp)
        hz += step_hz
    floor = statistics.median(spectrum) or 1e-9
    return best[0], best[1], best[1] / floor


def describe(name, values):
    return (
        f"  {name:8} p50 {pct(values, 50):6.2f}  p95 {pct(values, 95):6.2f}  p99 {pct(values, 99):6.2f}"
        f"  p99.9 {pct(values, 99.9):6.2f}  max {max(values):7.2f}  jitter {jitter(values):5.2f}"
    )


def report(rows, title):
    times = [r["t"] for r in rows]
    present = [r["present"] for r in rows if r["present"] is not None]
    display = [r["display"] for r in rows if r["display"] is not None]
    gpu = [r["gpu"] for r in rows if r["gpu"] is not None]
    seconds = (times[-1] - times[0]) / 1000 if len(times) > 1 else 0
    if not present or seconds <= 0:
        print(f"{title}: too little data")
        return
    print(f"{title}: {len(rows)} frames in {seconds:.1f} s = {len(present) / seconds:.1f} fps")
    print(describe("present", present))
    if display:
        print(describe("display", display))
    if gpu:
        print(f"  gpu      p50 {pct(gpu, 50):6.2f}  p99 {pct(gpu, 99):6.2f}  max {max(gpu):7.2f}")
    until = [r["until"] for r in rows if r["until"] is not None]
    not_shown = sum(1 for r in rows if r["display"] is None)
    print(f"  never displayed: {not_shown} ({not_shown / seconds:.2f}/s), present to screen p50 {pct(until, 50):.1f} ms")

    for name, values in (("present", present), ("display", display)):
        if not values:
            continue
        found = hitches(values)
        rate = len(found) / seconds
        over = sum(1 for v in values if v > 33.4)
        print(f"  {name} hitches: {len(found)} ({rate:.2f}/s), frames over 33 ms: {over}")

    paced = [(r["t"], r["present"]) for r in rows if r["present"] is not None]
    peak = strongest_period([p[0] for p in paced], [p[1] for p in paced])
    if peak:
        hz, amp, ratio = peak
        print(f"  period: {hz:.2f} Hz (every {1000 / hz:.0f} ms), {amp:.2f} ms, {ratio:.0f}x the noise floor")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv", nargs="+", help="PresentMon CSV files (wildcards allowed)")
    parser.add_argument("--segments", type=float, default=0, help="also report each N-second part of a run")
    parser.add_argument("--worst", type=int, default=5, help="list the N longest frames")
    args = parser.parse_args()

    paths = [m for pattern in args.csv for m in (glob.glob(pattern) or [pattern])]
    for path in paths:
        with open(path, newline="", encoding="utf-8-sig") as handle:
            raw = list(csv.DictReader(handle))
        rows, modes = [], {}
        for r in raw:
            t = num(r.get("TimeInMs"))
            if t is None:
                continue
            rows.append(
                {
                    "t": t,
                    "present": num(r.get("MsBetweenPresents")),
                    "display": num(r.get("MsBetweenDisplayChange")),
                    "gpu": num(r.get("MsGPUBusy")),
                    "until": num(r.get("MsUntilDisplayed")),
                    "tearing": r.get("AllowsTearing"),
                }
            )
            key = f"{r.get('PresentMode')} (tearing {r.get('AllowsTearing')})"
            modes[key] = modes.get(key, 0) + 1
        print(f"=== {path}")
        if not rows:
            print("  no frames")
            continue
        print("  present mode: " + "; ".join(f"{k} = {v}" for k, v in sorted(modes.items(), key=lambda x: -x[1])))
        report(rows, "whole run")

        if args.segments > 0:
            start, index = rows[0]["t"], 0
            while True:
                lo = start + index * args.segments * 1000
                hi = lo + args.segments * 1000
                part = [r for r in rows if lo + 1000 <= r["t"] < hi - 1000]  # 1 s margin around the beeps
                if not part:
                    break
                report(part, f"part {index + 1} ({index * args.segments:.0f}-{(index + 1) * args.segments:.0f} s)")
                index += 1

        worst = sorted((r for r in rows if r["present"] is not None), key=lambda r: -r["present"])[: args.worst]
        print("  longest frames: " + ", ".join(f"{r['present']:.1f} ms at {(r['t'] - rows[0]['t']) / 1000:.1f} s" for r in worst))
    return 0


if __name__ == "__main__":
    sys.exit(main())
