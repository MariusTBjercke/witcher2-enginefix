"""Finds candidate GOG addresses for the Steam code addresses in docs/addresses.md.

For each Steam code address, N bytes from the unpacked Steam exe are turned into a pattern:
4-byte values that look like absolute addresses (0x00400000..0x027FFFFF), and the rel32 of
E8/E9 and 0F 8x, are wildcards. The pattern is searched in the GOG exe's .text. A unique hit is
a candidate only; check it in Ghidra before using it (see docs/fixes/gog-build.md). Import slots
and data/vtable addresses are listed but not searched: imports are resolved by name, data and
vtables follow from the ported code.

usage: python -I tools/port-probe.py <steam unpacked exe> <gog exe> [docs/addresses.md] [-n 32]
"""
import re
import struct
import sys


def load_pe(path):
    d = open(path, "rb").read()
    e = struct.unpack_from("<I", d, 0x3C)[0]
    n = struct.unpack_from("<H", d, e + 6)[0]
    so = struct.unpack_from("<H", d, e + 20)[0]
    base = struct.unpack_from("<I", d, e + 24 + 28)[0]
    secs = []
    for i in range(n):
        o = e + 24 + so + 40 * i
        name = d[o:o + 8].rstrip(b"\0").decode()
        vs, va, rs, rp = struct.unpack_from("<IIII", d, o + 8)
        secs.append((name, va, vs, rs, rp))
    return d, base, secs


def section_of(pe, va):
    _, base, secs = pe
    r = va - base
    for name, sva, vs, rs, rp in secs:
        if sva <= r < sva + max(vs, rs):
            return name, sva, rs, rp
    return None, 0, 0, 0


def pattern(pe, va, n):
    d = pe[0]
    name, sva, rs, rp = section_of(pe, va)
    if name is None:
        return None
    off = rp + (va - pe[1]) - sva
    b = d[off:off + n]
    mask = [False] * len(b)
    for i in range(len(b) - 3):
        if 0x00400000 <= struct.unpack_from("<I", b, i)[0] < 0x02800000:
            for k in range(4):
                mask[i + k] = True
    for i in range(len(b)):
        if b[i] in (0xE8, 0xE9) and i + 4 < len(b):
            for k in range(1, 5):
                mask[i + k] = True
        if b[i] == 0x0F and i + 5 < len(b) and 0x80 <= b[i + 1] <= 0x8F:
            for k in range(2, 6):
                mask[i + k] = True
    rx = b"".join(b"." if m else re.escape(bytes([c])) for c, m in zip(b, mask))
    return re.compile(rx, re.DOTALL), sum(mask)


def main():
    args = [a for a in sys.argv[1:]]
    n = 32
    if "-n" in args:
        i = args.index("-n")
        n = int(args[i + 1])
        del args[i:i + 2]
    if len(args) < 2:
        sys.exit(__doc__)
    steam, gog = load_pe(args[0]), load_pe(args[1])
    table = args[2] if len(args) > 2 else "docs/addresses.md"
    name, tva, trs, trp = section_of(gog, gog[1] + [s for s in gog[2] if s[0] == ".text"][0][1])
    stats = {"unique": 0, "several": 0, "none": 0, "import": 0, "data": 0}
    for line in open(table, encoding="utf-8"):
        m = re.match(r"\| `(\w+)` \| `([0-9A-F]{8})` \|", line)
        if not m:
            continue
        key, v = m.group(1), int(m.group(2), 16)
        if key.startswith("IAT_"):
            stats["import"] += 1
            continue
        va = v + 0x400000 if key.startswith(("EF_", "RVA_")) else v
        sec = section_of(steam, va)[0]
        if sec != ".text":
            print(f"{key:24s} {va:08X} {sec or '-'}: data or vtable, port from the code that uses it")
            stats["data"] += 1
            continue
        rx, masked = pattern(steam, va, n)
        hits = [h.start() for h in rx.finditer(gog[0], trp, trp + trs)]
        if len(hits) == 1:
            gva = gog[1] + tva + hits[0] - trp
            print(f"{key:24s} {va:08X} -> GOG {gva:08X} candidate ({masked}/{n} bytes masked)")
            stats["unique"] += 1
        elif hits:
            print(f"{key:24s} {va:08X} -> {len(hits)} hits")
            stats["several"] += 1
        else:
            print(f"{key:24s} {va:08X} -> no hit")
            stats["none"] += 1
    print(stats)


main()
