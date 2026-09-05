#!/usr/bin/env python3
"""Check every audited Engine byte table against the pinned binaries.

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
from functools import lru_cache

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from pe import PE as BinaryPE


class PE(BinaryPE):
    # The pinned image is immutable throughout a verification. Re-parsing its
    # several thousand exports for every assertion obscures mutation testing.
    @lru_cache(maxsize=None)
    def exports(self):
        return super().exports()

GAME = os.environ.get("TQ_GAME_DIR") or os.path.expanduser(
    "~/Library/Application Support/CrossOver/Bottles/Titan Quest/drive_c/"
    "GOG Games/Titan Quest - Anniversary Edition")
SRC = os.environ.get("TQ_VERIFY_SRC") or os.path.join(
    HERE, "..", "..", "..", "src", "engine_probe.cpp")
CACHE_H = os.path.join(HERE, "..", "..", "..", "src", "arc_cache.h")
ARC_CPP = os.environ.get("TQ_VERIFY_ARC_CPP") or os.path.join(
    HERE, "..", "..", "..", "src", "arc_cache.cpp")
PROBE_CPP = os.environ.get("TQ_VERIFY_PROBE_CPP") or os.path.join(
    HERE, "..", "..", "..", "src", "probe.cpp")
PROBE_H = os.environ.get("TQ_VERIFY_PROBE_H") or os.path.join(
    HERE, "..", "..", "..", "src", "probe.h")
ENGINE_PROBE_H = os.environ.get("TQ_VERIFY_ENGINE_PROBE_H") or os.path.join(
    HERE, "..", "..", "..", "src", "engine_probe.h")
VISUAL_CPP = os.environ.get("TQ_VERIFY_VISUAL_CPP") or os.path.join(
    HERE, "..", "..", "..", "src", "visual.cpp")
SELFTEST_CPP = os.environ.get("TQ_VERIFY_SELFTEST_CPP") or os.path.join(
    HERE, "..", "..", "..", "test", "selftest.cpp")
README = os.environ.get("TQ_VERIFY_README") or os.path.join(
    HERE, "..", "..", "..", "README.md")
FLOW_DOC = os.environ.get("TQ_VERIFY_FLOW_DOC") or os.path.join(
    HERE, "..", "disassembly-targets.md")
FINDINGS = os.environ.get("TQ_VERIFY_FINDINGS") or os.path.join(
    HERE, "..", "findings.md")

ENGINE_UNITS = ('engine_probe.cpp', 'engine_hooks.cpp',
                'shadow_defer.cpp', 'terrain_preload.cpp',
                'secondary_admission.cpp', 'archive_hooks.cpp', 'engine_internal.h')
# TQ_VERIFY_SRC still accepts one combined source for deliberate mutations.
# Normal verification reads every actual compiled implementation and the shared
# contracts, so moving a table cannot remove it from verification.
src = (open(SRC).read() if os.environ.get('TQ_VERIFY_SRC') else
       '\n'.join(open(os.path.join(os.path.dirname(SRC), unit)).read()
                 for unit in ENGINE_UNITS))
cache_src = open(CACHE_H).read()
arc_src = open(ARC_CPP).read()
probe_src = open(PROBE_CPP).read()
probe_h = open(PROBE_H).read()
engine_h = open(ENGINE_PROBE_H).read()
engine_h += '\n' + open(os.path.join(os.path.dirname(SRC), 'engine.h')).read()
engine_h += '\n' + open(os.path.join(os.path.dirname(SRC), 'secondary_admission.h')).read()
visual_src = open(VISUAL_CPP).read()
selftest_src = open(SELFTEST_CPP).read()
readme_src = open(README).read()
flow_doc = open(FLOW_DOC).read()
findings_src = open(FINDINGS).read()
flat = re.sub(r'"\s*\n\s*"', '', src)          # joined string literals
failures = []


def ok(good, what):
    print("  %-4s %s" % ("OK" if good else "FAIL", what))
    if not good:
        failures.append(what)
    return good



def definition_start(text, marker):
    """Find a definition rather than a declaration in the shared contracts."""
    start = text.find(marker)
    while start >= 0:
        opening = text.find('{', start)
        semicolon = text.find(';', start)
        if opening >= 0 and (semicolon < 0 or opening < semicolon):
            return start
        start = text.find(marker, start + 1)
    return -1


def block_end(text, start):
    """Balanced C++ body boundary; independent of file and function order."""
    if start < 0:
        return -1
    opening = text.find('{', start)
    if opening < 0:
        return -1
    # Skip strings/comments, including assembly strings and their braces.
    token = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|[{}]')
    depth = 0
    for match in token.finditer(text, opening):
        if match[0] == '{':
            depth += 1
        elif match[0] == '}':
            depth -= 1
            if not depth:
                return match.end()
    return -1


def cpp_body(marker, text=None):
    text = src if text is None else text
    start = definition_start(text, marker)
    end = block_end(text, start)
    return text[start:end] if start >= 0 and end > start else ''


def check_trace_off_isolation():
    """Behavior ownership and the disabled recorder's actual call boundaries."""
    print("\nPerformance modules and trace-off isolation")
    root = os.path.dirname(SRC)
    probe_impl = open(os.path.join(root, 'engine_probe.cpp')).read()
    owners = {
        'shadow_defer.cpp': ('readShadowOptions', 'hookShadowMeshPassCount',
                             'hookShadowActorUpdateMeshInstance',
                             'shadowMaterialTextureFiltered', 'installShadow'),
        'terrain_preload.cpp': ('readTerrainOptions', 'hookTerrainRtLoadTextures',
                                'installTerrain'),
        'secondary_admission.cpp': ('readSecondaryOptions',
                                    'shouldDeferSecondaryAdmission',
                                    'hookGraphicsMeshInstanceRenderPass',
                                    'installReflections'),
        'archive_hooks.cpp': ('describeBlock', 'hookArchiveBlock', 'installArchive')}
    for unit, functions in owners.items():
        implementation = open(os.path.join(root, unit)).read()
        ok(all(cpp_body(name + '(', implementation)
               and not cpp_body(name + '(', probe_impl) for name in functions),
           unit + ' owns its behavior and installation independently of the observer')
    for name in ('now', 'microsecondsSince', 'isRenderThread',
                 'currentFrameIndex', 'currentGpuContext',
                 'gpuBegin', 'gpuEnd', 'beginFrame', 'endFrame'):
        gate = cpp_body(name + '(', probe_h)
        ok('detail::active' in gate and name + 'Internal(' in gate
           and 'QueryPerformanceCounter' not in gate,
           name + ' checks the inline enable flag before entering the recorder')
    for name in ('hookRenderDirectional', 'hookReflectionRenderLight',
                 'hookGraphicsMeshInstanceRenderPass',
                 'hookTerrainPlugRender', 'hookTerrainBlockRender'):
        hook = cpp_body(name + '(')
        fast = cpp_body('if (!g_tracing)', hook)
        ok(bool(fast) and 'return' in fast
           and 'tq::probe::' not in fast and 'GpuScope' not in fast
           and 'GpuChunkRenderableCallScope' not in fast
           and 'countAdmissionRenderable' not in fast,
           name + ' bypasses observer work with tracing disabled')
    light_install = cpp_body('bool installReflections(')
    ok('const bool buildOk = planeOk && (!trace || tq::detour::patchCall('
       in light_install,
       'trace-off admission installs no reflection BuildScene observer hook')
    material = cpp_body('shadowMaterialTextureFiltered(')
    ok('const bool cold = g_shadowTracing && inShadow' in material
       and 'if (!g_shadowTracing)\n'
           '        return g_graphicsTextureGetTexture(texture, nullptr);'
           in material,
       'shadow material filtering bypasses cold-texture diagnostic work')
    for name in ('countDeferredShadowMesh', 'countDeferredShadowActorPose'):
        ok(('if (g_shadowTracing) ' + name + '(') in src,
           name + ' is never called by the disabled behavior path')
    ok('if (g_tracing) resetEngineTraceState();' in cpp_body('bool install(')
       and 'memset(g_gpuChunkEvents' in cpp_body('void resetEngineTraceState('),
       'trace-off startup does not touch the large observer history rings')
    texture = cpp_body('HRESULT WINAPI hookCreateTexture2D(', visual_src)
    ok('if (!tq::probe::enabled())\n'
       '        return createTexture2DDispatch(device, desc, initial, texture, caller);'
       in texture,
       'texture creation bypasses thread/frame/descriptor observers with tracing off')
    buffer = cpp_body('HRESULT WINAPI hookCreateBuffer(', visual_src)
    ok('if (tq::probe::enabled() && SUCCEEDED(result)' in buffer,
       'grass buffer hooks do not enter Engine creation observers with tracing off')
    ok('exerciseTraceOffHooksForTest()' in selftest_src
       and 'without entering either recorder' in selftest_src,
       'off-game test executes real preload, cold-root and shared admission hooks without recorder entries')
    ok('if (detail::reporting) reportInternal();' in cache_src
       and 'detail::reporting = tq::hdr::readSettings().trace;' in arc_src,
       'cache debug reports remain explicit and take no reporting lock with debug trace off')


def check_legacy_scalar_contract(engine):
    """Cover historical constants exposed by the refactor perturbation audit."""
    print("\nHistorical call offsets, thread identity and diagnostic bounds")
    instruction = engine.read(engine.base + 0x14476b, 6)
    ok(instruction[:2] == b'\x3b\x05'
       and struct.unpack_from('<I', instruction, 2)[0]
           == engine.base + const('kMainThreadIdRva'),
       'Engine::Update CMP operand independently proves the main-thread-id RVA')
    for name, table_name, opcode in (
            ('kRegionLockCallOffset', 'kRegionLockEbxBytes', [0xff, 0x15]),
            ('kRegionLockCallOffset', 'kRegionLockEdiBytes', [0xff, 0x15]),
            ('kForceLoadCallOffset', 'kForceLoadDeferredBytes', [0xe8]),
            ('kForceLoadCallOffset', 'kForceLoadForwardBytes', [0xe8]),
            ('kFenceCallOffset', 'kFenceWindowBytes', [0xff, 0x15])):
        offset = const(name)
        window_bytes = table(table_name)
        ok(window_bytes[offset:offset + len(opcode)] == opcode,
           name + ' identifies the verified call opcode in ' + table_name)
    sweep_sites = re.search(r'const SweepSite kSweepSites\[kSweepCount\] = \{(.*?)\};', src, re.S)
    ok(bool(sweep_sites)
       and const('kSweepCount') == sweep_sites[1].count('{'),
       'sweep patch count equals the seven verified call-site entries')
    groups = ('All', 'Loads', 'Archive', 'Fence', 'Lock', 'Sweeps', 'Wait',
              'Frame', 'Game', 'Loop', 'Pump', 'Heap', 'ArcIo', 'Blocking',
              'Shadow', 'Terrain', 'DeferredPasses', 'Reflections')
    for bit, name in enumerate(groups):
        ok(const('kGroup' + name) == 1 << bit,
           'documented trace-mask bit for ' + name + ' is exact')
    # These are diagnostic policy, not Engine structure offsets. Pin the
    # established retention/scan limits so a typo cannot expand hot-path work.
    for name, expected in (('kSlowLoadUs', 1000), ('kLoadCallerSlots', 16),
                           ('kChainSlots', 8), ('kChainDepth', 32),
                           ('kStackWords', 2048), ('kChainModules', 3),
                           ('kMessageKinds', 64)):
        ok(const(name) == expected, name + ' preserves the existing diagnostic bound')


def const(name, text=None):
    """A `const DWORD/unsigned/uint32_t <name> = <n>;` from the source."""
    return source_constant(name, src if text is None else text)


@lru_cache(maxsize=1024)
def source_constant(name, text):
    # Include the source text in the key so in-memory perturbations cannot
    # accidentally reuse an unperturbed result.
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


def relocs(name):
    if not name:
        return []
    m = re.search(r"const Relocation %s\[\] = \{(.*?)\};" % name, src, re.S)
    out = []
    for off, r in re.findall(r"\{\s*(\w+)\s*,\s*(\w+)\s*\}", m.group(1)):
        target = int(r, 0) if re.match(r"^(?:0x|\d)", r) else const(r)
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


def check_configuration_contract():
    """Shipping defaults and removal of behavior experiments."""
    print("\nCurrent configuration contract")
    read_begin = definition_start(src, "void readOptions(const wchar_t* iniPath)")
    read_end = block_end(src, read_begin)
    options = src[read_begin:read_end] + "\n".join(cpp_body("void " + f + "(") for f in ("readShadowOptions", "readTerrainOptions", "readSecondaryOptions"))
    probe_begin = definition_start(probe_src, "void readOptions(const wchar_t* iniPath)")
    probe_end = block_end(probe_src, probe_begin)
    probe_options = probe_src[probe_begin:probe_end]
    visual_read_begin = definition_start(visual_src, "void readOptions()")
    visual_read_end = block_end(visual_src, visual_read_begin)
    visual_options = visual_src[visual_read_begin:visual_read_end]
    visual_install_begin = definition_start(visual_src, "void install(ID3D11Device* device, ID3D11DeviceContext* context,")
    visual_install_end = block_end(visual_src, visual_install_begin)
    visual_install = visual_src[visual_install_begin:visual_install_end]
    engine_install_begin = definition_start(src, "bool install(HMODULE engine)")
    engine_install_end = block_end(src, engine_install_begin)
    engine_install = src[engine_install_begin:engine_install_end]
    rejected = [
        "async_level_load", "timer_period_ms", "pump_timer_min_ms",
        "shadow_transition_reuse", "reflection_defer_admission_mesh",
        "reflection_defer_admission_all"]
    readme_performance = re.search(
        r"\[performance\]\n(.*?)\n\n\[debug\]", readme_src, re.S)
    readme_performance = readme_performance.group(1) \
        if readme_performance else ""

    ok('L"loose_texture_max",\n                                         4096' in visual_src,
       "loose_texture_max defaults to 4096")
    ok('lstrcpyW(value, L"8")' in arc_src
       and 'L"archive_cache_mb", L"8"' in arc_src
       and "archive_cache_mb defaults to the measured eight MiB size"
           in selftest_src,
       "archive_cache_mb defaults to eight with and without an INI")
    ok('L"shadow_defer_cold_resources", 1' in options
       and 'L"shadow_defer_cold_actor_pose", 1' in options
       and 'L"terrain_preload_layers", 1' in options
       and 'L"secondary_pass_admission_budget", 8' in options
       and options.count("        : 8;") == 1,
       "accepted shadow, terrain, and secondary defaults are exact")
    ok(all(('L"%s"' % key) not in options for key in rejected)
       and all((key + "=0") not in readme_src for key in rejected),
       "all six rejected behavior keys are absent from the parser and sample")
    ok('L"draw_timing"' not in probe_options
       and "detail::drawTiming = g_mode == ModeFull;" in probe_options
       and "hitch-only performance_trace omits high-frequency draw clocks"
           in selftest_src,
       "full performance_trace owns draw/map timing without a second key")
    ok(readme_performance.splitlines() == [
           "streaming=optimized", "loose_texture_max=4096",
           "archive_cache_mb=8", "shadow_defer_cold_resources=1",
           "shadow_defer_cold_actor_pose=1", "terrain_preload_layers=1",
           "secondary_pass_admission_budget=8"],
       "README performance sample matches all shipping defaults exactly")
    ok("shadow_defer_cold_alpha" not in options
       and "shadow_defer_cold_resources" in readme_src
       and "shadow_defer_cold_alpha" not in readme_src,
       "the current interface names the complete cold-resource shadow scope")
    ok('L"debug", L"trace", L"0"' in selftest_src
       and 'L"debug", L"performance_trace", L"0"' in selftest_src
       and "trace=0 and performance_trace=0 keep every accepted Engine-side"
           in selftest_src
       and "the trace-off accepted behavior set enables no trace group"
           in selftest_src,
       "the exact normal trace-off combination is self-tested")
    ok('L"performance", L"streaming", L"optimized"' in visual_options
       and 'L"performance", L"loose_texture_max",\n'
           '                                         4096' in visual_options
       and "if (g_options.streaming) startUnmapWorker();" in visual_install
       and "if (g_options.looseTextureMax) installFileSourceGate();"
           in visual_install,
       "streaming and loose-texture gates install independently of tracing")
    ok("if (wants(kGroupArchive) || cache)\n"
           "        installArchive(engine, wants(kGroupArchive), cache);"
           in engine_install
       and "if (!g_tracing && !cache && !shadowDefer && !terrainPreload"
           in engine_install,
       "archive caching enters Engine install independently of tracing")
    ok("const bool shadowDefer = g_shadowDeferColdResources || shadowActorPose;"
           in engine_install
       and "const bool shadowDeferReady = shadowDefer\n"
           "        && prepareShadowAlphaDefer(engine);" in engine_install
       and "if (traceShadow || shadowDeferReady || crossPass || secondaryAdmission)"
           in engine_install
       and "if (ok && (g_shadowDeferColdResources || g_shadowDeferColdActorPose))"
           in src
       and "if (ok && g_shadowDeferColdActorPose)" in src,
       "both directional-shadow fixes install independently of tracing")
    ok("if (traceTerrain || terrainPreload || secondaryAdmission)"
           in engine_install
       and "g_terrainPreloadLayersActive = ok && preloadLayers;" in src,
       "terrain-layer preload installs independently of tracing")
    ok("secondaryAdmissionDrawHooks" in visual_install
       and "g_options.smaa || toneEnabled || nativeBloomControl\n"
           "        || g_deferredBindingTracing || secondaryAdmissionDrawHooks"
           in visual_install
       and "setSecondaryAdmissionDrawHooksReady(" in visual_install
       and "if (traceReflections || secondaryAdmission)" in engine_install
       and "g_secondaryPassAdmissionActive = secondaryAdmission\n"
           "        && g_secondaryAdmissionDrawHooksReady && terrainReady\n"
           "        && shadowReady && reflectionReady;" in engine_install,
       "secondary admission gets its complete trace-independent hook chain")

    progressive_begin = definition_start(visual_src, "bool progressiveTextureCandidate(")
    progressive_end = block_end(visual_src, progressive_begin)
    progressive = visual_src[progressive_begin:progressive_end]
    present_begin = definition_start(visual_src, "void onPresent(IDXGISwapChain* swapChain)")
    present_end = block_end(visual_src, present_begin)
    present = visual_src[present_begin:present_end]
    ok("if (!g_options.streaming" in progressive
       and "probe::enabled()" not in progressive
       and present.find("secondaryAdmissionFrameBoundary();")
           < present.find("probe::beginFrame(")
       and "advanceTextureUploadsInternal();" in present,
       "streaming work and the behavior frame boundary run with the probe off")

    loose_begin = definition_start(visual_src, "void* __fastcall hookDirectoryOpenFile(")
    loose_end = block_end(visual_src, loose_begin)
    loose_hook = visual_src[loose_begin:loose_end]
    ok("const UINT limit = g_options.looseTextureMax;" in loose_hook
       and "if (!file || !limit" in loose_hook
       and "if (!texture || (width <= limit && height <= limit)) return file;"
           in loose_hook
       and "return nullptr;" in loose_hook
       and "probe::enabled()" not in loose_hook,
       "loose-texture rejection is gated only by its configured cap")

    archive_begin = definition_start(src, "int __fastcall hookArchiveBlock(")
    archive_end = block_end(src, archive_begin)
    archive_hook = src[archive_begin:archive_end]
    ok("if (tq::arccache::running())" in archive_hook
       and "if (tq::arccache::lookup(key, dest))" in archive_hook
       and "if (keyed && (result & 0xff)) tq::arccache::store(key, dest);"
           in archive_hook
       and "g_tracing" not in archive_hook
       and "probe::enabled()" not in archive_hook,
       "archive lookup/store behavior does not depend on tracing")

    actor_begin = definition_start(src, "void __fastcall hookShadowActorUpdateMeshInstance(")
    actor_end = block_end(src, actor_begin)
    actor_hook = src[actor_begin:actor_end]
    mesh_begin = actor_end + 3
    mesh_end = src.find("\n}\n\nint __fastcall hookBuildShadowRecord",
                        mesh_begin)
    mesh_hook = src[mesh_begin:mesh_end]
    directional_begin = definition_start(src, "int __fastcall hookRenderDirectional(")
    directional_end = block_end(src, directional_begin)
    directional_hook = src[directional_begin:directional_end]
    ok("if (!g_shadowActorPoseDeferActive" in actor_hook
       and "probe::enabled()" not in actor_hook
       and "if (!g_shadowDeferActive" in mesh_hook
       and "probe::enabled()" not in mesh_hook
       and "g_shadowTracing || g_shadowDeferActive" in directional_hook,
       "shadow behavior uses active fix flags while trace wraps counters only")

    terrain_begin = definition_start(src, "void __fastcall hookTerrainRtLoadTextures(")
    terrain_end = block_end(src, terrain_begin)
    terrain_hook = src[terrain_begin:terrain_end]
    ok("if (g_terrainTracing) {" in terrain_hook
       and "if (g_terrainPreloadLayersActive && g_terrainPreloadEntry)\n"
           "        g_terrainPreloadEntry(self, nullptr, 1);" in terrain_hook
       and terrain_hook.find("g_terrainPreloadLayersActive")
           > terrain_hook.find("if (g_terrainTracing) {"),
       "terrain preload executes outside its diagnostic-only trace block")

    secondary_begin = definition_start(src, "SecondaryAdmissionContext currentSecondaryAdmissionContext()")
    secondary_end = block_end(src, secondary_begin)
    secondary = cpp_body("SecondaryAdmissionContext currentSecondaryAdmissionContext(") + cpp_body("bool shouldDeferSecondaryAdmission(") + cpp_body("void armSecondaryAdmission(") + cpp_body("enum SecondaryAdmissionState {")
    draw_begin = definition_start(visual_src, "void WINAPI hookDraw(")
    draw_end = block_end(visual_src, draw_begin)
    draw_hook = visual_src[draw_begin:draw_end]
    indexed_begin = draw_end + 3
    indexed_end = visual_src.find("\n}\n\n}  // namespace", indexed_begin)
    indexed_hook = visual_src[indexed_begin:indexed_end]
    ok("g_secondaryAdmissionFrameSerial" in secondary
       and "probe::currentFrameIndex" not in secondary
       and draw_hook.find("secondaryAdmissionDrawSuppressed()")
           < draw_hook.find("probe::drawTimingEnabled()")
       and indexed_hook.find("secondaryAdmissionDrawSuppressed()")
           < indexed_hook.find("probe::drawTimingEnabled()"),
       "secondary admission uses its own frame serial and suppresses before timing")
    sections = [int(n) for n in re.findall(r"^## ([0-9]+)[.] ",
                                            findings_src, re.M)]
    ok(sections == sorted(set(sections)),
       "findings numbered sections are unique and corrected forward")


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


