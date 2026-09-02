#!/usr/bin/env python3
"""Parse Titan Quest .arc containers and check what the mod's design assumes.

    research/streaming/tools/arcinfo.py [--entries] ARCHIVE [ARCHIVE ...]
    research/streaming/tools/arcinfo.py --summary "$TQ_GAME_DIR"

The format is documented in ../arc-format.md; this is the program that
produced the numbers there, so that they can be re-derived rather than
believed.  Three properties are checked on every archive, because a plan for
the archive read path rests on each of them:

  compressed    Whether any entry is stored uncompressed.  The uncompressed
                branch of Archive::ReadFromFile is only reachable if one is.
  contiguous    Whether block k's extent ends exactly where block k+1 begins,
                across the whole archive.  A bounded prefetch that over-reads
                past the requested block is only safe if it is.
  blocks        The block size, and the largest entry's block count -- the
                per-block seek/read/inflate cycle is paid once per block.

Standard library only, and it never writes anything: this is an audit tool
against a read-only installation.
"""

import os
import struct
import sys

HEADER = b"ARC\0"
HEADER_SIZE = 0x800
BLOCK_RECORD = 12
FILE_RECORD = 44
FLAG_COMPRESSED = 2


class Archive:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as handle:
            head = handle.read(0x20)
            if len(head) < 0x20 or head[:4] != HEADER:
                raise ValueError(f"{path}: not an ARC container")
            (self.version, self.file_count, self.part_count,
             self.part_table_size, self.string_table_size,
             self.toc_offset) = struct.unpack_from("<IIIIII", head, 4)
            if self.part_table_size != self.part_count * BLOCK_RECORD:
                raise ValueError(
                    f"{path}: partTableSize {self.part_table_size} is not "
                    f"partCount {self.part_count} * {BLOCK_RECORD}")
            handle.seek(self.toc_offset)
            toc = handle.read(self.part_table_size + self.string_table_size
                              + self.file_count * FILE_RECORD)
        self.size = os.path.getsize(path)

        self.blocks = [
            struct.unpack_from("<III", toc, i * BLOCK_RECORD)
            for i in range(self.part_count)
        ]
        names = toc[self.part_table_size:
                    self.part_table_size + self.string_table_size]
        records = self.part_table_size + self.string_table_size
        self.entries = []
        for i in range(self.file_count):
            o = records + i * FILE_RECORD
            (flags, offset, compressed, decompressed,
             _t0, _t1, _t2, block_count, first_block,
             name_length, name_offset) = struct.unpack_from("<11I", toc, o)
            name = names[name_offset:name_offset + name_length]
            self.entries.append({
                "name": name.decode("latin-1"),
                "flags": flags,
                "offset": offset,
                "compressed": compressed,
                "decompressed": decompressed,
                "block_count": block_count,
                "first_block": first_block,
            })

    def block_size(self):
        """The archive's block size, or None if nothing here reveals it.

        Every entry's last block is short, so the block size is the maximum
        uncompressed block size -- but only in an archive that has at least one
        entry spanning more than one block.  In an archive where every entry
        fits in a single block the maximum is just the largest entry, which
        says nothing about the block size, and reporting it as one is how a
        survey ends up claiming seventeen different block sizes exist.
        """
        if not any(e["block_count"] > 1 for e in self.entries):
            return None
        sizes = {u for _, _, u in self.blocks}
        return max(sizes) if sizes else None

    def contiguity(self):
        """(gaps, overlaps) over the globally ordered block extents."""
        ordered = sorted(self.blocks, key=lambda b: b[0])
        gaps = overlaps = 0
        for a, b in zip(ordered, ordered[1:]):
            end = a[0] + a[1]
            if end < b[0]:
                gaps += 1
            elif end > b[0]:
                overlaps += 1
        return gaps, overlaps

    def blocks_consecutive(self):
        """Whether every entry's blocks are consecutive record indices."""
        for entry in self.entries:
            first, count = entry["first_block"], entry["block_count"]
            if first + count > len(self.blocks):
                return False
            for k in range(first, first + count - 1):
                if self.blocks[k][0] + self.blocks[k][1] != self.blocks[k + 1][0]:
                    return False
        return True

    def uncompressed_entries(self):
        return [e for e in self.entries if not (e["flags"] & FLAG_COMPRESSED)]

    def past_end(self):
        return [b for b in self.blocks if b[0] + b[1] > self.size]


