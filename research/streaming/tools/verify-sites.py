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


def dword_table(name):
    m = re.search(r"const DWORD %s\[\] = \{(.*?)\};" % name, src, re.S)
    body = re.sub(r"//[^\n]*", "", m.group(1))
    return [int(t, 0) for t in re.findall(
        r"0x[0-9a-fA-F]+|(?<![\w.])\d+", body)]


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
        target = (NAMED_RVA[r] if r in NAMED_RVA else
                  int(r, 0) if re.match(r"^(?:0x|\d)", r) else const(r))
        out.append((int(off, 0), target))
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
    """Prove Resource fields and the filename used for the type partition."""
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
    filename = table("kResourceFileNameBytes")
    ok(len(filename) == 16
       and filename[0:4] == [0x83, 0x79, 0x20, 0x10]
       and filename[4:7] == [0x8d, 0x41, 0x0c]
       and filename[7:12] == [0x72, 0x02, 0x8b, 0x00, 0xc3],
       "Resource::GetFileName returns the verified MSVC string at resource+0xc")
    ok(engine.exports().get(cstr("kResourceFileNameName"))
       == const("kResourceFileNameRva"),
       "filename accessor export resolves to its recorded RVA")


def check_shadow_mesh_boundary(engine):
    """Prove the per-mesh omission/preload boundary and its one patched call."""
    print("\nDirectional-shadow cold-mesh boundary")
    code = table("kShadowMeshPassCountBytes")
    at = const("kShadowMeshPassCountRva")
    off = const("kShadowMeshEnsureCallOffset")
    ensure = const("kEnsureAvailableRva")
    ok(len(code) == 24,
       "GetNumShadowRenderPasses verifies all %d bytes (required 16-24)"
       % len(code))
    ok(code[0:10] == [0x56, 0x8b, 0x71, 0x04, 0x85, 0xf6, 0x74, 0x0c,
                      0x8b, 0xce],
       "the boundary obtains GraphicsMeshInstance+4 and null-checks it")
    ok(off + 5 <= len(code) and code[off] == 0xe8,
       "cold-mesh call offset %d lands on E8" % off)
    dest = at + off + 5 + struct.unpack_from("<i", bytes(code), off + 1)[0]
    ok(dest == ensure,
       "the boundary call resolves to Resource::EnsureAvailable (%#x)" % dest)
    ok(code[15:20] == [0x8b, 0x46, 0x7c, 0x5e, 0xc3]
       and code[20:24] == [0x33, 0xc0, 0x5e, 0xc3],
       "after EnsureAvailable it returns mesh+0x7c; null returns zero passes")
    ok(engine.exports().get(cstr("kShadowMeshPassCountName")) == at,
       "GetNumShadowRenderPasses export resolves to its recorded RVA")
    ok(engine.exports().get(cstr("kEnsureAvailableName")) == ensure,
       "EnsureAvailable export resolves to its recorded RVA")


