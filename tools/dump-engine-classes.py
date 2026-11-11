#!/usr/bin/env python3
"""Dump the engine's class map from the unpacked Witcher 2 executable.

The exe is built with MSVC RTTI (/GR), so every class used polymorphically leaves
a TypeDescriptor (".?AV...") in .rdata, a CompleteObjectLocator, and a vtable that
points back at it. This script parses those structures and writes

    tools/engine-classes.json   machine-readable inventory
    docs/engine-classes.md      generated human index

Nothing is guessed: every row is a structure read out of the file. What the file
cannot tell us - parameter types, calling conventions, what a function does - is
out of scope on purpose. Proved addresses we call live in docs/addresses.md and
tools/address-table.py; this is the map of what exists, not a claim about how to
call it.

Usage, from the repo root:

    python tools/dump-engine-classes.py             # write both files
    python tools/dump-engine-classes.py --check     # verify the anchors, no writes

The exe is git-ignored: unpack a copy of the game's exe first (AGENTS.md,
"Research setup"). An --exe path overrides the default.
"""

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

IMAGE_BASE_EXPECTED = 0x400000
DEFAULT_EXE = Path("tools/ghidra-inputs/witcher2.exe.unpacked.exe")
DEFAULT_JSON = Path("tools/engine-classes.json")
DEFAULT_MD = Path("docs/engine-classes.md")

# Anchors already proved in docs/addresses.md (Steam 3.5.0.26, unpacked). If the
# parser gets these wrong, every row below is suspect, so stop instead of writing.
ANCHOR_CLASS = "CGameDataStorage"
ANCHOR_VTABLE = 0x00E7E264
ANCHOR_SLOT0 = 0x00421438  # VA_GDS_DTOR, vtable slot 0

# Classes the project already touches or names in docs/; listed first in the
# markdown so the reader does not have to search for them.
INTERESTING = [
    "CGame", "CWorld", "CLayer", "CNode", "CEntity", "CGameplayEntity", "CActor",
    "CPlayer", "CDoor", "CGameDataStorage", "CInventoryComponent", "CQuest",
    "CEncounter", "CCommunity", "CStoryScene", "CScriptingSystem", "CScriptedClass",
    "CScriptOperator", "CScriptDebugger", "CSoundSystem", "CDepot",
]

MAX_RTTI_NAME = 240
MAX_BASES = 64
MAX_VTABLES = 16
MAX_SLOTS = 512
SCN_MEM_EXECUTE = 0x20000000

# MSVC mangles a class declared inside a function as "Name@?1??Function@Class@@..."
# and one in an anonymous namespace as "Name@?A0x<hash>". The first is not an
# engine class; the second is, and only the hash suffix is noise.
LOCAL_CLASS_RE = re.compile(r"@\?\d")
ANON_MARK = "@?A0x"


# --------------------------------------------------------------------------- PE


class Section:
    __slots__ = ("name", "va", "vsize", "raw_ptr", "raw_size", "flags")

    def __init__(self, name, va, vsize, raw_ptr, raw_size, flags):
        self.name = name
        self.va = va
        self.vsize = vsize
        self.raw_ptr = raw_ptr
        self.raw_size = raw_size
        self.flags = flags

    @property
    def executable(self):
        return bool(self.flags & SCN_MEM_EXECUTE)

    def holds(self, rva):
        return self.va <= rva < self.va + self.vsize

    def file_offset(self, rva, size=1):
        """File offset for `size` bytes at `rva`, or None when not backed by the file."""
        if not self.holds(rva):
            return None
        delta = rva - self.va
        if delta + size > self.raw_size:
            return None  # BSS tail of .data: present in memory, not in the file
        return self.raw_ptr + delta