def check_terrain_diagnostics(engine):
    """Run 63/64 terrain identity, lifecycle, and colour-render brackets."""
    print("\nTerrainType/TerrainRT lifecycle and colour-render diagnostics")
    preload = table("kTerrainPreloadBytes")
    ground = table("kTerrainRenderGroundBytes")
    shader = table("kTerrainSetShaderParamsBytes")
    grass = table("kTerrainSetGrassShaderParamsBytes")
    rt_load = table("kTerrainRtLoadBytes")
    rt_render = table("kTerrainRtLoadRenderDataBytes")
    rt_preload = table("kTerrainRtPreloadBytes")
    plug = table("kTerrainPlugRenderBytes")
    block = table("kTerrainBlockRenderBytes")
    shared = [0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8]
    ok(len(preload) == 24 and preload[:6] == shared
       and len(ground) == 24 and ground[:6] == shared,
       "both shared-prologue terrain targets verify 24 bytes")
    ok(len(shader) == 21 and shader[:8] ==
       [0xa1, 0, 0, 0, 0, 0x83, 0xec, 0x08]
       and len(grass) == 21 and grass[:8] ==
       [0xa1, 0, 0, 0, 0, 0x83, 0xec, 0x0c],
       "both parameter targets verify 21 bytes and steal two instructions")
    ok(len(rt_load) == 23 and rt_load[:6] == shared
       and len(rt_preload) == 23 and rt_preload[:6] == shared
       and len(plug) == 19 and plug[:6] == shared
       and len(block) == 19 and block[:6] == shared,
       "all four new shared-prologue targets verify 19-23 bytes")
    ok(len(rt_render) == 20
       and rt_render[:3] == [0x83, 0xec, 0x64]
       and rt_render[3] == 0xa1,
       "TerrainRT::LoadRenderData verifies 20 bytes and starts with two"
       " complete instructions")

    install_begin = definition_start(src, "bool installTerrain(HMODULE engine, bool traceTerrain,"
        " bool preloadLayers,\n                    bool secondaryPassAdmission)")
    install_end = block_end(src, install_begin)
    install = src[install_begin:install_end]
    ok(install_begin >= 0 and install_end > install_begin
       and "kTerrainPreloadRelocs, 1),\n            6," in install
       and "kTerrainRenderGroundRelocs, 1),\n            6," in install,
       "shared-prologue terrain detours steal exactly six bytes")
    ok("kTerrainSetShaderParamsRelocs, 1),\n            8," in install
       and "kTerrainSetGrassShaderParamsRelocs, 1),\n            8," in install,
       "parameter detours steal exactly their first eight bytes")
    ok("kTerrainRtLoadRelocs, 1),\n            6," in install
       and "kTerrainRtPreloadRelocs, 1),\n            6," in install
       and "kTerrainPlugRenderRelocs, 1),\n            6," in install
       and "kTerrainBlockRenderRelocs, 1),\n            6," in install,
       "new shared-prologue terrain detours steal exactly six bytes")
    ok("kTerrainRtLoadRenderDataRelocs, 2),\n            8," in install,
       "LoadRenderData steals exactly its first eight bytes")
    ok(install.count("tq::detour::attach(") == 9
       and install.count("tq::detour::patchCall(") == 1
       and "const bool traceOk = !traceTerrain" in install
       and "const bool preloadOk = !preloadLayers" in install
       and "const bool secondaryOk = !secondaryPassAdmission" in install
       and "&& loadTexturesPatched && g_terrainPlugRender"
           " && g_terrainBlockRender" in install
       and install.count("tq::detour::detach(") == 9
       and install.count("tq::detour::restoreCall(") == 1,
       "nine detours and one exact call patch install and roll back atomically")
    ok(const("kGroupTerrain") == 0x8000
       and "const bool traceTerrain = wants(kGroupTerrain);" in src
       and "installTerrain(engine, traceTerrain, terrainPreload,\n"
           "                                      secondaryAdmission);" in src
       and "bool wants(unsigned group)" in src
       and "if (!g_tracing) return false;" in src,
       "terrain diagnostics occupy only trace-mask group 32768")

    # The game selects the unexported TerrainRT implementation through this
    # exact vtable. Check the identities independently of each entry's bytes:
    # a correct-looking prologue at the wrong implementation is still wrong.
    vtable = engine.base + const("kTerrainRtVtableRva")
    vslots = [
        ("Load", "kTerrainRtLoadVtableOffset", "kTerrainRtLoadRva"),
        ("LoadRenderData", "kTerrainRtLoadRenderDataVtableOffset",
         "kTerrainRtLoadRenderDataRva"),
        ("PreLoad", "kTerrainRtPreloadVtableOffset", "kTerrainRtPreloadRva"),
        ("GetNumLayers", "kTerrainRtNumLayersVtableOffset",
         "kTerrainRtNumLayersRva"),
        ("GetLayerTerrainType", "kTerrainRtLayerTypeVtableOffset",
         "kTerrainRtLayerTypeRva")]
    for label, offset_name, target_name in vslots:
        got = struct.unpack("<I", engine.read(
            vtable + const(offset_name), 4))[0]
        ok(got == engine.base + const(target_name),
           "TerrainRT vtable+%#x selects %s at Engine+%#x"
           % (const(offset_name), label, const(target_name)))

    load_textures = table("kTerrainRtLoadTexturesWindowBytes")
    load_textures_off = const("kTerrainRtLoadTexturesCallOffset")
    load_textures_at = const("kTerrainRtLoadTexturesWindowRva")
    load_textures_dest = (load_textures_at + load_textures_off + 5
                          + struct.unpack_from(
                              "<i", bytes(load_textures),
                              load_textures_off + 1)[0])
    ok(len(load_textures) == 23
       and load_textures[load_textures_off] == 0xe8
       and load_textures_dest == const("kTerrainLoadTexturesRva"),
       "runtime LoadRenderData's exact call reaches"
       " TerrainType::LoadTextures")
    ok("tq::detour::patchCall(\n            g_terrainRtLoadTexturesPatch" in install
       and "kTerrainRtLoadTexturesCallOffset, loadTextures" in install,
       "the existing LoadTextures call is patched instead of its code entry")

    num_layers = table("kTerrainRtNumLayersBytes")
    layer_type = table("kTerrainRtLayerTypeBytes")
    ok(len(num_layers) == 23
       and num_layers[:12] == [0x8b, 0x91, 0x88, 0, 0, 0,
                               0x2b, 0x91, 0x84, 0, 0, 0]
       and num_layers[12:17] == [0xb8, 0xab, 0xaa, 0xaa, 0x2a],
       "GetNumLayers subtracts TerrainRT+0x84 from +0x88 and divides by 12")
    ok(len(layer_type) == 24
       and layer_type[:16] == [0x8b, 0x44, 0x24, 0x04,
                               0x8d, 0x14, 0x40,
                               0x8b, 0x81, 0x84, 0, 0, 0,
                               0x8b, 0x04, 0x90]
       and layer_type[16:19] == [0xc2, 0x04, 0],
       "GetLayerTerrainType indexes 12-byte records at TerrainRT+0x84")
    ok(const("kTerrainRtLayerLimit") == 64
       and "count < kTerrainRtLayerLimit" in src
       and "CounterEngineTerrainRtLayerOverflow" in src,
       "runtime layer enumeration is bounded at 64 and reports overflow")

    for label, bytes_name, call_off in [
            ("TerrainPlug", "kTerrainPlugShaderWindowBytes", 5),
            ("TerrainBlock", "kTerrainBlockShaderWindowBytes", 7)]:
        body = table(bytes_name)
        dest = (const(bytes_name.replace("Bytes", "Rva")) + call_off + 5
                + struct.unpack_from("<i", bytes(body), call_off + 1)[0])
        ok(16 <= len(body) <= 24 and body[call_off] == 0xe8
           and dest == const("kTerrainSetShaderParamsRva"),
           "%s's unique body window calls TerrainType::SetShaderParams"
           % label)

    # Ghidra presents five parameters because it includes the implicit this.
    # The machine-code return is decisive for an x86 detour wrapper: C2 10 00
    # means four explicit stack arguments, not five. Run 64/65 used five and
    # could corrupt the caller as soon as colour terrain first ran.
    ok(engine.read(engine.base + const("kTerrainPlugRenderRva") + 0x504, 3)
       == bytes([0xc2, 0x10, 0x00])
       and engine.read(
           engine.base + const("kTerrainBlockRenderRva") + 0x418, 3)
           == bytes([0xc2, 0x10, 0x00]),
       "TerrainPlug and TerrainBlock each return with four stack arguments")
    colour_typedef = re.search(
        r"typedef void \(__fastcall\* TerrainColourRenderFn\)\((.*?)\);",
        src, re.S)
    ok(colour_typedef
       and colour_typedef.group(1).count("const void*") == 4
       and "const void* e" not in colour_typedef.group(1)
       and "g_terrainPlugRender(self, edx, a, b, c, d);" in src
       and "g_terrainBlockRender(self, edx, a, b, c, d);" in src
       and "const void* d, const void* e" not in src,
       "both colour-render wrappers preserve exactly four explicit arguments")

    # Re-read the body rather than trusting the function's name. A false bool
    # skips to the epilogue; true walks base [+24,+28], bump [+30,+34], and
    # grass [+88]. The latter two direct calls reach EnqueueResource.
    gate = engine.read(const("kTerrainPreloadRva") + 0x32, 16)
    gate_disp = struct.unpack_from("<i", gate, 6)[0] if len(gate) >= 10 else 0
    gate_dest = const("kTerrainPreloadRva") + 0x32 + 10 + gate_disp
    ok(gate[:6] == bytes([0x80, 0x7d, 0x08, 0x00, 0x0f, 0x84])
       and gate_dest == const("kTerrainPreloadRva") + 0x1fe,
       "PreLoad(false) jumps over all terrain resource queuing")
    ok(engine.read(const("kTerrainPreloadRva") + 0x3c, 6)
       == bytes([0x8b, 0x4e, 0x24, 0x8b, 0x46, 0x28]),
       "PreLoad(true) walks TerrainType's base-texture vector")
    ok(engine.read(const("kTerrainPreloadRva") + 0x1a1, 6)
       == bytes([0x8b, 0x4e, 0x30, 0x8b, 0x46, 0x34]),
       "PreLoad(true) walks TerrainType's bump-texture vector")
    bump_call = const("kTerrainPreloadRva") + 0x1c7
    bump_bytes = engine.read(bump_call, 5)
    bump_dest = bump_call + 5 + struct.unpack_from("<i", bump_bytes, 1)[0]
    ok(bump_bytes[0] == 0xe8 and bump_dest == const("kEnqueueRva"),
       "PreLoad's bump resources call ResourceLoader::EnqueueResource")
    ok(engine.read(const("kTerrainPreloadRva") + 0x1dc, 8)
       == bytes([0x8b, 0x86, 0x88, 0x00, 0x00, 0x00, 0x85, 0xc0]),
       "PreLoad(true) reads and null-checks TerrainType's grass texture")
    grass_call = const("kTerrainPreloadRva") + 0x1f9
    grass_bytes = engine.read(grass_call, 5)
    grass_dest = grass_call + 5 + struct.unpack_from("<i", grass_bytes, 1)[0]
    ok(grass_bytes[0] == 0xe8 and grass_dest == const("kEnqueueRva"),
       "PreLoad's grass texture calls ResourceLoader::EnqueueResource")

    slots = const("kTerrainPreloadStateSlots")
    ok(slots == 2048 and slots & (slots - 1) == 0,
       "TerrainType preload history is a fixed 2048-slot power-of-two table")
    remember_begin = definition_start(src, "void rememberTerrainPreloadAtFrame(")
    remember_end = block_end(src, remember_begin)
    remember = src[remember_begin:remember_end]
    ok("CounterEngineTerrainPreloadTableOverflow" in remember
       and "if (!state)" in remember,
       "a full TerrainType identity table is explicit in the CSV")
    rt_event_begin = definition_start(src, "void rememberTerrainRtEventAtFrame(")
    rt_event_end = block_end(src, rt_event_begin)
    rt_event = src[rt_event_begin:rt_event_end]
    ok(rt_event_begin >= 0 and rt_event_end > rt_event_begin
       and "InterlockedCompareExchange(first, framePlusOne, 0);" in rt_event
       and "InterlockedExchange(last, framePlusOne);" in rt_event
       and "InterlockedIncrement(count);" in rt_event
       and all(name in rt_event for name in [
           "rtLoadAttachCount", "rtLoadTexturesCount",
           "rtOwnerPreloadCount"]),
       "each TerrainRT boundary retains independent first/last/count history")
    report_begin = definition_start(src, "void reportOutsideDirResourcesAtMarker()")
    report_end = block_end(src, report_begin)
    report = src[report_begin:report_end]
    ok(report_begin >= 0 and report_end > report_begin
       and '"Engine trace: outside-dir Resource %ld TerrainRT"' in report
       and '" attach %u first %ld last %ld, LoadTextures %u"' in report
       and '" first %ld last %ld, owner PreLoad %u first %ld"' in report
       and "rtLoadAttachCount" in report
       and "rtLoadTexturesCount" in report
       and "rtOwnerPreloadCount" in report,
       "F12 writes all three exact TerrainRT histories during the session")
    load_begin = definition_start(src, "void __fastcall hookLoadResource(")
    load_end = block_end(src, load_begin)
    load = src[load_begin:load_end]
    original = load.find("g_loadResource(self, edx, resource);")
    snapshot = load.find("terrainPreloadSnapshot(terrainType)")
    retained = load.find("rememberOutsideDirResource(")
    ok(snapshot >= 0 and snapshot < original and retained > original
       and "g_activeTerrainThread == GetCurrentThreadId()" in load,
       "each terrain load retains its exact pre-call preload snapshot")
    render_begin = definition_start(src, "void __fastcall hookTerrainRenderGround(")
    render_end = block_end(src, render_begin)
    render = src[render_begin:render_end]
    original_ground = render.find("g_terrainRenderGround(")
    ok("GpuTerrainGround" in render and "currentGpuContext()" in render
       and original_ground >= 0
       and "CounterEngineTerrainGroundUs" in render,
       "RT RenderGround is bracketed by GPU timestamps and an engine _us span")

    for label, hook_name, gpu_name, count_name, duration_name in [
            ("LoadRenderData", "hookTerrainRtLoadRenderData",
             "GpuTerrainRtLoadRender", "CounterEngineTerrainRtLoadRender",
             "CounterEngineTerrainRtLoadRenderUs")]:
        begin = definition_start(src, "__fastcall %s(" % hook_name)
        end = block_end(src, begin)
        hook = src[begin:end]
        ok(begin >= 0 and end > begin and gpu_name in hook
           and "currentGpuContext()" in hook and count_name in hook
           and duration_name in hook,
           "%s has a GPU timestamp bracket and engine _us CPU span" % label)

    # Run 66 proved that per-call GPU timestamps are not passive at the colour
    # terrain frequency. Keep CPU counts/spans, but reject any reintroduction
    # of a GPU scope into either high-frequency wrapper.
    for label, hook_name, count_name, duration_name in [
            ("TerrainPlug", "hookTerrainPlugRender",
             "CounterEngineTerrainPlug", "CounterEngineTerrainPlugUs"),
            ("TerrainBlock", "hookTerrainBlockRender",
             "CounterEngineTerrainBlock", "CounterEngineTerrainBlockUs")]:
        begin = definition_start(src, "__fastcall %s(" % hook_name)
        end = block_end(src, begin)
        hook = src[begin:end]
        ok(begin >= 0 and end > begin
           and "GpuScope" not in hook and "currentGpuContext()" not in hook
           and count_name in hook and duration_name in hook,
           "%s retains CPU diagnostics without per-call GPU timestamps"
           % label)

    load_render_begin = definition_start(src, "int __fastcall hookTerrainRtLoadRenderData(")
    load_render_end = block_end(src, load_render_begin)
    load_render_hook = src[load_render_begin:load_render_end]
    ok(load_render_begin >= 0 and load_render_end > load_render_begin
       and "const bool main = onMainThread();" in load_render_hook
       and "main ? tq::probe::currentGpuContext() : nullptr"
           in load_render_hook
       and "main\n        ? tq::probe::CounterEngineTerrainRtLoadRenderMain\n"
           "        : tq::probe::CounterEngineTerrainRtLoadRenderOther);"
           in load_render_hook
       and "main\n        ? tq::probe::CounterEngineTerrainRtLoadRenderMainUs\n"
           "        : tq::probe::CounterEngineTerrainRtLoadRenderOtherUs,"
           " elapsed);" in load_render_hook
       and all(name in load_render_hook for name in [
           "CounterEngineTerrainRtLoadRenderMain",
           "CounterEngineTerrainRtLoadRenderMainUs",
           "CounterEngineTerrainRtLoadRenderOther",
           "CounterEngineTerrainRtLoadRenderOtherUs"]),
       "LoadRenderData timestamps only on the main thread and records an"
       " exact main/other CPU partition")

    gpu_context_begin = definition_start(probe_src, "ID3D11DeviceContext* currentGpuContextInternal()")
    gpu_context_end = block_end(probe_src, gpu_context_begin)
    gpu_context = probe_src[gpu_context_begin:gpu_context_end]
    ok(gpu_context_begin >= 0 and gpu_context_end > gpu_context_begin
       and "g_gpuCurrent && isRenderThread()" in gpu_context,
       "the shared GPU-context accessor rejects every non-render thread")

    load_hook_begin = definition_start(src, "int __fastcall hookTerrainRtLoad(")
    load_hook_end = block_end(src, load_hook_begin)
    load_hook = src[load_hook_begin:load_hook_end]
    texture_hook_begin = definition_start(src, "void __fastcall hookTerrainRtLoadTextures(")
    texture_hook_end = block_end(src, texture_hook_begin)
    texture_hook = src[texture_hook_begin:texture_hook_end]
    preload_hook_begin = definition_start(src, "void __fastcall hookTerrainRtPreload(")
    preload_hook_end = block_end(src, preload_hook_begin)
    preload_hook = src[preload_hook_begin:preload_hook_end]
    ok("if (result)" in load_hook
       and "TerrainRtLoadAttach" in load_hook,
       "successful TerrainRT::Load associates every admitted layer type")
    ok("g_terrainRtLoadTextures(self, edx);" in texture_hook
       and texture_hook.find(
           "rememberTerrainRtEvent(self, TerrainRtLoadTextures)")
           > texture_hook.find("g_terrainRtLoadTextures(self, edx);")
       and "CounterEngineTerrainRtLoadTexturesUs" in texture_hook,
       "LoadTextures completion is recorded after the exact original call")
    original_pos = texture_hook.find("g_terrainRtLoadTextures(self, edx);")
    preload_pos = texture_hook.find(
        "g_terrainPreloadEntry(self, nullptr, 1);")
    ok(original_pos >= 0 and preload_pos > original_pos
       and "g_terrainPreloadLayersActive && g_terrainPreloadEntry"
           in texture_hook,
       "layer fix invokes stock TerrainType::PreLoad(true) only after"
       " LoadTextures completes")
    read_options_begin = definition_start(src, "void readOptions(const wchar_t* iniPath)")
    read_options_end = block_end(src, read_options_begin)
    read_options = src[read_options_begin:read_options_end] + "\n".join(cpp_body("void " + f + "(") for f in ("readShadowOptions", "readTerrainOptions", "readSecondaryOptions"))
    public_install = src[read_options_end:]
    ok('L"performance", L"terrain_preload_layers", 1' in read_options
       and "const bool terrainPreload = g_terrainPreloadLayers;"
           in public_install
       and "!cache && !shadowDefer && !terrainPreload" in public_install
       and "traceTerrain || terrainPreload" in public_install,
       "terrain_preload_layers defaults on and independently reaches"
       " install()")
    ok("g_terrainPreloadEntry = needLoadTextures && preloadVerified" in install
       and "kTerrainPreloadRelocs, 1" in install
       and "g_terrainPreloadLayersActive = ok && preloadLayers;" in install,
       "behavior requires the verified exported TerrainType::PreLoad entry"
       " and activates only after the exact call patch succeeds")
    ok("TerrainRtOwnerPreload" in preload_hook
       and "CounterEngineTerrainRtPreloadLayers" in preload_hook
       and "CounterEngineTerrainRtPreloadUs" in preload_hook,
       "runtime owner PreLoad associates layers before calling through")

    shutdown_begin = definition_start(src, "void shutdown()")
    shutdown_end = block_end(src, shutdown_begin)
    shutdown = src[shutdown_begin:shutdown_end]
    ok(shutdown_begin >= 0 and shutdown_end > shutdown_begin
       and all(name in shutdown for name in [
           "detach(g_terrainRtLoadDetour)",
           "detach(g_terrainRtLoadRenderDataDetour)",
           "detach(g_terrainRtPreloadDetour)",
           "restoreCall(g_terrainRtLoadTexturesPatch)",
           "detach(g_terrainPlugRenderDetour)",
           "detach(g_terrainBlockRenderDetour)"])
       and "memset(g_terrainPreloadStates, 0" in shutdown,
       "shutdown restores every runtime terrain site and clears its history")
    ok("g_terrainPreloadEntry = nullptr;" in shutdown
       and "g_terrainPreloadLayersActive = false;" in shutdown
       and "g_terrainTracing = false;" in shutdown,
       "shutdown clears terrain layer-preload behavior and trace state")

    # Runtime PreLoad's complete body has no direct calls to either semantic
    # TerrainType preload routine. This does not claim that no virtual callee
    # can eventually touch a texture; it proves the omission in this owner.
    # The next function begins at 0x23d5c0, so 0x1c0 covers the complete
    # contiguous function plus its trailing alignment and nothing after it.
    rt_preload_body = engine.read(engine.base + const("kTerrainRtPreloadRva"),
                                  0x1c0)
    direct_targets = []
    for i in range(len(rt_preload_body) - 4):
        if rt_preload_body[i] == 0xe8:
            disp = struct.unpack_from("<i", rt_preload_body, i + 1)[0]
            direct_targets.append(const("kTerrainRtPreloadRva") + i + 5
                                  + disp)
    ok(const("kTerrainPreloadRva") not in direct_targets
       and const("kTerrainLoadTexturesRva") not in direct_targets,
       "TerrainRT::PreLoad directly calls neither TerrainType::PreLoad nor"
       " LoadTextures")

    required_counter_names = [
        "engine_terrain_rt_load", "engine_terrain_rt_load_us",
        "engine_terrain_rt_load_render", "engine_terrain_rt_load_render_us",
        "engine_terrain_rt_load_render_main",
        "engine_terrain_rt_load_render_main_us",
        "engine_terrain_rt_load_render_other",
        "engine_terrain_rt_load_render_other_us",
        "engine_terrain_rt_load_textures",
        "engine_terrain_rt_load_textures_us", "engine_terrain_rt_preload",
        "engine_terrain_rt_preload_us", "engine_terrain_rt_preload_layers",
        "engine_terrain_rt_layer_overflow", "engine_terrain_plug",
        "engine_terrain_plug_us", "engine_terrain_block",
        "engine_terrain_block_us"]
    required_gpu_names = ["gpu_terrain_rt_load_render"]
    ok(all(('"%s"' % name) in probe_src for name in required_counter_names)
       and all(('"%s"' % name) in probe_src for name in required_gpu_names)
       and all(re.search(r"\b%s\b" % re.escape(name), probe_h) for name in [
           "CounterEngineTerrainRtLoad",
           "CounterEngineTerrainRtLoadUs",
           "CounterEngineTerrainRtLoadRender",
           "CounterEngineTerrainRtLoadRenderUs",
           "CounterEngineTerrainRtLoadRenderMain",
           "CounterEngineTerrainRtLoadRenderMainUs",
           "CounterEngineTerrainRtLoadRenderOther",
           "CounterEngineTerrainRtLoadRenderOtherUs",
           "CounterEngineTerrainRtLoadTextures",
           "CounterEngineTerrainRtLoadTexturesUs",
           "CounterEngineTerrainRtPreload",
           "CounterEngineTerrainRtPreloadUs",
           "CounterEngineTerrainRtPreloadLayers",
           "CounterEngineTerrainRtLayerOverflow",
           "CounterEngineTerrainPlug", "CounterEngineTerrainPlugUs",
           "CounterEngineTerrainBlock", "CounterEngineTerrainBlockUs",
           "GpuTerrainRtLoadRender"]),
       "all Run 64 CPU and GPU fields have explicit CSV names")
    ok("GpuTerrainPlug" not in probe_h and "GpuTerrainBlock" not in probe_h
       and '"gpu_terrain_plug"' not in probe_src
       and '"gpu_terrain_block"' not in probe_src,
       "high-frequency colour classes have no GPU query fields")
    duration_bases = ["engine_terrain_rt_load",
                      "engine_terrain_rt_load_render",
                      "engine_terrain_rt_load_render_main",
                      "engine_terrain_rt_load_render_other",
                      "engine_terrain_rt_load_textures",
                      "engine_terrain_rt_preload", "engine_terrain_plug",
                      "engine_terrain_block"]
    ok(all(('"%s_us"' % name) in probe_src for name in duration_bases)
       and all(('"%s_ms"' % name) not in probe_src for name in duration_bases),
       "engine CPU durations use _us and no duplicate _ms field")
    csv_line = re.search(r"const unsigned kCsvLineBytes\s*=\s*(\d+);",
                         probe_src)
    ok(csv_line and int(csv_line.group(1)) == 32768
       and probe_src.count("char line[kCsvLineBytes];") == 2
       and "char line[8192];" not in probe_src,
       "the trace CSV header and rows share a 32 KiB audited bound")


