#!/usr/bin/env python3
"""Golden test for the DLL overlay build (plan Task 6).

Runs the reference patcher (tools/script-patches/ScriptPatches.ps1) on one scratch game
layout and build\\sptest.exe's overlay build on another, then compares the results:

  * every entry of the reference's base_scripts.dzip decompressed byte for byte, with an
    independent Python DZIP/LZF reader and the table hash verified;
  * the generated read-book icons, name for name and byte for byte (archive entries for the
    DLL build, loose files for the reference);
  * ui_trade.swf and ui_storage.swf byte for byte (cached loose file or archive entry);
  * a second build on unchanged inputs, which must reuse the cache and touch nothing.

Usage: python -I tools\\patchtest\\golden.py "<game>" "<scratch>"

<scratch> must be on the game's volume (the other archives are hard-linked) and is wiped.
Nothing under <game> is written. The reference inputs are the old patcher's backup when it
exists (the live files may carry the 0.5 patches), otherwise <game>\\CookedPC itself.
"""
import os
import pathlib
import shutil
import struct
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
SPTEST = REPO / "build" / "sptest.exe"
REF_SCRIPT = REPO / "tools" / "script-patches" / "ScriptPatches.ps1"
SEP = "\\"


class GoldenError(Exception):
    pass


# ---- an independent DZIP v2 reader --------------------------------------

def lzf_decompress(src, want):
    """LZF block decoder (reference semantics, also tolerant like the C port)."""
    out = bytearray()
    i, n = 0, len(src)
    while i < n and len(out) < want:
        c = src[i]
        i += 1
        if c < 32:
            run = c + 1
            take = min(run, want - len(out))
            out += src[i:i + take]
            i += run
        else:
            length = c >> 5
            ref = len(out) - ((c & 31) << 8) - 1
            if length == 7:
                length += src[i]
                i += 1
            ref -= src[i]
            i += 1
            for k in range(length + 2):
                if len(out) >= want:
                    break
                out.append(out[ref + k])
    return bytes(out)


def dzip_read(path):
    data = path.read_bytes()
    magic, version = struct.unpack_from("<II", data, 0)
    if magic != 0x50495A44 or version != 2:
        raise GoldenError(f"{path.name}: not a DZIP v2 archive")
    count, = struct.unpack_from("<i", data, 8)
    table, = struct.unpack_from("<q", data, 16)
    stored_hash, = struct.unpack_from("<Q", data, 24)
    entries = []
    p = table
    for _ in range(count):
        n, = struct.unpack_from("<H", data, p)
        p += 2
        name = data[p:p + n].rstrip(b"\0").decode("latin-1")
        p += n
        time, usize, offset, csize = struct.unpack_from("<qqqq", data, p)
        p += 32
        entries.append((name, time & 0xFFFFFFFFFFFFFFFF, usize, offset, csize))
    if dzip_hash(entries) != stored_hash:
        raise GoldenError(f"{path.name}: entry table hash mismatch")
    return data, entries


def dzip_hash(entries):
    prime = 0x100000001B3
    h = 0xFFFFFFFF
    for name, time, usize, offset, csize in entries:
        if name:
            for ch in name.encode("latin-1"):
                h = ((h ^ ch) * prime) & 0xFFFFFFFFFFFFFFFF
            h = ((h ^ len(name)) * prime) & 0xFFFFFFFFFFFFFFFF
        for v in (time, usize, offset, csize):
            h = ((h ^ (v & 0xFFFFFFFFFFFFFFFF)) * prime) & 0xFFFFFFFFFFFFFFFF
    return h


def dzip_extract(data, entry):
    name, _time, usize, offset, csize = entry
    if usize == 0:
        return b""
    blocks = (usize + 0xFFFF) >> 16
    if blocks * 4 > csize:
        raise GoldenError(f"{name}: block table does not fit the entry")
    offsets = struct.unpack_from(f"<{blocks}i", data, offset)
    out = bytearray()
    for k in range(blocks):
        start = offset + offsets[k]
        end = offset + (offsets[k + 1] if k + 1 < blocks else csize)
        want = min(0x10000, usize - len(out))
        out += lzf_decompress(data[start:end], want)
    if len(out) != usize:
        raise GoldenError(f"{name}: decompressed {len(out)} of {usize} bytes")
    return bytes(out)


