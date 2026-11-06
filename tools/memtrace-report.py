"""Summarise witcher2_enginefix_memtrace.tsv ([diagnostics] mem_trace).

Usage: python -I tools/memtrace-report.py <path to witcher2_enginefix_memtrace.tsv> [top]

Prints the memory and 64 KB page series, then the classes that grew most from the first census
to the last, with how often they went up between two censuses. A leak looks like a class that
grows in most steps and never falls back; a cache grows early and then holds.
"""
import csv
import sys
from collections import defaultdict


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 25
    series = defaultdict(dict)   # (kind, key) -> {census: value}
    minutes = {}
    with open(sys.argv[1], newline="", encoding="ascii", errors="replace") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            n = int(row["census"])
            minutes[n] = int(row["seconds"]) // 60
            series[(row["kind"], row["key"])][n] = int(row["value"])
    if not minutes:
        print("no censuses in the file")
        return 1
    order = sorted(minutes)
    first, last = order[0], order[-1]
    print(f"{len(order)} censuses, minute {minutes[first]} to {minutes[last]}")

    def row_of(kind, key):
        vals = series.get((kind, key), {})
        return " ".join(f"{vals.get(n, 0):>6}" for n in order)

    print("\n" + " " * 18 + " ".join(f"{minutes[n]:>5}m" for n in order))
    for key in ("private_mb", "free_mb", "largest_free_mb"):
        print(f"{key:<18}{row_of('mem', key)}")
    for key in ("state", "havok", "small", "other"):
        print(f"{'pages ' + key:<18}{row_of('page', key)}")

    grow = []
    for (kind, key), vals in series.items():
        if kind != "class":
            continue
        a, b = vals.get(first, 0), vals.get(last, 0)
        if b <= a:
            continue
        ups = sum(1 for x, y in zip(order, order[1:]) if vals.get(y, 0) > vals.get(x, 0))
        downs = sum(1 for x, y in zip(order, order[1:]) if vals.get(y, 0) < vals.get(x, 0))
        grow.append((b - a, ups, downs, key, a, b))
    grow.sort(reverse=True)
    steps = len(order) - 1
    print(f"\nclasses that grew most, first -> last ({steps} steps; up/down = steps it rose/fell)")
    for d, ups, downs, key, a, b in grow[:top]:
        name, vt = key.rsplit("@", 1)
        name = name[4:] if name.startswith(".?A") else name
        name = name[:-2] if name.endswith("@@") else name
        print(f"  {d:>+8}  {a:>7} -> {b:<7}  up {ups:>2} down {downs:>2}  {name}  ({vt})")
    steady = [g for g in grow if steps >= 3 and g[1] >= steps * 0.75 and g[2] == 0]
    if steady:
        print("\nrose in at least 3/4 of the steps and never fell (leak candidates):")
        for d, ups, downs, key, a, b in steady[:top]:
            print(f"  {d:>+8}  {key}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
