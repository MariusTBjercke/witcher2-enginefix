# Checks every character of ScriptPatches.ps1's $BookTexts against the game's own texts in that
# language (CookedPC\<lang>0.w2strings + <lang>_user.w2strings), the same idea as
# witcher-enginefix tools\check_texts.py: a language's UI font may only hold the characters the
# game itself uses. List labels (1-3) are checked upper-cased too, the swf shows them that way.
#
# Usage: python tools\script-patches\check_texts.py "<game>\CookedPC"
#
# .w2strings layout and decryption follow Gibbed.RED StringsFile.cs (version >= 200):
# uint32 version, uint16 key hi, key table (plain names), uint16 key lo, uint32 hash, then
# per text: uint32 index, length-prefixed UTF-16 XORed with ((len + 1) * key) and a key rotated
# left each unit. Files whose key is not in Gibbed's table (tr0) get the 16-bit text key by trying
# all 65536 and keeping the one that decodes to the most spaces.
import os
import re
import struct
import sys

KEYS = {0x18632176: 0x16875467, 0x18796651: 0x42387566, 0x23863176: 0x75921975, 0x24987354: 0x21793217,
        0x42378932: 0x67823218, 0x43975139: 0x79321793, 0x45931894: 0x12375973, 0x54834893: 0x59825646,
        0x63481486: 0x42386347, 0x75886138: 0x42791159, 0x77932179: 0x54932186, 0x83496237: 0x73946816, 0: 0}
FILES = {"EN": "en", "PL": "pl", "DE": "de", "FR": "fr", "ES": "es", "IT": "it", "RU": "ru",
         "CZ": "cz", "HU": "hu", "TR": "tr", "JP": "jp", "ZH": "zh"}


def varint(b, p):
    op = b[p]
    p += 1
    v = op & 0x3F
    if op & 0x40:
        sh = 6
        while True:
            e = b[p]
            p += 1
            v |= (e & 0x7F) << sh
            sh += 7
            if not e & 0x80:
                break
    return (-v if op & 0x80 else v), p


def parse(path):
    b = open(path, "rb").read()
    p = 4
    hi, = struct.unpack_from("<H", b, p)
    p += 2
    n, p = varint(b, p)
    for _ in range(n):
        ln, p = varint(b, p)
        p += -ln if ln < 0 else 2 * ln
        p += 4
    lo, = struct.unpack_from("<H", b, p)
    p += 2 + 4
    m, p = varint(b, p)
    raw = []
    for _ in range(m):
        p += 4
        ln, p = varint(b, p)
        raw.append(b[p:p + 2 * ln])
        p += 2 * ln
    return (hi << 16) | lo, raw


def decode(buf, sk):
    n = len(buf) // 2
    out = []
    for j in range(n):
        u = buf[2 * j] | (buf[2 * j + 1] << 8)
        out.append(chr(u ^ (((n + 1) * sk) & 0xFFFF)))
        sk = ((sk << 1) | (sk >> 15)) & 0xFFFF
    return "".join(out)


def chars_of(path):
    fk, raw = parse(path)
    if fk in KEYS:
        sk = (KEYS[fk] >> 8) & 0xFFFF
    else:
        sk = max(range(65536), key=lambda k: sum(decode(b, k).count(" ") for b in raw[:60]))
    return set("".join(decode(b, sk) for b in raw))


def book_texts():
    ps1 = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "ScriptPatches.ps1"), encoding="ascii").read()
    block = re.search(r"^\$BookTexts = \[ordered\]@\{(.*?)^\}", ps1, re.S | re.M).group(1)
    texts = {}
    for lang, body in re.findall(r"^\s+(\w+) = @\((.*?)^\s+\)", block, re.S | re.M):
        texts[lang] = [re.sub(r"\\u([0-9A-Fa-f]{4})", lambda m: chr(int(m.group(1), 16)), s)
                       for s in re.findall(r'"([^"]*)"', body)]
    return texts


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: check_texts.py <game>\\CookedPC")
    cooked = sys.argv[1]
    bad = 0
    for lang, texts in book_texts().items():
        have = set()
        for suffix in ("0", "_user"):
            path = os.path.join(cooked, FILES[lang] + suffix + ".w2strings")
            if os.path.exists(path):
                have |= chars_of(path)
        need = set("".join(texts)) | set("".join(t.upper() for t in texts[:3]))
        missing = sorted(c for c in need if c not in have and c not in "%1")
        bad += len(missing)
        print("%s %2d texts, %s" % (lang, len(texts),
                                    "ok" if not missing else "MISSING " + " ".join("U+%04X" % ord(c) for c in missing)))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