def check_outside_directional_resources():
    """Prove run 61's passive complement and reaction-time report."""
    print("\nOutside-directional Resource attribution")
    slots = const("kOutsideDirResourceReportSlots")
    frames = const("kOutsideDirResourceMarkerFrames")
    chars = const("kOutsideDirResourceNameChars")
    depth = const("kOutsideDirResourceCallerDepth")
    ok(slots == 128 and frames == 120 and chars == 128 and depth == 24,
       "marker ring is 128 records, 120 frames, 128 filename bytes, and"
       " 24 upstream frames")

    load_begin = definition_start(src, "void __fastcall hookLoadResource(")
    load_end = block_end(src, load_begin)
    load = src[load_begin:load_end]
    original = load.find("g_loadResource(self, edx, resource);")
    copied = load.find("copyResourceName(resourceNameCopy, resourceName);")
    retained = load.find("rememberOutsideDirResource(")
    ok(load_begin >= 0 and load_end > load_begin
       and "const bool outsideDir = g_outsideDirResourceTracing && main"
           " && !inShadow;" in load
       and "const bool classify = (inShadow || outsideDir)" in load,
       "outside-directional loads require main thread and the live shadow bracket")
    ok("const char* const resourceName = (inShadow || outsideDir)" in load
       and "shadowResourceType(resourceName)" in load
       and copied >= 0 and copied < original,
       "the verified engine filename is classified and copied before loading")
    ok("__builtin_return_address(0)" in load
       and "caller, &resource, resourceNameCopy" in load
       and "outsideDirResourcePhase()" in load
       and "tq::probe::currentFrameIndex()" in load,
       "each retained load carries its immediate caller, stack, Engine phase,"
       " and frame")
    ok(original >= 0 and load.count("g_loadResource(self, edx, resource);") == 1
       and retained > original
       and "countOutsideDirResource(resourceType, outsidePhase, elapsed);"
           in load,
       "the exact original call runs once before passive counting and retention")

    remember_begin = definition_start(src, "void rememberOutsideDirResource(")
    remember_end = block_end(src, remember_begin)
    remember = src[remember_begin:remember_end]
    ok(remember_begin >= 0 and remember_end > remember_begin
       and "% kOutsideDirResourceReportSlots" in remember
       and "moduleOf(address)" in remember
       and "precededByCall(address, *module)" in remember,
       "the bounded ring labels only a call-shaped return in a verified module")
    ok("VirtualQuery(stack, &info, sizeof(info))" in remember
       and "info.State == MEM_COMMIT" in remember
       and "i < kStackWords" in remember
       and "words + i < limit" in remember
       and "report.callerDepth < kOutsideDirResourceCallerDepth" in remember
       and "const ChainModule* const module = moduleOf(value);" in remember
       and "if (!module || !precededByCall(value, *module)) continue;"
           in remember
       and "report.callerFrames[report.callerDepth].rva = rva;" in remember
       and "report.callerFrames[report.callerDepth].tag = module->tag;"
           in remember
       and "++report.callerDepth;" in remember,
       "the retained upstream list is a bounded call-shaped verified-module"
       " stack superset")
    ok("report.callerFrames[report.callerDepth - 1].rva == rva" in remember
       and "report.callerFrames[report.callerDepth - 1].tag"
           in remember
       and "== module->tag" in remember
       and "continue;" in remember,
       "the upstream list preserves stack order and collapses only consecutive"
       " duplicate sites")
    ok("copyResourceName(report.resource, resource);" in remember
       and "InterlockedExchange(&report.ready, sequence + 1);" in remember,
       "the bounded filename and all metadata are published last")

    report_begin = definition_start(src, "void reportOutsideDirResourcesAtMarker(")
    report_end = block_end(src, report_begin)
    report = src[report_begin:report_end]
    window_begin = definition_start(src, "OutsideDirResourceWindow outsideDirResourceWindow(")
    window_end = block_end(src, window_begin)
    window = src[window_begin:window_end]
    member_begin = definition_start(src, "bool outsideDirResourceInWindow(")
    member_end = block_end(src, member_begin)
    member = src[member_begin:member_end]
    ok(report_begin >= 0 and report_end > report_begin
       and "outsideDirResourceWindow(markerFrame)" in report
       and "outsideDirResourceInWindow(report, sequence, markerFrame)" in report
       and "g_outsideDirResourceReportedThrough" in report,
       "F12 emits each retained event once from the exact 120-frame window")
    ok(window_begin >= 0 and window_end > window_begin
       and "window.total - (LONG)kOutsideDirResourceReportSlots" in window
       and "markerFrame - oldest.frame <= kOutsideDirResourceMarkerFrames"
           in window
       and member_begin >= 0 and member_end > member_begin
       and "report.ready == sequence + 1" in member
       and "markerFrame - report.frame <= kOutsideDirResourceMarkerFrames"
           in member,
       "window membership and truncation use the same bounded ring metadata")
    ok("report.callerVerified" in report
       and '" caller %c+%#lx, resource %.*s' in report
       and '" caller unverified, resource %.*s' in report
       and "report.resource" in report
       and "report.callerDepth" in report
       and "appendFrame(at, chain + sizeof(chain) - 1" in report
       and "outside-dir Resource %ld upstream %u" in report,
       "the live report includes caller status, Resource name, and the"
       " retained upstream candidates")
    ok("CounterEngineResOutsideDirMarkerTruncated" in report
       and "window.truncated ? 1u : 0u" in report,
       "a marker reports rather than hides ring truncation")

    update_begin = definition_start(src, "void __fastcall hookEngineUpdate(")
    update_end = block_end(src, update_begin)
    update = src[update_begin:update_end]
    render_begin = definition_start(src, "void __fastcall hookEngineRender(")
    render_end = block_end(src, render_begin)
    render = src[render_begin:render_end]
    update_in = update.find("InterlockedIncrement(&g_insideEngineUpdate)")
    update_call = update.find("g_engineUpdate(self, edx, frustum, flag);")
    update_out = update.find("InterlockedDecrement(&g_insideEngineUpdate)")
    render_in = render.find("InterlockedIncrement(&g_insideEngineRender)")
    render_call = render.find("g_engineRender(self, edx);")
    render_out = render.find("InterlockedDecrement(&g_insideEngineRender)")
    ok(0 <= update_in < update_call < update_out
       and 0 <= render_in < render_call < render_out,
       "verified Engine Update and Render hooks bracket both phase classes")

    marker_begin = definition_start(src, "BOOL __stdcall hookPeekMessage(")
    marker_end = block_end(src, marker_begin)
    marker = src[marker_begin:marker_end]
    emit = marker.find("reportOutsideDirResourcesAtMarker();")
    mark = marker.find("tq::probe::markStutter();")
    ok(emit >= 0 and mark > emit and "message->wParam == VK_F12" in marker,
       "the existing F12 retrieval emits the pre-reaction records before marking")

    install_begin = definition_start(src, "bool install(HMODULE engine)")
    install_end = block_end(src, install_begin)
    install = src[install_begin:install_end]
    ok("g_outsideDirResourceTracing = g_loadResource && g_engineUpdate"
       in install
       and "&& g_engineRender && g_shadowTracing && g_resourceStateVerified"
           in install
       and "&& g_resourceFileNameVerified;" in install,
       "attribution activates only after every load, phase, shadow, and accessor hook")
    shutdown_begin = definition_start(src, "void shutdown()")
    shutdown_end = block_end(src, shutdown_begin)
    shutdown = src[shutdown_begin:shutdown_end]
    ok("g_outsideDirResourceTracing = false;" in shutdown
       and "InterlockedExchange(&g_insideEngineUpdate, 0);" in shutdown
       and "InterlockedExchange(&g_insideEngineRender, 0);" in shutdown
       and "memset(g_outsideDirResourceReports, 0," in shutdown,
       "shutdown disables phase classification and clears the marker ring")


def check_directional_mesh_resource_retention():
    """Prove Run 68 retains only cold directional mesh-load evidence."""
    print("\nDirectional cold-mesh Resource retention")
    ok(const("kShadowMeshResourceReportSlots") == 128
       and const("kShadowMeshResourceMarkerFrames") == 120,
       "directional mesh ring is 128 records with a 120-frame horizon")

    load_begin = definition_start(src, "void __fastcall hookLoadResource(")
    load_end = block_end(src, load_begin)
    load = src[load_begin:load_end]
    original = load.find("g_loadResource(self, edx, resource);")
    retained = load.find("rememberShadowMeshResource(")
    ok("const bool shadowMeshReport = g_shadowMeshResourceTracing && inShadow"
       in load
       and "&& classify && state == 0"
           " && resourceType == ShadowResourceMesh;" in load,
       "retention selects only verified state-0 directional mesh Resources")
    ok("const void* const caller = outsideDir || shadowMeshReport" in load
       and "const unsigned frame = outsideDir || shadowMeshReport" in load
       and "if (outsideDir || shadowMeshReport)" in load
       and load.find("copyResourceName(resourceNameCopy, resourceName);")
           < original,
       "candidate-frame work is bounded caller/frame/name capture before load")
    ok(original >= 0 and retained > original
       and "caller, &resource, resourceNameCopy, inQueue, frame" in load,
       "the original load runs once before directional mesh retention")

    remember_begin = definition_start(src, "void rememberShadowMeshResource(")
    remember_end = block_end(src, remember_begin)
    remember = src[remember_begin:remember_end]
    ok(remember_begin >= 0 and remember_end > remember_begin
       and "% kShadowMeshResourceReportSlots" in remember
       and "moduleOf(address)" in remember
       and "precededByCall(address, *module)" in remember
       and "VirtualQuery(stack, &info, sizeof(info))" in remember
       and "report.callerDepth < kOutsideDirResourceCallerDepth" in remember
       and "copyResourceName(report.resource, resource);" in remember
       and "InterlockedExchange(&report.ready, sequence + 1);" in remember,
       "retention publishes a bounded verified caller-chain record last")

    window_begin = definition_start(src, "ShadowMeshResourceWindow shadowMeshResourceWindow(")
    window_end = block_end(src, window_begin)
    window = src[window_begin:window_end]
    member_begin = definition_start(src, "bool shadowMeshResourceInWindow(")
    member_end = block_end(src, member_begin)
    member = src[member_begin:member_end]
    ok("window.total - (LONG)kShadowMeshResourceReportSlots" in window
       and "markerFrame - oldest.frame <= kShadowMeshResourceMarkerFrames"
           in window
       and "report.ready == sequence + 1" in member
       and "markerFrame - report.frame <= kShadowMeshResourceMarkerFrames"
           in member,
       "mesh window membership and truncation share exact ring metadata")

    report_begin = definition_start(src, "void reportShadowMeshResourcesAtMarker(")
    report_end = block_end(src, report_begin)
    report = src[report_begin:report_end]
    ok(report_begin >= 0 and report_end > report_begin
       and "directional mesh Resource %ld" in report
       and "state 0, queued %u" in report
       and '" %c+%#lx, resource %.*s' in report
       and '" unverified, resource %.*s' in report
       and "directional mesh Resource %ld upstream" in report
       and "window.truncated ? 1u : 0u" in report
       and "g_shadowMeshResourceReportedThrough" in report,
       "F12 log emits exact mesh identity, queue state, caller chain and"
       " truncation once")

    marker_begin = definition_start(src, "BOOL __stdcall hookPeekMessage(")
    marker_end = block_end(src, marker_begin)
    marker = src[marker_begin:marker_end]
    outside = marker.find("reportOutsideDirResourcesAtMarker();")
    mesh = marker.find("reportShadowMeshResourcesAtMarker();")
    mark = marker.find("tq::probe::markStutter();")
    ok(0 <= outside < mesh < mark and "message->wParam == VK_F12" in marker,
       "the existing F12 path emits both delayed reports before marking")

    install_begin = definition_start(src, "bool install(HMODULE engine)")
    install_end = block_end(src, install_begin)
    install = src[install_begin:install_end]
    shutdown_begin = definition_start(src, "void shutdown()")
    shutdown_end = block_end(src, shutdown_begin)
    shutdown = src[shutdown_begin:shutdown_end]
    ok("g_shadowMeshResourceTracing = g_loadResource && g_shadowTracing"
       in install
       and "&& g_resourceStateVerified && g_resourceFileNameVerified;"
           in install,
       "mesh retention activates only after load, shadow, state and name"
       " dependencies")
    ok("g_shadowMeshResourceTracing = false;" in shutdown
       and "memset(g_shadowMeshResourceReports, 0," in shutdown
       and "InterlockedExchange(&g_shadowMeshResourceSequence, 0);"
           in shutdown
       and "InterlockedExchange(&g_shadowMeshResourceReportedThrough, 0);"
           in shutdown,
       "shutdown disables and clears directional mesh retention")


def check_shadow_mesh_boundary(engine):
    """Prove the per-mesh omission/preload boundary and its one patched call."""
    print("\nDirectional-shadow cold-mesh boundary")
    code = table("kShadowMeshPassCountBytes")
    at = const("kShadowMeshPassCountRva")
    off = const("kShadowMeshEnsureCallOffset")
    mesh_field = const("kGraphicsMeshResourceOffset")
    ensure = const("kEnsureAvailableRva")
    ok(len(code) == 24,
       "GetNumShadowRenderPasses verifies all %d bytes (required 16-24)"
       % len(code))
    ok(code[0:3] == [0x56, 0x8b, 0x71] and code[3] == mesh_field
       and code[4:10] == [0x85, 0xf6, 0x74, 0x0c, 0x8b, 0xce],
       "the boundary obtains GraphicsMeshInstance+%#x and null-checks it"
       % mesh_field)
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

    prepare_begin = definition_start(src, "bool prepareShadowAlphaDefer(HMODULE engine)")
    prepare_end = block_end(src, prepare_begin)
    prepare = src[prepare_begin:prepare_end]
    ok(prepare_begin >= 0 and prepare_end > prepare_begin
       and "resolve(engine, kShadowMeshPassCountName" in prepare
       and "&& tq::detour::matches(\n"
           "               engine, passCount,\n"
           "               signature(kShadowMeshPassCountBytes" in prepare,
       "the fix verifies the root-mesh export before changing any entry")

    hook_begin = definition_start(src, "int __fastcall hookShadowMeshPassCount(void* self, void* edx)")
    hook_end = block_end(src, hook_begin)
    hook = src[hook_begin:hook_end]
    predicate_begin = definition_start(src, "bool shouldDeferShadowMesh(unsigned state)")
    predicate_end = block_end(src, predicate_begin)
    predicate = src[predicate_begin:predicate_end]
    ok(predicate_begin >= 0 and predicate_end > predicate_begin
       and "return state <= 1;" in predicate,
       "root-mesh state gate covers exactly unloaded and loading")
    ok(hook_begin >= 0 and hook_end > hook_begin
       and "!g_shadowDeferActive || !onMainThread()" in hook
       and "InterlockedCompareExchange(&g_insideDirectional, 0, 0) <= 0"
           in hook
       and "self + kGraphicsMeshResourceOffset" in hook
       and "if (!shouldDeferShadowMesh(state))" in hook
       and hook.count("return g_shadowMeshPassCount(self, edx);") == 3,
       "cold root-mesh omission is exact-class, main-thread and directional-only")
    ok("g_shadowEnqueue(loader, nullptr, mesh, 1, 1, 0);" in hook
       and "kResourceInQueueOffset" in hook
       and "countDeferredShadowMesh(state, enqueued, failed);" in hook
       and hook.rstrip().endswith("return 0;\n}"),
       "state-0 root meshes use the stock enqueue tuple and return zero passes")

    install_begin = definition_start(src, "if (ok && (g_shadowDeferColdResources || g_shadowDeferColdActorPose)) {")
    install_end = block_end(src, install_begin)
    install = src[install_begin:install_end]
    ok(install_begin >= 0 and install_end > install_begin
       and "g_shadowMeshPassCountDetour" in install
       and "signature(kShadowMeshPassCountBytes" in install
       and "6, (const void*)&hookShadowMeshPassCount" in install
       and "&& meshOk;" in install
       and "detach(g_shadowMeshPassCountDetour)" in install
       and "g_shadowMeshPassCount = nullptr;" in install,
       "the 24-byte root-mesh target steals six bytes and is atomic with the fix")
    ok("g_shadowDeferActive = deferOk;" in install,
       "root-mesh behavior becomes active only after the complete patch set")
    trace_begin = definition_start(src, "if (ok && trace && g_resourceStateVerified) {")
    trace_end = block_end(src, trace_begin)
    trace = src[trace_begin:trace_end]
    ok(trace_begin >= 0 and trace_end > trace_begin
       and "bool meshOk = g_shadowDeferActive\n"
           "            && g_shadowMeshPassCountDetour.installed;" in trace
       and "if (!meshOk)" in trace
       and "g_shadowMeshEnsurePatch" in trace,
       "the trace reuses the behavior detour instead of patching inside it")
    shutdown_begin = definition_start(src, "void shutdown()")
    shutdown_end = block_end(src, shutdown_begin)
    shutdown = src[shutdown_begin:shutdown_end]
    ok(shutdown_begin >= 0 and shutdown_end > shutdown_begin
       and "restoreCall(g_shadowMeshEnsurePatch);" in shutdown
       and "detach(g_shadowMeshPassCountDetour);" in shutdown
       and "g_shadowMeshPassCount = nullptr;" in shutdown,
       "shutdown restores either mesh instrument and clears the trampoline")


