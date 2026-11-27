"""Builds src/area_doors.h, the doors between whole areas, from the game's own pack0.dzip.

[loading] preload_areas loads the small areas behind an area's doors with that area. The DLL
learns those doors when an area has loaded (pa_learn); this table lets the very first load of
an area preload as well. See docs/fixes/interior-preload.md, "The shipped door table".

Every CStreamingDoor in every .w2l under a world's folder names its two partitions
(`inside`/`outside`, SDoorPartition.worldPartition). A pair goes into the table, in both
directions, when the DLL would learn it: both partitions are in the world's partition tree, in
the same folder (ko_same_folder), and neither is a house or a small place (kx_small: an
interior environment group, or at most 8 groups of its own).

Usage: python tools\\build-area-doors.py "<game>\\CookedPC\\pack0.dzip" [--check]
--check compares the generated text with src/area_doors.h and exits 1 when they differ.
Only pack0.dzip holds worlds and doors (checked 2026-11-12 against every archive of the
Steam install, DLC and patches included).
"""
import collections
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "src", "area_doors.h")
HOUSE_GROUPS = 8   # KX_HOUSE_GROUPS
NAME_MAX = 127     # PA_NAME - 1

_spec = importlib.util.spec_from_file_location("bim", os.path.join(HERE, "build-interior-maps.py"))
bim = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bim)


# ---- CR2W -----------------------------------------------------------------------------------
def packed_int(d, o):
    """CR2W packed int: bit 7 sign, bit 6 more, 6 bits; then 7 bits a byte, bit 7 more."""
    b = d[o]
    o += 1
    v = b & 0x3F
    sh = 6
    more = b & 0x40
    while more:
        c = d[o]
        o += 1
        v |= (c & 0x7F) << sh
        sh += 7
        more = c & 0x80
    return (-v if b & 0x80 else v), o


def estr(d, o):
    """Encoded string: bit 7 of the first byte = 8-bit text, else UTF-16; length as above."""
    b = d[o]
    o += 1
    n = b & 0x3F
    sh = 6
    more = b & 0x40
    while more:
        c = d[o]
        o += 1
        n |= (c & 0x7F) << sh
        sh += 7
        more = c & 0x80
    if b & 0x80:
        return d[o:o + n].decode("latin1"), o + n
    return d[o:o + 2 * n].decode("utf-16-le", "replace"), o + 2 * n


def cr2w(d):
    magic, _ver, _flags, no, nc, oo, oc = struct.unpack_from("<4s6I", d, 0)
    if magic != b"CR2W":
        raise ValueError("not CR2W")
    names = []
    o = no
    for _ in range(nc):
        s, o = estr(d, o)
        names.append(s)
    objs = []
    o = oo
    for _ in range(oc):
        t, _parent, size, off, _a, _b = struct.unpack_from("<HIIIII", d, o)
        o += 22
        _tpl, o = estr(d, o)
        objs.append((names[t - 1] if 0 < t <= len(names) else "?", off, size))
    return names, objs


def props(d, names, o, end):
    """A property list: u16 name, u16 type, u16 0xFFFF, u32 size (with itself), value; u16 0 ends it.
    Returns ({name: (type, value offset, value end)}, offset after the list)."""
    out = {}
    while o + 2 <= end:
        nm, = struct.unpack_from("<H", d, o)
        if nm == 0:
            return out, o + 2
        ty, _ff, sz = struct.unpack_from("<HHI", d, o + 2)
        out[names[nm - 1]] = (names[ty - 1], o + 10, o + 6 + sz)
        o += 6 + sz
    raise ValueError("property list runs past its object")


# ---- partition tree (CWorldPartitions, embedded in the .w2w) --------------------------------
def partitions(d):
    """{full name below the root, lower case: (name as written, [own groups])}."""
    names, objs = cr2w(d)
    found = [(off, size) for t, off, size in objs if t == "CWorldPartitions"]
    if len(found) != 1:
        raise ValueError("%d CWorldPartitions objects" % len(found))
    off, size = found[0]
    end = off + size
    tree = {}

    def node(o, prefix):
        p, o = props(d, names, o, end)
        name, _ = estr(d, p["name"][1])
        groups = []
        if "layerGroups" in p:
            ty, v, vend = p["layerGroups"]
            # u32 count, u16 element type, u16 0xFFFF, then the encoded strings
            cnt, = struct.unpack_from("<I", d, v)
            q = v + 8
            for _ in range(cnt):
                g, q = estr(d, q)
                groups.append(g)
            if q != vend:
                raise ValueError("layerGroups of %s: %d bytes left" % (name, vend - q))
        full = name if prefix is None else (prefix + "\\" + name if prefix else name)
        if prefix is not None:
            tree[full.lower()] = (full, groups)
        kids, o = packed_int(d, o)
        for _ in range(kids):
            o = node(o, full if prefix is not None else "")
        return o

    # Three bytes, then the root node (`Root`, not part of any partition name).
    o = node(off + 3, None)
    if o > end:
        raise ValueError("partition tree runs past its object")
    return tree


def env_interior(g):
    """ko_env_interior: `interior` after an `environments\\` folder."""
    s = g.lower()
    i = s.find("environments\\")
    while i >= 0:
        if (i == 0 or s[i - 1] == "\\") and "interior" in s[i + 13:]:
            return True
        i = s.find("environments\\", i + 1)
    return False