def check_shadow_material_textures(engine):
    """Prove the two adjacent material-texture calls and use query."""
    print("\nDirectional-shadow material textures")
    getter = table("kShadowMaterialTextureWindowBytes")
    getter_at = const("kShadowMaterialTextureWindowRva")
    getter_off = const("kShadowMaterialTextureCallOffset")
    getter_target = const("kGraphicsTextureGetTextureRva")
    setter = table("kShadowTextureParameterWindowBytes")
    setter_at = const("kShadowTextureParameterWindowRva")
    setter_off = const("kShadowTextureParameterCallOffset")
    setter_target = const("kSetTextureParameterRva")
    owner = const("kGraphicsMeshSetShaderParametersRva")
    mesh_frame = table("kGraphicsMeshSetShaderParametersFrameBytes")
    loop_frame = table("kShadowMaterialLoopFrameBytes")

    ok(16 <= len(mesh_frame) <= 24
       and mesh_frame[0:8] == [0x83, 0xec, 0x08, 0x53, 0x55, 0x56,
                               0x8b, 0xf1],
       "mesh material frame allocates 8 bytes and saves EBX/EBP/ESI")
    ok(16 <= len(loop_frame) <= 24 and loop_frame[13] == 0x57,
       "mesh material loop saves EDI before entering the texture cases")
    outer_stack = const("kShadowMaterialOuterCallerStackOffset")
    shader_stack = const("kShadowMaterialShaderStackOffset")
    derived_outer_stack = mesh_frame[2] + 4 * 4 + 4
    ok(outer_stack == derived_outer_stack == 0x1c
       and shader_stack == outer_stack + 4,
       "getter adapter stack offsets retain enclosing caller and shader")

    ok(16 <= len(getter) <= 24,
       "material texture getter verifies %d bytes (required 16-24)"
       % len(getter))
    ok(getter[0:3] == [0x8b, 0x4e, 0x14],
       "material getter receives the type-7 entry's resource at +0x14")
    ok(getter_off + 5 <= len(getter) and getter[getter_off] == 0xe8,
       "material texture call offset %d lands on E8" % getter_off)
    dest = getter_at + getter_off + 5 + struct.unpack_from(
        "<i", bytes(getter), getter_off + 1)[0]
    ok(dest == getter_target,
       "material call resolves to GraphicsTexture::GetTexture (%#x)" % dest)

    ok(16 <= len(setter) <= 24,
       "texture-parameter setter verifies %d bytes (required 16-24)"
       % len(setter))
    ok(setter[0:9] == [0x50, 0x51, 0x8b, 0x4c, 0x24, 0x24,
                       0x6a, 0x00, 0x56],
       "setter receives texture output, shadow shader, and material Name")
    ok(setter[5] - 8 + 4 == shader_stack,
       "getter adapter's shader is at entry ESP+%#x" % shader_stack)
    ok(setter_off + 5 <= len(setter) and setter[setter_off] == 0xe8,
       "texture-parameter call offset %d lands on E8" % setter_off)
    dest = setter_at + setter_off + 5 + struct.unpack_from(
        "<i", bytes(setter), setter_off + 1)[0]
    ok(dest == setter_target,
       "texture-parameter call resolves to its original setter (%#x)" % dest)
    ok(getter_at + len(getter) == setter_at
       and owner < getter_at < setter_at,
       "getter and setter windows are adjacent inside GraphicsMesh material setup")

    has = table("kShaderHasParameterBytes")
    has_at = const("kShaderHasParameterRva")
    name_hash = table("kNameHashBytes")
    name_hash_at = const("kNameHashRva")
    ensure = const("kEnsureAvailableRva")
    ok(16 <= len(has) <= 24,
       "HasParameter verifies %d bytes (required 16-24)" % len(has))
    ok(has[4] == 0xe8,
       "HasParameter's shader-residency call lands on E8")
    dest = has_at + 9 + struct.unpack_from("<i", bytes(has), 5)[0]
    ok(dest == ensure,
       "HasParameter first ensures the shader resource (%#x)" % dest)
    ok(has[9:18] == [0xff, 0x74, 0x24, 0x0c,
                      0x8d, 0x44, 0x24, 0x10, 0x50]
       and has[18:24] == [0x8d, 0x8e, 0xa0, 0x00, 0x00, 0x00],
       "HasParameter looks up the supplied Name in shader+0xa0")
    ok(engine.exports().get(cstr("kGraphicsMeshSetShaderParametersName"))
       == owner,
       "GraphicsMesh::SetShaderParameters export owns both call sites")
    ok(engine.exports().get(cstr("kGraphicsTextureGetTextureName"))
       == getter_target,
       "GraphicsTexture::GetTexture export resolves to its recorded RVA")
    ok(engine.exports().get(cstr("kShaderHasParameterName")) == has_at,
       "GraphicsShader2::HasParameter export resolves to its recorded RVA")
    ok(len(name_hash) == 16 and name_hash[0:3] == [0x8b, 0x01, 0xc3]
       and name_hash[3:] == [0xcc] * 13,
       "Name::Hash proves the material Name digest starts at offset zero")
    ok(engine.exports().get(cstr("kNameHashName")) == name_hash_at,
       "Name::Hash export resolves to its recorded RVA")

    adapter = re.search(
        r"hookShadowMaterialTexture\(.*?__asm__ __volatile__\((.*?)\);",
        src, re.S)
    adapter_text = adapter.group(1) if adapter else ""
    ok(('"pushl %#x(%%%%esp)' % outer_stack) in adapter_text
       and ('"pushl %#x(%%%%esp)' % (shader_stack + 4)) in adapter_text
       and '"addl $16, %%esp' in adapter_text,
       "material adapter forwards its verified enclosing caller and shader")

    report_begin = src.find("void reportShadowMaterialDependency(")
    report_end = src.find("\n}\n\nvoid flushPendingShadowMaterialTexture",
                          report_begin)
    report = src[report_begin:report_end]
    flush_begin = src.find("void flushPendingShadowMaterialTexture(")
    flush_end = src.find("\n}\n\nextern \"C\" void* __cdecl",
                         flush_begin)
    flush = src[flush_begin:flush_end]
    ok(report_begin >= 0 and report_end > report_begin
       and "g_shadowMaterialReports" in report
       and "report >= (LONG)kChainSlots" in report
       and "Name::Hash=%#lx" in report
       and flush_begin >= 0 and flush_end > flush_begin
       and "if (known && used)" in flush
       and "reportShadowMaterialDependency(" in flush,
       "cold used material identities are logged live and bounded")