def check_shadow_actor_pose_boundary(engine):
    """Prove the earlier exact Actor cold-root deferral and call patch."""
    print("\nDirectional-shadow Actor pose boundary")
    callee = table("kActorUpdateMeshInstanceBytes")
    callee_at = const("kActorUpdateMeshInstanceRva")
    actor_mesh = const("kActorMeshInstanceOffset")
    call = table("kActorAddToSceneUpdateMeshWindowBytes")
    call_at = const("kActorAddToSceneUpdateMeshWindowRva")
    call_off = const("kActorAddToSceneUpdateMeshCallOffset")

    ok(len(callee) == 24,
       "Actor::UpdateMeshInstance verifies all 24 bytes around the shared"
       " prologue")
    ok(callee[:6] == [0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8]
       and callee[12:18] == [0x8b, 0x86, 0x84, 0x01, 0x00, 0x00]
       and struct.unpack_from("<I", bytes(callee), 14)[0] == actor_mesh,
       "the callee independently proves Actor+%#x is its mesh instance"
       % actor_mesh)
    ok(len(call) == 23,
       "Actor::AddToScene verifies a 23-byte call window")
    ok(call_off + 5 <= len(call) and call[call_off] == 0xe8,
       "Actor::AddToScene call offset lands on E8")
    dest = call_at + call_off + 5 + struct.unpack_from(
        "<i", bytes(call), call_off + 1)[0]
    ok(dest == callee_at,
       "Actor::AddToScene E8 resolves to Actor::UpdateMeshInstance (%#x)"
       % dest)
    ok(engine.exports().get(cstr("kActorUpdateMeshInstanceName")) == callee_at,
       "Actor::UpdateMeshInstance export resolves to its recorded RVA")

    hook_begin = definition_start(src, "void __fastcall hookShadowActorUpdateMeshInstance(")
    hook_end = block_end(src, hook_begin)
    hook = src[hook_begin:hook_end]
    ok(hook_begin >= 0 and hook_end > hook_begin
       and "!g_shadowActorPoseDeferActive || !onMainThread()" in hook
       and "InterlockedCompareExchange(&g_insideDirectional, 0, 0) <= 0"
           in hook
       and "self + kActorMeshInstanceOffset" in hook
       and "instance + kGraphicsMeshResourceOffset" in hook
       and "if (!shouldDeferShadowMesh(state))" in hook,
       "the earlier gate is exact Actor class, main-thread, directional and"
       " cold-state only")
    confirm_begin = definition_start(src, "bool shadowActorPoseQueueConfirmed(")
    confirm_end = block_end(src, confirm_begin)
    confirm = src[confirm_begin:confirm_end]
    ok(confirm_begin >= 0 and confirm_end > confirm_begin
       and "return state == 1 || (state == 0 && inQueue);" in confirm,
       "the queue postcondition accepts only loading or observably queued"
       " cold roots")
    ok(hook.count("g_actorUpdateMeshInstance(self, edx);") == 5
       and "g_shadowEnqueue(loader, nullptr, mesh, 1, 1, 0);" in hook
       and "countDeferredShadowActorPose(state, enqueued, false);" in hook,
       "all non-target paths forward once and state 0 uses the stock queue"
       " tuple")
    resident = hook.find("if (after >= 2) {")
    failed = hook.find("if (failed) {")
    counted = hook.find("countDeferredShadowActorPose(state, enqueued, false);")
    ok("enqueued = shadowActorPoseQueueConfirmed(after, inQueue);" in hook
       and 0 <= resident < failed < counted
       and "countShadowActorPoseEnqueueFailure();" in hook[failed:counted]
       and "g_actorUpdateMeshInstance(self, edx);" in hook[resident:failed]
       and "g_actorUpdateMeshInstance(self, edx);" in hook[failed:counted]
       and "return;" in hook[resident:failed]
       and "return;" in hook[failed:counted],
       "resident-after-enqueue and unconfirmed-enqueue paths fall back to"
       " stock pose work before a deferred event is counted")

    install_begin = definition_start(src, "if (ok && g_shadowDeferColdActorPose) {")
    install_end = block_end(src, install_begin)
    install = src[install_begin:install_end]
    ok(install_begin >= 0 and install_end > install_begin
       and "resolve(engine, kActorUpdateMeshInstanceName" in install
       and "signature(kActorUpdateMeshInstanceBytes" in install
       and "g_shadowDeferActive && updateMeshVerified" in install
       and "g_shadowActorUpdateMeshPatch" in install
       and "signature(kActorAddToSceneUpdateMeshWindowBytes" in install
       and "kActorAddToSceneUpdateMeshCallOffset, updateMesh" in install,
       "the earlier behavior activates only after both exact windows and the"
       " later caster gate")
    ok("g_shadowActorPoseDeferActive = actorPoseOk;" in install
       and "if (!actorPoseOk) g_actorUpdateMeshInstance = nullptr;" in install,
       "a failed call patch cannot leave the earlier behavior active")

    read_begin = definition_start(src, "void readOptions(const wchar_t* iniPath)")
    read_end = block_end(src, read_begin)
    options = src[read_begin:read_end] + "\n".join(cpp_body("void " + f + "(") for f in ("readShadowOptions", "readTerrainOptions", "readSecondaryOptions"))
    install_all_begin = definition_start(src, "bool install(HMODULE engine)")
    install_all_end = block_end(src, install_all_begin)
    install_all = src[install_all_begin:install_all_end]
    ok('L"shadow_defer_cold_actor_pose", 1' in options
       and "const bool shadowDefer = g_shadowDeferColdResources || shadowActorPose;"
           in install_all
       and "!g_tracing && !cache && !shadowDefer && !terrainPreload"
           in install_all,
       "the default-on fix reaches install with tracing disabled and implies"
       " the later root gate")
    ok('"engine_shadow_actor_pose_deferred"' in probe_src
       and '"engine_shadow_actor_pose_state0"' in probe_src
       and '"engine_shadow_actor_pose_state1"' in probe_src
       and '"engine_shadow_actor_pose_enqueued"' in probe_src
       and '"engine_shadow_actor_pose_enqueue_failed"' in probe_src
       and '"engine_shadow_actor_pose_deferred_us"' not in probe_src,
       "Actor pose diagnostics are counts and no engine duration is charged"
       " to the mod")

    shutdown_begin = definition_start(src, "void shutdown()")
    shutdown_end = block_end(src, shutdown_begin)
    shutdown = src[shutdown_begin:shutdown_end]
    ok("restoreCall(g_shadowActorUpdateMeshPatch);" in shutdown
       and "g_actorUpdateMeshInstance = nullptr;" in shutdown
       and "g_shadowActorPoseDeferActive = false;" in shutdown,
       "shutdown restores the Actor call before the later caster gate")


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

    report_begin = definition_start(src, "void reportShadowMaterialDependency(")
    report_end = block_end(src, report_begin)
    report = src[report_begin:report_end]
    flush_begin = definition_start(src, "void flushPendingShadowMaterialTexture(")
    flush_end = block_end(src, flush_begin)
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
    miss_begin = definition_start(src, "void explainShadowRecordMiss(")
    miss_end = block_end(src, miss_begin)
    miss = src[miss_begin:miss_end]
    ok(miss_begin >= 0 and miss_end > miss_begin
       and "g_meshShadowStyle" not in miss
       and "g_meshGetTexture" not in miss
       and "g_graphicsTextureGetTexture" not in miss
       and "context->match = ShadowContextInstanceMissing" in miss,
       "context miss explanation scans retained identities without engine calls")
    build_begin = definition_start(src, "int __fastcall hookBuildShadowRecord(")
    build_end = block_end(src, build_begin)
    build = src[build_begin:build_end]
    accepted = build.find("const int result = g_buildShadowRecord(")
    retained = build.find("rememberShadowRecordContext(")
    ok(build_begin >= 0 and build_end > build_begin
       and accepted >= 0 and retained > accepted
       and "if (result && g_shadowTracing" in build,
       "only accepted directional records populate the diagnostic join")
    filtered_begin = definition_start(src, 'extern "C" void* __cdecl shadowMaterialTextureFiltered(')
    filtered_end = block_end(src, filtered_begin)
    filtered = src[filtered_begin:filtered_end]
    ok(filtered_begin >= 0 and filtered_end > filtered_begin
       and "if (cold && !context.active) explainShadowRecordMiss(&context);"
           in filtered,
       "only a cold material-texture miss pays for the fallback identity scan")
    ok("context.outerInstanceSite = context.instance" in filtered
       and "|| outerCaller ==" in filtered,
       "outer caller retains the verified instance site through its wrapper")

    chain_begin = definition_start(src, "void reportUnresolvedShadowTextureChain(")
    chain_end = block_end(src, chain_begin)
    chain = src[chain_begin:chain_end]
    load_begin = definition_start(src, "void __fastcall hookLoadResource(")
    load_end = block_end(src, load_begin)
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

    wrapper_begin = definition_start(src, 'extern "C" void __cdecl shadowInstanceBumpEnsureFiltered(')
    wrapper_end = block_end(src, wrapper_begin)
    wrapper = src[wrapper_begin:wrapper_end]
    ok(wrapper_begin >= 0 and wrapper_end > wrapper_begin
       and "g_shadowDeferActive && inShadow && shader" in wrapper
       and "&& !g_shaderHasParameter(" in wrapper
       and "g_engineBase + kBumpTextureNameRva" in wrapper
       and "g_ensureAvailable(texture, nullptr)" in wrapper,
       "bump omission is directional-only and forwards every used case")
    install_begin = definition_start(src, "if (ok && (g_shadowDeferColdResources || g_shadowDeferColdActorPose)) {")
    install_end = block_end(src, install_begin)
    install = src[install_begin:install_end]
    ok(install_begin >= 0 and install_end > install_begin
       and "g_shadowInstanceBumpEnsurePatch" in install
       and "hookShadowInstanceBumpEnsure" in install
       and "restoreCall(g_shadowInstanceBumpEnsurePatch)" in install
       and "const bool deferOk = recordOk && contextOk && filterOk && bumpOk"
           in install
       and "&& meshOk;" in install,
       "cold-resource fix requires verified bump and mesh patches atomically")

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

    filtered_begin = definition_start(src, 'extern "C" void* __cdecl shadowMaterialTextureFiltered(')
    filtered_end = block_end(src, filtered_begin)
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
    context_begin = definition_start(src, 'extern "C" void __cdecl shadowMeshSetShaderParametersContext(')
    context_end = block_end(src, context_begin)
    context_adapter = src[context_begin:context_end]
    ok(context_begin >= 0 and context_end > context_begin
       and "if (!onMainThread()" in context_adapter
       and "|| InterlockedCompareExchange(&g_insideDirectional, 0, 0) <= 0"
           in context_adapter
       and "g_graphicsMeshSetShaderParameters(" in context_adapter
       and "return;" in context_adapter,
       "global call patch exposes context only on the main directional path")


def check_play_render_flow(engine):
    """Static authority for the interpreted DX11 play-render flow.

    These are documentation targets, not patch sites.  Keeping independent
    16--24-byte windows here prevents a stale decompiler name or prose address
    from silently becoming the authority for the next trace.
    """
    print("\nComplete DX11 play-render flow documentation targets")
    sites = [
        (0x133240, "Display::Render",
         "56578bf933f68b0f8b47042bc1c1f80285c07428538b5c24"),
        (0x1584c0, "GraphicsEngine::Update",
         "558bec83e4c06aff689988291064a1000000005083ec6853"),
        (0x182230, "GraphicsPortalRenderer::Render",
         "558bec83e4f8f30f101548962f1083ec0c0f57c9568bf18b"),
        (0x17e2c0, "portal recursion",
         "558bec83e4f86aff68fb9c291064a1000000005081ec7c04"),
        (0x17e579, "DX11 branch call",
         "8b4c2428f30f110424565750e8460500008bf089742414"),
        (0x17ead0, "DX11 region branch",
         "558bec83e4f06aff683f9d291064a1000000005051b8581d"),
        (0x17f2c6, "reflection-manager call",
         "8d4c2458660fd6842444010000e8987f00000186f8000000"),
        (0x187270, "GraphicsReflectionManager::RenderReflections",
         "558bec83e4f883ec0c535657ff750c8bf189742414e876f7"),
        (0x1872b0, "per-reflection-plane call",
         "ff750c8b0bff750803ce51e810efffff83c6484f"),
        (0x1861d0, "reflection forward-render helper",
         "6aff68cb9d291064a1000000005081ec480f0000a100f036"),
        (0x1864f0, "reflection BuildScene call",
         "66c78424200e00000000898424e00d0000e8ca74ffff"),
        (0x18693b, "reflection RenderLightStyle call",
         "6a0068c8b34110ff70188d8c24d004000056e8ee30ffff"),
        (0x179a40, "GraphicsForwardRenderer::RenderLightStyle",
         "558bec83e4f881eca0000000a100f0361033c48984249c00"),
        (0x179b8f, "forward scene-list wrapper call",
         "6a0068204e00006affff742444ff74241857e8fa0e0000"),
        (0x17aaa0, "forward scene-list wrapper",
         "6aff68329b291064a1000000005083ec1057a100f0361033"),
        (0x17ab04, "sorted scene-list executor call",
         "516a00ff7424348d44241850ff7424388bcfe8d5d80000"),
        (0x1883f0, "sorted scene-list executor",
         "6aff683f9f291064a1000000005083ec3053555657a100f0"),
        (0x1885a2, "renderable virtual dispatch",
         "8b02ff7424588b08ff7424648b1150ff52288b44"),
        (0x18ce70, "GraphicsShadowMapRenderer::Render",
         "558bec83e4f86aff68e9a3291064a1000000005083ec2853"),
        (0x18d04f, "DX11 shadow-record builder call",
         "e81cf8ffff518d44241450578bcee8bef4ffffc6"),
        (0x18c870, "DX11 shadow-record builder loop",
         "81eca8000000a100f0361033c4898424a4000000538b9c24"),
        (0x18d05d, "DX11 shadow-record executor call",
         "e8bef4ffffc6442440008b44241085c0740a50ff"),
        (0x18c520, "DX11 shadow-record executor",
         "83ec10538b5c241c558b5304894c24108b0b5657c644241c"),
        (0x18c613, "shadow renderable virtual dispatch",
         "ff56288b4c241ceb048b5c242881c58800000049"),
    ]
    for rva, label, expected_hex in sites:
        expected = bytes.fromhex(expected_hex)
        actual = engine.read(engine.base + rva, len(expected))
        ok(16 <= len(expected) <= 24 and actual == expected,
           "%s @ Engine+%#x verifies %d bytes"
           % (label, rva, len(expected)))
        row_key = "| `0x%x` |" % rva
        ok(row_key in flow_doc and label in flow_doc,
           "%s RVA is present in disassembly-targets.md" % label)

    calls = [
        (0x17e585, 0x17ead0, "portal recursion -> DX11 region branch"),
        (0x17f2d3, 0x187270, "DX11 branch -> reflection manager"),
        (0x1872bb, 0x1861d0, "reflection manager -> per-plane renderer"),
        (0x186501, 0x17d9d0, "reflection renderer -> BuildScene"),
        (0x18694d, 0x179a40, "reflection renderer -> RenderLightStyle"),
        (0x179ba1, 0x17aaa0, "RenderLightStyle -> scene-list wrapper"),
        (0x17ab16, 0x1883f0, "wrapper -> sorted scene-list executor"),
        (0x17fc9b, 0x166130, "DX11 branch -> deferred renderer"),
        (0x18d04f, 0x18c870,
         "DX11 shadow renderer -> shadow-record builder"),
        (0x18d05d, 0x18c520,
         "DX11 shadow renderer -> shadow-record executor"),
    ]
    for call_rva, target_rva, label in calls:
        body = engine.read(engine.base + call_rva, 5)
        target = call_rva + 5 + struct.unpack_from("<i", body, 1)[0]
        ok(body[0] == 0xe8 and target == target_rva,
           "%s exact E8 resolves to Engine+%#x" % (label, target_rva))

    dispatch = engine.read(engine.base + 0x1885b1, 3)
    ok(dispatch == b"\xff\x52\x28",
       "sorted scene-list executor dispatches renderable vtable slot +0x28")
    shadow_dispatch = engine.read(engine.base + 0x18c613, 3)
    ok(shadow_dispatch == b"\xff\x56\x28",
       "DX11 shadow-record executor dispatches renderable vtable slot +0x28")


