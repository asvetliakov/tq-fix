#!/usr/bin/env python3
"""Extract every structurally valid embedded DXBC container from a TQ .ssh.

The tool does not understand the surrounding Titan Quest shader format.  It
uses the self-describing DXBC header (total size at byte 24 and chunk offsets
at byte 32), rejects overlapping/truncated candidates, and emits files in
source-offset order for a separate D3DDisassemble pass.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


def valid_dxbc(blob: bytes, start: int) -> tuple[int, int] | None:
    if start + 32 > len(blob):
        return None
    version, total_size, chunk_count = struct.unpack_from("<III", blob, start + 20)
    if version != 1 or total_size < 32 or start + total_size > len(blob):
        return None
    if chunk_count == 0 or chunk_count > 64 or 32 + chunk_count * 4 > total_size:
        return None
    for index in range(chunk_count):
        offset = struct.unpack_from("<I", blob, start + 32 + index * 4)[0]
        if offset + 8 > total_size:
            return None
        chunk_size = struct.unpack_from("<I", blob, start + offset + 4)[0]
        if offset + 8 + chunk_size > total_size:
            return None
    return total_size, chunk_count


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    data = args.input.read_bytes()
    args.output.mkdir(parents=True, exist_ok=True)
    found: list[tuple[int, int, int]] = []
    cursor = 0
    while True:
        offset = data.find(b"DXBC", cursor)
        if offset < 0:
            break
        header = valid_dxbc(data, offset)
        if header is not None:
            size, chunks = header
            found.append((offset, size, chunks))
            cursor = offset + size
        else:
            cursor = offset + 4

    print("index,source_offset,size,chunks,sha256,file")
    for index, (offset, size, chunks) in enumerate(found, 1):
        payload = data[offset : offset + size]
        name = f"chunk-{index:02d}-at-{offset:08x}.dxbc"
        destination = args.output / name
        destination.write_bytes(payload)
        digest = hashlib.sha256(payload).hexdigest()
        print(f"{index},0x{offset:08x},{size},{chunks},{digest},{name}")
    return 0 if found else 1


if __name__ == "__main__":
    raise SystemExit(main())
