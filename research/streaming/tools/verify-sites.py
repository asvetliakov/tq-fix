#!/usr/bin/env python3
"""Check every byte table in src/engine_probe.cpp against the pinned binaries.

`findings.md` records the patch sites, but a record is not a licence: this
reads the tables out of the source as they are actually compiled and compares
them to the installed game, resolving relocated dwords the way
`detour::matches` does at runtime. Run it after touching any table, and after
any game update.

It found two real bugs the first time it was run: a relocation offset that was
one byte short, which would have silently skipped all three region-lock hooks,
and a `moduleText` check that failed for every hook after the first.

    TQ_GAME_DIR='/path/to/Titan Quest - Anniversary Edition' \
      research/streaming/tools/verify-sites.py
"""
import os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from pe import PE

GAME = os.environ.get("TQ_GAME_DIR") or os.path.expanduser(
    "~/Library/Application Support/CrossOver/Bottles/Titan Quest/drive_c/"
    "GOG Games/Titan Quest - Anniversary Edition")
SRC = os.path.join(HERE, "..", "..", "..", "src", "engine_probe.cpp")

src = open(SRC).read()
flat = re.sub(r'"\s*\n\s*"', '', src)          # joined string literals
failures = []


def ok(good, what):
    print("  %-4s %s" % ("OK" if good else "FAIL", what))
    if not good:
        failures.append(what)
    return good


def const(name):
    return int(re.search(r"const DWORD %s = (0x[0-9a-fA-F]+);" % name, src).group(1), 0)


def table(name):
    m = re.search(r"const BYTE %s\[\] = \{(.*?)\};" % name, src, re.S)
    body = re.sub(r"//[^\n]*", "", m.group(1))
    return [int(t, 0) for t in re.findall(r"0x[0-9a-fA-F]+|(?<![\w.])\d+", body)]


NAMED_RVA = {"kEnterCriticalSectionSlotRva": 0x2ac17c,
             "kLeaveCriticalSectionSlotRva": 0x2ac178,
             "kWaitForSingleObjectSlotRva": 0x2ac188}


def relocs(name):
    if not name:
        return []
    m = re.search(r"const Relocation %s\[\] = \{(.*?)\};" % name, src, re.S)
    out = []
    for off, r in re.findall(r"\{\s*(\w+)\s*,\s*(\w+)\s*\}", m.group(1)):
        out.append((int(off, 0), NAMED_RVA[r] if r in NAMED_RVA else int(r, 0)))
    return out


def cstr(name):
    m = re.search(r'const char %s\[\] =\s*((?:"(?:[^"\\]|\\.)*"\s*)+);' % name, src, re.S)
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)))


def window(pe, label, bytes_name, at, reloc_name):
    want = table(bytes_name)
    got = list(pe.read(pe.base + at, len(want)))
    skip, why = set(), []
    for off, r in relocs(reloc_name):
        if off + 4 > len(want):
            why.append("relocation at %d runs past the %d-byte table" % (off, len(want)))
            continue
        value = struct.unpack_from("<I", bytes(got), off)[0]
        if value != pe.base + r:
            why.append("reloc@%d = %#x, expected %#x" % (off, value, pe.base + r))
        skip.update(range(off, off + 4))
    for i in range(len(want)):
        if i not in skip and want[i] != got[i]:
            why.append("byte %d: table %02x, image %02x" % (i, want[i], got[i]))
    ok(not why, "%-28s @ %#010x %2d bytes%s"
       % (label, pe.base + at, len(want), "" if not why else "  -- " + "; ".join(why)))


