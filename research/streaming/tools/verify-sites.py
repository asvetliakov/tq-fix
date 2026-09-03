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
SRC = os.environ.get("TQ_VERIFY_SRC") or os.path.join(
    HERE, "..", "..", "..", "src", "engine_probe.cpp")
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


def check_async_level_load(engine, lock_sites):
    """The two retargeted call sites, against the function they are sent to.

    `async_level_load` rests on one claim that no byte table states on its
    own: the flag the renderer tests after the call is the flag
    BackgroundLoadLevel raises. If those two offsets ever disagreed the
    renderer would draw a region whose level had not loaded, silently. So this
    reads both out of the operands -- the `cmp byte [edi+0x74],0` in the call
    site and the `mov byte [ecx+0x74],1` in BackgroundLoadLevel -- and makes
    them agree with each other and with the constant the source names.

    It also re-derives each site's call destination from the displacement in
    the table, which is the same statement as "this call goes to
    Region::LoadLevel" and is what makes retargeting it meaningful.
    """
    print("\nasync_level_load: the forced loads and where they are sent")
    sites = [(int(a, 16), int(b, 16), c) for a, b, c in re.findall(
        r"0x([0-9a-f]{6}), 0x([0-9a-f]{6}), (kForceLoad\w+Bytes)", src)]
    ok(len(sites) == 2, "two exported forced-load sites in kForceLoadSites")

    call_off = 12
    loadlevel = const("kLoadLevelRva")
    lock_by_bytes = {}
    for owner_rva, win_rva, _ in lock_sites:
        lock_by_bytes[owner_rva] = win_rva

    for owner_rva, win_rva, bytes_name in sites:
        want = table(bytes_name)
        window(engine, "%s@%#x" % (bytes_name, win_rva), bytes_name, win_rva,
               None)

        # the owner is a real export, and it is the renderer it claims to be
        got = [n for n, v in engine.exports().items() if v == owner_rva]
        ok(any(('"%s"' % n) in flat for n in got)
           and any("AddElementsInBox" in n for n in got),
           "forced-load owner at %#x is an AddElementsInBox named in the source"
           % owner_rva)

        # E8 at the recorded offset, and its displacement reaches LoadLevel
        ok(want[call_off] == 0xe8,
           "%s, offset %d, E8" % (bytes_name, call_off))
        disp = struct.unpack_from("<i", bytes(want), call_off + 1)[0]
        dest = win_rva + call_off + 5 + disp
        ok(dest == loadlevel,
           "%s call at +%d resolves to Region::LoadLevel (%#x)"
           % (bytes_name, call_off, dest))

        # test edi,edi / jz before the call: why the thunk needs no null check
        ok(want[0:2] == [0x85, 0xff] and want[2:4] == [0x0f, 0x84],
           "%s guards the Region* before the call (test edi,edi / jz)"
           % bytes_name)

        # cmp byte [edi+0x74],0 / ... / jnz -- the skip the deferral relies on
        ok(want[call_off + 5:call_off + 7] == [0x80, 0x7f]
           and want[call_off + 7] == const("kRegionLoadingOffset")
           and want[call_off + 8] == 0x00,
           "%s tests region+%#x after the call (cmp byte [edi+%#x],0)"
           % (bytes_name, const("kRegionLoadingOffset"), want[call_off + 7]))
        ok(want[-6:-4] == [0x0f, 0x85],
           "%s skips the region when that flag is set (jnz)" % bytes_name)

        # the unload countdown is cleared by a MOV, which does not touch flags,
        # so a deferred region cannot be evicted while its load is in flight
        ok(want[call_off + 9:call_off + 12] == [0xc7, 0x47, 0x6c],
           "%s clears the unload countdown on the skip path too" % bytes_name)

        # the window ends exactly where this renderer's region-lock window
        # begins, which is an address cross-check on both tables at once
        ok(lock_by_bytes.get(owner_rva) == win_rva + len(want),
           "%s ends at %#x, where the region-lock window for the same owner"
           " begins" % (bytes_name, win_rva + len(want)))

    print("\nasync_level_load: Region::BackgroundLoadLevel's behaviour")
    entry = table("kBackgroundEntryBytes")
    flags = table("kBackgroundFlagsBytes")
    tail = table("kBackgroundTailBytes")

    ok(engine.exports().get(cstr("kBackgroundLoadLevelName"))
       == const("kBackgroundLoadLevelRva"),
       "Engine!%s" % cstr("kBackgroundLoadLevelName"))

    # mov eax,[ecx+0x50] -- the field the thunk reads to decide, taken from
    # the operand of the instruction the engine itself decides on
    ok(entry[0:2] == [0x8b, 0x41]
       and entry[2] == const("kRegionLevelOffset"),
       "the level pointer is region+%#x  (mov eax,[ecx+%#x])"
       % (const("kRegionLevelOffset"), entry[2]))
    ok(flags[12:15] == [0x83, 0x79, const("kRegionLevelOffset")],
       "and BackgroundLoadLevel branches on that same field  (cmp dword"
       " [ecx+%#x],0)" % const("kRegionLevelOffset"))

    # mov dl,[esp+4] / test dl,dl / jz -- the trap: resident plus a false flag
    # returns having done nothing, which is why those calls go to the original
    ok(entry[3:7] == [0x8a, 0x54, 0x24, 0x04],
       "only the first bool is read  (mov dl,[esp+4])")
    ok(entry[10:12] == [0x85, 0xc0] and entry[12] == 0x74,
       "a non-resident region skips the flag test  (test eax,eax / jz)")
    tail_rva = const("kBackgroundTailRva")
    trap = const("kBackgroundLoadLevelRva") + 18 + entry[17]
    ok(entry[14:16] == [0x84, 0xd2] and entry[16] == 0x74
       and trap == tail_rva,
       "a resident region with a false flag jumps straight to the epilogue at"
       " %#x and does nothing" % trap)

    # cmp byte [ecx+0x74],0 / jnz and the same for 0x75: the re-entry guard
    # that is why the thunk needs no in-flight check of its own
    ok(flags[0:4] == [0x80, 0x79, const("kRegionLoadingOffset"), 0x00]
       and flags[4] == 0x75 and flags[6:10] == [0x80, 0x79, 0x75, 0x00]
       and flags[10] == 0x75,
       "it guards its own re-entry on region+%#x and +0x75"
       % const("kRegionLoadingOffset"))

    # mov byte [ecx+0x74],1 -- THE claim. This is the flag the call sites test.
    ok(flags[24:28] == [0xc6, 0x41, const("kRegionLoadingOffset"), 0x01],
       "and on the non-resident path it sets region+%#x, which is the byte"
       " both call sites skip on" % const("kRegionLoadingOffset"))

    # ret 8 -- two stack arguments, callee popped, which is the ABI the thunk
    # calls it with
    ok(tail[3:6] == [0xc2, 0x08, 0x00],
       "BackgroundLoadLevel pops two stack arguments")

    # --- Region::GuaranteedGetLevel, which run 29 named as the caller of the
    # forced load. It is hooked to find *its* caller, and the checks here are
    # about the two properties that would let Stage 5.1 be pointed at it: the
    # call sits at the same offset as the sites already patched, and the
    # function already answers "still loading" with NULL.
    print("\nRegion::GuaranteedGetLevel, the site run 29 named")
    g = table("kGuaranteedGetLevelBytes")
    call_off = const("kGuaranteedCallOffset")
    ok(g[0:6] == [0x56, 0x8b, 0xf1, 0x57, 0x85, 0xf6],
       "the six stolen bytes hold no relative branch (push/mov/push/test)")
    ok(g[6] == 0x74, "the jz at offset 6 stays in place, outside the steal")
    ok(g[call_off] == 0xe8, "GuaranteedGetLevel, offset %d, E8" % call_off)
    disp = struct.unpack_from("<i", bytes(g), call_off + 1)[0]
    dest = const("kGuaranteedGetLevelRva") + call_off + 5 + disp
    ok(dest == const("kLoadLevelRva"),
       "its call at +%d resolves to Region::LoadLevel (%#x)" % (call_off, dest))
    ok(call_off == 12,
       "and it is the same call offset as the two renderer windows")
    ok(g[call_off + 5:call_off + 9]
       == [0x80, 0x7e, const("kRegionLoadingOffset"), 0x00],
       "it tests region+%#x after the call, like the sites already patched"
       % const("kRegionLoadingOffset"))
    ok(g[call_off + 9:call_off + 12] == [0xc7, 0x46, 0x6c],
       "and clears the unload countdown with a MOV, which does not touch flags")
    # +5 cmp (4 bytes), +9 mov (7 bytes), so the branch is at +16
    ok(g[call_off + 16] == 0x74
       and g[call_off + 18:call_off + 20] == [0x33, 0xff],
       "still loading falls through to xor edi,edi -- it already returns NULL")
    ok(g[-3:] == [0xc2, 0x04, 0x00],
       "GuaranteedGetLevel pops one stack argument")

    # --- The portal-traversal site. Not exported, so the bytes and the
    # relocated EnterCriticalSection slot are its whole identity -- and its
    # call sits at offset 8, not 12, because the guard above it is the
    # two-byte jz rather than the six-byte form the renderers use. Getting
    # that offset wrong would rewrite four bytes in the middle of a
    # displacement that is still live.
    print("\nThe portal-traversal forced load (run 30's in-play event)")
    window(engine, "kPortalLoadBytes", "kPortalLoadBytes",
           const("kPortalLoadWindowRva"), "kPortalLoadRelocs")
    pl = table("kPortalLoadBytes")
    poff = const("kPortalLoadCallOffset")
    ok(poff == 8 and pl[poff] == 0xe8, "kPortalLoadBytes, offset %d, E8" % poff)
    pdest = const("kPortalLoadWindowRva") + poff + 5 + struct.unpack_from(
        "<i", bytes(pl), poff + 1)[0]
    ok(pdest == const("kLoadLevelRva"),
       "its call at +%d resolves to Region::LoadLevel (%#x)" % (poff, pdest))
    ok(pl[0:2] == [0x85, 0xf6] and pl[2] == 0x74,
       "it guards the Region* with the two-byte jz that shortens the offset")
    ok(pl[poff + 5:poff + 9]
       == [0x80, 0x7e, const("kRegionLoadingOffset"), 0x00],
       "it tests region+%#x after the call, like the other sites"
       % const("kRegionLoadingOffset"))
    ok(pl[poff + 9:poff + 12] == [0xc7, 0x46, 0x6c],
       "and clears the unload countdown with a MOV")
    ok(pl[poff + 16] == 0x75, "and branches away when that flag is set")
    # Region::GetPortal's result is null-checked two instructions past the
    # branch target. That is what makes this site safer to defer than either
    # of the two already patched, so it is asserted rather than remembered.
    guard = list(engine.read(engine.base + 0x117ac8, 9))
    ok(guard[0] == 0xe8 and guard[5:7] == [0x85, 0xc0] and guard[7] == 0x74,
       "and Region::GetPortal's result is null-checked at 0x10117acd")