def by_name(entries):
    """lowercase name -> entry; two names differing only in case are a failure."""
    out = {}
    for e in entries:
        key = e[0].lower()
        if key in out and out[key][0] != e[0]:
            raise GoldenError(f"entries differing only in case: {out[key][0]} / {e[0]}")
        out[key] = e
    return out


# ---- scratch layout ------------------------------------------------------

def run(cmd, what, cwd=None, encoding="oem"):
    proc = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode(encoding, "replace")
    if proc.returncode != 0:
        raise GoldenError(f"{what} failed (exit {proc.returncode}):\n{text}")
    return text


def make_layout(layout, game, source, backup):
    cooked = layout / "CookedPC"
    gui = cooked / "globals" / "gui"
    gui.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / "base_scripts.dzip", cooked / "base_scripts.dzip")
    for name in ("ui_trade.swf", "ui_storage.swf"):
        src = (source / name) if backup else (source / "globals" / "gui" / name)
        if src.is_file():
            shutil.copy2(src, gui / name)
    for dzip in sorted((game / "CookedPC").glob("*.dzip")):
        if dzip.name.lower() == "base_scripts.dzip":
            continue
        for target in (cooked / dzip.name,):
            if target.exists():
                continue
            try:
                os.link(dzip, target)
            except OSError as exc:
                raise GoldenError(
                    f"cannot hard-link {dzip.name} into the scratch layout ({exc}); "
                    "<scratch> must be on the game's volume") from exc


def snapshot(root):
    return {
        p.relative_to(root).as_posix(): (p.stat().st_size, p.stat().st_mtime_ns)
        for p in sorted(root.rglob("*")) if p.is_file()
    }


# ---- comparison ----------------------------------------------------------

def compare(ref_zip, our_zip, ref_cooked, out):
    ref_data, ref_entries = dzip_read(ref_zip)
    our_data, our_entries = dzip_read(our_zip)
    ref = by_name(ref_entries)
    ours = by_name(our_entries)
    problems = []
    compared = 0

    for key, entry in ref.items():
        if key not in ours:
            problems.append(f"missing entry: {entry[0]}")
            continue
        if dzip_extract(ref_data, entry) != dzip_extract(our_data, ours[key]):
            problems.append(f"entry differs: {entry[0]}")
            continue
        compared += 1

    icons = 0
    for key, entry in ours.items():
        if key in ref:
            continue
        if entry[0].lower().startswith("globals\\gui\\icons\\items\\w2ef_read\\"):
            loose = ref_cooked / entry[0]
            icons += 1
        elif entry[0].lower() in ("globals\\gui\\ui_trade.swf", "globals\\gui\\ui_storage.swf"):
            loose = ref_cooked / entry[0]
        else:
            problems.append(f"unexpected entry: {entry[0]}")
            continue
        if not loose.is_file():
            problems.append(f"{entry[0]}: the reference wrote no loose file")
        elif dzip_extract(our_data, entry) != loose.read_bytes():
            problems.append(f"{entry[0]}: differs from the reference's loose file")

    # The icon folder from the other direction: every reference icon must be in our archive.
    ref_icons = ref_cooked / "globals" / "gui" / "icons" / "items" / "w2ef_read"
    if ref_icons.is_dir():
        for dds in sorted(ref_icons.glob("*.dds")):
            name = f"globals\\gui\\icons\\items\\w2ef_read\\{dds.name}".lower()
            if name not in ours:
                problems.append(f"read icon missing from the archive: {dds.name}")
                continue
            if dzip_extract(our_data, ours[name]) != dds.read_bytes():
                problems.append(f"read icon differs: {dds.name}")

    # The movies, loose in both layouts (the reference always leaves them loose).
    movies = []
    for name in ("ui_trade.swf", "ui_storage.swf"):
        ref_swf = ref_cooked / "globals" / "gui" / name
        our_swf = out / name
        entry = ours.get(f"globals\\gui\\{name}".lower())
        if not ref_swf.is_file():
            problems.append(f"{name}: the reference wrote no loose file")
            continue
        want = ref_swf.read_bytes()
        got = our_swf.read_bytes() if our_swf.is_file() else (
            dzip_extract(our_data, entry) if entry else None)
        if got is None:
            problems.append(f"{name}: the DLL build produced nothing")
            continue
        if got != want:
            problems.append(f"{name}: differs ({len(got)} vs {len(want)} bytes)")
            continue
        movies.append((name, len(got), "loose" if our_swf.is_file() else "archive"))

    return compared, icons, movies, problems