def check_shadow_texture_attribution(engine):
    """Prove the instance/pass context call and exhaustive direct callers."""
    print("\nDirectional-shadow texture dependency attribution")
    frame = table("kShadowMeshParameterFrameBytes")
    entry = table("kShadowMeshParameterEntryBytes")
    context = table("kShadowMeshParameterContextBytes")
    args = table("kShadowMeshParameterArgsBytes")
    call = table("kShadowMeshParameterCallBytes")
    call_at = const("kShadowMeshParameterCallRva")
    call_off = const("kShadowMeshParameterCallOffset")
    mesh_setter = const("kGraphicsMeshSetShaderParametersRva")
    instance_setter = const("kGraphicsMeshInstanceSetShaderParametersRva")
    frame_relocs = relocs("kShadowMeshParameterFrameRelocs")

    ok(16 <= len(frame) <= 24,
       "mesh-parameter frame verifies %d bytes (required 16-24)" % len(frame))
    ok(frame[6] == 0xa1
       and struct.unpack_from("<I", bytes(frame), 7)[0]
           == engine.base + 0x41b044
       and frame_relocs == [(7, 0x41b044)],
       "frame A1 operand has the exact runtime relocation descriptor")
    ok(frame[0:6] == [0x81, 0xec, 0x8c, 0, 0, 0]
       and frame[11] == 0x53
       and frame[12:19] == [0x8b, 0x9c, 0x24, 0x94, 0, 0, 0],
       "frame allocates 0x8c locals, saves EBX, and loads shader arg1")
    ok(16 <= len(entry) <= 24,
       "mesh-parameter entry verifies %d bytes (required 16-24)" % len(entry))
    ok(entry[0] == 0x53
       and entry[1:8] == [0x8b, 0x9c, 0x24, 0x94, 0, 0, 0]
       and entry[8] == 0x55
       and entry[9:16] == [0x8b, 0xac, 0x24, 0xa0, 0, 0, 0],
       "entry keeps shader arg1 in EBX and pass arg3 in EBP")
    ok(16 <= len(context) <= 24,
       "mesh-parameter context verifies %d bytes (required 16-24)"
       % len(context))
    ok(context[0:7] == [0x8b, 0xac, 0x24, 0xa0, 0, 0, 0]
       and context[7:9] == [0x56, 0x57]
       and context[9:16] == [0x8b, 0xbc, 0x24, 0xac, 0, 0, 0]
       and context[16:18] == [0x8b, 0xf1],
       "context keeps pass in EBP, render info in EDI, and instance in ESI")
    ok(16 <= len(args) <= 24,
       "mesh material arguments verify %d bytes (required 16-24)" % len(args))
    ok(args[0:7] == [0xff, 0xb4, 0x24, 0xa4, 0, 0, 0]
       and args[7:13] == [0x6b, 0xed, 0x34, 0x03, 0x6f, 0x1c]
       and args[13:16] == [0x8b, 0x4e, 0x04],
       "call pushes arg2, then converts EBP pass to a MeshRenderInfo pointer")
    ok(16 <= len(call) <= 24,
       "mesh material call verifies %d bytes (required 16-24)" % len(call))
    ok(call[0:9] == [0x6b, 0xed, 0x34, 0x03, 0x6f, 0x1c,
                      0x8b, 0x4e, 0x04]
       and call[9:14] == [0x53, 0x89, 0x6c, 0x24, 0x20],
       "call keeps ESI instance but changes EBP from pass to render-info pointer")
    ok(call_off + 5 <= len(call) and call[call_off] == 0xe8,
       "mesh material call offset %d lands on E8" % call_off)
    dest = call_at + call_off + 5 + struct.unpack_from(
        "<i", bytes(call), call_off + 1)[0]
    ok(dest == mesh_setter,
       "mesh material call resolves to GraphicsMesh::SetShaderParameters (%#x)"
       % dest)
    ok(engine.exports().get(
           cstr("kGraphicsMeshInstanceSetShaderParametersName"))
       == instance_setter,
       "GraphicsMeshInstance::SetShaderParameters owns the context call")

    # Re-derive all direct E8 callers from the pinned .text rather than merely
    # checking that the source's chosen subset happens to point at E8 bytes.
    # The material call is verified separately because run 51 retargets it at
    # runtime; the DWORD table must contain every other direct caller exactly.
    text_section = next(s for s in engine.sections if s[0] == ".text")
    _, text_rva, text_size, _, _ = text_section
    code = engine.read(engine.base + text_rva, text_size)
    found = []
    for i in range(len(code) - 4):
        if code[i] != 0xe8:
            continue
        target = text_rva + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
        if target == const("kGraphicsTextureGetTextureRva"):
            found.append(text_rva + i)
    recorded = dword_table("kShadowTextureDirectCallerRvas")
    material_call = const("kShadowMaterialTextureWindowRva") \
        + const("kShadowMaterialTextureCallOffset")
    ok(len(recorded) == 9 and len(set(recorded)) == len(recorded),
       "nine non-material direct texture callers are recorded exactly once")
    ok(sorted(recorded + [material_call]) == sorted(found),
       "caller table plus the material site equals all %d direct GetTexture calls"
       % len(found))
    for at in recorded:
        opcode = engine.read(engine.base + at, 5)
        target = at + 5 + struct.unpack_from("<i", opcode, 1)[0]
        ok(opcode[0] == 0xe8 and target == const("kGraphicsTextureGetTextureRva"),
           "texture caller %#x resolves to GraphicsTexture::GetTexture" % at)

    adapter = re.search(
        r"hookShadowMeshSetShaderParameters\(.*?__asm__ __volatile__\((.*?)\);",
        src, re.S)
    adapter_text = adapter.group(1) if adapter else ""
    pass_offset = const("kShadowMeshParameterAdapterPassOffset")
    # Entry ESP is original ESP-0xa8: 0x8c locals, four saved registers,
    # two call arguments, and the E8 return. Original arg3 is then +0xb4;
    # the adapter's first two pushes move it to +0xbc.
    local_bytes = struct.unpack_from("<I", bytes(frame), 2)[0]
    derived_pass_offset = local_bytes + 4 * 4 + 2 * 4 + 4 + 3 * 4 + 2 * 4
    ok(pass_offset == derived_pass_offset == 0xbc,
       "adapter pass offset accounts for the complete stack frame and pushes")
    ok(adapter_text.count('"pushl 8(%%esp)') == 2
       and ('"pushl %#x(%%%%esp)' % pass_offset) in adapter_text
       and '"pushl %%esi' in adapter_text
       and '"pushl %%ecx' in adapter_text
       and '"ret $8' in adapter_text,
       "naked adapter forwards material, shader, pass, instance, and mesh")

    # Run 54's diagnostic join deliberately has no new engine call or binary
    # patch site. Verify its source invariants here so changing the table or
    # moving an engine call into the rare miss path cannot pass unnoticed.
    slots = const("kShadowRecordContextSlots")
    ok(slots == 4096 and slots & (slots - 1) == 0,
       "shadow context join has the recorded 4096 power-of-two slots")
    miss_begin = src.find("void explainShadowRecordMiss(")
    miss_end = src.find("\n}\n\n// Which call site", miss_begin)
    miss = src[miss_begin:miss_end]
    ok(miss_begin >= 0 and miss_end > miss_begin
       and "g_meshShadowStyle" not in miss
       and "g_meshGetTexture" not in miss
       and "g_graphicsTextureGetTexture" not in miss
       and "context->match = ShadowContextInstanceMissing" in miss,
       "context miss explanation scans retained identities without engine calls")
    build_begin = src.find("int __fastcall hookBuildShadowRecord(")
    build_end = src.find("\n}\n\nvoid __fastcall hookShadowMeshEnsure", build_begin)
    build = src[build_begin:build_end]
    accepted = build.find("const int result = g_buildShadowRecord(")
    retained = build.find("rememberShadowRecordContext(")
    ok(build_begin >= 0 and build_end > build_begin
       and accepted >= 0 and retained > accepted
       and "if (result && g_shadowTracing" in build,
       "only accepted directional records populate the diagnostic join")
    filtered_begin = src.find(
        'extern "C" void* __cdecl shadowMaterialTextureFiltered(')
    filtered_end = src.find("\n}\n\n// The material Name", filtered_begin)
    filtered = src[filtered_begin:filtered_end]
    ok(filtered_begin >= 0 and filtered_end > filtered_begin
       and "if (cold && !context.active) explainShadowRecordMiss(&context);"
           in filtered,
       "only a cold material-texture miss pays for the fallback identity scan")
    ok("context.outerInstanceSite = context.instance" in filtered
       and "|| outerCaller ==" in filtered,
       "outer caller retains the verified instance site through its wrapper")

    chain_begin = src.find("void reportUnresolvedShadowTextureChain(")
    chain_end = src.find("\n}\n\nvoid reportChains()", chain_begin)
    chain = src[chain_begin:chain_end]
    load_begin = src.find("void __fastcall hookLoadResource(")
    load_end = src.find("\n}\n\nbool reusePreviousShadow", load_begin)
    load = src[load_begin:load_end]
    ok(chain_begin >= 0 and chain_end > chain_begin
       and "g_shadowTextureChainReports" in chain
       and "report >= (LONG)kChainSlots" in chain
       and "i < kStackWords" in chain
       and "unresolved shadow texture" in chain,
       "unresolved texture chains are written live with fixed bounds")
    ok(load_begin >= 0 and load_end > load_begin
       and "textureCaller == ShadowTextureUnresolved" in load
       and "reportUnresolvedShadowTextureChain(" in load,
       "only unresolved directional-shadow textures emit chain diagnostics")


