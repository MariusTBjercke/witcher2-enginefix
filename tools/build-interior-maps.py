# Builds the interior-map files shipped in the release zip from the game's own pack0.dzip:
#   build\interior-maps\CookedPC\globals\map_scales.csv
#   build\interior-maps\CookedPC\globals\gui\maps\<minimap>_2048x2048.dds
# Rows of globals\map_scales.csv with an empty "Map name" (temples, inns, houses) get a
# 2048x2048 upscale of their minimap as the big map. Every other byte of the csv, including
# CDPR's trailing junk rows, is kept. See docs/fixes/interior-maps.md.
#
# Usage: python tools\build-interior-maps.py "<game>\CookedPC\pack0.dzip"
# Needs Pillow >= 11.2 (DXT1 DDS writing).
import io
import os
import re
import struct
import sys

from PIL import Image

SEP = "\\"
CSV = "globals\\map_scales.csv"
MAPS = "globals\\gui\\maps\\"
SIZE = 2048


def dzip_index(path):
    # DZIP 2 layout as in Gibbed.RED PackageFile.cs
    with open(path, "rb") as f:
        magic, _ver, count, _unk, table, _hash = struct.unpack("<4sIIIQQ", f.read(32))
        if magic != b"DZIP":
            raise ValueError(f"{path}: not a DZIP archive")
        f.seek(table)
        out = {}
        for _ in range(count):
            n, = struct.unpack("<H", f.read(2))
            name = f.read(n).rstrip(b"\0").decode("latin1")
            _ts, usize, off, csize = struct.unpack("<QQQQ", f.read(32))
            out[name.lower()] = (usize, off, csize)
        return out


def lzf(src, outlen):
    o = bytearray()
    i = 0
    while i < len(src) and len(o) < outlen:
        c = src[i]
        i += 1
        if c < 32:
            o += src[i:i + c + 1]
            i += c + 1
        else:
            ln = c >> 5
            ref = len(o) - ((c & 31) << 8) - 1
            if ln == 7:
                ln += src[i]
                i += 1
            ref -= src[i]
            i += 1
            for k in range(ln + 2):
                o.append(o[ref + k])
    return bytes(o)


def dzip_read(path, index, name):
    usize, off, csize = index[name.lower()]
    blocks = (usize + 0xFFFF) >> 16
    with open(path, "rb") as f:
        f.seek(off)
        offs = list(struct.unpack(f"<{blocks}I", f.read(4 * blocks))) + [csize]
        out = b""
        for b in range(blocks):
            f.seek(off + offs[b])
            out += lzf(f.read(offs[b + 1] - offs[b]), min(0x10000, usize - len(out)))
    if len(out) != usize:
        raise ValueError(f"{name}: got {len(out)} bytes, expected {usize}")
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__ or "usage: build-interior-maps.py <pack0.dzip>")
    pack = sys.argv[1]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_root = os.path.join(root, "build", "interior-maps", "CookedPC")
    out_maps = os.path.join(out_root, "globals", "gui", "maps")
    os.makedirs(out_maps, exist_ok=True)

    index = dzip_index(pack)
    existing = {n[len(MAPS):-4] for n in index if n.startswith(MAPS)}

    # UTF-16LE with BOM, LF line ends; junk rows at the end do not decode and are left alone.
    sep = b"\n\x00"
    lines = dzip_read(pack, index, CSV).split(sep)
    built = {}
    for i, raw in enumerate(lines[1:], 1):
        try:
            row = raw.decode("utf-16-le")
        except UnicodeDecodeError:
            continue
        cols = row.split(";")
        if not (row.isprintable() and len(cols) >= 7 and cols[0].isdigit()):
            continue
        if cols[1] or not cols[2]:
            continue
        mini = cols[2]
        big = re.sub(r"_\d+x\d+$", "", mini) + f"_{SIZE}x{SIZE}"
        if big in existing:
            raise ValueError(f"{big} already exists in {pack}")
        if mini not in built:
            im = Image.open(io.BytesIO(dzip_read(pack, index, MAPS + mini + ".dds")))
            im = im.convert("RGB").resize((SIZE, SIZE), Image.LANCZOS)
            im.save(os.path.join(out_maps, big + ".dds"), pixel_format="DXT1")
            built[mini] = big
            print(f"map {cols[0]:>3}: {mini} -> {big}")
        cols[1] = big
        lines[i] = ";".join(cols).encode("utf-16-le")

    with open(os.path.join(out_root, "globals", "map_scales.csv"), "wb") as f:
        f.write(sep.join(lines))
    print(f"{len(built)} maps written to {out_root}")


if __name__ == "__main__":
    main()