def check_directional_shadow(engine):
    """Re-derive the call target and the object field the wrapper reads."""
    print("\nDirectional-shadow attribution")
    call = table("kShadowCallWindowBytes")
    call_rva = const("kShadowCallWindowRva")
    call_off = const("kShadowCallOffset")
    target_rva = const("kRenderDirectionalRva")

    ok(16 <= len(call) <= 24,
       "directional call verifies %d bytes (required 16-24)" % len(call))
    ok(call_off + 5 <= len(call) and call[call_off] == 0xe8,
       "directional call offset %d lands on E8" % call_off)
    dest = call_rva + call_off + 5 + struct.unpack_from(
        "<i", bytes(call), call_off + 1)[0]
    ok(dest == target_rva,
       "directional call resolves to GraphicsShadowMapDx11::RenderDirectional"
       " (%#x)" % dest)
    ok(call[0:5] == [0x50, 0x8d, 0x43, 0x28, 0x50]
       and call[5:9] == [0xff, 0x74, 0x24, 0x3c]
       and call[9:16] == [0x8d, 0x8c, 0x24, 0xbc, 0x00, 0x00, 0x00],
       "call window carries frustum, camera, canvas and shadow-map self setup")

    field = table("kShadowRegionConstructorBytes")
    field_off = const("kShadowRegionOffset")
    ok(16 <= len(field) <= 24,
       "shadow region field verifies %d bytes (required 16-24)" % len(field))
    ok(field[11:13] == [0x89, 0x4e] and field[13] == field_off,
       "constructor writes its region argument to shadow map+%#x" % field_off)

    name = cstr("kRenderDirectionalName")
    ok(engine.exports().get(name) == target_rva,
       "Engine!%s" % name[:58])

    argument = table("kShadowOutputArgumentBytes")
    copy = table("kShadowOutputCopyBytes")
    dwords = const("kShadowMatrixDwords")
    ok(argument[7:10] == [0x8b, 0x45, 0x1c]
       and argument[17:21] == [0x89, 0x44, 0x24, 0x7c],
       "RenderDirectional saves its seventh Mat4& argument at stack+0x7c")
    ok(copy[0:7] == [0x8b, 0xbc, 0x24, 0x80, 0x00, 0x00, 0x00]
       and copy[7:9] == [0x8b, 0xf0]
       and copy[9] == 0xb9
       and struct.unpack_from("<I", bytes(copy), 10)[0] == dwords
       and copy[14:16] == [0xf3, 0xa5],
       "matrix output copies %d dwords (%d bytes) with rep movsd"
       % (dwords, dwords * 4))
    ok(argument[20] + 4 == copy[3],
       "the saved output pointer moves from stack+%#x to +%#x after one push"
       % (argument[20], copy[3]))


