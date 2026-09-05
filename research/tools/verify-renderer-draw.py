#!/usr/bin/env python3
"""Read-only verification of the renderer draw sites and both grass call paths."""
from pathlib import Path
import hashlib
import re
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "streaming/tools"))
from pe import PE


def require(condition, description):
    if not condition:
        raise SystemExit("FAIL: " + description)
    print("OK: " + description)


def target(pe, rva):
    code = pe.read(rva, 5)
    require(len(code) == 5 and code[0] == 0xE8, f"direct call at {rva:#x}")
    return rva + 5 + struct.unpack_from("<i", code, 1)[0]


def verify(renderer_path, engine_path):
    pe = PE(renderer_path)
    require(struct.unpack_from("<H", pe.data, pe.lfa + 4)[0] == 0x14C,
            "renderer is x86")
    require(pe.imagesize == 0x192000, "renderer image size matches")
    print("Renderer SHA256:", hashlib.sha256(pe.data).hexdigest())
    windows = [
        ("Draw", 0x5DA90, 15,
         "8b462c558b0850ff51608b462c6a008b085750ff5134019eb0000000ff86b4000000",
         "8b085750ff5134"),
        ("DrawIndexed", 0x5DB13, 21,
         "8b462c528b0850ff5160ff7424148b462cff7424208b085350ff513001beb0000000ff86b4000000",
         "8b085350ff5130"),
    ]
    header = (Path(__file__).resolve().parents[2] / "src/renderer_draw_sites.h").read_text()
    for name, rva, offset, encoded, patch in windows:
        expected = bytes.fromhex(encoded)
        prefix = "Indexed" if name == "DrawIndexed" else "Draw"
        match = re.search(r"k" + prefix + r"Window\[\]\s*=\s*\{([^}]+)\}", header)
        require(match is not None, f"{name} production signature exists")
        production = bytes(int(value, 16) for value in re.findall(r"0x[0-9a-fA-F]+", match.group(1)))
        require(production == expected, f"{name} production signature matches independent audit")
        for suffix, value in [("WindowRva", rva), ("PatchOffset", offset)]:
            constant = re.search(r"k" + prefix + suffix + r"\s*=\s*(0x[0-9a-fA-F]+|\d+)", header)
            require(constant is not None and int(constant.group(1), 0) == value,
                    f"{name} production {suffix} matches audit")
        require(any(section == ".text" and begin <= rva
                    and rva + len(expected) <= begin + size
                    for section, begin, size, _, _ in pe.sections),
                f"{name} window is in .text")
        require(pe.read(rva, len(expected)) == expected,
                f"{name} complete {len(expected)}-byte signature at {rva:#x}")
        require(pe.read(rva + offset, 7) == bytes.fromhex(patch),
                f"{name} seven-byte interception window at {rva + offset:#x}")

    for slot, wrapper in [(0xFC, 0x602E0), (0x100, 0x5F9D0),
                          (0x104, 0x5FE20), (0x108, 0x5F4A0)]:
        require(struct.unpack("<I", pe.read(0x86200 + slot, 4))[0]
                == pe.base + wrapper,
                f"renderer vtable +{slot:#x} points to wrapper {wrapper:#x}")
    for call, helper in [(0x60362, 0x5DAD0), (0x602BB, 0x5D9A0),
                         (0x5FDFD, 0x5D9A0), (0x5F9B3, 0x5D9A0)]:
        require(target(pe, call) == helper, f"call {call:#x} reaches helper {helper:#x}")

    engine = PE(engine_path)
    exports = engine.exports()
    for symbol, entry, call in [
        ("?RenderGrass@TerrainRenderInterface@GAME@@EBEXABVName@2@AAVGraphicsCanvas@2@ABVGraphicsSceneRenderer@2@ABURenderablePass@2@@Z",
         0x2390B0, 0x2393A8),
        ("?RenderGrass@TerrainRenderInterfaceRT@GAME@@EBEXABVName@2@AAVGraphicsCanvas@2@ABVGraphicsSceneRenderer@2@ABURenderablePass@2@@Z",
         0x23AFC0, 0x23B265),
    ]:
        require(exports.get(symbol) == entry, f"grass export at {entry:#x}")
        require(engine.read(call, 6) == bytes.fromhex("ff92fc000000"),
                f"grass renderer call at {call:#x} uses render-device slot +0xfc")
    print("ALL RENDERER DRAW SITES VERIFIED (no files modified)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: verify-renderer-draw.py Direct3D11.dll Engine.dll")
    verify(sys.argv[1], sys.argv[2])