def check_reflections(engine):
    """Run 73's exact recursive-branch reflection attribution."""
    print("\nDX11 recursive-branch reflection attribution")
    window(engine, "kReflectionManagerBytes", "kReflectionManagerBytes",
           const("kReflectionManagerRva"), None)
    manager = table("kReflectionManagerBytes")
    ok(16 <= len(manager) <= 24,
       "reflection manager entry verifies 16-24 bytes")
    ok(engine.exports().get(cstr("kReflectionManagerName"))
       == const("kReflectionManagerRva"),
       "GraphicsReflectionManager::RenderReflections export resolves")

    window(engine, "kReflectionManagerCallWindowBytes",
           "kReflectionManagerCallWindowBytes",
           const("kReflectionManagerCallWindowRva"), None)
    manager_call = table("kReflectionManagerCallWindowBytes")
    manager_offset = const("kReflectionManagerCallOffset")
    manager_site = const("kReflectionManagerCallWindowRva") + manager_offset
    manager_target = manager_site + 5 + struct.unpack_from(
        "<i", bytes(manager_call), manager_offset + 1)[0]
    ok(16 <= len(manager_call) <= 24
       and manager_call[manager_offset] == 0xe8
       and manager_target == const("kReflectionManagerRva"),
       "DX11 branch reflection E8 resolves to the exported manager")

    window(engine, "kReflectionManagerTailBytes",
           "kReflectionManagerTailBytes",
           const("kReflectionManagerTailRva"), None)
    manager_tail = table("kReflectionManagerTailBytes")
    ok(len(manager_tail) == 16
       and manager_tail[-3:] == [0xc2, 0x08, 0x00],
       "reflection manager tail proves two explicit arguments")

    window(engine, "kReflectionPlaneBytes", "kReflectionPlaneBytes",
           const("kReflectionPlaneRva"), "kReflectionPlaneRelocs")
    plane = table("kReflectionPlaneBytes")
    plane_relocs = relocs("kReflectionPlaneRelocs")
    ok(16 <= len(plane) <= 24 and plane_relocs == [(3, 0x299dcb)]
       and plane[3:7] == [0, 0, 0, 0],
       "reflection-plane helper verifies 16-24 bytes and its relocation")

    window(engine, "kReflectionPlaneCallWindowBytes",
           "kReflectionPlaneCallWindowBytes",
           const("kReflectionPlaneCallWindowRva"), None)
    plane_call = table("kReflectionPlaneCallWindowBytes")
    plane_offset = const("kReflectionPlaneCallOffset")
    plane_site = const("kReflectionPlaneCallWindowRva") + plane_offset
    plane_target = plane_site + 5 + struct.unpack_from(
        "<i", bytes(plane_call), plane_offset + 1)[0]
    ok(16 <= len(plane_call) <= 24
       and plane_call[plane_offset] == 0xe8
       and plane_target == const("kReflectionPlaneRva"),
       "manager's per-plane E8 resolves to the forward-render helper")

    window(engine, "kReflectionPlaneTailBytes", "kReflectionPlaneTailBytes",
           const("kReflectionPlaneTailRva"), None)
    plane_tail = table("kReflectionPlaneTailBytes")
    ok(len(plane_tail) == 24
       and plane_tail[20:23] == [0xc2, 0x0c, 0x00],
       "reflection-plane tail proves three explicit arguments")

    for prefix, label, args, entry_relocs, call_relocs in [
            ("kReflectionBuildScene", "reflection BuildScene", 1,
             [(9, 0x299ca0)], []),
            ("kReflectionRenderLight", "reflection RenderLightStyle", 4,
             [(13, 0x36f000)], [(3, 0x41b3c8)])]:
        window(engine, prefix + "Bytes", prefix + "Bytes",
               const(prefix + "Rva"), prefix + "Relocs")
        entry = table(prefix + "Bytes")
        ok(16 <= len(entry) <= 24 and relocs(prefix + "Relocs") == entry_relocs
           and all(entry[off:off + 4] == [0, 0, 0, 0]
                   for off, _ in entry_relocs),
           "%s entry verifies 16-24 bytes and exact relocations" % label)
        ok(engine.exports().get(cstr(prefix + "Name")) == const(prefix + "Rva"),
           "%s export resolves to its recorded RVA" % label)
        ok(("| `0x%x` |" % const(prefix + "Rva")) in flow_doc
           and ("`0x%x`" % const(prefix + "TailRva")) in flow_doc,
           "%s target and ABI tail are documented" % label)
        window(engine, prefix + "CallWindowBytes", prefix + "CallWindowBytes",
               const(prefix + "CallWindowRva"),
               prefix + "CallRelocs" if call_relocs else None)
        call = table(prefix + "CallWindowBytes")
        offset = const(prefix + "CallOffset")
        if not ok(0 <= offset and offset + 5 <= len(call),
                  label + ' call offset fits its verified E8 window'):
            continue
        site = const(prefix + "CallWindowRva") + offset
        target = site + 5 + struct.unpack_from("<i", bytes(call), offset + 1)[0]
        ok(16 <= len(call) <= 24 and call[offset] == 0xe8
           and target == const(prefix + "Rva")
           and (not call_relocs or relocs(prefix + "CallRelocs") == call_relocs)
           and all(call[off:off + 4] == [0, 0, 0, 0]
                   for off, _ in call_relocs),
           "%s exact E8 and call-window relocations verify" % label)
        window(engine, prefix + "TailBytes", prefix + "TailBytes",
               const(prefix + "TailRva"), None)
        tail = table(prefix + "TailBytes")
        cleanup = bytes([0xc2, args * 4, 0])
        ok(16 <= len(tail) <= 24 and bytes(tail).endswith(cleanup),
           "%s tail proves %u explicit argument(s)" % (label, args))

    install_begin = definition_start(src, "bool installReflections(HMODULE engine, bool trace,"
        " bool deferAdmissionMesh,\n                        bool deferAdmissionAll,\n"
        "                        bool secondaryPassAdmission)")
    install_end = block_end(src, install_begin)
    install = src[install_begin:install_end]
    first_patch = install.find("tq::detour::patchCall(")
    ok(install_begin >= 0 and install_end > install_begin
       and first_patch > install.find("const bool verified")
       and all(name in install[:first_patch] for name in [
           "kReflectionManagerBytes", "kReflectionManagerCallWindowBytes",
           "kReflectionManagerTailBytes", "kReflectionPlaneBytes",
           "kReflectionPlaneCallWindowBytes", "kReflectionPlaneTailBytes",
           "kReflectionBuildSceneBytes",
           "kReflectionBuildSceneCallWindowBytes",
           "kReflectionBuildSceneTailBytes",
           "kReflectionRenderLightBytes",
           "kReflectionRenderLightCallWindowBytes",
           "kReflectionRenderLightTailBytes"]),
       "all reflection entries, call windows, and ABI tails verify before writes")
    ok("patchCall(\n        g_reflectionManagerPatch" in install
       and "patchCall(\n        g_reflectionPlanePatch" in install
       and "patchCall(\n        g_reflectionBuildScenePatch" in install
       and "patchCall(\n        g_reflectionRenderLightPatch" in install
       and "kReflectionManagerCallOffset, manager,\n"
           "        (const void*)&hookReflectionManager" in install
       and "kReflectionPlaneCallOffset, base + kReflectionPlaneRva,\n"
           "        (const void*)&hookReflectionPlane" in install
       and "restoreCall(g_reflectionManagerPatch)" in install
       and "restoreCall(g_reflectionBuildScenePatch)" in install
       and "restoreCall(g_reflectionRenderLightPatch)" in install
       and "g_reflectionChildTracing = trace;" in install
       and "g_reflectionTracing = trace;" in install
       and "g_reflectionDeferAdmissionMeshActive = deferAdmissionMesh;"
           in install
       and "g_reflectionDeferAdmissionAllActive = deferAdmissionAll;"
           in install
       and "g_secondaryPassAdmissionActive = secondaryPassAdmission;"
           in install,
       "manager, plane, both children, and mesh use verified atomic patches")

    expected_names = [
        "engine_reflection_manager", "engine_reflection_manager_us",
        "engine_reflection_manager_draw_us",
        "engine_reflection_manager_res_load",
        "engine_reflection_manager_res_load_us",
        "engine_reflection_manager_tex_create",
        "engine_reflection_manager_tex_create_us",
        "engine_reflection_manager_buf_create",
        "engine_reflection_manager_buf_create_us",
        "engine_reflection_manager_overflow",
        "engine_reflection_i1", "engine_reflection_i1_us",
        "engine_reflection_i1_draw_us",
        "engine_reflection_i2", "engine_reflection_i2_us",
        "engine_reflection_i2_draw_us", "engine_reflection_plane_overflow",
    ]
    for prefix in ["i1_p1", "i1_p2", "i2_p1", "i2_p2"]:
        expected_names.extend([
            "engine_reflection_" + prefix,
            "engine_reflection_" + prefix + "_us",
            "engine_reflection_" + prefix + "_draw_us",
            "engine_reflection_" + prefix + "_res_load",
            "engine_reflection_" + prefix + "_res_load_us",
            "engine_reflection_" + prefix + "_tex_create",
            "engine_reflection_" + prefix + "_tex_create_us",
            "engine_reflection_" + prefix + "_buf_create",
            "engine_reflection_" + prefix + "_buf_create_us",
            "engine_reflection_" + prefix + "_build_scene",
            "engine_reflection_" + prefix + "_build_scene_us",
            "engine_reflection_" + prefix + "_render_light",
            "engine_reflection_" + prefix + "_render_light_us",
        ])
    expected_names.extend([
        "engine_reflection_admission_deferred",
        "engine_reflection_admission_mesh_deferred",
        "engine_reflection_admission_all_deferred",
    ])
    actual_names = re.findall(r'"(engine_reflection_[^"]+)"', probe_src)
    ok(actual_names == expected_names,
       "all 72 reflection counters have exact ordered CSV columns")
    manager_header = re.search(
        r"CounterEngineReflectionManager,(.*?)#define TQ_REFLECTION_CELL_COUNTERS",
        probe_h, re.S)
    actual_manager_enums = (["CounterEngineReflectionManager"]
        + re.findall(r"\b(CounterEngineReflection\w+)\b",
                     manager_header.group(1))) if manager_header else []
    expected_manager_enums = [
        "CounterEngineReflectionManager",
        "CounterEngineReflectionManagerUs",
        "CounterEngineReflectionManagerDrawUs",
        "CounterEngineReflectionManagerResLoad",
        "CounterEngineReflectionManagerResLoadUs",
        "CounterEngineReflectionManagerTexCreate",
        "CounterEngineReflectionManagerTexCreateUs",
        "CounterEngineReflectionManagerBufCreate",
        "CounterEngineReflectionManagerBufCreateUs",
        "CounterEngineReflectionManagerOverflow",
        "CounterEngineReflectionI1", "CounterEngineReflectionI1Us",
        "CounterEngineReflectionI1DrawUs",
        "CounterEngineReflectionI2", "CounterEngineReflectionI2Us",
        "CounterEngineReflectionI2DrawUs",
        "CounterEngineReflectionPlaneOverflow"]
    ok(actual_manager_enums == expected_manager_enums,
       "reflection manager counter enum order is exact")
    header_cell_macro = re.search(
        r"#define TQ_REFLECTION_CELL_COUNTERS\(prefix\)(.*?)\n    TQ_REFLECTION_CELL_COUNTERS\(I1P1\)",
        probe_h, re.S)
    actual_header_cell_fields = re.findall(
        r"CounterEngineReflection##prefix(?:##\w+)*\b",
        header_cell_macro.group(1)) if header_cell_macro else []
    expected_header_cell_fields = [
        "CounterEngineReflection##prefix",
        "CounterEngineReflection##prefix##Us",
        "CounterEngineReflection##prefix##DrawUs",
        "CounterEngineReflection##prefix##ResLoad",
        "CounterEngineReflection##prefix##ResLoadUs",
        "CounterEngineReflection##prefix##TexCreate",
        "CounterEngineReflection##prefix##TexCreateUs",
        "CounterEngineReflection##prefix##BufCreate",
        "CounterEngineReflection##prefix##BufCreateUs",
        "CounterEngineReflection##prefix##BuildScene",
        "CounterEngineReflection##prefix##BuildSceneUs",
        "CounterEngineReflection##prefix##RenderLight",
        "CounterEngineReflection##prefix##RenderLightUs"]
    ok(actual_header_cell_fields == expected_header_cell_fields
       and re.findall(r"TQ_REFLECTION_CELL_COUNTERS\((I\dP\d)\)", probe_h)
           == ["I1P1", "I1P2", "I2P1", "I2P2"],
       "reflection cell counter enum expansion is exact")
    reflection_durations = [name for name in actual_names
                            if name.endswith("_us")]
    ok(len(reflection_durations) == 37
       and all(name.endswith("_us") for name in reflection_durations),
       "all reflection engine durations are integer `_us` counters")
    expected_gpu_names = [
        "gpu_reflection_i1", "gpu_reflection_i2",
        "gpu_reflection_i1_p1", "gpu_reflection_i1_p2",
        "gpu_reflection_i2_p1", "gpu_reflection_i2_p2"]
    for prefix in ["i1_p1", "i1_p2", "i2_p1", "i2_p2"]:
        expected_gpu_names.extend([
            "gpu_reflection_" + prefix + "_build_scene",
            "gpu_reflection_" + prefix + "_render_light"])
    actual_gpu_names = re.findall(r'"(gpu_reflection_[^"]+)"', probe_src)
    ok(actual_gpu_names == expected_gpu_names,
       "manager, plane, and child GPU phases all have exact CSV columns")
    gpu_header = re.search(r"enum GpuPhase \{(.*?)GpuPhaseCount", probe_h,
                           re.S)
    actual_gpu_enums = re.findall(r"\b(GpuReflection\w+)\b",
                                  gpu_header.group(1)) \
        if gpu_header else []
    ok(actual_gpu_enums == [
           "GpuReflectionI1", "GpuReflectionI2",
           "GpuReflectionI1P1", "GpuReflectionI1P2",
           "GpuReflectionI2P1", "GpuReflectionI2P2",
           "GpuReflectionI1P1BuildScene", "GpuReflectionI1P1RenderLight",
           "GpuReflectionI1P2BuildScene", "GpuReflectionI1P2RenderLight",
           "GpuReflectionI2P1BuildScene", "GpuReflectionI2P1RenderLight",
           "GpuReflectionI2P2BuildScene", "GpuReflectionI2P2RenderLight"],
       "reflection GPU phase enum order is exact")
    cell_macro = re.search(
        r"#define TQ_REFLECTION_CELL_ROW\(prefix\)(.*?)\nconst ",
        src, re.S)
    expected_cell_fields = [
        "CounterEngineReflection##prefix",
        "CounterEngineReflection##prefix##Us",
        "CounterEngineReflection##prefix##DrawUs",
        "CounterEngineReflection##prefix##ResLoad",
        "CounterEngineReflection##prefix##ResLoadUs",
        "CounterEngineReflection##prefix##TexCreate",
        "CounterEngineReflection##prefix##TexCreateUs",
        "CounterEngineReflection##prefix##BufCreate",
        "CounterEngineReflection##prefix##BufCreateUs",
        "CounterEngineReflection##prefix##BuildScene",
        "CounterEngineReflection##prefix##BuildSceneUs",
        "CounterEngineReflection##prefix##RenderLight",
        "CounterEngineReflection##prefix##RenderLightUs",
    ]
    actual_cell_fields = re.findall(
        r"CounterEngineReflection##prefix(?:##\w+)*\b",
        cell_macro.group(1)) if cell_macro else []
    ok(actual_cell_fields == expected_cell_fields
       and all(("TQ_REFLECTION_CELL_ROW(%s)" % prefix) in src
               for prefix in ["I1P1", "I1P2", "I2P1", "I2P2"]),
       "four reflection cells map all thirteen count/duration classes")
    for table_name, expected in [
            ("kReflectionManagerCountCounters",
             ["CounterCount", "CounterEngineReflectionI1",
              "CounterEngineReflectionI2"]),
            ("kReflectionManagerDurationCounters",
             ["CounterCount", "CounterEngineReflectionI1Us",
              "CounterEngineReflectionI2Us"]),
            ("kReflectionManagerDrawCounters",
             ["CounterCount", "CounterEngineReflectionI1DrawUs",
              "CounterEngineReflectionI2DrawUs"]),
            ("kReflectionManagerGpuPhases",
             ["GpuPhaseCount", "GpuReflectionI1", "GpuReflectionI2"]),
            ("kReflectionCellGpuPhases",
             ["GpuPhaseCount", "GpuReflectionI1P1", "GpuReflectionI1P2",
              "GpuReflectionI2P1", "GpuReflectionI2P2"])]:
        match = re.search(r"const tq::probe::\w+ %s\[\] = \{(.*?)\};"
                          % table_name, src, re.S)
        actual = re.findall(r"tq::probe::(\w+)", match.group(1)) \
            if match else []
        ok(actual == expected, "%s preserves exact enum order" % table_name)
    child_gpu = re.search(
        r"const tq::probe::GpuPhase kReflectionChildGpuPhases"
        r"\[\]\[ReflectionChildCount\] = \{(.*?)\n\};", src, re.S)
    child_gpu_symbols = re.findall(r"tq::probe::(Gpu\w+)",
                                   child_gpu.group(1)) if child_gpu else []
    ok(child_gpu_symbols == [
           "GpuPhaseCount", "GpuPhaseCount",
           "GpuReflectionI1P1BuildScene", "GpuReflectionI1P1RenderLight",
           "GpuReflectionI1P2BuildScene", "GpuReflectionI1P2RenderLight",
           "GpuReflectionI2P1BuildScene", "GpuReflectionI2P1RenderLight",
           "GpuReflectionI2P2BuildScene", "GpuReflectionI2P2RenderLight"],
       "reflection child GPU phases preserve exact cell/child order")
    ok("kGroupReflections = 0x20000" in src
       and "if (traceReflections || secondaryAdmission)" in src
       and "reflectionReady = installReflections(\n"
           "            engine, traceReflections, false, false, secondaryAdmission);"
           in src
       and "g_traceMask & kGroupReflections" in src
       and "//131072 the unique reflection-manager call" in engine_h,
       "reflection trace is group 131072 and shares the existing draw clock")

    build_hook_begin = definition_start(src, "void __fastcall hookReflectionBuildScene(")
    build_hook_end = block_end(src, build_hook_begin)
    build_hook = src[build_hook_begin:build_hook_end]
    light_hook_begin = build_hook_end
    light_hook_end = src.find("\n}\n\nstruct DeferredOwnerScope", light_hook_begin)
    light_hook = src[light_hook_begin:light_hook_end]
    mesh_hook_begin = definition_start(src, "void __fastcall hookGraphicsMeshInstanceRenderPass(")
    mesh_hook_end = block_end(src, mesh_hook_begin)
    mesh_hook = src[mesh_hook_begin:mesh_hook_end]
    buffer_note_begin = definition_start(src, "void noteDeferredBufferCreated(")
    buffer_note_end = block_end(src, buffer_note_begin)
    buffer_note = src[buffer_note_begin:buffer_note_end]
    read_begin = definition_start(src, "void readOptions(const wchar_t* iniPath)")
    read_end = block_end(src, read_begin)
    read_options = src[read_begin:read_end] + "\n".join(cpp_body("void " + f + "(") for f in ("readShadowOptions", "readTerrainOptions", "readSecondaryOptions"))
    ok(const("kReflectionAdmissionBufferThreshold") == 32
       and "reflectionAdmissionThresholdReached(buffers)" in build_hook
       and "g_reflectionAdmissionBuildActive = true;" in build_hook
       and "g_reflectionAdmissionPending" in build_hook
       and "CounterEngineReflectionAdmissionDeferred" in build_hook
       and "g_reflectionAdmissionRenderActive" in light_hook
       and "g_reflectionAdmissionPending = false;" in light_hook
       and "CounterEngineReflectionAdmissionMeshDeferred" in mesh_hook
       and "return;" in mesh_hook
       and "g_reflectionAdmissionBuildActive" in buffer_note
       and "++g_reflectionAdmissionBuildBuffers;" in buffer_note,
       "32-buffer BuildScene admission defers only the following mesh pass")
    ok('L"reflection_defer_admission_mesh"' not in read_options
       and 'L"reflection_defer_admission_all"' not in read_options
       and "reflectionAdmissionBufferTrackingRequested()" in visual_src
       and "return false;" in src[buffer_note_end:buffer_note_end + 140]
       and "the removed reflection_defer_admission_mesh key is ignored"
           in selftest_src
       and "the removed reflection_defer_admission_all key is ignored"
           in selftest_src
       and "reflectionAdmissionTriggeredForTest(31)" in selftest_src
       and "reflectionAdmissionTriggeredForTest(32)" in selftest_src,
       "rejected reflection omissions cannot be configured or buffer-arm")
    ok("const bool deferAll = g_reflectionDeferAdmissionAllActive"
           in light_hook
       and "if (!deferAll && g_gpuChunkTracing" in light_hook
       and "CounterEngineReflectionAdmissionAllDeferred" in light_hook
       and "const bool needMesh = trace || deferAdmissionMesh\n"
           "                       || secondaryPassAdmission;" in install
       and "(!needMesh || tq::detour::attach(" in install
       and "false, false, secondaryAdmission" in src
       and "engine_reflection_admission_all_deferred" in probe_src,
       "archived whole-admission fields retain schema but behavior stays disabled")
    admission_prefixes = [
        "reflection_i2_p1", "deferred_i2_setup", "deferred_i2_scene",
        "shadow_directional"]
    expected_admission_names = []
    for prefix in admission_prefixes:
        expected_admission_names.extend([
            "engine_admission_" + prefix + "_draw",
            "engine_admission_" + prefix + "_terrain_plug",
            "engine_admission_" + prefix + "_terrain_plug_first",
            "engine_admission_" + prefix + "_terrain_block",
            "engine_admission_" + prefix + "_terrain_block_first",
            "engine_admission_" + prefix + "_mesh",
            "engine_admission_" + prefix + "_mesh_first"])
    expected_admission_names.append("engine_admission_identity_overflow")
    actual_admission_names = re.findall(r'"(engine_admission_[^"]+)"',
                                        probe_src)
    ok(actual_admission_names == expected_admission_names
       and not any(name.endswith(("_us", "_ms"))
                   for name in actual_admission_names),
       "all 29 admission identity columns are exact counts in consumer order")
    ok(const("kAdmissionRenderableIdentitySlots") == 8192
       and const("kAdmissionRenderableIdentityProbe") == 16
       and const("kAdmissionRenderableIdentityHashSalt") == 0x9e3779b1
       and "g_admissionRenderableIdentities["
           "kAdmissionRenderableIdentitySlots]" in src
       and "value ^= value >> 7;" in src
       and "value ^= value >> 15;" in src
       and "(uintptr_t)kind * kAdmissionRenderableIdentityHashSalt" in src
       and "(start + i) & (kAdmissionRenderableIdentitySlots - 1)" in src
       and "i < kAdmissionRenderableIdentityProbe" in src
       and "CounterEngineAdmissionIdentityOverflow" in src,
       "admission renderable identity lookup is bounded, power-of-two, and overflow-visible")
    admission_consumer_begin = definition_start(src, "enum AdmissionConsumer {")
    admission_consumer_end = block_end(src, admission_consumer_begin)
    admission_consumer = cpp_body("AdmissionConsumer currentAdmissionConsumer(") + cpp_body("bool admissionRenderableFirst(")
    ok(admission_consumer_begin >= 0
       and "reflection.cell == ReflectionCellI2P1" in admission_consumer
       and "g_insideDirectional" in admission_consumer
       and "deferred.invocation != 2" in admission_consumer
       and "deferred.site == DeferredGeometrySiteSetup" in admission_consumer
       and "deferred.site == DeferredGeometrySiteScene" in admission_consumer
       and "entry->consumerMask & mask" in admission_consumer,
       "first-seen identity is distinct for all four exact rendering consumers")
    ok("countAdmissionRenderable(GpuChunkTerrainPlug, self);" in src
       and "countAdmissionRenderable(GpuChunkTerrainBlock, self);" in src
       and "countAdmissionRenderable(GpuChunkMeshInstance, self);" in src
       and "countAdmissionDraw();" in src
       and "memset(g_admissionRenderableIdentities, 0," in src
       and "admission identities are first once per renderable class"
           in selftest_src,
       "existing exact wrappers count renderables and draws without a new patch or clock")

    secondary_names = re.findall(
        r'"(engine_secondary_admission_[^"]+)"', probe_src)
    ok(secondary_names == [
           "engine_secondary_admission_trigger",
           "engine_secondary_admission_reflection_admitted",
           "engine_secondary_admission_reflection_deferred",
           "engine_secondary_admission_shadow_admitted",
           "engine_secondary_admission_shadow_deferred",
           "engine_secondary_admission_draw_skipped"]
       and not any(name.endswith(("_us", "_ms"))
                   for name in secondary_names),
       "all six progressive secondary-admission columns are exact counts")
    secondary_begin = definition_start(src, "enum SecondaryAdmissionState {")
    secondary_end = block_end(src, secondary_begin)
    secondary = cpp_body("SecondaryAdmissionContext currentSecondaryAdmissionContext(") + cpp_body("bool shouldDeferSecondaryAdmission(") + cpp_body("void armSecondaryAdmission(") + cpp_body("enum SecondaryAdmissionState {")
    draw_begin = definition_start(visual_src, "void WINAPI hookDraw(")
    draw_end = definition_start(visual_src, "void WINAPI hookDrawIndexed(")
    draw_hook = visual_src[draw_begin:draw_end]
    indexed_begin = draw_end
    indexed_end = visual_src.find("\n}\n\n}  // namespace", indexed_begin)
    indexed_hook = visual_src[indexed_begin:indexed_end]
    ok(const("kSecondaryPassAdmissionBudgetMax") == 64
       and 'L"secondary_pass_admission_budget", 8' in src
       and "secondaryBudget <= (int)kSecondaryPassAdmissionBudgetMax"
           in src
       and "g_secondaryPassAdmissionBudget = secondaryBudget > 0"
           in src,
       "secondary-pass object budget defaults to eight and is bounded at 64")
    ok("SecondaryAdmissionUnseen" in secondary
       and "SecondaryAdmissionAdmitted" in secondary
       and "SecondaryAdmissionPending" in secondary
       and "g_secondaryAdmissionUsedThisFrame"
           " < g_secondaryPassAdmissionBudget" in secondary
       and "entry->secondaryState = SecondaryAdmissionPending;" in secondary
       and "entry->secondaryState = SecondaryAdmissionAdmitted;" in secondary
       and "findAdmissionRenderableIdentity(object, kind, true)" in secondary,
       "one bounded identity table carries pending/admitted state across both secondary consumers")
    ok("g_insideReflectionRenderLight" in secondary
       and "g_insideDirectional" in secondary
       and "SecondaryAdmissionContextReflection" in secondary
       and "SecondaryAdmissionContextShadow" in secondary
       and "currentSecondaryAdmissionContext()" in secondary
       and "currentDeferredLocation" not in secondary,
       "progressive behavior applies only to reflection and exact directional shadow, never normal colour")
    ok("(g_tracing && tq::probe::drawTimingEnabled())" in build_hook
       and "armSecondaryAdmission();" in secondary
       and "if (admission) armSecondaryAdmission();" not in light_hook
       and "g_insideReflectionRenderLight = true;" in light_hook
       and "g_insideReflectionRenderLight = priorInsideReflection;"
           in light_hook
       and "if (regionChanged) armSecondaryAdmission();" not in src
       and "g_secondaryAdmissionUsedThisFrame"
           " < g_secondaryPassAdmissionBudget" in secondary
       and secondary.find("armSecondaryAdmission();")
           > secondary.find("g_secondaryAdmissionUsedThisFrame"
                            " < g_secondaryPassAdmissionBudget")
       and "CounterEngineSecondaryAdmissionTrigger" in secondary,
       "budget-plus-one unseen identity self-arms one coordinated population")
    ok("SecondaryAdmissionDrawScope secondaryAdmission(" in src
       and src.count("SecondaryAdmissionDrawScope secondaryAdmission(") == 6
       and "g_terrainPlugRender(self, edx, a, b, c, d);" in src
       and "g_terrainBlockRender(self, edx, a, b, c, d);" in src
       and "g_graphicsMeshInstanceRenderPass(\n"
           "        self, edx, pass, name, canvas, sceneRenderer);" in src
       and "secondaryAdmissionDrawSuppressed()" in draw_hook
       and "secondaryAdmissionDrawSuppressed()" in indexed_hook
       and "noteSecondaryAdmissionDrawSkipped();" in draw_hook
       and "noteSecondaryAdmissionDrawSkipped();" in indexed_hook
       and "g_draw(context, count, start);" in draw_hook
       and "g_drawIndexed(context, count, start, base);" in indexed_hook,
       "deferred renderables keep resource/material setup while only their D3D draws are suppressed")
    frame_boundary = definition_start(src, "void secondaryAdmissionFrameBoundary()")
    ok(frame_boundary >= 0
       and "++g_secondaryAdmissionFrameSerial;" in
           src[frame_boundary:frame_boundary + 220]
       and "tq::secondaryadmission::secondaryAdmissionFrameBoundary();"
           in visual_src
       and visual_src.find(
           "tq::secondaryadmission::secondaryAdmissionFrameBoundary();")
           < visual_src.find("tq::probe::beginFrame(g_context);")
       and "currentFrameIndex" not in secondary,
       "the fix has a frame serial even when performance_trace is off")
    ok("const bool secondaryAdmission = g_secondaryPassAdmissionBudget != 0;"
           in src
       and "|| secondaryAdmission" in src
       and "terrainReady = installTerrain" in src
       and "shadowReady = installShadow" in src
       and "reflectionReady = installReflections" in src
       and "secondaryAdmission\n"
           "        && g_secondaryAdmissionDrawHooksReady && terrainReady\n"
           "        && shadowReady && reflectionReady" in src
       and "secondaryPassAdmissionRequested()" in visual_src
       and "setSecondaryAdmissionDrawHooksReady(" in visual_src
       and "|| secondaryAdmissionDrawHooks" in visual_src
       and "secondaryAdmissionDrawHooksReady = tq::rendererdraw::install("
           in visual_src
       and visual_src.find("setSecondaryAdmissionDrawHooksReady(")
           < visual_src.find("tq::engine::install(")
       and "return g_secondaryPassAdmissionBudget != 0;" in src
       and "bool reflectionAdmissionBufferTrackingRequested() {\n"
           "    return false;" in src
       and "g_secondaryAdmissionDrawHooksReady = false;" in src
       and "secondary_pass_admission_budget reaches install()"
           in selftest_src
       and "a secondary-pass-admission-only boot installs no trace group"
           in selftest_src
       and "one frame admits two shared secondary identities without arming"
           in selftest_src
       and "identity budget plus one self-arms and remains pending globally"
           in selftest_src
       and "secondary-pass admission does not use reflection buffers"
           in selftest_src
       and "a pending secondary identity can enter on the next frame's budget"
           in selftest_src,
       "secondary-pass admission is default-on, trace-independent, Draw-atomic, and self-tested")
    ok("countReflectionDraw(elapsedUs);" in src
       and "countReflectionCreation(kind == DeferredCreationTexture" in src
       and "countReflectionResource(elapsed);" in src,
       "existing draw, D3D creation, and Resource samples gain reflection context")
    ok("reflection i%u/p%u" in src
       and "report.reflectionManager" in src
       and "report.reflectionPlane" in src,
       "F12 Resource identities retain exact reflection manager/plane context")
    ok("CounterEngineReflectionManagerOverflow" in src
       and "CounterEngineReflectionPlaneOverflow" in src
       and "invocation <= 2" in src
       and "reflectionCell(manager, plane)" in src,
       "two managers by two planes are bounded and overflow-visible")
    ok("active(g_reflectionTracing && onMainThread())" in src
       and "currentReflectionLocation()" in src
       and "if (!g_reflectionTracing || !onMainThread()) return result;" in src
       and "TerrainPreloadSnapshot(), DeferredLocation(),\n"
           "                               ReflectionLocation());" in src,
       "reflection context is main-thread-only and cannot leak to loader work")
    ok("setReflectionContextForTest(2, 1);" in selftest_src
       and "0, tq::probe::CounterEngineReflectionI2P1DrawUs) == 31" in selftest_src
       and "0, tq::probe::CounterEngineReflectionI1P1DrawUs) == 0" in selftest_src
       and "0, tq::probe::CounterEngineReflectionI2P1TexCreateUs) == 29" in selftest_src
       and "0, tq::probe::CounterEngineReflectionI2P1BufCreateUs) == 19" in selftest_src,
       "self-test requires exact reflection draw and D3D creation partition")
    ok("g_reflectionTracing = false;" in src
       and "g_reflectionChildTracing = false;" in src
       and "restoreCall(g_reflectionRenderLightPatch)" in src
       and "restoreCall(g_reflectionBuildScenePatch)" in src
       and "restoreCall(g_reflectionPlanePatch)" in src
       and "restoreCall(g_reflectionManagerPatch)" in src,
       "shutdown disables reflection classification before restoring all calls")


