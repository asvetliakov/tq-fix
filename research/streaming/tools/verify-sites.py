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
CACHE_H = os.path.join(HERE, "..", "..", "..", "src", "arc_cache.h")

src = open(SRC).read()
cache_src = open(CACHE_H).read()
flat = re.sub(r'"\s*\n\s*"', '', src)          # joined string literals
failures = []


def ok(good, what):
    print("  %-4s %s" % ("OK" if good else "FAIL", what))
    if not good:
        failures.append(what)
    return good


def const(name, text=None):
    """A `const DWORD/unsigned/uint32_t <name> = <n>;` from the source."""
    text = src if text is None else text
    m = re.search(r"const (?:DWORD|unsigned|uint32_t) %s = (0x[0-9a-fA-F]+|\d+);"
                  % name, text)
    return int(m.group(1), 0)


def table(name):
    m = re.search(r"const BYTE %s\[\] = \{(.*?)\};" % name, src, re.S)
    body = re.sub(r"//[^\n]*", "", m.group(1))
    return [int(t, 0) for t in re.findall(r"0x[0-9a-fA-F]+|(?<![\w.])\d+", body)]


NAMED_RVA = {"kEnterCriticalSectionSlotRva": 0x2ac17c,
             "kLeaveCriticalSectionSlotRva": 0x2ac178,
             "kWaitForSingleObjectSlotRva": 0x2ac188,
             "kSetFilePointerExSlotRva": 0x2ac190,
             "kReadFileSlotRva": 0x2ac1a4}


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