def small(groups):
    """kx_small: a house or a small place."""
    return len(groups) <= HOUSE_GROUPS or any(env_interior(g) for g in groups)


def folder(name):
    i = name.rfind("\\")
    return name[:i].lower() if i >= 0 else ""


# ---- doors ----------------------------------------------------------------------------------
def doors(d):
    """[(inside, outside)] of every CStreamingDoor in a layer."""
    if b"CStreamingDoor" not in d:
        return []
    names, objs = cr2w(d)
    out = []
    for t, off, size in objs:
        if t != "CStreamingDoor":
            continue
        p, _ = props(d, names, off, off + size)
        side = []
        for key in ("inside", "outside"):
            if key not in p:
                side.append("")
                continue
            _ty, v, vend = p[key]
            sp, _ = props(d, names, v, vend)
            if "worldPartition" in sp:
                s, _ = estr(d, sp["worldPartition"][1])
                side.append(s)
            else:
                side.append("")
        out.append(tuple(side))
    return out


def c_wide(s):
    if not s.isascii() or '"' in s:
        raise ValueError("unexpected partition name %r" % s)
    return 'L"' + s.replace("\\", "\\\\") + '"'


def build(pack):
    idx = bim.dzip_index(pack)
    worlds = sorted(n for n in idx if n.startswith("levels\\") and n.endswith(".w2w"))
    rows = []
    stats = collections.Counter()
    report = []
    trees = {}
    for w in worlds:
        base = w.rsplit("\\", 1)[0] + "\\"
        tree = trees[w] = partitions(bim.dzip_read(pack, idx, w))
        seen = set()
        n_doors = 0
        for n in sorted(idx):
            if not (n.startswith(base) and n.endswith(".w2l")):
                continue
            for a, b in doors(bim.dzip_read(pack, idx, n)):
                n_doors += 1
                if a and b and a.lower() != b.lower():
                    seen.add(tuple(sorted((a, b), key=str.lower)))
        stats["doors"] += n_doors
        stats["pairs"] += len(seen)
        kept = 0
        for a, b in sorted(seen, key=lambda p: (p[0].lower(), p[1].lower())):
            ta, tb = tree.get(a.lower()), tree.get(b.lower())
            if not ta or not tb:
                stats["not in tree"] += 1
                report.append("  %s: not in the partition tree: %s <-> %s" % (w, a, b))
                continue
            if folder(ta[0]) != folder(tb[0]):
                stats["other folder"] += 1
                continue
            if small(ta[1]) or small(tb[1]):
                stats["house"] += 1
                continue
            if len(ta[0]) > NAME_MAX or len(tb[0]) > NAME_MAX:
                raise ValueError("name longer than PA_NAME: %s / %s" % (ta[0], tb[0]))
            kept += 1
            rows.append((w, ta[0], tb[0], len(ta[1]), len(tb[1])))
            rows.append((w, tb[0], ta[0], len(tb[1]), len(ta[1])))
        report.append("%s: %d doors, %d partition pairs, %d between whole areas"
                      % (w, n_doors, len(seen), kept))
    # The DLL matches names without the world (pa_plan finds them in the world it is in), so a
    # name of the table must name no partition of another world.
    for w, a, _b, _ga, _gb in rows:
        other = [o for o in worlds if o != w and a.lower() in trees[o]]
        if other:
            raise ValueError("%s of %s is also a partition of %s" % (a, w, ", ".join(other)))
    stats["area"] = len(rows) // 2
    return rows, stats, report


def render(rows):
    lines = [
        "// Generated by tools/build-area-doors.py from the game's pack0.dzip - do not edit.",
        "// The doors between two whole areas of one folder, both directions: what pa_learn learns",
        "// after an area load, known before the first one ([loading] preload_areas).",
        "// Partition names as the game writes them (CStreamingDoor inside/outside); see",
        "// docs/fixes/interior-preload.md, \"The shipped door table\".",
        "#pragma once",
        "",
        "static const struct { const wchar_t* from; const wchar_t* to; } k_areaDoors[] = {",
    ]
    world = None
    for w, a, b, ga, gb in rows:
        if w != world:
            lines.append("    // %s" % w.replace("\\", "/"))
            world = w
        lines.append("    { %s, %s },   // %d -> %d groups" % (c_wide(a), c_wide(b), ga, gb))
    lines.append("};")
    return "\n".join(lines) + "\n"


def main():
    args = [a for a in sys.argv[1:] if a != "--check"]
    if len(args) != 1:
        sys.exit(__doc__)
    rows, stats, report = build(args[0])
    print("\n".join(report))
    print("%(doors)d doors, %(pairs)d partition pairs, %(area)d between whole areas of one folder "
          "(left out: %(house)d with a house or small place, %(other folder)d across folders, "
          "%(not in tree)d not in the tree)" % collections.defaultdict(int, stats))
    text = render(rows)
    if "--check" in sys.argv:
        with open(OUT, "r", encoding="ascii", newline="") as f:
            same = f.read().replace("\r\n", "\n") == text
        print("src/area_doors.h is %s" % ("up to date" if same else "OUT OF DATE"))
        sys.exit(0 if same else 1)
    with open(OUT, "w", encoding="ascii", newline="\r\n") as f:
        f.write(text)
    print("wrote %s (%d rows)" % (OUT, len(rows)))


if __name__ == "__main__":
    main()