def check_cross_pass_identity():
    """Run 74's bounded first-use buffer correlation."""
    print("\nCross-pass first-use buffer identity")
    expected_names = [
        "engine_crosspass_buffer_created",
        "engine_crosspass_buffer_created_bytes",
        "engine_crosspass_reflection_draw",
        "engine_crosspass_shadow_draw",
        "engine_crosspass_deferred_draw",
        "engine_crosspass_fresh_reflection_buffer",
        "engine_crosspass_fresh_shadow_buffer",
        "engine_crosspass_fresh_deferred_buffer",
        "engine_crosspass_join_reflection_shadow",
        "engine_crosspass_join_reflection_deferred",
        "engine_crosspass_join_shadow_deferred",
        "engine_crosspass_join_all_three",
        "engine_crosspass_index_overflow",
        "engine_crosspass_recent_eviction",
    ]
    actual_names = re.findall(r'"(engine_crosspass_[^"]+)"', probe_src)
    ok(actual_names == expected_names
       and '"engine_shadow_directional_draw"' in probe_src,
       "all cross-pass and directional-draw counters have exact CSV columns")
    ok(all(not name.endswith("_ms") for name in actual_names)
       and not any("crosspass" in name and name.endswith("_us")
                   for name in actual_names),
       "cross-pass identity columns are counts/bytes, never mod durations")
    ok(const("kCrossPassBufferSlots") == 4096
       and const("kCrossPassIndexSlots") == 8192
       and const("kCrossPassIndexProbe") == 16
       and const("kCrossPassFreshFrames") == 120
       and const("kCrossPassMarkerReportLimit") == 128
       and "g_crossPassBuffers[(sequence - 1) % kCrossPassBufferSlots]" in src,
       "cross-pass creation retention/index has exact fixed bounds and wrapping")
    family = re.search(r"enum CrossPassFamily \{(.*?)\};", src, re.S)
    family_values = re.findall(r"(CrossPass\w+)\s*=\s*(\d+)",
                               family.group(1)) if family else []
    ok(family_values == [("CrossPassNone", "0"),
                         ("CrossPassReflection", "1"),
                         ("CrossPassShadow", "2"),
                         ("CrossPassDeferred", "4")]
       and "kCrossPassIndexTombstone = (const void*)(uintptr_t)1" in src
       and "kCrossPassIndexSlots - 1" in src,
       "cross-pass family bits, tombstone, and power-of-two index mask are exact")
    note_begin = definition_start(src, "void noteCrossPassBufferCreated(")
    note_end = block_end(src, note_begin)
    note_body = src[note_begin:note_end]
    ok("!g_crossPassTracing || !object || !onMainThread()" in note_body
       and "currentReflectionLocation()" in note_body
       and "currentDeferredLocation()" in note_body
       and "CounterEngineCrossPassRecentEviction" in note_body
       and "CounterEngineCrossPassIndexOverflow" in note_body,
       "only main-thread creations retain exact creation context and loss")
    lookup_begin = definition_start(src, "CrossPassBufferRecord* findCrossPassBuffer(")
    lookup_end = block_end(src, lookup_begin)
    lookup = src[lookup_begin:lookup_end]
    ok("i < kCrossPassIndexProbe" in lookup
       and "g_crossPassIndex[(start + i) & (kCrossPassIndexSlots - 1)]"
           in lookup
       and "record.sequence != entry.sequence" in lookup
       and "for (unsigned back" not in lookup,
       "draw-time identity lookup is bounded hash probing, not a ring scan")
    draw_begin = definition_start(src, "void countCrossPassDraw(")
    draw_end = block_end(src, draw_begin)
    draw = src[draw_begin:draw_end]
    ok(draw.find("reflection.cell") < draw.find("else if (directional)")
       < draw.find("else if (deferred.invocation)")
       and "CounterEngineCrossPassReflectionDraw" in draw
       and "CounterEngineCrossPassShadowDraw" in draw
       and "CounterEngineCrossPassDeferredDraw" in draw
       and "CounterEngineShadowDirectionalDraw" in draw,
       "draws classify reflection, then exact directional, then deferred owner")
    ok("DeferredTraceVertexBufferSlots + 1" in draw
       and "bindings->indexBuffer" in draw
       and "duplicate |= objects[j] == object" in draw
       and "duplicate |= objects[j] == bindings->indexBuffer" in draw,
       "existing four-VB plus index snapshot is deduplicated without getters")
    ok(draw.count("CounterEngineCrossPassFresh") == 3
       and draw.count("TQ_COUNT_CROSS_JOIN(") == 5
       and all(name in draw for name in [
           "CounterEngineCrossPassJoinReflectionShadow",
           "CounterEngineCrossPassJoinReflectionDeferred",
           "CounterEngineCrossPassJoinShadowDeferred",
           "CounterEngineCrossPassJoinAllThree"]),
       "first-family and all pair/all-three join milestones are one-shot")
    ok("now()" not in draw and "gpuBegin" not in draw
       and "GetVertexBuffers" not in draw and "GetIndexBuffer" not in draw,
       "cross-pass draw classification adds no clock, GPU query, or state getter")
    report_begin = definition_start(src, "void reportCrossPassBuffersAtMarker()")
    report_end = block_end(src, report_begin)
    report = src[report_begin:report_end]
    ok("marker - record.createdFrame > kCrossPassFreshFrames" in report
       and "emitted >= kCrossPassMarkerReportLimit" in report
       and 'ring capacity"' in report and '" %u\\r\\n"' in report
       and "index overflows %u" in report
       and "recent ring evictions %u" in report
       and "omitted" in report,
       "F12 report bounds its horizon/output and exposes truncation")
    ok("reportCrossPassBuffersAtMarker();" in src
       and src.find("reportCrossPassBuffersAtMarker();")
           < src.find("tq::probe::markStutter();"),
       "cross-pass identities are written during the session before F12 marks")
    ok("const bool crossPass = wants(kGroupReflections)" in src
       and "traceShadow || shadowDeferReady || crossPass" in src
       and "wants(kGroupDeferredPasses) || crossPass" in src
       and "g_deferredPassTracing && g_reflectionTracing" in src
       and "g_reflectionChildTracing;" in src,
       "cross-pass activation requires every exact reflection/shadow/deferred bracket")
    ok("setCrossPassTracingForTest(true);" in selftest_src
       and "CounterEngineCrossPassJoinAllThree) == 1" in selftest_src
       and "CounterEngineCrossPassIndexOverflow) == 0" in selftest_src
       and "CounterEngineCrossPassRecentEviction) == 0" in selftest_src,
       "self-test requires exact one-shot all-three correlation")


def _check_gpu_draw_chunks_run76(engine):
    """Sparse reflection/shadow GPU draw subdivision, corrected by Run 76."""
    print("\nSparse reflection/shadow GPU draw chunks")
    ok(const("kGpuChunkDraws") == 64
       and const("kGpuChunkCount") == 16
       and const("kGpuChunkEventSlots") == 32
       and const("kGpuChunkMarkerFrames") == 120
       and const("kGpuChunkReflectionBuildSceneTriggerUs") == 2000,
       "chunk width/count, rings, horizon, and reflection trigger are exact")

    counter_names = re.findall(r'"(engine_gpu_chunk_[^"]+)"', probe_src)
    ok(counter_names == [
           "engine_gpu_chunk_reflection_arm",
           "engine_gpu_chunk_reflection_start_draw",
           "engine_gpu_chunk_reflection_draw",
           "engine_gpu_chunk_reflection_overflow",
           "engine_gpu_chunk_reflection_collision",
           "engine_gpu_chunk_shadow_arm",
           "engine_gpu_chunk_shadow_draw",
           "engine_gpu_chunk_shadow_overflow",
           "engine_gpu_chunk_shadow_collision"],
       "all sparse-event counters have exact count-only CSV columns")
    ok(not any(name.endswith("_us") or name.endswith("_ms")
               for name in counter_names),
       "sparse-event metadata is never charged as a mod or engine duration")

    expected_gpu = (["gpu_chunk_reflection_%02d" % i for i in range(16)]
                    + ["gpu_chunk_shadow_setup"]
                    + ["gpu_chunk_shadow_%02d" % i for i in range(16)])
    actual_gpu = re.findall(
        r'"(gpu_chunk_(?:reflection_\d\d|shadow_(?:setup|\d\d)))"',
        probe_src)
    ok(actual_gpu == expected_gpu,
       "reflection chunks, shadow setup, and shadow chunks are exact")
    gpu_header = re.search(r"enum GpuPhase \{(.*?)GpuPhaseCount", probe_h,
                           re.S)
    gpu_enums = re.findall(
        r"\b(GpuChunk(?:Reflection\d\d|Shadow(?:Setup|\d\d)))\b",
                           gpu_header.group(1)) if gpu_header else []
    expected_enums = (["GpuChunkReflection%02d" % i for i in range(16)]
                      + ["GpuChunkShadowSetup"]
                      + ["GpuChunkShadow%02d" % i for i in range(16)])
    ok(gpu_enums == expected_enums,
       "the 33 unique sparse query IDs preserve exact enum order")

    arm_begin = definition_start(src, "void armGpuChunks(")
    arm_end = block_end(src, arm_begin)
    arm = src[arm_begin:arm_end]
    ok("g_gpuChunkEvents[g_gpuChunkEventSequence++"
           " % kGpuChunkEventSlots]" in arm
       and "g_gpuChunkLastFrame[kind] == frame + 1" in arm
       and "gpuChunkCollisionCounter(kind)" in arm
       and "deferRecording" in arm
       and "GpuChunkShadowSetup" in arm,
       "one bounded event either records now or opens separate shadow setup")

    reflection_hook = cpp_body('void __fastcall hookReflectionRenderLight(')
    child_begin = src.find("struct ReflectionChildScope")
    child_end = src.find("\n};\n\nint __fastcall hookReflectionManager", child_begin)
    child = src[child_begin:child_end]
    ok("child == ReflectionChildBuildScene" in child
       and "cell == ReflectionCellI2P1" in child
       and "elapsed >= kGpuChunkReflectionBuildSceneTriggerUs" in child
       and "g_reflectionGpuChunkTriggerUs = elapsed" in child
       and "armGpuChunks(GpuChunkReflection, 1, &location" in reflection_hook
       and reflection_hook.find("armGpuChunks(GpuChunkReflection")
           < reflection_hook.find("ReflectionChildScope scope(")
           < reflection_hook.rfind("g_reflectionRenderLight(self"),
       "slow exact i2/p1 BuildScene sparsely arms the whole following RenderLightStyle")

    shadow_begin = definition_start(src, "int __fastcall hookRenderDirectional(")
    shadow_end = block_end(src, shadow_begin)
    shadow = src[shadow_begin:shadow_end]
    reuse = shadow.find("reusePreviousShadow(")
    shadow_arm = shadow.find("armGpuChunks(GpuChunkShadow")
    original = shadow.find("g_renderDirectional(self")
    shadow_close = shadow.find("closeGpuChunks(GpuChunkShadow);")
    ok("g_gpuChunkTracing && regionChanged" in shadow
       and 0 <= reuse < shadow_arm < original < shadow_close,
       "region-changing directional opens setup and retains an outer fallback close")

    ok("&g_reflectionChild, (LONG)which + 1" in child
       and "closeGpuChunks(GpuChunkReflection);" in child
       and "InterlockedExchange(&g_reflectionChild, priorChild);" in child,
       "exact reflection child tracks/restores identity and closes whole-child chunks")

    for label, rva, rel in [
            ("kShadowRecordExecutorCallWindowBytes",
             "kShadowRecordExecutorCallWindowRva", None),
            ("kShadowRecordExecutorBytes", "kShadowRecordExecutorRva", None),
            ("kShadowRecordExecutorTailBytes",
             "kShadowRecordExecutorTailRva", None)]:
        window(engine, label, label, const(rva), rel)
    call = table("kShadowRecordExecutorCallWindowBytes")
    call_at = const("kShadowRecordExecutorCallWindowRva")
    call_off = const("kShadowRecordExecutorCallOffset")
    target = call_at + call_off + 5 + struct.unpack_from(
        "<i", bytes(call), call_off + 1)[0]
    tail = bytes(table("kShadowRecordExecutorTailBytes"))
    ok(16 <= len(call) <= 24 and call[call_off] == 0xe8
       and target == const("kShadowRecordExecutorRva")
       and 16 <= len(tail) <= 24 and tail.endswith(b"\xc2\x0c\x00"),
       "verified DX11 executor E8 resolves exactly and ABI pops three arguments")
    executor = cpp_body('void __fastcall hookShadowRecordExecutor(')
    ok(executor.find("beginShadowRecordExecutorChunks();")
           < executor.find("g_shadowRecordExecutor(self")
           < executor.find("closeGpuChunks(GpuChunkShadow);")
       and "GpuChunkShadowSetup" in src[
           src.find("void beginShadowRecordExecutorChunks()"):
           src.find("\n}\n\nvoid closeGpuChunks")],
       "DX11 executor ends setup, records only executor draws, and closes there")

    before_begin = definition_start(src, "void beginGpuChunkDrawInternal(")
    before_end = block_end(src, before_begin)
    before = src[before_begin:before_end]
    finish_begin = before_end + 3
    finish_end = src.find("\n}\n\nvoid reportGpuChunksAtMarker", finish_begin)
    finish = src[finish_begin:finish_end]
    ok("openGpuChunk(active);" in before
       and "if (bin.draws != kGpuChunkDraws) continue;" in finish
       and "gpuEnd(" in finish
       and "++active.chunk;" in finish
       and "active.event->overflow = true;" in finish,
       "game draws advance exact 64-draw bins with bounded overflow")
    ok("GetVertexBuffers" not in before + finish
       and "GetIndexBuffer" not in before + finish
       and "GetShader" not in before + finish
       and "bindings->pixelResources[0]" in finish
       and "bindings->vertexBuffers[0]" in finish,
       "chunk identity reuses setter snapshots and adds no D3D state getter")

    draw_begin = definition_start(visual_src, "void WINAPI hookDraw(")
    indexed_begin = definition_start(visual_src, "void WINAPI hookDrawIndexed(")
    draw = visual_src[draw_begin:indexed_begin]
    indexed_end = block_end(visual_src, indexed_begin)
    indexed = visual_src[indexed_begin:indexed_end]
    ok(draw.find("beginGpuChunkDraw(context)") < draw.find("g_draw(context")
       < draw.find("finishGpuChunkDraw(")
       and indexed.find("beginGpuChunkDraw(context)")
           < indexed.find("g_drawIndexed(context")
           < indexed.find("drawGrassCross(context")
           < indexed.find("finishGpuChunkDraw("),
       "Draw hooks bracket game submission and include an enhanced-grass companion")
    ok("gpuChunkDrawActive()" in draw
       and draw.find("gpuChunkDrawActive()")
           < draw.find("beginGpuChunkDraw(context)")
       and "gpuChunkDrawActive()" in indexed
       and "extern volatile LONG gpuChunkDrawActive" in engine_h,
       "ordinary draws inline-gate both helper calls before crossing modules")

    install_begin = definition_start(src, "bool install(HMODULE engine)")
    install_end = block_end(src, install_begin)
    install = src[install_begin:install_end]
    ok("wants(kGroupShadow) && wants(kGroupReflections)" in install
       and "&& tq::probe::drawTimingEnabled()" in install
       and "g_gpuChunkTracing = gpuChunks && g_renderDirectional" in install
       and "&& g_shadowRecordExecutor" in install
       and "&& g_shadowTracing && g_reflectionTracing" in install
       and "&& g_reflectionChildTracing" in install,
       "activation requires exact shadow executor, reflection, and draw dependencies")
    shadow_install = cpp_body('bool installShadow(')
    first_write = shadow_install.find("tq::detour::patchCall(")
    ok(first_write > shadow_install.find("kShadowRecordExecutorTailBytes")
       and "patchCall(\n            g_shadowRecordExecutorPatch" in shadow_install
       and "if (!executorOk)" in shadow_install
       and shadow_install.find("restoreCall(g_shadowDirectionalPatch)")
           < shadow_install.find("if (ok && (g_shadowDeferColdResources")
       and "restoreCall(g_shadowRecordExecutorPatch)" in src[
           src.find("void shutdown()"):src.find("#ifdef TQ_SELFTEST")],
       "executor and outer E8 install atomically and restore on shutdown")
    marker = cpp_body('BOOL __stdcall hookPeekMessage(')
    ok(marker.find("reportGpuChunksAtMarker();")
           < marker.find("tq::probe::markStutter();"),
       "F12 writes retained chunk metadata during the session before marking")
    shutdown = cpp_body('void shutdown()')
    ok("g_gpuChunkTracing = false;" in shutdown
       and "gpuChunkDrawActive, 0" in shutdown
       and "InterlockedExchange(&g_reflectionChild, 0);" in shutdown,
       "shutdown disables sparse classification before restoring hooks")
    ok("sparse reflection and shadow GPU chunks partition at 64 draws"
           in selftest_src
       and "gpuChunkBinDrawsForTest(true, 0) == 64" in selftest_src
       and "gpuChunkBinDrawsForTest(true, 1) == 1" in selftest_src
       and "ordinary draws bypass sparse GPU chunk helpers" in selftest_src
       and "directional setup stays outside draw chunks" in selftest_src,
       "self-test requires gate transitions and exact 64+1 reflection partition")


