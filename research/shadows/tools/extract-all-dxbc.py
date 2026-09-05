#!/usr/bin/env python3
"""Writes every unique DXBC program the inventory records, not only the
shadow-bound ones.

disassemble-shaders.sh extracts the subset that carries shadow bindings, which
is the right corpus for a shadow transform and the wrong one for anything else:
grass and terrain are absent from it, so a match count taken against it cannot
show that a transform is unique.

Usage: extract-all-dxbc.py <inventory.csv> <archives-dir> <output-dir>
"""
import csv
import os
import sys


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    inventory, archives, output = sys.argv[1:]
    os.makedirs(output, exist_ok=True)
    cache = {}
    written, skipped = 0, 0
    with open(inventory, newline="") as handle:
        for row in csv.DictReader(handle):
            path = os.path.join(archives, row["resource"])
            if path not in cache:
                try:
                    with open(path, "rb") as archive:
                        cache[path] = archive.read()
                except OSError:
                    cache[path] = None
            data = cache[path]
            if data is None:
                skipped += 1
                continue
            at = int(row["source_offset"], 16)
            size = int(row["size"])
            blob = data[at:at + size]
            if len(blob) != size or blob[:4] != b"DXBC":
                skipped += 1
                continue
            target = os.path.join(output, row["sha256"] + ".dxbc")
            if not os.path.exists(target):
                with open(target, "wb") as out:
                    out.write(blob)
                written += 1
    print("wrote %d unique programs to %s (%d rows skipped)"
          % (written, output, skipped))


if __name__ == "__main__":
    main()