class PE:
    def __init__(self, data):
        self.data = data
        if data[:2] != b"MZ":
            raise ValueError("not an MZ file")
        nt = struct.unpack_from("<I", data, 0x3C)[0]
        if data[nt:nt + 4] != b"PE\0\0":
            raise ValueError("no PE signature")
        coff = nt + 4
        self.machine, self.nsec = struct.unpack_from("<HH", data, coff)
        opt_size = struct.unpack_from("<H", data, coff + 16)[0]
        opt = coff + 20
        magic = struct.unpack_from("<H", data, opt)[0]
        if magic != 0x10B:
            raise ValueError("not a PE32 image (magic %#x)" % magic)
        self.image_base = struct.unpack_from("<I", data, opt + 28)[0]
        table = opt + opt_size
        self.sections = []
        for i in range(self.nsec):
            off = table + i * 40
            name = data[off:off + 8].rstrip(b"\0").decode("latin1")
            vsize, va, raw_size, raw_ptr = struct.unpack_from("<IIII", data, off + 8)
            # Characteristics sits at +36; +24 is PointerToRelocations.
            flags = struct.unpack_from("<I", data, off + 36)[0]
            self.sections.append(Section(name, va, vsize, raw_ptr, raw_size, flags))

    def section_for(self, rva):
        for s in self.sections:
            if s.holds(rva):
                return s
        return None

    def read_u32(self, rva):
        s = self.section_for(rva)
        if s is None:
            return None
        off = s.file_offset(rva, 4)
        if off is None:
            return None
        return struct.unpack_from("<I", self.data, off)[0]

    def is_va(self, va):
        """True when `va` looks like a pointer into a file-backed data section."""
        s = self.section_for(va - self.image_base)
        return s is not None and s.file_offset(va - self.image_base, 4) is not None

    def in_executable(self, va):
        s = self.section_for(va - self.image_base)
        return s is not None and s.executable

    def type_name(self, rva):
        """NUL-terminated RTTI name at `rva`, or None."""
        s = self.section_for(rva)
        if s is None:
            return None
        off = s.file_offset(rva, 1)
        if off is None:
            return None
        end = off
        limit = min(off + MAX_RTTI_NAME, s.raw_ptr + s.raw_size)
        while end < limit and self.data[end]:
            end += 1
        if end == off or end >= limit:
            return None
        return self.data[off:end].decode("latin1")


# ------------------------------------------------------------------------- RTTI


def rva(pe, va):
    return va - pe.image_base


def scan_type_descriptors(pe):
    """Map name VA -> name for every RTTI TypeDescriptor in the file."""
    found = {}
    data = pe.data
    for s in pe.sections:
        if s.executable or s.raw_size == 0:
            continue
        blob = data[s.raw_ptr:s.raw_ptr + s.raw_size]
        pos = 0
        while True:
            pos = blob.find(b".?AV", pos)
            if pos < 0:
                break
            name = pe.type_name(s.va + pos)
            pos += 4
            if not name or not name.startswith(".?AV"):
                continue
            # The descriptor starts 8 bytes before its name and its first field
            # points at the type_info vftable, which lives in a data section.
            td_rva = s.va + (pos - 4) - 8
            if pe.read_u32(td_rva) is None or not pe.is_va(pe.read_u32(td_rva)):
                continue
            tail = strip_rtti(name)
            found[td_rva + pe.image_base] = tail
    return found


def strip_rtti(name):
    """'.?AVCActor@@' -> 'CActor'."""
    if name.startswith(".?AV"):
        name = name[4:]
    if name.endswith("@@"):
        name = name[:-2]
    return name


def display_name(raw_tail):
    """Readable name for a mangled tail; anonymous-namespace classes lose the hash."""
    if ANON_MARK in raw_tail:
        return raw_tail.split(ANON_MARK)[0]
    return demangle_name(raw_tail)


def kind_of(name):
    """Separate engine classes from the scaffolding the code generator emits."""
    if name.endswith("ClassBuilder"):
        return "class-builder"
    if "<" in name:
        return "template"
    return "class"