def check_gpu_draw_chunks(engine):
    """Run 79's continuous reflection subdivision and class correlation."""
    print("\nContinuous reflection GPU draw chunks")
    ok(const("kGpuChunkDraws") == 20
       and const("kGpuChunkStartDraw") == 1
       and const("kGpuChunkCount") == 16
       and const("kGpuChunkEventSlots") == 32
       and const("kGpuChunkMarkerFrames") == 120
       and const("kGpuChunkRenderableCallSlots") == 256
       and const("kGpuChunkRenderableHotCpuUs") == 250
       and const("kGpuChunkReflectionBuildSceneTriggerUs") == 2000,
       "20-draw continuous width/start, query/event/call bounds, horizon, and trigger are exact")

    window(engine, "kGraphicsMeshInstanceRenderPassBytes",
           "kGraphicsMeshInstanceRenderPassBytes",
           const("kGraphicsMeshInstanceRenderPassRva"),
           "kGraphicsMeshInstanceRenderPassRelocs")
    window(engine, "kGraphicsMeshInstanceRenderPassTailBytes",
           "kGraphicsMeshInstanceRenderPassTailBytes",
           const("kGraphicsMeshInstanceRenderPassTailRva"), None)
    mesh_entry = table("kGraphicsMeshInstanceRenderPassBytes")
    mesh_tail = bytes(table("kGraphicsMeshInstanceRenderPassTailBytes"))
    ok(len(mesh_entry) == 24
       and mesh_entry[:6] == [0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8]
       and mesh_entry[13:17] == [0, 0, 0, 0]
       and relocs("kGraphicsMeshInstanceRenderPassRelocs")
           == [(13, 0x36f000)]
       and len(mesh_tail) == 23 and mesh_tail.endswith(b"\xc2\x10\x00"),
       "mesh RenderPass verifies 24 shared-prologue bytes, steals six, and proves four arguments")
    install_reflections = cpp_body('bool installReflections(HMODULE engine, bool trace, bool deferAdmissionMesh,\n                        bool deferAdmissionAll,\n                        bool secondaryPassAdmission)')
    ok("resolve(\n        engine, kGraphicsMeshInstanceRenderPassName,"
           in install_reflections
       and "const bool meshOk = lightOk && (!needMesh || tq::detour::attach("
           in install_reflections
       and "const bool needMesh = trace || deferAdmissionMesh\n"
           "                       || secondaryPassAdmission;"
           in install_reflections
       and "g_graphicsMeshInstanceRenderPassDetour" in install_reflections
       and "kGraphicsMeshInstanceRenderPassRelocs, 1),\n        6,"
           in install_reflections
       and install_reflections.find(
               "detach(g_graphicsMeshInstanceRenderPassDetour)")
           < install_reflections.find(
               "restoreCall(g_reflectionRenderLightPatch)"),
       "mesh virtual-entry detour is exact, steals six, and rolls back before call patches")
    kind = re.search(r"enum GpuChunkRenderableKind \{(.*?)\};", src, re.S)
    kind_names = re.findall(r"\b(GpuChunk(?:RenderableNone|TerrainPlug|"
                            r"TerrainBlock|MeshInstance))\b",
                            kind.group(1)) if kind else []
    ok(kind_names == ["GpuChunkRenderableNone", "GpuChunkTerrainPlug",
                      "GpuChunkTerrainBlock", "GpuChunkMeshInstance"],
       "renderable-call kind order keeps explicit terrain and mesh classes")

    counter_names = re.findall(r'"(engine_gpu_chunk_[^"]+)"', probe_src)
    ok(counter_names == [
           "engine_gpu_chunk_reflection_arm",
           "engine_gpu_chunk_reflection_start_draw",
           "engine_gpu_chunk_reflection_draw",
           "engine_gpu_chunk_reflection_overflow",
           "engine_gpu_chunk_reflection_collision"],
       "only reflection sparse-event counters remain in the CSV")
    ok(not any(name.endswith("_us") or name.endswith("_ms")
               for name in counter_names),
       "reflection sparse metadata is never charged as a duration")

    expected_gpu = ["gpu_chunk_reflection_%02d" % i for i in range(16)]
    actual_gpu = re.findall(
        r'"(gpu_chunk_(?:reflection_\d\d|shadow_(?:setup|\d\d)))"',
        probe_src)
    ok(actual_gpu == expected_gpu,
       "only sixteen continuous-reflection query columns remain")
    gpu_header = re.search(r"enum GpuPhase \{(.*?)GpuPhaseCount", probe_h,
                           re.S)
    gpu_enums = re.findall(
        r"\b(GpuChunk(?:Reflection\d\d|Shadow(?:Setup|\d\d)))\b",
        gpu_header.group(1)) if gpu_header else []
    ok(gpu_enums == ["GpuChunkReflection%02d" % i for i in range(16)],
       "directional setup/chunk query IDs are removed, not merely left idle")

    arm_begin = definition_start(src, "void armGpuChunks(")
    arm_end = block_end(src, arm_begin)
    arm = src[arm_begin:arm_end]
    ok("event.startDraw = kGpuChunkStartDraw;" in arm
       and "g_gpuChunkEvents[g_gpuChunkEventSequence++"
           " % kGpuChunkEventSlots]" in arm
       and "g_gpuChunkLastFrame[GpuChunkReflection] == frame + 1" in arm
       and "CounterEngineGpuChunkReflectionCollision" in arm
       and "gpuBegin" not in arm,
       "one bounded reflection event starts at draw 1 without arming early")

    reflection_hook = cpp_body('void __fastcall hookReflectionRenderLight(')
    child_begin = src.find("struct ReflectionChildScope")
    child_end = block_end(src, child_begin)
    child = src[child_begin:child_end]
    ok("child == ReflectionChildBuildScene" in child
       and "cell == ReflectionCellI2P1" in child
       and "elapsed >= kGpuChunkReflectionBuildSceneTriggerUs" in child
       and "g_reflectionGpuChunkTriggerUs = elapsed" in child
       and "armGpuChunks(location, g_reflectionGpuChunkTriggerUs)"
           in reflection_hook
       and reflection_hook.find("armGpuChunks(location")
           < reflection_hook.find("ReflectionChildScope scope(")
           < reflection_hook.rfind("g_reflectionRenderLight(self"),
       "slow exact i2/p1 BuildScene sparsely arms the following RenderLightStyle")
    ok("closeGpuChunks();" in child
       and "InterlockedExchange(&g_reflectionChild, priorChild);" in child,
       "the exact reflection child closes and restores the tail trace")

    # Keep re-reading the recovered Run-76 executor bytes even though Run 77
    # deliberately writes no call there. They remain durable disassembly
    # evidence and must not become an unverified stale table.
    for label, rva, rel in [
            ("kShadowRecordExecutorCallWindowBytes",
             "kShadowRecordExecutorCallWindowRva", None),
            ("kShadowRecordExecutorBytes", "kShadowRecordExecutorRva", None),
            ("kShadowRecordExecutorTailBytes",
             "kShadowRecordExecutorTailRva", None)]:
        window(engine, label, label, const(rva), rel)
    call = table("kShadowRecordExecutorCallWindowBytes")
    call_at = const("kShadowRecordExecutorCallWindowRva")
    call_off = const("kShadowRecordExecutorCallOffset")
    target = call_at + call_off + 5 + struct.unpack_from(
        "<i", bytes(call), call_off + 1)[0]
    tail = bytes(table("kShadowRecordExecutorTailBytes"))
    ok(16 <= len(call) <= 24 and call[call_off] == 0xe8
       and target == const("kShadowRecordExecutorRva")
       and 16 <= len(tail) <= 24 and tail.endswith(b"\xc2\x0c\x00"),
       "dormant DX11 executor evidence still resolves and proves three arguments")

    shadow_begin = definition_start(src, "int __fastcall hookRenderDirectional(")
    shadow_end = block_end(src, shadow_begin)
    shadow = src[shadow_begin:shadow_end]
    ok("armGpuChunks" not in shadow and "closeGpuChunks" not in shadow
       and "GpuChunkShadow" not in src
       and "g_shadowRecordExecutorPatch" not in src,
       "directional shadow issues no sparse query and receives no executor patch")

    before_begin = definition_start(src, "void beginGpuChunkDrawInternal(")
    before_end = block_end(src, before_begin)
    before = src[before_begin:before_end]
    finish_begin = before_end + 3
    finish_end = src.find("\n}\n\nconst char* gpuChunkRenderableName",
                          finish_begin)
    finish = src[finish_begin:finish_end]
    ok("active.drawsSeen + 1 < active.event->startDraw" in src[
           src.find("void openGpuChunk("):before_begin]
       and "const unsigned ordinal = ++active.drawsSeen;" in finish
       and "if (ordinal + 1 == active.event->startDraw) openGpuChunk(active);"
           in finish
       and "if (bin.draws != kGpuChunkDraws) return;" in finish
       and "if (active.chunk < kGpuChunkCount) openGpuChunk(active);" in finish,
       "queries continuously cover draw 1 and include inter-renderable work in 20-draw bins")
    ok("GetVertexBuffers" not in before + finish
       and "GetIndexBuffer" not in before + finish
       and "GetShader" not in before + finish
       and "bindings->pixelResources[0]" in finish
       and "bindings->vertexBuffers[0]" in finish,
       "continuous identity reuses setter snapshots and adds no D3D getter")

    renderable_struct = cpp_body("struct GpuChunkRenderableCall {") + cpp_body("struct GpuChunkEvent {")
    renderable_scope = cpp_body('struct GpuChunkRenderableCallScope {')
    plug = cpp_body('void __fastcall hookTerrainPlugRender(')
    block = cpp_body('void __fastcall hookTerrainBlockRender(')
    mesh = cpp_body('void __fastcall hookGraphicsMeshInstanceRenderPass(')
    load = cpp_body('void __fastcall hookLoadResource(')
    creation = cpp_body('void countReflectionCreation(')
    report = cpp_body('void reportGpuChunksAtMarker()')
    ok("GpuChunkRenderableCall renderables[kGpuChunkRenderableCallSlots]"
           in renderable_struct
       and "event.renderableCalls >= kGpuChunkRenderableCallSlots"
           in renderable_scope
       and "event.renderableCallOverflow = true" in renderable_scope,
       "renderable-call identity is embedded in each event with an explicit bound")
    ok("if (active.drawsSeen + 1 < event.startDraw) return;"
           in renderable_scope
       and renderable_scope.find("active.drawsSeen + 1 < event.startDraw")
           < renderable_scope.find(
               "event.renderableCalls >= kGpuChunkRenderableCallSlots"),
       "renderable-call retention begins at draw 1 before consuming a slot")
    ok("GpuChunkRenderableCallScope terrainCall(GpuChunkTerrainPlug, self);"
           in plug and "terrainCall.finish(elapsed);" in plug
       and "GpuChunkRenderableCallScope terrainCall(GpuChunkTerrainBlock, self);"
           in block and "terrainCall.finish(elapsed);" in block,
       "exact TerrainPlug and TerrainBlock wrappers retain one-call CPU/draw ranges")
    ok("GpuChunkRenderableCallScope renderableCall(GpuChunkMeshInstance, self);"
           in mesh
       and "g_graphicsMeshInstanceRenderPass(" in mesh
       and mesh.count("g_graphicsMeshInstanceRenderPass(") == 3
       and mesh.find("if (!renderableCall.call)")
           < mesh.find("const int64_t started")
       and "renderableCall.finish(" in mesh,
       "exact GraphicsMeshInstance wrapper forwards once and clocks only selected calls")
    ok("noteGpuChunkRenderableResource(elapsed, terrainType," in load
       and "noteGpuChunkRenderableCreation(texture, elapsedUs);" in creation,
       "selected renderable calls receive nested Resource and creation totals")
    ok("reflection renderable calls frame %u, retained %u" in report
       and "reflection hot renderable frame %u, call %u" in report
       and "reflection renderable class frame %u, %s" in report
       and "cheap calls represented by class totals" in report
       and "Resource %u/%u" in report
       and '" us, texture create %u/%u us' in report
       and "texture create %u/%u us" in report,
       "F12 writes hot identities and compact complete class totals during the session")
    ok("g_gpuChunkEventSequence - count + offset" in report
       and "g_gpuChunkEventSequence - 1 - back" not in report
       and "GPU chunk classes frame %u, bin %u" in report,
       "F12 reports older reaction candidates first and maps every bin to classes")

    draw_begin = definition_start(visual_src, "void WINAPI hookDraw(")
    indexed_begin = definition_start(visual_src, "void WINAPI hookDrawIndexed(")
    draw = visual_src[draw_begin:indexed_begin]
    indexed_end = block_end(visual_src, indexed_begin)
    indexed = visual_src[indexed_begin:indexed_end]
    ok(draw.find("beginGpuChunkDraw(context)") < draw.find("g_draw(context")
       < draw.find("finishGpuChunkDraw(")
       and indexed.find("beginGpuChunkDraw(context)")
           < indexed.find("g_drawIndexed(context")
           < indexed.find("drawGrassCross(context")
           < indexed.find("finishGpuChunkDraw("),
       "Draw hooks bracket game submission and the enhanced-grass companion")
    ok("gpuChunkDrawActive()" in draw
       and draw.find("gpuChunkDrawActive()")
           < draw.find("beginGpuChunkDraw(context)")
       and "gpuChunkDrawActive()" in indexed
       and "extern volatile LONG gpuChunkDrawActive" in engine_h,
       "ordinary draws inline-gate the reflection helper calls")

    install_begin = definition_start(src, "bool install(HMODULE engine)")
    install_end = block_end(src, install_begin)
    install = src[install_begin:install_end]
    ok("wants(kGroupReflections) && wants(kGroupTerrain)" in install
       and "&& tq::probe::drawTimingEnabled()" in install
       and "g_gpuChunkTracing = gpuChunks && g_reflectionTracing" in install
       and "&& g_reflectionChildTracing && g_terrainPlugRender" in install
       and "&& g_terrainBlockRender" in install
       and "&& g_graphicsMeshInstanceRenderPass" in install
       and "installShadow(engine, traceShadow);" in install,
       "activation requires exact reflection/terrain/mesh/draw dependencies, not shadow chunks")
    marker = cpp_body('BOOL __stdcall hookPeekMessage(')
    ok(marker.find("reportGpuChunksAtMarker();")
           < marker.find("tq::probe::markStutter();"),
       "F12 writes continuous reflection metadata before marking")
    shutdown = cpp_body('void shutdown()')
    ok("g_gpuChunkTracing = false;" in shutdown
       and "gpuChunkDrawActive, 0" in shutdown
       and "g_activeGpuChunkRenderableCall = nullptr;" in shutdown
       and "detach(g_graphicsMeshInstanceRenderPassDetour)" in shutdown,
       "shutdown disables reflection classification before detaching the mesh hook")
    ok("reflection GPU chunks cover draws 1--40 as two 20-draw bins"
           in selftest_src
       and "gpuChunkBinDrawsForTest(0) == 20" in selftest_src
       and "gpuChunkBinDrawsForTest(1) == 20" in selftest_src
       and "selected TerrainBlock retains draw ordinals and nested work"
           in selftest_src
       and "reflection renderable calls distinguish terrain and mesh classes"
           in selftest_src
       and "CounterEngineGpuChunkShadow" not in selftest_src,
       "self-test requires continuous 20-draw bins and both retained classes")


def check_off_main_texture_trace():
    """Run 80's loader-thread texture descriptor retention."""
    print("\nOff-main texture descriptor retention")
    ok(const("kOffMainTextureSlots") == 512
       and const("kOffMainTextureMarkerFrames") == 120
       and const("kOffMainTextureReportLimit") == 192,
       "off-main texture ring, reaction horizon, and output bound are exact")
    record = cpp_body('struct OffMainTextureRecord {')
    note = cpp_body('void noteOffMainTextureCreated(')
    report = cpp_body('void reportOffMainTexturesAtMarker()')
    hook = cpp_body('HRESULT WINAPI hookCreateTexture2D(', visual_src)
    ok("volatile LONG publishedSequence" in record
       and "unsigned startFrame;" in record
       and "unsigned finishFrame;" in record
       and "unsigned threadId;" in record
       and "bool hasInitialData;" in record,
       "retained descriptors carry publication, frame extent, thread, and initial-data state")
    ok(note.find("InterlockedExchange(&record.publishedSequence, 0)")
           < note.find("record.startFrame = startFrame")
           < note.find("MemoryBarrier();")
           < note.find("InterlockedExchange(&record.publishedSequence, sequence)"),
       "loader-thread records publish only after every field is written")
    ok("if (!g_deferredPassTracing) return;" in note
       and "currentDeferredLocation" not in note
       and "countReflectionCreation" not in note,
       "off-main texture metadata is passive and never inherits a main-thread owner")
    ok("const unsigned startFrame = started ?" in hook
       and "if (!renderThread && SUCCEEDED(hr)" in hook
       and "noteOffMainTextureCreated(" in hook
       and "initial != nullptr" in hook,
       "CreateTexture2D records only successful off-main calls with exact descriptors")
    ok("for (LONG sequence = first; sequence <= snapshot; ++sequence)" in report
       and "emitted >= kOffMainTextureReportLimit" in report
       and "omitted %u" in report,
       "F12 emits loader records oldest-first with an explicit bounded omission count")
    marker = cpp_body('BOOL __stdcall hookPeekMessage(')
    ok(0 <= marker.find("reportOffMainTexturesAtMarker();")
           < marker.find("reportGpuChunksAtMarker();")
           < marker.find("tq::probe::markStutter();"),
       "F12 writes texture and reflection evidence during the session before marking")
    ok("off-main texture trace publishes exact descriptor and frame extent"
           in selftest_src
       and "latestOffMainTextureForTest(" in selftest_src,
       "self-test exercises the lock-free texture record payload")


