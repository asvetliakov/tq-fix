#!/usr/bin/env python3
"""Inventory shadow-related DXBC programs embedded in extracted TQ .ssh files.

The surrounding .ssh container is proprietary, but each D3D11 program is a
self-describing DXBC container.  This tool deliberately records only facts
that can be recovered without guessing the .ssh style-table layout.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import struct
from pathlib import Path


MARKERS = (
    b"worldToShadowMatrix",
    b"shadowTexture",
    b"shadowBluriness",
    b"objectToScreenMatrix",
    b"boneMatrix",
    b"baseMap",
)

PROGRAM_TYPES = {
    0: "pixel",
    1: "vertex",
    2: "geometry",
    3: "hull",
    4: "domain",
    5: "compute",
}


def dxbc_at(data: bytes, start: int) -> tuple[int, list[tuple[bytes, bytes]]] | None:
    if start + 32 > len(data):
        return None
    version, total_size, count = struct.unpack_from("<III", data, start + 20)
    if version != 1 or total_size < 32 or start + total_size > len(data):
        return None
    if not 0 < count <= 64 or 32 + count * 4 > total_size:
        return None
    chunks: list[tuple[bytes, bytes]] = []
    for index in range(count):
        offset = struct.unpack_from("<I", data, start + 32 + index * 4)[0]
        if offset + 8 > total_size:
            return None
        fourcc = data[start + offset : start + offset + 4]
        size = struct.unpack_from("<I", data, start + offset + 4)[0]
        if offset + 8 + size > total_size:
            return None
        chunks.append((fourcc, data[start + offset + 8 : start + offset + 8 + size]))
    return total_size, chunks


def stage_of(chunks: list[tuple[bytes, bytes]]) -> tuple[str, str]:
    for fourcc, payload in chunks:
        if fourcc in (b"SHEX", b"SHDR") and len(payload) >= 4:
            token = struct.unpack_from("<I", payload)[0]
            kind = token >> 16
            major = (token >> 4) & 0xF
            minor = token & 0xF
            return PROGRAM_TYPES.get(kind, f"type-{kind}"), f"{major}_{minor}"
    return "unknown", "unknown"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path, help="directory containing extracted .ssh files")
    parser.add_argument("output", type=Path, help="CSV destination")
    parser.add_argument(
        "--extract-unique", type=Path,
        help="optionally extract each unique shadow-related DXBC by SHA-256",
    )
    args = parser.parse_args()

    rows: list[dict[str, object]] = []
    raw_shadow_resources = 0
    extracted: set[str] = set()
    if args.extract_unique:
        args.extract_unique.mkdir(parents=True, exist_ok=True)
    for path in sorted(args.root.rglob("*.ssh")):
        data = path.read_bytes()
        resource_markers = [m.decode() for m in MARKERS if m in data]
        if "worldToShadowMatrix" in resource_markers:
            raw_shadow_resources += 1
        cursor = 0
        ordinal = 0
        while True:
            start = data.find(b"DXBC", cursor)
            if start < 0:
                break
            parsed = dxbc_at(data, start)
            if parsed is None:
                cursor = start + 4
                continue
            size, chunks = parsed
            ordinal += 1
            payload = data[start : start + size]
            markers = [m.decode() for m in MARKERS if m in payload]
            if markers:
                stage, model = stage_of(chunks)
                digest = hashlib.sha256(payload).hexdigest()
                rows.append({
                    "resource": path.relative_to(args.root).as_posix(),
                    "dxbc_ordinal": ordinal,
                    "source_offset": f"0x{start:08x}",
                    "size": size,
                    "stage": stage,
                    "shader_model": model,
                    "sha256": digest,
                    "markers": ";".join(markers),
                })
                if (args.extract_unique and digest not in extracted and (
                    "worldToShadowMatrix" in markers or "shadowTexture" in markers
                )):
                    (args.extract_unique / f"{digest}.dxbc").write_bytes(payload)
                    extracted.add(digest)
            cursor = start + size

    args.output.parent.mkdir(parents=True, exist_ok=True)
    columns = (
        "resource", "dxbc_ordinal", "source_offset", "size", "stage",
        "shader_model", "sha256", "markers",
    )
    with args.output.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)

    shadow_rows = [r for r in rows if "worldToShadowMatrix" in str(r["markers"])
                   or "shadowTexture" in str(r["markers"])]
    recoverable_resources = {str(r["resource"]) for r in shadow_rows}
    unique = {str(r["sha256"]) for r in shadow_rows}
    print(f"containers mentioning worldToShadowMatrix: {raw_shadow_resources}")
    print(f"resources with recoverable shadow DXBC: {len(recoverable_resources)}")
    print(f"shadow-related DXBC rows: {len(shadow_rows)}")
    print(f"unique shadow-related DXBC programs: {len(unique)}")
    if args.extract_unique:
        print(f"extracted unique DXBC programs to {args.extract_unique}")
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