def demangle_name(name):
    """Readable form of a mangled tail such as "?$TTypedClass@VCActor@@@@"."""
    if not name.startswith("?"):
        return name
    if name.startswith("?$"):
        head, _, args = name[2:].partition("@")
        inner = args.rstrip("@")
        inner = inner[1:] if inner.startswith(("V", "U")) else inner
        return f"{head}<{inner}>"
    return name


def scan_hierarchies(pe, descriptors):
    """Find CompleteObjectLocators and, from them, class hierarchies and vtables."""
    data = pe.data
    locators = {}  # locator VA -> (type descriptor VA, hierarchy VA)

    for s in pe.sections:
        if s.executable or s.raw_size < 24:
            continue
        blob = data[s.raw_ptr:s.raw_ptr + s.raw_size]
        for off in range(0, len(blob) - 4, 4):
            td_va = struct.unpack_from("<I", blob, off)[0]
            if td_va not in descriptors:
                continue
            # pTypeDescriptor sits at +12 in the locator, so the locator starts 12
            # bytes earlier.
            col_va = s.va + off - 12
            if col_va < 0:
                continue
            if pe.read_u32(col_va) != 0:  # reserved signature
                continue
            hd_va = pe.read_u32(col_va + 16)
            if hd_va is None or not pe.is_va(hd_va):
                continue
            hd_rva = rva(pe, hd_va)
            if pe.read_u32(hd_rva) != 0:
                continue
            nbase = pe.read_u32(hd_rva + 8)
            array_va = pe.read_u32(hd_rva + 12)
            if nbase is None or not 1 <= nbase <= MAX_BASES:
                continue
            if array_va is None or not pe.is_va(array_va):
                continue
            locators[col_va + pe.image_base] = (td_va, hd_va)

    # A vtable's slot -1 holds the pointer to its locator.
    vtables = {}
    col_set = set(locators)
    for s in pe.sections:
        if s.executable or s.raw_size < 4:
            continue
        blob = data[s.raw_ptr:s.raw_ptr + s.raw_size]
        for off in range(0, len(blob) - 4, 4):
            value = struct.unpack_from("<I", blob, off)[0]
            if value in col_set:
                vtables.setdefault(pe.image_base + s.va + off + 4, value)
    return locators, vtables


def class_hierarchy(pe, hd_va):
    """Ordered base class names: index 0 is the class itself, the rest are bases."""
    hd_rva = rva(pe, hd_va)
    nbase = pe.read_u32(hd_rva + 8) or 0
    array_va = pe.read_u32(hd_rva + 12)
    names = []
    if array_va is None or not pe.is_va(array_va):
        return names
    for i in range(nbase):
        bcd_va = pe.read_u32(rva(pe, array_va) + i * 4)
        if bcd_va is None:
            break
        td_va = pe.read_u32(rva(pe, bcd_va))
        if td_va is None:
            break
        name = descriptor_name(pe, td_va)
        if name is None:
            break
        names.append(display_name(strip_rtti(name)))
    return names


def descriptor_name(pe, td_va):
    return pe.type_name(rva(pe, td_va) + 8)


def vtable_slots(pe, vt_va):
    """Count the leading run of pointers into executable sections (estimate)."""
    count = 0
    while count < MAX_SLOTS:
        value = pe.read_u32(rva(pe, vt_va + count * 4))
        if value is None or not pe.in_executable(value):
            break
        count += 1
    return count


# ------------------------------------------------------------------------ build


