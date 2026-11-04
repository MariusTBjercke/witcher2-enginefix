#!/usr/bin/env python3
"""Read a bootprofile.exe capture: what the main thread did, second by second.

  python -I tools/bootprofile/report.py bootprofile.txt [--ghidra] [--program /witcher2/witcher2.exe.unpacked.exe]
                                        [--from MS] [--to MS] [--top N] [--step MS] [--children ADDR]

Addresses inside the game become function names through the Ghidra bridge (AGENTS.md, "Research
setup") with --ghidra, and stay plain addresses without it. "self" counts the innermost game
function on the stack; "incl" counts each function once per sample, wherever it is on the stack.
Stack entries are found by scanning, not unwinding: an old return address that happens to look
like a call can add a little to "incl", so read small numbers with care.
--children ADDR splits the time of the function holding ADDR by the return address inside it,
which is how a long init function shows which of its calls takes the time.
"""
import argparse
import bisect
import collections
import json
import re
import sys
import urllib.parse
import urllib.request

BRIDGE = "http://127.0.0.1:8089/"


class Syms:
    def __init__(self, mods, use_ghidra, program):
        self.exe = mods[0] if mods else None   # EnumProcessModules lists the exe first
        self.mods = sorted(mods)
        self.bases = [m[0] for m in self.mods]
        self.use_ghidra = use_ghidra
        self.program = program
        self.cache = {}

    def module(self, a):
        i = bisect.bisect_right(self.bases, a) - 1
        if i >= 0 and a < self.mods[i][0] + self.mods[i][1]:
            return self.mods[i]
        return None

    def in_exe(self, a):
        return self.exe is not None and self.exe[0] <= a < self.exe[0] + self.exe[1]

    def func(self, a):
        """(start address or None, name) of the function holding a."""
        m = self.module(a)
        if m is None:
            return (None, "%08X" % a)
        if not self.in_exe(a):
            return (m[0], m[2])
        if a in self.cache:
            return self.cache[a]
        r = (a, "%08X" % a)
        if self.use_ghidra:
            q = urllib.parse.urlencode({"address": "%08x" % a, "program": self.program})
            try:
                with urllib.request.urlopen(BRIDGE + "get_function_by_address?" + q, timeout=30) as f:
                    t = f.read().decode("utf-8", "replace")
                r = parse_function(t) or r
            except OSError:
                pass
        self.cache[a] = r
        return r


def parse_function(t):
    try:
        d = json.loads(t)
        name, entry = d.get("name"), d.get("entry_point") or d.get("entry") or d.get("address")
        if name and entry:
            return (int(str(entry), 16), name)
    except ValueError:
        pass
    m = re.search(r"Function:\s*(\S+)\s+at\s+([0-9a-fA-F]+)", t)
    if m:
        return (int(m.group(2), 16), m.group(1))
    return None


def load(path):
    head, mods, samples, cpu, window = [], [], [], [], None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#"):
                head.append(line.strip())
            elif line.startswith("M "):
                _, b, s, n = line.rstrip("\n").split(" ", 3)
                mods.append((int(b, 16), int(s, 16), n))
            elif line.startswith("W "):
                window = int(line.split()[1])
            elif line.startswith("C "):
                cpu.append(tuple(int(x) for x in line.split()[1:4]))
            elif line.startswith("S "):
                p = line.split()
                samples.append((int(p[1]), int(p[2]), int(p[3]), [int(x, 16) for x in p[4:]]))
    return head, mods, samples, cpu, window


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--ghidra", action="store_true")
    ap.add_argument("--program", default="/witcher2/witcher2.exe.unpacked.exe")
    ap.add_argument("--from", dest="t0", type=int, default=0)
    ap.add_argument("--to", dest="t1", type=int, default=10**9)
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--step", type=int, default=1000)
    ap.add_argument("--children", default=None)
    a = ap.parse_args()

    head, mods, samples, cpu, window = load(a.capture)
    for h in head:
        print(h)
    m = re.search(r"main_tid (\d+)", " ".join(head))
    main_tid = int(m.group(1)) if m else None
    print("game window first seen at +%s ms" % window if window else "game window not seen")
    syms = Syms(mods, a.ghidra, a.program)

    main_s = [s for s in samples if s[1] == main_tid and a.t0 <= s[0] <= a.t1]
    if not main_s:
        sys.exit("no main thread samples in that range")
    print("main thread: %d samples, +%d to +%d ms\n" % (len(main_s), main_s[0][0], main_s[-1][0]))

    if cpu:
        print("%8s %10s %10s" % ("ms", "main cpu", "process"))
        last = None
        for t, mc, pc in cpu:
            if last is None or t - last[0] >= a.step:
                if last is not None:
                    print("%8d %9.0f%% %9.0f%%" % (t, 100.0 * (mc - last[1]) / (t - last[0]), 100.0 * (pc - last[2]) / (t - last[0])))
                last = (t, mc, pc)
        print()

    self_t, incl_t = collections.Counter(), collections.Counter()
    names = {}
    timeline = collections.defaultdict(collections.Counter)
    weight = {}
    prev = None
    for s in main_s:
        dt = (s[0] - prev) if prev is not None else 0
        prev = s[0]
        weight[id(s)] = dt
        seen, first = set(), None
        for addr in s[3]:
            fa, name = syms.func(addr)
            key = fa if fa is not None else addr
            names[key] = name
            if first is None and syms.in_exe(addr):
                first = key
            if key not in seen:
                seen.add(key)
                incl_t[key] += dt
        inner = syms.func(s[3][0])
        ikey = inner[0] if inner[0] is not None else s[3][0]
        names[ikey] = inner[1]
        self_t[ikey] += dt
        if first is not None:
            timeline[s[0] // a.step][first] += dt

    total = sum(weight.values()) or 1
    print("timeline, every %d ms: innermost game function" % a.step)
    for b in sorted(timeline):
        parts = ["%s %d%%" % (names[k], 100 * v / a.step) for k, v in timeline[b].most_common(3)]
        print("%8d  %s" % (b * a.step, ", ".join(parts)))

    print("\nself (innermost address), of %d ms" % total)
    for k, v in self_t.most_common(a.top):
        print("%7d ms %5.1f%%  %s" % (v, 100.0 * v / total, names[k]))
    print("\ninclusive")
    for k, v in incl_t.most_common(a.top):
        print("%7d ms %5.1f%%  %s" % (v, 100.0 * v / total, names[k]))

    if a.children:
        target = syms.func(int(a.children, 16))[0]
        kids = collections.Counter()
        prev = None
        for s in main_s:
            dt = (s[0] - prev) if prev is not None else 0
            prev = s[0]
            for addr in s[3][1:]:
                if syms.in_exe(addr) and syms.func(addr)[0] == target:
                    kids[addr] += dt
                    break   # the innermost frame of that function is the live call
        print("\ncall sites inside %s" % names.get(target, a.children))
        for k, v in sorted(kids.items()):
            print("%7d ms %5.1f%%  return address %08X" % (v, 100.0 * v / total, k))


if __name__ == "__main__":
    main()