def main():
    engine = PE(os.path.join(GAME, "Engine.dll"))
    game = PE(os.path.join(GAME, "Game.dll"))
    exe = PE(os.path.join(GAME, "TQ.exe"))

    print("Image identity")
    ok(engine.imagesize == const("kEngineImageSize"), "Engine.dll SizeOfImage %#x" % engine.imagesize)
    ok(game.imagesize == const("kGameImageSize"), "Game.dll SizeOfImage %#x" % game.imagesize)
    ok(exe.imagesize == const("kExecutableImageSize"), "TQ.exe SizeOfImage %#x" % exe.imagesize)

    print("\nEngine.dll byte tables")
    for label, rva, rel in [
            ("kLoadLevelBytes", "kLoadLevelRva", None),
            ("kLoadResourceBytes", "kLoadResourceRva", "kLoadResourceRelocs"),
            ("kUnloadLevelBytes", "kUnloadLevelRva", "kUnloadLevelRelocs"),
            ("kEnqueueBytes", "kEnqueueRva", "kEnqueueRelocs"),
            ("kReadFromFileBytes", "kReadFromFileRva", None),
            ("kArchiveBlockBytes", "kArchiveBlockRva", None),
            ("kArchiveInflateCallBytes", "kArchiveInflateCallRva", None),
            ("kWaitForLoadingBytes", "kWaitForLoadingRva", None),
            ("kFenceWindowBytes", "kFenceWindowRva", "kFenceWindowRelocs"),
            ("kSweepWindowABytes", "kSweepWindowARva", None),
            ("kSweepWindowBBytes", "kSweepWindowBRva", None),
            ("kEngineUpdateBytes", "kEngineUpdateRva", "kEngineUpdateRelocs"),
            ("kEngineRenderBytes", "kEngineRenderRva", "kEngineRenderRelocs")]:
        window(engine, label, label, const(rva), rel)

    # the three region-lock windows share two byte tables, so their addresses
    # come from the kLockSites table rather than from a named constant
    print("\nEngine.dll region-lock windows")
    sites = [(int(a, 16), int(b, 16), c) for a, b, c in re.findall(
        r"0x([0-9a-f]{6}), 0x([0-9a-f]{6}), (kRegionLock\w+)", src)]
    ok(len(sites) == 3, "three lock sites in kLockSites")
    for owner_rva, win_rva, bytes_name in sites:
        window(engine, "%s@%#x" % (bytes_name, win_rva), bytes_name,
               win_rva, "kRegionLockRelocs")

    print("\nGame.dll byte tables")
    window(game, "kGameUpdateBytes", "kGameUpdateBytes", const("kGameUpdateRva"),
           "kGameUpdateRelocs")

    print("\nExported targets resolve to the recorded RVAs")
    for pe, label, name_const, rva_const in [
            (engine, "Engine", "kLoadLevelName", "kLoadLevelRva"),
            (engine, "Engine", "kLoadResourceName", "kLoadResourceRva"),
            (engine, "Engine", "kUnloadLevelName", "kUnloadLevelRva"),
            (engine, "Engine", "kEnqueueName", "kEnqueueRva"),
            (engine, "Engine", "kReadFromFileName", "kReadFromFileRva"),
            (engine, "Engine", "kWaitForLoadingName", "kWaitForLoadingRva"),
            (engine, "Engine", "kEngineUpdateName", "kEngineUpdateRva"),
            (engine, "Engine", "kEngineRenderName", "kEngineRenderRva"),
            (engine, "Engine", "kSweepTargetName", "kSweepTargetRva"),
            (game, "Game", "kGameUpdateName", "kGameUpdateRva")]:
        name = cstr(name_const)
        ok(pe.exports().get(name) == const(rva_const),
           "%s!%s" % (label, name[:58]))

    for owner_rva, win_rva, _ in sites:
        # kLockSites pairs a decorated owner name with its RVA; match by value
        got = [n for n, v in engine.exports().items() if v == owner_rva]
        ok(any(('"%s"' % n) in flat for n in got),
           "lock-site owner at %#x is named in the source" % owner_rva)

    print("\nCall offsets land on call opcodes")
    lock = table("kRegionLockEbxBytes")
    ok(lock[3] == 0xff and lock[4] == 0x15, "region lock, offset 3, FF 15")
    fence = table("kFenceWindowBytes")
    ok(fence[8] == 0xff and fence[9] == 0x15, "loader fence, offset 8, FF 15")
    a, b = table("kSweepWindowABytes"), table("kSweepWindowBBytes")
    for off in (3, 11, 19, 27):
        ok(a[off] == 0xe8, "sweep window A, offset %d, E8" % off)
    for off in (3, 14, 28):
        ok(b[off] == 0xe8, "sweep window B, offset %d, E8" % off)

    print("\nImport-table targets exist in TQ.exe and Engine.dll")
    exe_imports = {n for _, (_, n) in exe.imports().items()}
    engine_imports = {n for _, (_, n) in engine.imports().items()}
    for n in ["Sleep", "WaitForSingleObject", "GetMessageA", "SetTimer",
              "THQNO_Process", "?PresentSurface@Engine@GAME@@QAEXXZ",
              "?UpdateFromOptions@GraphicsEngine@GAME@@QAEXXZ",
              "?Update@Jukebox@GAME@@QAEXXZ",
              "?Update@SoundManager@GAME@@QAEXPBVWorldFrustum@2@@Z",
              "?FireTriggers@QuestRepository@GAME@@QAEXXZ",
              "?ProcessMessages@EWindow@GAME@@QAE_NXZ",
              "?FixupCharacterCollisions@InterpenetrationManager@GAME@@QAEXABVGameCamera@2@@Z"]:
        ok(n in exe_imports and ('"%s"' % n) in flat, "TQ.exe imports %s" % n[:58])
    for n in ["PeekMessageA", "DispatchMessageA"]:
        ok(n in engine_imports and ('"%s"' % n) in flat, "Engine.dll imports %s" % n)

    print("\n%s" % ("ALL SITES VERIFIED" if not failures
                    else "%d FAILURE(S)" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