def build(pe, data_sha):
    descriptors = scan_type_descriptors(pe)
    if not descriptors:
        raise SystemExit("no RTTI type descriptors found - wrong binary?")
    # Function-local classes (".?AVCBase@?1??PureVirtualTest@...") are real RTTI
    # entries but not engine classes; they are counted, not listed.
    local_types = [n for n in descriptors.values() if LOCAL_CLASS_RE.search(n)]
    class_desc = {va: n for va, n in descriptors.items() if not LOCAL_CLASS_RE.search(n)}
    locators, vtables = scan_hierarchies(pe, class_desc)

    def short(raw):
        """Display name: anonymous-namespace classes drop the ?A0x<hash> suffix."""
        return raw.split(ANON_MARK)[0] if ANON_MARK in raw else demangle_name(raw)

    name_set = {short(n) for n in class_desc.values()}
    builders = {n[:-len("ClassBuilder")] for n in name_set if n.endswith("ClassBuilder")}
    typed = set()
    for n in class_desc.values():
        if n.startswith("?$TTypedClass@V"):
            typed.add(n[len("?$TTypedClass@V"):].split("@", 1)[0])

    by_raw = {}
    for td_va, raw_name in class_desc.items():
        entry = by_raw.get(raw_name)
        if entry is None:
            # Same class, several descriptors (multi-inheritance / reuse): keep one
            # entry and remember the extra descriptor instead of dropping it.
            entry = by_raw[raw_name] = {
                "name": short(raw_name),
                "rtti_name": demangle_name(raw_name),
                "kind": kind_of(short(raw_name)),
                "anon_namespace": ANON_MARK in raw_name,
                "type_info": [],
                "vtables": [],
                "bases": [],
                "script_class": short(raw_name) in builders,
                "typed": short(raw_name) in typed,
            }
        entry["type_info"].append(f"{td_va:08X}")

    for vt_va, col_va in sorted(vtables.items()):
        td_va, hd_va = locators[col_va]
        entry = by_raw.get(class_desc[td_va])
        if entry is None:
            continue
        if len(entry["vtables"]) >= MAX_VTABLES:
            continue
        slots = vtable_slots(pe, vt_va)
        if slots == 0:
            continue  # a pointer that merely equals a locator, not a real vtable
        entry["vtables"].append({"va": f"{vt_va:08X}", "slots": slots})
        if not entry["bases"]:
            chain = class_hierarchy(pe, hd_va)
            if len(chain) > 1:
                entry["bases"] = chain[1:]

    for entry in by_raw.values():
        entry["vtables"].sort(key=lambda v: v["va"])

    classes = sorted(by_raw.values(), key=lambda c: c["name"].lower())
    # Two anonymous-namespace classes can share a short name; keep them apart with
    # the mangled name rather than merging two different classes.
    seen = {}
    for c in classes:
        seen.setdefault(c["name"], []).append(c)
    for group in (g for g in seen.values() if len(g) > 1):
        for c in group:
            c["name"] = c["rtti_name"]
    classes.sort(key=lambda c: c["name"].lower())

    script_classes = sum(1 for c in classes if c["script_class"])
    typed_classes = sum(1 for c in classes if c["typed"])
    kinds = {k: sum(1 for c in classes if c["kind"] == k)
             for k in ("class", "class-builder", "template")}
    return {
        "schema": 1,
        "source": {
            "file": "witcher2.exe.unpacked.exe",
            "sha256": data_sha,
            "image_base": f"{pe.image_base:08X}",
            "note": "RTTI only; no names are invented. See docs/addresses.md for proved calls.",
        },
        "counts": {
            "classes": len(classes),
            "engine_classes": kinds["class"],
            "class_builders": kinds["class-builder"],
            "templates": kinds["template"],
            "with_vtable": sum(1 for c in classes if c["vtables"]),
            "builders": len(builders),
            "script_classes": script_classes,
            "script_without_class": len(builders) - script_classes,
            "typed_classes": typed_classes,
            "typed_without_class": len(typed) - typed_classes,
            "type_descriptors": len(descriptors),
            "function_local_types": len(local_types),
            "vtables": sum(len(c["vtables"]) for c in classes),
        },
        "classes": classes,
    }


# ----------------------------------------------------------------------- anchors