def check_deferred_passes(engine):
    """Run 71's owner-exact DX11 geometry and draw-identity trace."""
    print("\nGraphicsDeferredRendererX top-level pass partition")
    window(engine, "kDeferredRenderBytes", "kDeferredRenderBytes",
           const("kDeferredRenderRva"), "kDeferredRenderRelocs")
    owner_body = table("kDeferredRenderBytes")
    owner_relocs = relocs("kDeferredRenderRelocs")
    ok(len(owner_body) == 24
       and all(owner_body[offset:offset + 4] == [0, 0, 0, 0]
               for offset, _ in owner_relocs),
       "deferred-renderer owner verifies 24 bytes with zero relocation slots")
    ok(engine.exports().get(cstr("kDeferredRenderName"))
       == const("kDeferredRenderRva"),
       "GraphicsDeferredRendererX::Render export resolves to its recorded RVA")

    window(engine, "kDeferredOwnerCallWindowBytes",
           "kDeferredOwnerCallWindowBytes",
           const("kDeferredOwnerCallWindowRva"), None)
    owner_call = table("kDeferredOwnerCallWindowBytes")
    owner_offset = const("kDeferredOwnerCallOffset")
    owner_call_rva = const("kDeferredOwnerCallWindowRva") + owner_offset
    owner_dest = owner_call_rva + 5 + struct.unpack_from(
        "<i", bytes(owner_call), owner_offset + 1)[0]
    ok(len(owner_call) == 24 and owner_offset == 16
       and owner_call[owner_offset] == 0xe8
       and owner_dest == const("kDeferredRenderRva"),
       "sole owner caller verifies 24 bytes and its E8 resolves to the renderer")
    window(engine, "kDeferredRenderTailBytes", "kDeferredRenderTailBytes",
           const("kDeferredRenderTailRva"), None)
    owner_tail = table("kDeferredRenderTailBytes")
    ok(len(owner_tail) == 20 and owner_tail[-3:] == [0xc2, 0x1c, 0x00],
       "renderer tail verifies 20 bytes and proves seven explicit arguments")

    calls = [
        ("kDeferredGeometrySetupCallBytes", 0x1663a8, 0x1653a0,
         "kDeferredGeometrySetupCallRelocs", 1, "DeferredPassGeometry", 2),
        ("kDeferredGeometrySceneCallBytes", 0x166412, 0x1883f0,
         None, 0, "DeferredPassGeometry", 5),
        ("kDeferredShadowsCallBytes", 0x166454, 0x164050,
         None, 0, "DeferredPassShadows", 2),
        ("kDeferredLightingCallBytes", 0x166461, 0x164640,
         None, 0, "DeferredPassLighting", 2),
        ("kDeferredResolveCallBytes", 0x16647d, 0x166800,
         None, 0, "DeferredPassResolve", 3),
        ("kDeferredAoCallBytes", 0x16648f, 0x15c8e0,
         None, 0, "DeferredPassResolve", 1),
        ("kDeferredLateSceneACallBytes", 0x1664a6, 0x161c80,
         None, 0, "DeferredPassLateScene", 2),
        ("kDeferredLateSceneBCallBytes", 0x1664ae, 0x161a00,
         "kDeferredLateSceneBCallRelocs", 1, "DeferredPassLateScene", 1),
        ("kDeferredLateSceneListCallBytes", 0x166502, 0x1883f0,
         None, 0, "DeferredPassLateScene", 5),
        ("kDeferredPostHighlightCallBytes", 0x16650a, 0x161a70,
         None, 0, "DeferredPassPost", 1),
        ("kDeferredPostFogCallBytes", 0x166515, 0x165aa0,
         None, 0, "DeferredPassPost", 2),
        ("kDeferredPostMaskCallBytes", 0x166525, 0x162200,
         None, 0, "DeferredPassPost", 1),
        ("kDeferredPostCompositeCallBytes", 0x166588, 0x1657b0,
         "kDeferredPostCompositeCallRelocs", 1, "DeferredPassPost", 5),
        ("kDeferredPostDebugCallBytes", 0x1665a4, 0x161720,
         None, 0, "DeferredPassPost", 1),
    ]
    call_table = re.search(
        r"const DeferredCallSite kDeferredCallSites\[\] = \{(.*?)\n\};",
        src, re.S)
    call_table_flat = re.sub(r"\s+", "", call_table.group(1)) \
        if call_table else ""
    for name, call_rva, target_rva, reloc, reloc_count, pass_name, arguments in calls:
        body = table(name)
        window(engine, name, name, call_rva, reloc)
        dest = call_rva + 5 + struct.unpack_from("<i", bytes(body), 1)[0]
        ok(len(body) == 16 and body[0] == 0xe8 and dest == target_rva,
           "%s verifies 16 bytes and resolves to Engine+%#x"
           % (name, target_rva))
        ok(not reloc or all(body[offset:offset + 4] == [0, 0, 0, 0]
                            for offset, _ in relocs(reloc)),
           "%s uses zero placeholders for every relocation" % name)
        # Bind every field in the runtime table, not merely the byte array.
        # In particular, a wrong pass or relocation pointer would otherwise
        # still leave the independently checked on-disk bytes looking valid.
        reloc_name = reloc if reloc else "nullptr"
        row = ("{%s,%s,%s,%s,%s,%s,%s}" %
               (hex(call_rva), hex(target_rva), name, reloc_name,
                reloc_count, pass_name, arguments))
        ok(row in call_table_flat,
           "%s source row binds RVA, target, relocation, pass, and ABI"
           % name)

    tails = [
        ("kDeferredGeometrySetupTailBytes", 0x16557f, 2),
        ("kDeferredSceneListTailBytes", 0x1885ec, 5),
        ("kDeferredShadowsTailBytes", 0x16458b, 2),
        ("kDeferredLightingTailBytes", 0x16532a, 2),
        ("kDeferredResolveTailBytes", 0x166bd2, 3),
        ("kDeferredAoTailBytes", 0x15c9c6, 1),
        ("kDeferredLateSceneATailBytes", 0x16212d, 2),
        ("kDeferredLateSceneBTailBytes", 0x161a5d, 1),
        ("kDeferredPostHighlightTailBytes", 0x161c6b, 1),
        ("kDeferredPostFogTailBytes", 0x166120, 2),
        ("kDeferredPostMaskTailBytes", 0x1625b0, 1),
        ("kDeferredPostCompositeTailBytes", 0x165a89, 5),
        ("kDeferredPostDebugTailBytes", 0x16193f, 1),
    ]
    abi_table = re.search(
        r"const DeferredTargetAbi kDeferredTargetAbis\[\] = \{(.*?)\n\};",
        src, re.S)
    abi_table_flat = re.sub(r"\s+", "", abi_table.group(1)) \
        if abi_table else ""
    target_for_tail = {
        0x16557f: 0x1653a0, 0x1885ec: 0x1883f0,
        0x16458b: 0x164050, 0x16532a: 0x164640,
        0x166bd2: 0x166800, 0x15c9c6: 0x15c8e0,
        0x16212d: 0x161c80, 0x161a5d: 0x161a00,
        0x161c6b: 0x161a70, 0x166120: 0x165aa0,
        0x1625b0: 0x162200, 0x165a89: 0x1657b0,
        0x16193f: 0x161720,
    }
    for name, tail_rva, arguments in tails:
        body = table(name)
        window(engine, name, name, tail_rva, None)
        ok(len(body) == 16
           and body[-3:] == [0xc2, arguments * 4, 0],
           "%s proves callee cleanup for %d explicit argument(s)"
           % (name, arguments))
        row = ("{%s,%s,%s,%s}" %
               (hex(target_for_tail[tail_rva]), hex(tail_rva), name,
                arguments))
        ok(row in abi_table_flat,
           "%s source ABI row binds target, tail, bytes, and count" % name)

    install_begin = definition_start(src, "bool installDeferredPasses(HMODULE engine)")
    install_end = block_end(src, install_begin)
    install = src[install_begin:install_end]
    owner_check = install.find("bool verified = owner")
    wide_check = install.find("Validate every overlapping original window")
    first_patch = install.find("tq::detour::patchCall(")
    ok(install_begin >= 0 and install_end > install_begin
       and owner_check >= 0 and wide_check > owner_check
       and first_patch > wide_check
       and "kDeferredOwnerCallWindowBytes" in install[owner_check:wide_check]
       and "kDeferredRenderTailBytes" in install[owner_check:wide_check]
       and "kDeferredRenderTailBytes[18] == 7 * sizeof(uintptr_t)"
           in install[owner_check:wide_check]
       and "signature(site.bytes, 16" in install
       and "signature(abi->bytes, 16)" in install,
       "owner, all wide call windows, and ABI tails verify before the first write")
    ok("installed != kDeferredCallSiteCount" in install
       and "restoreCall(g_deferredCallPatches[--installed])" in install
       and "restoreCall(g_deferredOwnerPatch)" in install
       and "kDeferredOwnerCallOffset, owner," in install
       and "(const void*)&hookDeferredRender))" in install
       and "signature(site.bytes, 5), 0," in install
       and "g_deferredPassTracing = true;" in install,
       "owner plus fourteen child calls install atomically before tracing activates")
    ok("InterlockedExchange(&g_deferredOwnerInvocation, 0);" in install
       and "g_deferredOwnerFrame = UINT_MAX;" in install
       and "g_deferredOwnerCallsThisFrame = 0;" in install
       and "memset(g_deferredCreations, 0, sizeof(g_deferredCreations));"
           in install
       and "memset(g_deferredSlowFrames, 0, sizeof(g_deferredSlowFrames));"
           in install,
       "installation resets owner numbering and both bounded identity rings")
    ok(const("kGroupDeferredPasses") == 0x10000
       and "if (wants(kGroupDeferredPasses) || crossPass)\n"
           "        installDeferredPasses(engine);" in src,
       "deferred-pass trace is group 65536 plus the reflection cross-pass dependency")

    counter_names = [
        "engine_deferred_geometry_us", "engine_deferred_geometry_draw_us",
        "engine_deferred_shadows_us", "engine_deferred_shadows_draw_us",
        "engine_deferred_lighting_us", "engine_deferred_lighting_draw_us",
        "engine_deferred_resolve_us", "engine_deferred_resolve_draw_us",
        "engine_deferred_late_scene_us",
        "engine_deferred_late_scene_draw_us",
        "engine_deferred_post_us", "engine_deferred_post_draw_us",
    ]
    ok(all(('"%s"' % name) in probe_src and name.endswith("_us")
           for name in counter_names),
       "all engine pass and draw durations use `_us`, never `_ms`")
    exact_gpu_names = [
        "gpu_deferred_i1_geometry_setup",
        "gpu_deferred_i1_geometry_scene",
        "gpu_deferred_i2_geometry_setup",
        "gpu_deferred_i2_geometry_scene",
    ]
    ok(all(('"%s"' % name) in probe_src for name in exact_gpu_names)
       and '"gpu_deferred_geometry"' not in probe_src
       and '"gpu_deferred_shadows"' not in probe_src,
       "four owner-exact geometry GPU columns replace overlapping group spans")
    exact_counter_names = [
        "engine_deferred_owner", "engine_deferred_owner_overflow",
    ]
    for invocation in ("i1", "i2"):
        for site in ("geometry_setup", "geometry_scene"):
            exact_counter_names.extend([
                "engine_deferred_%s_%s" % (invocation, site),
                "engine_deferred_%s_%s_us" % (invocation, site),
                "engine_deferred_%s_%s_draw_us" % (invocation, site),
            ])
        for site in ("other", "geometry_setup", "geometry_scene"):
            for kind in ("res_load", "tex_create", "buf_create"):
                exact_counter_names.extend([
                    "engine_deferred_%s_%s_%s" %
                        (invocation, site, kind),
                    "engine_deferred_%s_%s_%s_us" %
                        (invocation, site, kind),
                ])
    ok(all(('"%s"' % name) in probe_src for name in exact_counter_names)
       and all(name.endswith("_us")
               for name in exact_counter_names
               if name.endswith(("_draw_us", "_load_us", "_create_us"))
                  or name.rsplit("_", 1)[-1] == "us"),
       "all owner/site game durations have exact CSV names ending in `_us`")
    draw_hooks = []
    for hook, next_hook in [("hookDraw(", "hookDrawIndexed("),
                            ("hookDrawIndexed(", "hookClearDepthStencilView(")]:
        begin = visual_src.find(hook)
        end = visual_src.find(next_hook, begin + 1)
        draw_hooks.append(visual_src[begin:end])
    ok(all(body.count("tq::probe::finishPhase(") == 1
           and body.count("tq::engineprobe::countDeferredDraw(") == 1
           and "&g_deferredBindings" in body for body in draw_hooks),
       "Draw and DrawIndexed reuse one clock sample and snapshot tracked bindings")
    mappings = [
        ("kDeferredPassCountCounters",
         ["CounterCount", "CounterEngineDeferredGeometry",
          "CounterEngineDeferredShadows", "CounterEngineDeferredLighting",
          "CounterEngineDeferredResolve", "CounterEngineDeferredLateScene",
          "CounterEngineDeferredPost"]),
        ("kDeferredPassDurationCounters",
         ["CounterCount", "CounterEngineDeferredGeometryUs",
          "CounterEngineDeferredShadowsUs",
          "CounterEngineDeferredLightingUs",
          "CounterEngineDeferredResolveUs",
          "CounterEngineDeferredLateSceneUs", "CounterEngineDeferredPostUs"]),
        ("kDeferredPassDrawCounters",
         ["CounterCount", "CounterEngineDeferredGeometryDrawUs",
          "CounterEngineDeferredShadowsDrawUs",
          "CounterEngineDeferredLightingDrawUs",
          "CounterEngineDeferredResolveDrawUs",
          "CounterEngineDeferredLateSceneDrawUs",
          "CounterEngineDeferredPostDrawUs"]),
    ]
    for table_name, expected in mappings:
        m = re.search(r"const tq::probe::(?:Counter|GpuPhase) %s\[\] = \{"
                      r"(.*?)\};" % table_name, src, re.S)
        got = re.findall(r"tq::probe::(\w+)", m.group(1)) if m else []
        ok(got == expected, "%s follows the six-pass enum exactly" % table_name)
    exact_mappings = [
        ("kDeferredGeometryCountCounters",
         ["CounterCount", "CounterEngineDeferredI1GeometrySetup",
          "CounterEngineDeferredI1GeometryScene",
          "CounterEngineDeferredI2GeometrySetup",
          "CounterEngineDeferredI2GeometryScene"]),
        ("kDeferredGeometryDurationCounters",
         ["CounterCount", "CounterEngineDeferredI1GeometrySetupUs",
          "CounterEngineDeferredI1GeometrySceneUs",
          "CounterEngineDeferredI2GeometrySetupUs",
          "CounterEngineDeferredI2GeometrySceneUs"]),
        ("kDeferredGeometryDrawCounters",
         ["CounterCount", "CounterEngineDeferredI1GeometrySetupDrawUs",
          "CounterEngineDeferredI1GeometrySceneDrawUs",
          "CounterEngineDeferredI2GeometrySetupDrawUs",
          "CounterEngineDeferredI2GeometrySceneDrawUs"]),
        ("kDeferredGeometryGpuPhases",
         ["GpuPhaseCount", "GpuDeferredI1GeometrySetup",
          "GpuDeferredI1GeometryScene", "GpuDeferredI2GeometrySetup",
          "GpuDeferredI2GeometryScene"]),
    ]
    for table_name, expected in exact_mappings:
        m = re.search(r"const tq::probe::(?:Counter|GpuPhase) %s\[\] = \{"
                      r"(.*?)\};" % table_name, src, re.S)
        got = re.findall(r"tq::probe::(\w+)", m.group(1)) if m else []
        ok(got == expected,
           "%s follows the four owner/site cells exactly" % table_name)
    owner_bins = re.search(
        r"const DeferredOwnerBinCounters kDeferredOwnerBinCounters\[\] = \{"
        r"(.*?)\n\};", src, re.S)
    owner_symbols = re.findall(r"tq::probe::(Counter\w+)",
                               owner_bins.group(1)) if owner_bins else []
    expected_owner = ["CounterCount"] * 6
    for invocation in ("I1", "I2"):
        for site in ("Other", "GeometrySetup", "GeometryScene"):
            for kind in ("ResLoad", "TexCreate", "BufCreate"):
                expected_owner.extend([
                    "CounterEngineDeferred%s%s%s" % (invocation, site, kind),
                    "CounterEngineDeferred%s%s%sUs" % (invocation, site, kind)])
    ok(owner_symbols == expected_owner,
       "six owner/site bins map Resource, texture, and buffer count/us exactly")
    scope_begin = src.find("struct DeferredPassScope")
    scope_end = src.find("\n};", scope_begin)
    scope = src[scope_begin:scope_end]
    ok("g_deferredPassTracing && onMainThread()" in scope
       and "&g_deferredOwnerInvocation, 0, 0) > 0" in scope
       and "gpuBegin(context, kDeferredGeometryGpuPhases[cell]);" in scope
       and "gpuEnd(context, kDeferredGeometryGpuPhases[cell]);" in scope
       and "kDeferredPassCountCounters[pass]" in scope
       and "kDeferredPassDurationCounters[pass]" in scope
       and "kDeferredGeometryCountCounters[cell]" in scope
       and "kDeferredGeometryDurationCounters[cell]" in scope
       and "InterlockedExchange(&g_deferredPass, prior);" in scope,
       "pass scopes require an owner and keep exact geometry GPU pairs nested")
    draw_begin = definition_start(src, "void countDeferredDrawInternal(")
    draw_end = block_end(src, draw_begin)
    draw = src[draw_begin:draw_end]
    ok("!g_deferredPassTracing || !elapsedUs" in draw
       and "kDeferredPassDrawCounters[pass]" in draw
       and "kDeferredGeometryDrawCounters[cell]" in draw
       and "rememberDeferredDraw(" in draw,
       "draw attribution records both pass and exact owner/site buckets")
    wrapper_map = [
        ("hookDeferredGeometrySetup", "DeferredPassGeometry",
         "g_deferredGeometrySetup"),
        ("hookDeferredGeometryScene", "DeferredPassGeometry",
         "g_deferredGeometryScene"),
        ("hookDeferredShadows", "DeferredPassShadows", "g_deferredShadows"),
        ("hookDeferredLighting", "DeferredPassLighting", "g_deferredLighting"),
        ("hookDeferredResolve", "DeferredPassResolve", "g_deferredResolve"),
        ("hookDeferredAo", "DeferredPassResolve", "g_deferredAo"),
        ("hookDeferredLateSceneA", "DeferredPassLateScene",
         "g_deferredLateSceneA"),
        ("hookDeferredLateSceneB", "DeferredPassLateScene",
         "g_deferredLateSceneB"),
        ("hookDeferredLateSceneList", "DeferredPassLateScene",
         "g_deferredLateSceneList"),
        ("hookDeferredPostHighlight", "DeferredPassPost",
         "g_deferredPostHighlight"),
        ("hookDeferredPostFog", "DeferredPassPost", "g_deferredPostFog"),
        ("hookDeferredPostMask", "DeferredPassPost", "g_deferredPostMask"),
        ("hookDeferredPostComposite", "DeferredPassPost",
         "g_deferredPostComposite"),
        ("hookDeferredPostDebug", "DeferredPassPost", "g_deferredPostDebug"),
    ]
    for wrapper, pass_name, original in wrapper_map:
        begin = definition_start(src, " __fastcall %s(" % wrapper)
        end = block_end(src, begin)
        body = src[begin:end]
        expected_scope = "DeferredPassScope scope(%s" % pass_name
        ok(begin >= 0 and expected_scope in body
           and original in body,
           "%s preserves its original and exact pass class" % wrapper)
    owner_begin = definition_start(src, " __fastcall hookDeferredRender(")
    owner_end = block_end(src, owner_begin)
    owner_wrapper = src[owner_begin:owner_end]
    ok(owner_begin >= 0 and "DeferredOwnerScope scope;" in owner_wrapper
       and "g_deferredRender(self, edx, a, b, c, d, e, f, g)" in owner_wrapper,
       "sole owner wrapper preserves the verified seven-argument ABI")
    owner_scope_begin = src.find("struct DeferredOwnerScope")
    owner_scope_end = src.find("\n};", owner_scope_begin)
    owner_scope = src[owner_scope_begin:owner_scope_end]
    ok("active(g_deferredPassTracing && onMainThread())" in owner_scope
       and "++g_deferredOwnerCallsThisFrame" in owner_scope
       and "if (invocation > 2)" in owner_scope
       and "InterlockedExchange(&g_deferredOwnerInvocation, priorInvocation)"
           in owner_scope,
       "owner invocation numbering is main-thread-only, overflow-visible, and restored")

    ok(const("kDeferredCreationSlots") == 4096
       and const("kDeferredSlowFrameSlots") == 128
       and const("kDeferredTopDrawsPerFrame") == 12
       and const("kDeferredSlowMarkerFrames") == 120
       and const("kDeferredSlowReportFrames") == 8
       and const("kDeferredSlowFrameMinUs") == 15000,
       "creation and slow-draw retention is statically bounded")
    ok("g_deferredCreations[g_deferredCreationSequence++\n"
       "                            % kDeferredCreationSlots]" in src
       and "DeferredSlowFrame& slot =\n"
           "        g_deferredSlowFrames[frame % kDeferredSlowFrameSlots]"
           in src,
       "creation and slow-draw writes wrap within their verified rings")
    marker_begin = definition_start(src, "void reportDeferredSlowDrawsAtMarker()")
    marker_end = block_end(src, marker_begin)
    marker = src[marker_begin:marker_end]
    ok(marker_begin >= 0 and marker_end > marker_begin
       and "back <= kDeferredSlowMarkerFrames" in marker
       and "insert < kDeferredSlowReportFrames" in marker
       and "j < frame.recordCount" in marker
       and "textures < 32" in marker,
       "F12 report remains bounded by horizon, frames, top draws, and textures")
    ok("reportDeferredSlowDrawsAtMarker();" in src
       and src.find("reportDeferredSlowDrawsAtMarker();")
           < src.find("tq::probe::markStutter();"),
       "F12 writes retained geometry identity before recording its marker")
    binding_hooks = cpp_body('void WINAPI hookPSSetShaderResources(', visual_src)
    ok(all(name in visual_src for name in [
           "hookVSSetShader", "hookIASetVertexBuffers", "hookIASetIndexBuffer",
           "hookPSSetShaderResources", "hookPSSetShader"])
       and all(slot in visual_src for slot in
               ["&cv[11]", "&cv[18]", "&cv[19]", "&cv[8]", "&cv[9]"])
       and "GetShader" not in binding_hooks
       and "GetVertexBuffers" not in binding_hooks
       and "GetIndexBuffer" not in binding_hooks,
       "binding identity uses setter hooks without per-draw state getters")
    slot_decl = re.search(
        r"enum \{\s*DeferredTraceVertexBufferSlots = (\d+),\s*"
        r"DeferredTracePixelResourceSlots = (\d+)\s*\};", engine_h, re.S)
    ok(slot_decl and slot_decl.groups() == ("4", "8"),
       "binding snapshots retain exactly four vertex buffers and eight SRVs")
    texture_hook = cpp_body('HRESULT WINAPI hookCreateTexture2D(', visual_src)
    buffer_hook = cpp_body('HRESULT WINAPI hookCreateBuffer(', visual_src)
    ok(texture_hook.count("finishPhase(") == 1
       and "noteDeferredTextureCreated(" in texture_hook
       and buffer_hook.count("finishPhase(") == 1
       and "noteDeferredBufferCreated(" in buffer_hook,
       "D3D creation attribution reuses each existing timing sample")
    finish_begin = definition_start(probe_src, "uint32_t finishPhaseInternal(Phase phase, int64_t startTicks)")
    finish_end = block_end(probe_src, finish_begin)
    finish = probe_src[finish_begin:finish_end]
    ok(finish_begin >= 0 and finish.count("now()") == 1
       and "g_current.phaseMs[phase] +=" in finish
       and "1000000.0" in finish,
       "finishPhase records precise phase time and returns microseconds from one clock read")
    shutdown_begin = definition_start(src, "void shutdown()")
    shutdown_end = block_end(src, shutdown_begin)
    shutdown = src[shutdown_begin:shutdown_end]
    ok("g_deferredPassTracing = false;" in shutdown
       and "restoreCall(g_deferredCallPatches[i]);" in shutdown
       and "restoreCall(g_deferredOwnerPatch);" in shutdown,
       "shutdown disables classification before restoring child and owner calls")


def main():
    engine = PE(os.environ.get("TQ_VERIFY_ENGINE_DLL")
                or os.path.join(GAME, "Engine.dll"))
    game = PE(os.environ.get("TQ_VERIFY_GAME_DLL") or os.path.join(GAME, "Game.dll"))
    exe = PE(os.path.join(GAME, "TQ.exe"))

    print("Image identity")
    ok(engine.imagesize == const("kEngineImageSize"), "Engine.dll SizeOfImage %#x" % engine.imagesize)
    grass_src = open(os.path.join(os.path.dirname(SRC), "grass.cpp")).read()
    grass_flat = re.sub(r'"\s*\n\s*"', '', grass_src)
    grass_prologue = re.search(r'kRenderPrologue\[\]\s*=\s*\{([^}]+)\}', grass_src)
    grass_bytes = bytes(int(value.strip(), 0)
                        for value in grass_prologue.group(1).split(','))
    for symbol in ("kRenderGrassName", "kRenderGrassRtName"):
        name = re.search(symbol + r'\[\]\s*=\s*"([^"]+)"', grass_flat).group(1)
        rva = engine.exports().get(name)
        ok(rva is not None and engine.read(rva, len(grass_bytes)) == grass_bytes,
           "grass render export/prologue matches: %s" % symbol)
    check_legacy_scalar_contract(engine)
    hek = game.imagesize == const("kGameHekImageSize")
    ok(game.imagesize in (const("kGameImageSize"), const("kGameHekImageSize")),
       "Game.dll supported SizeOfImage %#x" % game.imagesize)
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
            ("kTerrainPreloadBytes", "kTerrainPreloadRva",
             "kTerrainPreloadRelocs"),
            ("kTerrainSetShaderParamsBytes", "kTerrainSetShaderParamsRva",
             "kTerrainSetShaderParamsRelocs"),
            ("kTerrainSetGrassShaderParamsBytes",
             "kTerrainSetGrassShaderParamsRva",
             "kTerrainSetGrassShaderParamsRelocs"),
            ("kTerrainRenderGroundBytes", "kTerrainRenderGroundRva",
             "kTerrainRenderGroundRelocs"),
            ("kTerrainRtLoadBytes", "kTerrainRtLoadRva",
             "kTerrainRtLoadRelocs"),
            ("kTerrainRtLoadRenderDataBytes", "kTerrainRtLoadRenderDataRva",
             "kTerrainRtLoadRenderDataRelocs"),
            ("kTerrainRtPreloadBytes", "kTerrainRtPreloadRva",
             "kTerrainRtPreloadRelocs"),
            ("kTerrainRtLoadTexturesWindowBytes",
             "kTerrainRtLoadTexturesWindowRva", None),
            ("kTerrainRtNumLayersBytes", "kTerrainRtNumLayersRva", None),
            ("kTerrainRtLayerTypeBytes", "kTerrainRtLayerTypeRva", None),
            ("kTerrainPlugRenderBytes", "kTerrainPlugRenderRva",
             "kTerrainPlugRenderRelocs"),
            ("kTerrainPlugShaderWindowBytes", "kTerrainPlugShaderWindowRva",
             "kTerrainPlugShaderWindowRelocs"),
            ("kTerrainBlockRenderBytes", "kTerrainBlockRenderRva",
             "kTerrainBlockRenderRelocs"),
            ("kTerrainBlockShaderWindowBytes",
             "kTerrainBlockShaderWindowRva", None),
            ("kShadowMeshPassCountBytes", "kShadowMeshPassCountRva", None),
            ("kActorUpdateMeshInstanceBytes",
             "kActorUpdateMeshInstanceRva", None),
            ("kActorAddToSceneUpdateMeshWindowBytes",
             "kActorAddToSceneUpdateMeshWindowRva", None),
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
    update_bytes = "kGameHekUpdateBytes" if hek else "kGameUpdateBytes"
    window(game, update_bytes, update_bytes, const("kGameUpdateRva"),
           "kGameUpdateRelocs")
    if hek:
        window(game, "HekTo Update wrapper", "kGameHekWrapperBytes",
               const("kGameHekWrapperRva"), None)
        window(game, "HekTo relocated prologue", "kGameHekTrampolineBytes",
               const("kGameHekTrampolineRva"), None)
        ok(len(game.sections) == 7
           and game.sections[5][:3] == ('.rxHekTo', 0x59a000, 0x1000)
           and game.sections[6][:3] == ('.rwHekTo', 0x59b000, 0x1000),
           "HekTo appended section layout")
        for site, target in [(const("kGameUpdateRva"), const("kGameHekWrapperRva")),
                             (const("kGameHekWrapperRva") + 6, const("kGameHekTrampolineRva")),
                             (const("kGameHekTrampolineRva") + 6, const("kGameUpdateRva") + 6)]:
            delta = struct.unpack('<i', game.read(site + 1, 4))[0]
            ok(site + 5 + delta == target,
               "HekTo branch %#x -> %#x preserves original update chain" % (site, target))

    # The existing frustum hook resolves imports and requires this unique
    # complete call window, independently of the GameEngine update profile.
    imports_by_name = {name: address for address, (_, name) in game.imports().items()}
    viewport = imports_by_name.get('??0Viewport@GAME@@QAE@HHHH@Z')
    frustum = imports_by_name.get('?GetFrustum@WorldCamera@GAME@@QBE?AVWorldFrustum@2@ABVViewport@2@@Z')
    pattern = (bytes.fromhex('68 00 03 00 00 68 00 04 00 00 6a 00 6a 00 8d 4c 24 18 ff 15')
               + struct.pack('<I', viewport or 0)
               + bytes.fromhex('8d 44 24 08 50 8d 84 24 5c 06 00 00 50 8d 4c 24 20 ff 15')
               + struct.pack('<I', frustum or 0)
               + bytes.fromhex('b9 02 01 00 00 8b f0 f3 a5'))
    text_section = next(s for s in game.sections if s[0] == '.text')
    code = game.read(text_section[1], text_section[2])
    ok(viewport and frustum and code.count(pattern) == 1,
       "Game.dll retains the unique imported update-viewport/frustum call window")

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
            (engine, "Engine", "kDeferredRenderName", "kDeferredRenderRva"),
            (engine, "Engine", "kSweepTargetName", "kSweepTargetRva"),
            (engine, "Engine", "kRenderDirectionalName",
             "kRenderDirectionalRva"),
            (engine, "Engine", "kTerrainPreloadName", "kTerrainPreloadRva"),
            (engine, "Engine", "kTerrainSetShaderParamsName",
             "kTerrainSetShaderParamsRva"),
            (engine, "Engine", "kTerrainSetGrassShaderParamsName",
             "kTerrainSetGrassShaderParamsRva"),
            (engine, "Engine", "kTerrainRenderGroundName",
             "kTerrainRenderGroundRva"),
            (engine, "Engine", "kTerrainLoadTexturesName",
             "kTerrainLoadTexturesRva"),
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
            (engine, "Engine", "kGraphicsMeshInstanceRenderPassName",
             "kGraphicsMeshInstanceRenderPassRva"),
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
    terrain_load_textures = table("kTerrainRtLoadTexturesWindowBytes")
    ok(terrain_load_textures[const("kTerrainRtLoadTexturesCallOffset")]
       == 0xe8,
       "TerrainRT LoadTextures window, recorded offset, E8")
    ok(table("kTerrainPlugShaderWindowBytes")[5] == 0xe8,
       "TerrainPlug shader window, offset 5, E8")
    ok(table("kTerrainBlockShaderWindowBytes")[7] == 0xe8,
       "TerrainBlock shader window, offset 7, E8")

    check_archive_cache(engine)
    check_configuration_contract()
    check_trace_off_isolation()
    check_async_level_load(engine, sites)
    check_directional_shadow(engine)
    check_resource_lifecycle(engine)
    check_terrain_diagnostics(engine)
    check_outside_directional_resources()
    check_directional_mesh_resource_retention()
    check_shadow_mesh_boundary(engine)
    check_shadow_actor_pose_boundary(engine)
    check_shadow_material_textures(engine)
    check_shadow_alpha_defer(engine)
    check_shadow_texture_attribution(engine)
    check_play_render_flow(engine)
    check_reflections(engine)
    check_cross_pass_identity()
    check_gpu_draw_chunks(engine)
    check_off_main_texture_trace()
    check_deferred_passes(engine)

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