# ---- main ----------------------------------------------------------------

def main(argv):
    if len(argv) != 3:
        print(__doc__.strip())
        return 2
    game = pathlib.Path(argv[1]).resolve()
    scratch = pathlib.Path(argv[2]).resolve()
    if not (game / "CookedPC" / "base_scripts.dzip").is_file():
        raise GoldenError(f"{game} does not look like the game folder (CookedPC\\base_scripts.dzip)")
    if not SPTEST.is_file():
        raise GoldenError(f"{SPTEST} is missing - run tools\\patchtest\\build.bat first")
    if not REF_SCRIPT.is_file():
        raise GoldenError(f"{REF_SCRIPT} is missing")
    if os.path.splitdrive(str(scratch))[0].lower() != os.path.splitdrive(str(game))[0].lower():
        raise GoldenError("<scratch> must be on the game's volume (the dzips are hard-linked)")

    backup = game / "Witcher2EngineFix_Optional" / "ScriptPatches" / "backup"
    if (backup / "base_scripts.dzip").is_file():
        source, source_note = backup, "the old patcher's backup (unpatched originals)"
    else:
        source, source_note = game / "CookedPC", (
            "the live CookedPC files (no old backup found - if the 0.5 patches are installed, "
            "run Uninstall.bat first)")
    print(f"game:    {game}")
    print(f"scratch: {scratch}")
    print(f"source:  {source_note}")

    ref = scratch / "ref"
    dll = scratch / "dll"
    out = scratch / "out"
    for path in (ref, dll, out, scratch / "backup", scratch / "ref-patch.ps1"):
        if path.is_dir():
            shutil.rmtree(path)
        elif path.exists():
            path.unlink()
    scratch.mkdir(parents=True, exist_ok=True)

    # Two identical layouts: one for the reference, one for the DLL build.
    make_layout(ref, game, source, backup)
    make_layout(dll, game, source, backup)

    print("\n== reference: ScriptPatches.ps1 -Action Install -Features All ==")
    ref_copy = scratch / "ref-patch.ps1"
    shutil.copy2(REF_SCRIPT, ref_copy)
    text = run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(ref_copy),
                "-Action", "Install", "-Features", "All", "-GameDir", str(ref)], "the reference")
    for line in text.splitlines():
        if line.strip():
            print("  " + line.strip())

    print("\n== DLL build: build\\sptest.exe build ==")
    text = run([str(SPTEST), "build", str(dll), str(out)], "the overlay build")
    for line in text.splitlines():
        print("  " + line)

    print("\n== compare ==")
    compared, icons, movies, problems = compare(
        ref / "CookedPC" / "base_scripts.dzip", out / "base_scripts.dzip",
        ref / "CookedPC", out)
    print(f"  {compared} base_scripts.dzip entries identical")
    print(f"  {icons} read-book icons identical")
    for name, size, where in movies:
        print(f"  {name} identical ({size} bytes, {where})")

    # Cache reuse: the same inputs again must not rebuild or touch anything.
    before = snapshot(out)
    text = run([str(SPTEST), "build", str(dll), str(out)], "the second build")
    if "cache: reused" not in text:
        problems.append("the second build did not reuse the cache:\n" + text)
    after = snapshot(out)
    if before != after:
        problems.append("the second build changed files under the cache")

    if problems:
        print("\nDIFFERENCES")
        for problem in problems:
            print("  " + problem.replace("\n", "\n  "))
        return 1
    print("\nGOLDEN OK")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except GoldenError as exc:
        print(f"error: {exc}")
        sys.exit(1)