def check_resource_lifecycle(engine):
    """Prove the two Resource fields sampled before a shadow-forced load."""
    print("\nShadow-resource lifecycle")
    state = table("kResourceLoadedStateBytes")
    state_offset = const("kResourceLoadedStateOffset")
    queue = table("kResourceInQueueBytes")
    queue_offset = const("kResourceInQueueOffset")
    ok(len(state) == 16 and state[0:2] == [0x8b, 0x41]
       and state[2] == state_offset and state[3] == 0xc3,
       "Resource::GetLoadedState returns resource+%#x" % state_offset)
    ok(len(queue) == 16 and queue[0:2] == [0x33, 0xc0]
       and queue[2:4] == [0x39, 0x41] and queue[4] == queue_offset
       and queue[5:9] == [0x0f, 0x95, 0xc0, 0xc3],
       "Resource::GetInLoadingQueue tests resource+%#x" % queue_offset)
    ok(engine.exports().get(cstr("kResourceLoadedStateName"))
       == const("kResourceLoadedStateRva"),
       "loaded-state accessor export resolves to its recorded RVA")
    ok(engine.exports().get(cstr("kResourceInQueueName"))
       == const("kResourceInQueueRva"),
       "in-queue accessor export resolves to its recorded RVA")


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
            ("kResourceLoadedStateBytes", "kResourceLoadedStateRva", None),
            ("kResourceInQueueBytes", "kResourceInQueueRva", None),
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
            ("kEngineRenderBytes", "kEngineRenderRva", "kEngineRenderRelocs"),
            ("kShadowCallWindowBytes", "kShadowCallWindowRva", None),
            ("kShadowRegionConstructorBytes", "kShadowRegionConstructorRva",
             None),
            ("kShadowOutputArgumentBytes", "kShadowOutputArgumentRva", None),
            ("kShadowOutputCopyBytes", "kShadowOutputCopyRva", None),
            ("kGuaranteedGetLevelBytes", "kGuaranteedGetLevelRva", None),
            ("kBackgroundEntryBytes", "kBackgroundLoadLevelRva", None),
            ("kBackgroundFlagsBytes", "kBackgroundFlagsRva", None),
            ("kBackgroundTailBytes", "kBackgroundTailRva", None)]:
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
            (engine, "Engine", "kResourceLoadedStateName",
             "kResourceLoadedStateRva"),
            (engine, "Engine", "kResourceInQueueName",
             "kResourceInQueueRva"),
            (engine, "Engine", "kUnloadLevelName", "kUnloadLevelRva"),
            (engine, "Engine", "kEnqueueName", "kEnqueueRva"),
            (engine, "Engine", "kReadFromFileName", "kReadFromFileRva"),
            (engine, "Engine", "kWaitForLoadingName", "kWaitForLoadingRva"),
            (engine, "Engine", "kBackgroundLoadLevelName",
             "kBackgroundLoadLevelRva"),
            (engine, "Engine", "kGuaranteedGetLevelName",
             "kGuaranteedGetLevelRva"),
            (engine, "Engine", "kEngineUpdateName", "kEngineUpdateRva"),
            (engine, "Engine", "kEngineRenderName", "kEngineRenderRva"),
            (engine, "Engine", "kSweepTargetName", "kSweepTargetRva"),
            (engine, "Engine", "kRenderDirectionalName",
             "kRenderDirectionalRva"),
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
    check_async_level_load(engine, sites)
    check_directional_shadow(engine)
    check_resource_lifecycle(engine)

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