def check_archive_cache(engine):
    """The offsets src/arc_cache.cpp keys on, against the operands that use them.

    The cache reads five fields out of structures the game never names, and
    every one of them is an immediate inside an instruction in the block
    routine. This is where the C++ constants and the instruction stream are
    made to agree: nothing here trusts the byte tables to *mean* anything, it
    reads the operand out of the table and compares it to the constant
    describeBlock() dereferences with.
    """
    print("\nArchive cache offsets, against the operands that use them")
    prologue = table("kArchiveBlockBytes")
    seek = table("kArchiveSeekWindowBytes")
    read = table("kArchiveReadWindowBytes")
    inflate = table("kArchiveInflateWindowBytes")
    tail = table("kArchiveBlockTailBytes")
    size = table("kArchiveBlockSizeBytes")

    # mov eax,[ecx+0x2c] -- the archive's entry table
    ok(prologue[12:14] == [0x8b, 0x41]
       and prologue[14] == const("kArchiveEntryTableOffset"),
       "entry table at archive+%#x  (mov eax,[ecx+%#x])"
       % (const("kArchiveEntryTableOffset"), prologue[14]))

    # shl edx,4 / add edx,[ebp+8] / lea esi,[eax+edx*4] -- stride (16+1)*4
    stride_ok = (prologue[15:18] == [0xc1, 0xe2, 0x04]
                 and prologue[18:21] == [0x03, 0x55, 0x08]
                 and prologue[26:29] == [0x8d, 0x34, 0x90])
    ok(stride_ok and ((1 << prologue[17]) + 1) * 4 == const("kArchiveEntryStride"),
       "entry record stride %#x  (shl %d / add / lea [eax+edx*4])"
       % (const("kArchiveEntryStride"), prologue[17]))

    # mov eax,[esi+0x20] -- this entry's block descriptors
    ok(prologue[33:35] == [0x8b, 0x46]
       and prologue[35] == const("kArchiveEntryDescriptorsOffset"),
       "descriptors at entry+%#x  (mov eax,[esi+%#x])"
       % (const("kArchiveEntryDescriptorsOffset"), prologue[35]))

    # lea ecx,[ebx+ebx*2] / lea edi,[eax+ecx*4] -- descriptor stride 3*4
    ok(prologue[36:39] == [0x8d, 0x0c, 0x5b]
       and prologue[40:43] == [0x8d, 0x3c, 0x88]
       and const("kArchiveDescriptorStride") == 12,
       "descriptor stride %d  (lea [ebx+ebx*2] / lea [eax+ecx*4])"
       % const("kArchiveDescriptorStride"))

    # push [eax+0xc] in both the seek and the read -- the open .arc HANDLE
    ok(seek[12:14] == [0xff, 0x70]
       and seek[14] == const("kArchiveHandleOffset")
       and read[21:23] == [0xff, 0x70]
       and read[23] == const("kArchiveHandleOffset"),
       "file handle at archive+%#x  (push [eax+%#x], seek and read)"
       % (const("kArchiveHandleOffset"), seek[14]))

    # push [edi] -- the descriptor's offset, its first dword
    ok(seek[10:12] == [0xff, 0x37], "descriptor offset at +0  (push [edi])")

    # push [ecx+4] / mov eax,[ecx+8] -- the two sizes, as uncompress's
    # sourceLen and destLen
    ok(inflate[0:3] == [0xff, 0x71, 0x04] and inflate[3:6] == [0x8b, 0x41, 0x08],
       "descriptor sizes at +4 and +8  (uncompress sourceLen and destLen)")

    # mov ecx,[edi+8] -- the buffer the inflate writes, which is the block
    ok(inflate[9:12] == [0x8b, 0x4f, 0x08],
       "the inflate writes blockBuffer+8, which is what a hit fills")

    # mov [edi],ebx / mov al,1 / ret 0xc -- the contract a hit reproduces
    ok(tail[0:2] == [0x89, 0x1f], "a hit must write blockBuffer+0 = block index")
    ok(tail[4:6] == [0xb0, 0x01], "a hit must return 1 in AL")
    ok(tail[10:13] == [0xc2, 0x0c, 0x00], "the routine pops three stack arguments")

    # mov [esi+0x40],0x40000 -- the only writer of the block size
    ok(size[0:2] == [0xc7, 0x46] and size[2] == const("kArchiveBlockSizeOffset"),
       "block size at archive+%#x" % const("kArchiveBlockSizeOffset"))
    coded = struct.unpack_from("<I", bytes(size), 3)[0]
    ok(coded == const("kSlotBytes", cache_src),
       "a cache slot is %#x bytes, which is the block size the engine sets"
       % coded)

    # the two relocated dwords are the syscalls whose operands name the fields
    imports = engine.imports()
    for slot, want in (("kSetFilePointerExSlotRva", "SetFilePointerEx"),
                       ("kReadFileSlotRva", "ReadFile")):
        rva = const(slot)
        got = imports.get(engine.base + rva)
        ok(got is not None and got[1] == want,
           "Engine.dll+%#x is KERNEL32!%s" % (rva, want))


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
            ("kArchiveSeekWindowBytes", "kArchiveSeekWindowRva",
             "kArchiveSeekWindowRelocs"),
            ("kArchiveReadWindowBytes", "kArchiveReadWindowRva",
             "kArchiveReadWindowRelocs"),
            ("kArchiveInflateWindowBytes", "kArchiveInflateWindowRva", None),
            ("kArchiveBlockTailBytes", "kArchiveBlockTailRva", None),
            ("kArchiveBlockSizeBytes", "kArchiveBlockSizeRva", None),
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
    inflate = table("kArchiveInflateWindowBytes")
    ok(inflate[20] == 0xe8, "archive inflate window, offset 20, E8")

    check_archive_cache(engine)

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
    for n in ["PeekMessageA", "DispatchMessageA", "SetFilePointerEx", "ReadFile",
              "WaitForMultipleObjects"]:
        ok(n in engine_imports and ('"%s"' % n) in flat, "Engine.dll imports %s" % n)

    # The four import instruments added for the heap and archive-I/O groups
    # assert an RVA beside the name they resolve by; check both agree, and that
    # the decorated allocator names are the ones MSVCR110 actually exports.
    print("\nImport slots the heap and archive-I/O groups assert")
    imports = engine.imports()
    for name_const, rva_const, dll in [
            ("kNewArrayName", "kNewArraySlotRva", "MSVCR110.dll"),
            ("kDeleteArrayName", "kDeleteArraySlotRva", "MSVCR110.dll")]:
        name = cstr(name_const)
        rva = const(rva_const)
        got = imports.get(engine.base + rva)
        ok(got is not None and got[1] == name and got[0].lower() == dll.lower(),
           "Engine.dll+%#x is %s!%s" % (rva, dll, name))
    for name, rva_const in [("SetFilePointerEx", "kSetFilePointerExSlotRva"),
                            ("ReadFile", "kReadFileSlotRva"),
                            ("Sleep", "kSleepSlotRva"),
                            ("WaitForMultipleObjects",
                             "kWaitForMultipleObjectsSlotRva"),
                            ("EnterCriticalSection",
                             "kEnterCriticalSectionSlotRva"),
                            ("WaitForSingleObject",
                             "kWaitForSingleObjectSlotRva")]:
        rva = const(rva_const)
        got = imports.get(engine.base + rva)
        ok(got is not None and got[1] == name,
           "Engine.dll+%#x is KERNEL32!%s" % (rva, name))

    print("\n%s" % ("ALL SITES VERIFIED" if not failures
                    else "%d FAILURE(S)" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