def describe(archive, show_entries=False):
    gaps, overlaps = archive.contiguity()
    loose = archive.uncompressed_entries()
    largest = max(archive.entries, key=lambda e: e["decompressed"],
                  default=None)
    print(f"{archive.path}")
    print(f"  {archive.file_count} entries, {archive.part_count} blocks, "
          f"{archive.size / (1 << 20):.1f} MiB on disk")
    size = archive.block_size()
    print(f"  block size          "
          f"{f'{size} bytes' if size else 'not revealed (no multi-block entry)'}")
    print(f"  uncompressed entries {len(loose)}")
    print(f"  data region          {'contiguous' if not (gaps or overlaps) else f'{gaps} gaps, {overlaps} overlaps'}")
    print(f"  per-entry block runs {'consecutive' if archive.blocks_consecutive() else 'NOT consecutive'}")
    print(f"  extents past EOF     {len(archive.past_end())}")
    if largest:
        print(f"  largest entry        {largest['name']} "
              f"{largest['decompressed']} bytes uncompressed, "
              f"{largest['compressed'] / (1 << 20):.1f} MiB compressed, "
              f"{largest['block_count']} blocks")
    if show_entries:
        for entry in sorted(archive.entries, key=lambda e: -e["decompressed"]):
            print(f"    {entry['decompressed']:12d}  {entry['block_count']:6d} "
                  f"blocks  {entry['name']}")


def summary(paths):
    totals = {"entries": 0, "blocks": 0, "loose": 0, "gaps": 0, "overlaps": 0,
              "past_end": 0, "nonconsecutive": 0, "archives": 0,
              "discontiguous": 0, "single_block": 0, "loose_archives": 0}
    block_sizes = set()
    big = []
    for path in paths:
        try:
            archive = Archive(path)
        except ValueError as error:
            print(f"skipped: {error}")
            continue
        gaps, overlaps = archive.contiguity()
        totals["archives"] += 1
        totals["entries"] += archive.file_count
        totals["blocks"] += archive.part_count
        totals["loose"] += len(archive.uncompressed_entries())
        totals["gaps"] += gaps
        totals["overlaps"] += overlaps
        totals["discontiguous"] += 1 if (gaps or overlaps) else 0
        totals["loose_archives"] += 1 if archive.uncompressed_entries() else 0
        totals["past_end"] += len(archive.past_end())
        totals["nonconsecutive"] += 0 if archive.blocks_consecutive() else 1
        block_sizes.add(archive.block_size())
        if archive.block_size() is None:
            totals["single_block"] += 1
        for entry in archive.entries:
            if entry["name"].endswith(".tex") and entry["decompressed"] >= (2 << 20):
                big.append((entry["decompressed"], entry["block_count"],
                            os.path.basename(path), entry["name"]))

    print(f"{totals['archives']} archives, {totals['entries']} entries, "
          f"{totals['blocks']} blocks")
    print(f"  entries stored uncompressed    {totals['loose']} "
          f"in {totals['loose_archives']} archives")
    print(f"  archives with a gap or overlap {totals['discontiguous']} "
          f"({totals['gaps']} gaps, {totals['overlaps']} overlaps)")
    print(f"  archives with a non-consecutive entry {totals['nonconsecutive']}")
    print(f"  block extents past end of file {totals['past_end']}")
    print(f"  block sizes, where revealed    {sorted(s for s in block_sizes if s)}"
          f" ({totals['single_block']} archives had no multi-block entry)")
    print(f"  .tex entries >= 2 MiB          {len(big)}")
    for size, blocks, archive, name in sorted(big, reverse=True)[:5]:
        print(f"    {size / (1 << 20):8.2f} MiB  {blocks:4d} blocks  "
              f"{archive}:{name}")


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    flags = {a for a in argv[1:] if a.startswith("--")}
    if not args:
        raise SystemExit(__doc__)
    if "--summary" in flags:
        paths = []
        for root in args:
            for base, _, files in os.walk(root):
                paths += [os.path.join(base, f) for f in files
                          if f.lower().endswith(".arc")]
        summary(sorted(paths))
        return
    for path in args:
        describe(Archive(path), "--entries" in flags)


if __name__ == "__main__":
    main(sys.argv)