def check_anchors(report):
    """Verify the parser against addresses already proved in docs/addresses.md."""
    by_name = {c["name"]: c for c in report["classes"]}
    problems = []
    anchor = by_name.get(ANCHOR_CLASS)
    if anchor is None:
        problems.append(f"{ANCHOR_CLASS} missing from the class map")
        return problems
    vts = [v["va"] for v in anchor["vtables"]]
    if f"{ANCHOR_VTABLE:08X}" not in vts:
        problems.append(
            f"{ANCHOR_CLASS} vtables {vts or '(none)'} do not include "
            f"the proved {ANCHOR_VTABLE:08X}"
        )
    return problems


# ---------------------------------------------------------------------- markdown


def fmt_vtables(entry):
    if not entry["vtables"]:
        return "-"
    return ", ".join(f"`{v['va']}` ({v['slots']})" for v in entry["vtables"])


def markdown(report, anchor_slot0):
    c = report["counts"]
    lines = [
        "# Engine class map",
        "",
        "Generated by `tools/dump-engine-classes.py` from the unpacked Steam 3.5.0.26",
        "executable - **do not edit**. Regenerate after unpacking a new build; the",
        "GOG build is not covered yet.",
        "",
        "Every row is an MSVC RTTI structure read out of the file: the class name, its",
        "base classes and the address of its vtable. Nothing here is a guess, and",
        "nothing here says how to call a function - calling conventions, argument",
        "counts and lifetimes are proved per function in `docs/addresses.md`.",
        "",
        f"Source: `{report['source']['file']}`, SHA-256",
        f"`{report['source']['sha256']}`.",
        "",
        "## Counts",
        "",
        "| | |",
        "|---|---|",
        f"| Types with RTTI | {c['classes']} |",
        f"| ... engine classes | {c['engine_classes']} |",
        f"| ... `*ClassBuilder` scaffolding | {c['class_builders']} |",
        f"| ... template instantiations | {c['templates']} |",
        f"| With a vtable | {c['with_vtable']} |",
        f"| Vtables found | {c['vtables']} |",
        f"| Script-visible classes (`ClassBuilder` registrations) | {c['builders']} |",
        f"| ... matching an engine class above | {c['script_classes']} |",
        f"| ... with no RTTI of their own (value types) | {c['script_without_class']} |",
        f"| `TTypedClass<>` argument types | {c['typed_classes'] + c['typed_without_class']} |",
        f"| ... matching an engine class above | {c['typed_classes']} |",
        f"| Function-local classes (not listed) | {c['function_local_types']} |",
        "",
        f"The {c['type_descriptors']} type descriptors in the file are {c['classes']} types plus "
        f"{c['function_local_types']} classes declared inside functions. A `ClassBuilder` or a "
        "`TTypedClass<>` whose class has no RTTI of its own is a non-polymorphic value type: "
        "the script system sees it, the vtable map cannot.",
        "",
        "## Parser anchor",
        "",
        f"`{ANCHOR_CLASS}` must carry vtable `{ANCHOR_VTABLE:08X}`, whose slot 0 is",
        f"`{ANCHOR_SLOT0:08X}` (`VA_GDS_DTOR` in `docs/addresses.md`). Parsed:",
        f"{anchor_slot0}.",
        "",
        "## Classes this project already touches",
        "",
        "`Base` is the first entry of the RTTI base class array, which is the direct",
        "base in single inheritance. `Vtable (slots)` counts the leading run of code",
        "pointers, so a lower bound is possible only where a class has several vtables.",
        "",
        "| Class | Base | Vtable (slots) | Script | Typed |",
        "|---|---|---|---|---|",
    ]
    by_name = {cl["name"]: cl for cl in report["classes"]}
    for name in INTERESTING:
        entry = by_name.get(name)
        if entry is None:
            continue
        base = entry["bases"][0] if entry["bases"] else "-"
        lines.append(
            f"| `{name}` | {base if base == '-' else '`' + base + '`'} | "
            f"{fmt_vtables(entry)} | {'yes' if entry['script_class'] else '-'} | "
            f"{'yes' if entry['typed'] else '-'} |"
        )

    script = [cl for cl in report["classes"] if cl["script_class"]]
    lines += [
        "",
        "## Classes exposed to WitcherScript",
        "",
        f"{len(script)} classes have a generated `ClassBuilder`, which is how the",
        "engine registers a class, its properties and its methods with the script",
        "system. This is the script-visible half of the engine:",
        "",
        "| Class | Base | Vtable (slots) | Typed |",
        "|---|---|---|---|",
    ]
    for entry in script:
        base = entry["bases"][0] if entry["bases"] else "-"
        lines.append(
            f"| `{entry['name']}` | {base if base == '-' else '`' + base + '`'} | "
            f"{fmt_vtables(entry)} | {'yes' if entry['typed'] else '-'} |"
        )

    lines += [
        "",
        "## All engine classes",
        "",
        "Compact list for searching. `*ClassBuilder` scaffolding and template",
        "instantiations are in the JSON only (`kind` tells them apart).",
        "",
        "```",
    ]
    names = [cl["name"] for cl in report["classes"] if cl["kind"] == "class"]
    width = 0
    row = []
    for name in names:
        if width + len(name) + 2 > 100:
            lines.append(", ".join(row))
            row, width = [], 0
        row.append(name)
        width += len(name) + 2
    if row:
        lines.append(", ".join(row))
    lines += ["```", ""]
    return "\n".join(lines)