def check_shadow_alpha_defer(engine):
    """Prove the exact caster omission point and every called dependency."""
    print("\nDirectional-shadow cold alpha-caster deferral")
    call = table("kShadowRecordCallWindowBytes")
    call_at = const("kShadowRecordCallWindowRva")
    call_off = const("kShadowRecordCallOffset")
    helper_at = const("kBuildShadowRecordRva")
    helper = table("kBuildShadowRecordBytes")
    ok(16 <= len(call) <= 24,
       "shadow-record call verifies %d bytes (required 16-24)" % len(call))
    ok(call[0:9] == [0x57, 0x56, 0x8d, 0x44, 0x24, 0x30, 0x50,
                      0x8b, 0xcd],
       "record decision receives pass, renderable entry, output, and renderer")
    ok(call_off + 5 <= len(call) and call[call_off] == 0xe8,
       "shadow-record call offset %d lands on E8" % call_off)
    dest = call_at + call_off + 5 + struct.unpack_from(
        "<i", bytes(call), call_off + 1)[0]
    ok(dest == helper_at,
       "shadow-record call resolves to its original helper (%#x)" % dest)
    ok(call[14:22] == [0x84, 0xc0, 0x0f, 0x84, 0x8c, 0x00, 0x00, 0x00],
       "a false helper result skips appending the caster/pass record")
    ok(16 <= len(helper) <= 24,
       "build-record helper verifies %d bytes (required 16-24)" % len(helper))
    ok(helper[8:19] == [0x8b, 0x0b, 0x56, 0x8b, 0x01, 0x57,
                         0x8b, 0x40, 0x24, 0xff, 0xd0],
       "the original helper rejects through renderable virtual slot 0x24")

    style_at = const("kMeshShadowStyleRva")
    style = table("kMeshShadowStyleBytes")
    no_name = const("kNameNoNameRva")
    ok(16 <= len(style) <= 24,
       "mesh shadow-style entry verifies %d bytes (required 16-24)" % len(style))
    ok(style[4] == 0x68
       and struct.unpack_from("<I", bytes(style), 5)[0] == engine.base + no_name,
       "shadow style requests the base texture with Name::noName")
    ok(style[9:20] == [0x8b, 0x06, 0xff, 0x74, 0x24, 0x10,
                        0x32, 0xdb, 0xff, 0x50, 0x1c],
       "shadow style calls the instance's virtual GetTexture and starts opaque")
    alpha = table("kMeshShadowStyleAlphaBytes")
    skinned = table("kMeshShadowStyleSkinnedBytes")
    foliage = table("kMeshShadowStyleFoliageBytes")
    static = table("kMeshShadowStyleStaticBytes")
    for label, values in [("alpha flag", alpha), ("skinned return", skinned),
                          ("foliage return", foliage), ("static return", static)]:
        ok(16 <= len(values) <= 24,
           "%s verifies %d bytes (required 16-24)" % (label, len(values)))
    ok(alpha[0:17] == [0x80, 0xb8, 0x81, 0x00, 0x00, 0x00, 0x02,
                        0xb3, 0x01, 0x7c, 0x06,
                        0x8a, 0x98, 0x80, 0x00, 0x00, 0x00],
       "texture metadata supplies the alpha-tested selector")
    ok(skinned[9:19] == [0xb8, 0x01, 0x00, 0x00, 0x00,
                          0xb9, 0x04, 0x00, 0x00, 0x00]
       and foliage[4:14] == [0xb8, 0x02, 0x00, 0x00, 0x00,
                              0xb9, 0x05, 0x00, 0x00, 0x00]
       and static[0:9] == [0x33, 0xc0, 0x84, 0xdb,
                            0xb9, 0x03, 0x00, 0x00, 0x00],
       "styles 0-2 are opaque and 3-5 are their alpha-tested counterparts")

    getter_at = const("kMeshGetTextureRva")
    getter = table("kMeshGetTextureBytes")
    mesh = table("kMeshGetTextureMeshBytes")
    mesh_at = const("kMeshGetTextureMeshRva")
    mesh_off = const("kMeshGetTextureEnsureCallOffset")
    returned = table("kMeshGetTextureReturnBytes")
    ok(16 <= len(getter) <= 24,
       "mesh GetTexture entry verifies %d bytes (required 16-24)" % len(getter))
    ok(16 <= len(mesh) <= 24 and mesh[mesh_off] == 0xe8,
       "mesh GetTexture's dependency call is inside a %d-byte window" % len(mesh))
    dest = mesh_at + mesh_off + 5 + struct.unpack_from(
        "<i", bytes(mesh), mesh_off + 1)[0]
    ok(dest == const("kEnsureAvailableRva"),
       "mesh GetTexture ensures the owning mesh, not the returned texture")
    ok(16 <= len(returned) <= 24
       and returned[7:11] == [0x8b, 0x44, 0x01, 0x14],
       "mesh GetTexture returns the material entry's resource at +0x14")

    accessor = table("kResourceLoaderAccessorBytes")
    ok(len(accessor) == 16 and accessor[0:4] == [0x8b, 0x41, 0x24, 0xc3],
       "Resource::GetResourceLoader returns resource+0x24")
    preload = table("kPreloadEnqueueWindowBytes")
    preload_at = const("kPreloadEnqueueWindowRva")
    preload_off = const("kPreloadEnqueueCallOffset")
    ok(16 <= len(preload) <= 24,
       "stock preload enqueue verifies %d bytes (required 16-24)" % len(preload))
    ok(preload[3:10] == [0x6a, 0x00, 0x6a, 0x01, 0x6a, 0x01, 0x56],
       "stock preload tuple is immediate=false, notify=true, priority=1")
    dest = preload_at + preload_off + 5 + struct.unpack_from(
        "<i", bytes(preload), preload_off + 1)[0]
    ok(dest == const("kEnqueueRva"),
       "stock preload call resolves to ResourceLoader::EnqueueResource")

    for name_const, rva_const, label in [
            ("kMeshShadowStyleName", "kMeshShadowStyleRva",
             "GraphicsMeshInstance::GetShadowRenderStyle"),
            ("kMeshGetTextureName", "kMeshGetTextureRva",
             "GraphicsMeshInstance::GetTexture"),
            ("kResourceLoaderAccessorName", "kResourceLoaderAccessorRva",
             "Resource::GetResourceLoader"),
            ("kPreloadResourceName", "kPreloadResourceRva",
             "BaseResourceManager::PreLoadResource")]:
        ok(engine.exports().get(cstr(name_const)) == const(rva_const),
           "%s export resolves to its recorded RVA" % label)

    bump = table("kShadowInstanceBumpEnsureWindowBytes")
    bump_at = const("kShadowInstanceBumpEnsureWindowRva")
    bump_off = const("kShadowInstanceBumpEnsureCallOffset")
    ok(16 <= len(bump) <= 24,
       "instance bump Ensure verifies %d bytes (required 16-24)" % len(bump))
    ok(bump[0:9] == [0x8b, 0x7e, 0x18, 0x85, 0xff, 0x74, 0x6b,
                      0x8b, 0xcf],
       "instance+0x18 supplies the optional texture Resource in ECX")
    ok(bump_off + 5 <= len(bump) and bump[bump_off] == 0xe8,
       "instance bump call offset %d lands on E8" % bump_off)
    dest = bump_at + bump_off + 5 + struct.unpack_from(
        "<i", bytes(bump), bump_off + 1)[0]
    ok(dest == const("kEnsureAvailableRva"),
       "instance bump call resolves to Resource::EnsureAvailable")

    setter = table("kShadowInstanceBumpSetterWindowBytes")
    setter_at = const("kShadowInstanceBumpSetterWindowRva")
    bump_name = const("kBumpTextureNameRva")
    ok(16 <= len(setter) <= 24,
       "instance bump setter verifies %d bytes (required 16-24)" % len(setter))
    ok(setter[12] == 0x68
       and struct.unpack_from("<I", bytes(setter), 13)[0]
           == engine.base + bump_name,
       "the post-Ensure setter uses the same bumpTexture Name")
    setter_dest = setter_at + 19 + 5 + struct.unpack_from(
        "<i", bytes(setter), 20)[0]
    ok(setter[19] == 0xe8 and setter_dest == const("kSetTextureParameterRva"),
       "instance bump block ends at the verified texture-parameter setter")

    init = table("kBumpTextureNameInitWindowBytes")
    literal = const("kBumpTextureLiteralRva")
    ok(16 <= len(init) <= 24 and init[8:10] == [0x6a, 0x0b],
       "bumpTexture Name initialization verifies its 11-byte length")
    ok(engine.read(engine.base + literal, 12) == b"bumpTexture\0",
       "bumpTexture static Name source is the exact engine string")

    missing = table("kSetTextureParameterMissingWindowBytes")
    missing_at = const("kSetTextureParameterMissingWindowRva")
    missing_return = const("kSetTextureParameterMissingReturnRva")
    ok(16 <= len(missing) <= 24,
       "texture-setter missing path verifies %d bytes (required 16-24)"
       % len(missing))
    first_target = missing_at + 12 + struct.unpack_from("b", bytes(missing), 11)[0]
    second_target = missing_at + 20 + struct.unpack_from("b", bytes(missing), 19)[0]
    ok(missing[10] == 0x74 and missing[18] == 0x74
       and first_target == missing_return and second_target == missing_return,
       "an absent Name or parameter index returns before reading textureValue")
    missing_tail = table("kSetTextureParameterMissingReturnBytes")
    ok(16 <= len(missing_tail) <= 24
       and missing_tail[0:7] == [0x5f, 0xb0, 0x01, 0x5e,
                                  0xc2, 0x10, 0x00],
       "texture setter's missing-parameter target returns success")

    wrapper_begin = src.find(
        'extern "C" void __cdecl shadowInstanceBumpEnsureFiltered(')
    wrapper_end = src.find("\n}\n\n// At the patched E8", wrapper_begin)
    wrapper = src[wrapper_begin:wrapper_end]
    ok(wrapper_begin >= 0 and wrapper_end > wrapper_begin
       and "g_shadowDeferActive && inShadow && shader" in wrapper
       and "&& !g_shaderHasParameter(" in wrapper
       and "g_engineBase + kBumpTextureNameRva" in wrapper
       and "g_ensureAvailable(texture, nullptr)" in wrapper,
       "bump omission is directional-only and forwards every used case")
    install_begin = src.find("if (ok && g_shadowDeferColdAlpha) {")
    install_end = src.find("\n    if (ok && trace", install_begin)
    install = src[install_begin:install_end]
    ok(install_begin >= 0 and install_end > install_begin
       and "g_shadowInstanceBumpEnsurePatch" in install
       and "hookShadowInstanceBumpEnsure" in install
       and "restoreCall(g_shadowInstanceBumpEnsurePatch)" in install
       and "const bool deferOk = recordOk && contextOk && filterOk && bumpOk;"
           in install,
       "cold-alpha fix requires the verified bump call patch atomically")

    base_ensure = table("kShadowInstanceBaseEnsureWindowBytes")
    base_ensure_at = const("kShadowInstanceBaseEnsureWindowRva")
    base_ensure_off = const("kShadowInstanceBaseEnsureCallOffset")
    ok(16 <= len(base_ensure) <= 24,
       "instance base override verifies %d bytes (required 16-24)"
       % len(base_ensure))
    ok(base_ensure[0:9] == [0x8b, 0x7e, 0x14, 0x85, 0xff, 0x74, 0x6b,
                             0x8b, 0xcf],
       "instance+0x14 supplies the non-null base override in ECX")
    ok(base_ensure_off + 5 <= len(base_ensure)
       and base_ensure[base_ensure_off] == 0xe8,
       "instance base override call offset %d lands on E8" % base_ensure_off)
    dest = base_ensure_at + base_ensure_off + 5 + struct.unpack_from(
        "<i", bytes(base_ensure), base_ensure_off + 1)[0]
    ok(dest == const("kEnsureAvailableRva"),
       "instance base override is ensured after the generic material call")

    base_setter = table("kShadowInstanceBaseSetterWindowBytes")
    base_setter_at = const("kShadowInstanceBaseSetterWindowRva")
    base_name = const("kBaseTextureNameRva")
    ok(16 <= len(base_setter) <= 24,
       "instance base setter verifies %d bytes (required 16-24)"
       % len(base_setter))
    ok(base_setter[12] == 0x68
       and struct.unpack_from("<I", bytes(base_setter), 13)[0]
           == engine.base + base_name,
       "the instance override binds to the exact baseTexture Name")
    base_setter_dest = base_setter_at + 19 + 5 + struct.unpack_from(
        "<i", bytes(base_setter), 20)[0]
    ok(base_setter[19] == 0xe8
       and base_setter_dest == const("kSetTextureParameterRva"),
       "instance base override ends at the verified texture setter")
    generic_call = (const("kShadowMeshParameterCallRva")
                    + const("kShadowMeshParameterCallOffset"))
    ok(generic_call < base_ensure_at < base_setter_at,
       "generic material application precedes ensure and binding of override")

    base_init = table("kBaseTextureNameInitWindowBytes")
    base_literal = const("kBaseTextureLiteralRva")
    ok(16 <= len(base_init) <= 24 and base_init[8:10] == [0x6a, 0x0b],
       "baseTexture Name initialization verifies its 11-byte length")
    ok(engine.read(engine.base + base_literal, 12) == b"baseTexture\0",
       "baseTexture static Name source is the exact engine string")

    filtered_begin = src.find(
        'extern "C" void* __cdecl shadowMaterialTextureFiltered(')
    filtered_end = src.find("\n}\n\n// The material Name", filtered_begin)
    filtered = src[filtered_begin:filtered_end]
    ok(filtered_begin >= 0 and filtered_end > filtered_begin
       and "const bool overriddenBase = g_shadowDeferActive && inShadow"
           " && texture" in filtered
       and "context.instance + 0x14" in filtered
       and "texture != baseOverride" in filtered
       and "memcmp(name, g_engineBase + kBaseTextureNameRva, 16) == 0"
           in filtered
       and "CounterEngineShadowBaseOverrideSkippedCold" in filtered,
       "base omission requires directional context, a distinct live override,"
       " and the exact Name")
    ok("const bool contextOk = recordOk" in install
       and "const bool filterOk = contextOk" in install
       and "const bool contextActive = deferOk && contextOk;" in install
       and "g_shadowMaterialTextureHooked = deferOk && filterOk;" in install
       and "restoreCall(g_shadowMeshParameterPatch)" in install,
       "base-override context patch is an atomic fix dependency")
    context_begin = src.find(
        'extern "C" void __cdecl shadowMeshSetShaderParametersContext(')
    context_end = src.find("\n}\n\n// At this patched E8", context_begin)
    context_adapter = src[context_begin:context_end]
    ok(context_begin >= 0 and context_end > context_begin
       and "if (!onMainThread()" in context_adapter
       and "|| InterlockedCompareExchange(&g_insideDirectional, 0, 0) <= 0"
           in context_adapter
       and "g_graphicsMeshSetShaderParameters(" in context_adapter
       and "return;" in context_adapter,
       "global call patch exposes context only on the main directional path")


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
            ("kResourceFileNameBytes", "kResourceFileNameRva", None),
            ("kUnloadLevelBytes", "kUnloadLevelRva", "kUnloadLevelRelocs"),
            ("kEnqueueBytes", "kEnqueueRva", "kEnqueueRelocs"),
            ("kPreloadEnqueueWindowBytes", "kPreloadEnqueueWindowRva", None),
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
            ("kShadowMeshPassCountBytes", "kShadowMeshPassCountRva", None),
            ("kGraphicsMeshSetShaderParametersFrameBytes",
             "kGraphicsMeshSetShaderParametersFrameRva", None),
            ("kShadowMaterialLoopFrameBytes",
             "kShadowMaterialLoopFrameRva", None),
            ("kShadowMaterialTextureWindowBytes",
             "kShadowMaterialTextureWindowRva", None),
            ("kShadowTextureParameterWindowBytes",
             "kShadowTextureParameterWindowRva", None),
            ("kShaderHasParameterBytes", "kShaderHasParameterRva", None),
            ("kNameHashBytes", "kNameHashRva", None),
            ("kShadowMeshParameterFrameBytes",
             "kShadowMeshParameterFrameRva",
             "kShadowMeshParameterFrameRelocs"),
            ("kShadowMeshParameterEntryBytes",
             "kShadowMeshParameterEntryRva", None),
            ("kShadowMeshParameterContextBytes",
             "kShadowMeshParameterContextRva", None),
            ("kShadowMeshParameterArgsBytes",
             "kShadowMeshParameterArgsRva", None),
            ("kShadowMeshParameterCallBytes",
             "kShadowMeshParameterCallRva", None),
            ("kShadowInstanceBumpEnsureWindowBytes",
             "kShadowInstanceBumpEnsureWindowRva", None),
            ("kShadowInstanceBumpSetterWindowBytes",
             "kShadowInstanceBumpSetterWindowRva",
             "kShadowInstanceBumpSetterWindowRelocs"),
            ("kBumpTextureNameInitWindowBytes",
             "kBumpTextureNameInitWindowRva",
             "kBumpTextureNameInitWindowRelocs"),
            ("kSetTextureParameterMissingWindowBytes",
             "kSetTextureParameterMissingWindowRva", None),
            ("kSetTextureParameterMissingReturnBytes",
             "kSetTextureParameterMissingReturnRva", None),
            ("kShadowInstanceBaseEnsureWindowBytes",
             "kShadowInstanceBaseEnsureWindowRva", None),
            ("kShadowInstanceBaseSetterWindowBytes",
             "kShadowInstanceBaseSetterWindowRva",
             "kShadowInstanceBaseSetterWindowRelocs"),
            ("kBaseTextureNameInitWindowBytes",
             "kBaseTextureNameInitWindowRva",
             "kBaseTextureNameInitWindowRelocs"),
            ("kShadowRecordCallWindowBytes", "kShadowRecordCallWindowRva", None),
            ("kBuildShadowRecordBytes", "kBuildShadowRecordRva", None),
            ("kMeshShadowStyleBytes", "kMeshShadowStyleRva",
             "kMeshShadowStyleRelocs"),
            ("kMeshShadowStyleAlphaBytes", "kMeshShadowStyleAlphaRva", None),
            ("kMeshShadowStyleSkinnedBytes", "kMeshShadowStyleSkinnedRva", None),
            ("kMeshShadowStyleFoliageBytes", "kMeshShadowStyleFoliageRva", None),
            ("kMeshShadowStyleStaticBytes", "kMeshShadowStyleStaticRva", None),
            ("kMeshGetTextureBytes", "kMeshGetTextureRva",
             "kMeshGetTextureRelocs"),
            ("kMeshGetTextureMeshBytes", "kMeshGetTextureMeshRva", None),
            ("kMeshGetTextureReturnBytes", "kMeshGetTextureReturnRva", None),
            ("kResourceLoaderAccessorBytes", "kResourceLoaderAccessorRva", None),
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
            (engine, "Engine", "kResourceFileNameName",
             "kResourceFileNameRva"),
            (engine, "Engine", "kUnloadLevelName", "kUnloadLevelRva"),
            (engine, "Engine", "kEnqueueName", "kEnqueueRva"),
            (engine, "Engine", "kPreloadResourceName", "kPreloadResourceRva"),
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
            (engine, "Engine", "kShadowMeshPassCountName",
             "kShadowMeshPassCountRva"),
            (engine, "Engine", "kEnsureAvailableName",
             "kEnsureAvailableRva"),
            (engine, "Engine", "kGraphicsMeshSetShaderParametersName",
             "kGraphicsMeshSetShaderParametersRva"),
            (engine, "Engine", "kGraphicsTextureGetTextureName",
             "kGraphicsTextureGetTextureRva"),
            (engine, "Engine", "kShaderHasParameterName",
             "kShaderHasParameterRva"),
            (engine, "Engine", "kGraphicsMeshInstanceSetShaderParametersName",
             "kGraphicsMeshInstanceSetShaderParametersRva"),
            (engine, "Engine", "kMeshShadowStyleName",
             "kMeshShadowStyleRva"),
            (engine, "Engine", "kMeshGetTextureName",
             "kMeshGetTextureRva"),
            (engine, "Engine", "kResourceLoaderAccessorName",
             "kResourceLoaderAccessorRva"),
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
    check_shadow_mesh_boundary(engine)
    check_shadow_material_textures(engine)
    check_shadow_alpha_defer(engine)
    check_shadow_texture_attribution(engine)

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