# -------------------------------------------------------------------------- main


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    ap.add_argument("--json", type=Path, default=DEFAULT_JSON)
    ap.add_argument("--md", type=Path, default=DEFAULT_MD)
    ap.add_argument("--check", action="store_true", help="verify the anchors only")
    args = ap.parse_args(argv)

    if not args.exe.exists():
        raise SystemExit(
            f"{args.exe} is missing. Unpack a copy of the game's witcher2.exe first "
            "(AGENTS.md, Research setup); the binary is git-ignored."
        )

    data = args.exe.read_bytes()
    pe = PE(data)
    if pe.image_base != IMAGE_BASE_EXPECTED:
        raise SystemExit(f"unexpected image base {pe.image_base:#x}")

    report = build(pe, hashlib.sha256(data).hexdigest().upper())
    problems = check_anchors(report)
    if problems:
        for p in problems:
            print(f"anchor check failed: {p}", file=sys.stderr)
        return 1

    anchor = next(c for c in report["classes"] if c["name"] == ANCHOR_CLASS)
    vt_va = next(v["va"] for v in anchor["vtables"] if v["va"] == f"{ANCHOR_VTABLE:08X}")
    vt_va = int(vt_va, 16)
    slot0 = pe.read_u32(rva(pe, vt_va))
    anchor_slot0 = f"`{slot0:08X}` ({'matches' if slot0 == ANCHOR_SLOT0 else 'MISMATCH'})"
    if slot0 != ANCHOR_SLOT0:
        print(f"anchor check failed: slot 0 is {slot0:08X}, expected {ANCHOR_SLOT0:08X}",
              file=sys.stderr)
        return 1

    c = report["counts"]
    print(
        f"{c['classes']} classes, {c['with_vtable']} with a vtable, "
        f"{c['script_classes']} script-visible, {c['typed_classes']} typed; "
        f"anchor {ANCHOR_CLASS} at {anchor['vtables'][0]['va']}, slot 0 {slot0:08X}"
    )
    if args.check:
        return 0

    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.md.parent.mkdir(parents=True, exist_ok=True)
    with args.json.open("w", encoding="utf-8", newline="\n") as f:
        f.write("{\n")
        head = json.dumps({k: report[k] for k in ("schema", "source", "counts")}, indent=1)
        f.write(head[2:-1])  # the outer braces are written here by hand
        f.write(',\n"classes": [\n')
        f.write(",\n".join(json.dumps(cl, separators=(",", ":")) for cl in report["classes"]))
        f.write("\n]\n}\n")
    args.md.write_text(markdown(report, anchor_slot0), encoding="utf-8", newline="\n")
    print(f"wrote {args.json} and {args.md}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
