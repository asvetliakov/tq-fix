#include "engine_internal.h"

namespace tq { namespace engineprobe { namespace detail {
volatile LONG gpuChunkDrawActive = 0;
} } }

namespace tq { namespace engine { namespace detail {
#ifdef TQ_SELFTEST
static unsigned g_runtimeEntriesForTest;
#define TQ_ENGINE_PROBE_ENTER() (++g_runtimeEntriesForTest)
#else
#define TQ_ENGINE_PROBE_ENTER() ((void)0)
#endif


const BYTE kLoadLevelBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,        // push ebp; mov ebp,esp; and esp,-8
    0x83, 0xec, 0x0c,                          // sub esp,0xc
    0x53,                                      // push ebx
    0x8b, 0xd9,                                // mov ebx,ecx
    0x56,                                      // push esi
    0x8b, 0x43, 0x50,                          // mov eax,[ebx+0x50]
    0x57,                                      // push edi
    0x85, 0xc0,                                // test eax,eax
    0x74, 0x4c,                                // jz  the load path
    0x80, 0x7d, 0x08                           // cmp byte [ebp+8],...
};

// --- Region::GuaranteedGetLevel, which run 29 named as the caller of the
// forced load: two calls, both on the main thread, 515.5 ms between them and
// 402.4 ms in the worse one, all inside Engine::Render on the
// zone-transition frame. It is hooked to ask the same question one level up
// -- which of its seventeen callers those two came from -- because the answer
// decides which single call site Stage 5.1 should be pointed at. See §28.
//
// The window runs past the call to the whole null return, because that path
// is the one a deferring thunk would have to reproduce, and it is cheaper to
// verify it now than to re-read it later:
//
//   1020e7b0  56              push esi
//   1020e7b1  8b f1           mov esi,ecx           Region*
//   1020e7b3  57              push edi
//   1020e7b4  85 f6           test esi,esi
//   1020e7b6  74 42           jz  0x1020e7fa        null region -> null
//   1020e7b8  ff 74 24 0c     push [esp+0xc]        the bool argument
//   1020e7bc  e8 <rel32>      call Region::LoadLevel      <- offset 12
//   1020e7c1  80 7e 74 00     cmp byte [esi+0x74],0       the loading flag
//   1020e7c5  c7 46 6c 00..   mov dword [esi+0x6c],0      unload countdown
//   1020e7cc  74 09           jz  0x1020e7d7        loaded -> take the lock
//   1020e7ce  33 ff           xor edi,edi           still loading -> NULL
//   1020e7d0  8b c7           mov eax,edi
//   1020e7d2  5f 5e           pop edi; pop esi
//   1020e7d4  c2 04 00        ret 4
//
// Two things that follow, and both matter later. **Despite the name it
// already returns NULL while the region is loading** -- so deferring here
// uses a state the function has rather than inventing one. And the loaded
// path at 0x1020e7d7 returns `[esi+0x50]` under the region lock, which is an
// independent statement that `[0x50]` is the Level* -- the same field the
// async thunk tests, read out of the instruction that returns it.
//
// The six stolen bytes are `push esi; mov esi,ecx; push edi; test esi,esi`
// and contain no relative branch, so the `74 42` at offset 6 keeps meaning
// what it means and an ordinary trampoline works.
const DWORD kGuaranteedGetLevelRva = 0x20e7b0;
const char kGuaranteedGetLevelName[] =
    "?GuaranteedGetLevel@Region@GAME@@QBEPAVLevel@2@_N@Z";
const BYTE kGuaranteedGetLevelBytes[] = {
    0x56,
    0x8b, 0xf1,
    0x57,
    0x85, 0xf6,
    0x74, 0x42,
    0xff, 0x74, 0x24, 0x0c,
    0xe8, 0xff, 0xd6, 0xff, 0xff,              // call 0x1020bec0
    0x80, 0x7e, 0x74, 0x00,
    0xc7, 0x46, 0x6c, 0x00, 0x00, 0x00, 0x00,
    0x74, 0x09,
    0x33, 0xff,
    0x8b, 0xc7,
    0x5f, 0x5e,
    0xc2, 0x04, 0x00
};
// Its call to Region::LoadLevel sits at the same offset as the two
// AddElementsInBox sites, which is what makes it a kForceLoadSites entry
// rather than a new mechanism.
const unsigned kGuaranteedCallOffset = 12;

// --- ResourceLoader::LoadResource. The duration includes its wait on the
// resource's own critical section, which is the stall worth naming.
const DWORD kLoadResourceRva = 0x213ed0;
const char kLoadResourceName[] =
    "?LoadResource@ResourceLoader@GAME@@QAEXPAVResource@2@@Z";
const BYTE kLoadResourceBytes[] = {
    0x6a, 0xff,                                // push -1
    0x68, 0, 0, 0, 0,                          // push <SEH handler>
    0x64, 0xa1, 0x00, 0x00, 0x00, 0x00,        // mov eax,fs:[0]
    0x50,                                      // push eax
    0x83, 0xec, 0x08,                          // sub esp,8
    0x53, 0x55, 0x56, 0x57,                    // push ebx,ebp,esi,edi
    0xa1                                       // mov eax,ds:[__security_cookie]
};
const Relocation kLoadResourceRelocs[] = {{3, 0x2a2db8}};
// The material getter runs after 8 bytes of locals and four saved registers
// (EBX/EBP/ESI in the prologue, EDI at the loop entry). Its E8 adds one more
// word, so the return address of the enclosing GraphicsMesh call is at the
// getter adapter's ESP+0x1c. These two windows prove that stack shape without
// relying on the decompiler's variable recovery.
const DWORD kGraphicsMeshSetShaderParametersFrameRva = 0x169c40;
const BYTE kGraphicsMeshSetShaderParametersFrameBytes[] = {
    0x83, 0xec, 0x08,                          // sub esp,8
    0x53, 0x55, 0x56,                          // save ebx/ebp/esi
    0x8b, 0xf1,                                // esi = this mesh
    0xe8, 0xa3, 0x94, 0x0a, 0x00,              // EnsureAvailable
    0x8b, 0x44, 0x24, 0x1c,                    // material-index argument
    0x8b, 0xae, 0xe0, 0x00, 0x00, 0x00         // ebp = material table
};
const DWORD kShadowMaterialLoopFrameRva = 0x169c78;
const BYTE kShadowMaterialLoopFrameBytes[] = {
    0x1f, 0x03, 0xc2,                          // finish quotient
    0x0f, 0x84, 0xbf, 0x00, 0x00, 0x00,       // skip empty material
    0x8b, 0x4c, 0x24, 0x18,                    // shader before EDI save
    0x57,                                      // fourth saved register
    0x33, 0xff, 0xeb, 0x06,
    0x8d, 0x9b, 0x00, 0x00, 0x00, 0x00
};
// At the patched getter's entry, the caller's active GraphicsShader2* is this
// far above ESP: it was at caller ESP+0x1c and E8 pushed one return address.
const unsigned kShadowMaterialShaderStackOffset = 0x20;
const unsigned kShadowMaterialOuterCallerStackOffset = 0x1c;
const DWORD kShadowMeshParameterArgsRva = 0x173857;
const BYTE kShadowMeshParameterArgsBytes[] = {
    0xff, 0xb4, 0x24, 0xa4, 0x00, 0x00, 0x00, // push arg2 material index
    0x6b, 0xed, 0x34,                          // pass * 0x34
    0x03, 0x6f, 0x1c,                          // + MeshRenderInfo base
    0x8b, 0x4e, 0x04                           // ecx = instance+4 mesh
};
const unsigned kShadowMeshParameterAdapterPassOffset = 0xbc;
const unsigned kShadowInstanceBaseEnsureCallOffset = 9;

// Every other direct Engine.dll caller of GraphicsTexture::GetTexture. At a
// nested texture ResourceLoader::LoadResource call the original caller's E8
// return address is still on the stack. Run 52 scans only that committed stack
// region and compares exact return RVAs; indirect or unrecognized paths remain
// in an explicit bucket. The existing material E8 at 0x169cab is represented
// by a dynamic bracket because shadow_defer_cold_resources has already retargeted
// it before the load occurs.
const DWORD kShadowTextureDirectCallerRvas[] = {
    0x1159ed, // FUN_101155b0
    0x120f37, // GraphicsBillboard::RenderPass
    0x130027, // FUN_1012fa30
    0x17c5c2, // GraphicsForwardRenderer::Render
    0x18a90e, // FUN_1018a610, render-state parameter application
    0x1b8d3e, // LineEffect::RenderPass
    0x2049a0, // PieOmatic::Render
    0x23e7cb, // FUN_1023e1e0
    0x26ae2b  // WaterRenderInterface::RenderWaveElements
};
const unsigned kMeshGetTextureEnsureCallOffset = 8;

// GraphicsShadowMapRenderer::Render first builds a vector of 0x88-byte
// caster/pass records, then submits it through this one DX11-only E8. The DX9
// branch calls FUN_10187360 instead. Run 75 began bin zero at the outer
// GraphicsShadowMapDx11::RenderDirectional boundary and therefore mixed
// setup/record construction with record submission. Patch this call to put
// the boundary at the executor without touching its shared entry.
const DWORD kShadowRecordExecutorCallWindowRva = 0x18d054;
const unsigned kShadowRecordExecutorCallOffset = 9;
const DWORD kShadowRecordExecutorRva = 0x18c520;
const BYTE kShadowRecordExecutorCallWindowBytes[] = {
    0x51,
    0x8d, 0x44, 0x24, 0x14,
    0x50,
    0x57,
    0x8b, 0xce,
    0xe8, 0xbe, 0xf4, 0xff, 0xff,
    0xc6, 0x44, 0x24, 0x40, 0x00,
    0x8b, 0x44, 0x24, 0x10
};
const BYTE kShadowRecordExecutorBytes[] = {
    0x83, 0xec, 0x10, 0x53, 0x8b, 0x5c, 0x24, 0x1c,
    0x55, 0x8b, 0x53, 0x04, 0x89, 0x4c, 0x24, 0x10,
    0x8b, 0x0b, 0x56, 0x57, 0xc6, 0x44, 0x24, 0x1c
};
const DWORD kShadowRecordExecutorTailRva = 0x18c631;
const BYTE kShadowRecordExecutorTailBytes[] = {
    0x85, 0xff, 0x74, 0x07, 0x8b, 0xcf,
    0xe8, 0xc4, 0xf8, 0xff, 0xff,
    0x5f, 0x5e, 0x5d, 0x5b, 0x83, 0xc4, 0x10,
    0xc2, 0x0c, 0x00                         // three explicit arguments
};
const unsigned kTerrainRtLayerLimit = 64;

// --- Region::UnloadLevel.
const DWORD kUnloadLevelRva = 0x20e040;
const char kUnloadLevelName[] = "?UnloadLevel@Region@GAME@@QAEX_N_N@Z";
const BYTE kUnloadLevelBytes[] = {
    0x6a, 0xff,
    0x68, 0, 0, 0, 0,
    0x64, 0xa1, 0x00, 0x00, 0x00, 0x00,
    0x50,
    0x51, 0x56, 0x57,                          // push ecx,esi,edi
    0xa1, 0, 0, 0, 0                           // mov eax,ds:[__security_cookie]
};
const Relocation kUnloadLevelRelocs[] = {{3, 0x2a29a3}, {18, 0x36f000}};
const unsigned kPreloadEnqueueCallOffset = 10;

// --- Engine.dll's array allocator and the two syscalls under the block
// routine, all four reached through the import table rather than by patching
// code. The RVAs are identity assertions only; patchImport resolves by name.
//
// `??_U@YAPAXI@Z` is `operator new[](unsigned int)` and `??_V@YAXPAX@Z` is
// `operator delete[](void*)`, both __cdecl, both MSVCR110's. `FUN_1014d020`
// -- the archive `File` constructor -- calls the first twice per compressed
// entry opened, for `min(size, blockSize)` each:
//
//   1014d0b5  ff 31           push [ecx]         min(compressedSize, 256 KiB)
//   1014d0b7  ff 15 18c32a10  call [0x102ac318]  operator new[]
//   1014d0bd  89 43 20        mov [ebx+0x20],eax
//
// and `Archive::FreeFileBuffer` (`0x1011dce0`) is literally
// `PUSH EAX; CALL [0x102ac304]`.
// The four things in Engine.dll that can block, so that the main thread
// waiting on the loader thread stops being invisible. Slot RVAs are identity
// assertions; patchImport resolves by name.
const DWORD kSleepSlotRva = 0x2ac108;
const DWORD kWaitForMultipleObjectsSlotRva = 0x2ac154;

const char kNewArrayName[] = "??_U@YAPAXI@Z";
const char kDeleteArrayName[] = "??_V@YAXPAX@Z";
const DWORD kNewArraySlotRva = 0x2ac318;
const DWORD kDeleteArraySlotRva = 0x2ac304;
// At or above this, an allocation is one of the ones worth separating out --
// the block scratch buffers are `min(size, 0x40000)` and the `File`'s own
// decompressed scratch is the entry's full size.
const unsigned kHeapBigBytes = 64 * 1024;

// --- Region::WaitForLoadingToFinish. Seven bytes: `cmp byte [ecx+0x78],1`,
// `jz -6`, `ret`. The stolen bytes would contain that relative jump, so a
// trampoline is impossible and the replacement implements the spin itself.
const DWORD kWaitForLoadingRva = 0x20bde0;
const char kWaitForLoadingName[] = "?WaitForLoadingToFinish@Region@GAME@@QAEXXZ";
const BYTE kWaitForLoadingBytes[] = {0x80, 0x79, 0x78, 0x01, 0x74, 0xfa, 0xc3};
const DWORD kLeaveCriticalSectionSlotRva = 0x2ac178;
const Relocation kRegionLockRelocs[] = {{5, kEnterCriticalSectionSlotRva},
                                        {24, kLeaveCriticalSectionSlotRva}};
static_assert(sizeof(kRegionLockEbxBytes) == sizeof(kRegionLockEdiBytes),
              "both region-lock windows are the same twenty-eight byte shape");
const unsigned kRegionLockCallOffset = 3;
static_assert(sizeof(kForceLoadDeferredBytes) == sizeof(kForceLoadForwardBytes),
              "both forced-load windows are the same thirty-four byte shape");
const unsigned kForceLoadCallOffset = 12;

// The one field of Region this file dereferences, taken from the operand of
// the instruction that uses it -- `mov eax,[ecx+0x50]`, the first instruction
// of both Region::LoadLevel and Region::BackgroundLoadLevel -- rather than
// from a layout table. Non-null means the level is resident.
// The loading flag the renderer tests and the asynchronous path raises. Named
// once so verify-sites.py can assert the two instructions agree about it.
const unsigned kRegionLoadingOffset = 0x74;

// --- Engine::Update. Two of the call-site groups live inside it, so its own
// RVA is asserted once and their windows are offsets into the same function.
const DWORD kEngineUpdateRva = 0x1443a0;
const char kEngineUpdateName[] = "?Update@Engine@GAME@@QAEXPBVWorldFrustum@2@_N@Z";
// It is also detoured whole, which is a different question from the call sites
// inside it. Run 10 named a fifth to a third of the hitch time and left the
// rest dark: 950 ms of its worst frame, and every millisecond of a class of
// 200-240 ms frames that showed zero in every other engine column. Bracketing
// the engine's own two halves says which half the dark time is in -- or that
// it is in neither, which would put it outside Engine.dll and rule out all of
// Stages 4 and 5 at once. Both run once a frame.
const BYTE kEngineUpdateBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,
    0x6a, 0xff,
    0x68, 0, 0, 0, 0,                          // push <SEH handler>
    0x64, 0xa1, 0x00, 0x00, 0x00, 0x00,
    0x50,
    0x81, 0xec, 0x70, 0x08                     // sub esp,0x870
};
const Relocation kEngineUpdateRelocs[] = {{9, 0x296a30}};

// --- Engine::Render. Aligns its frame to 64 rather than 8, so unlike almost
// everything else here its opening six bytes are already distinctive; the
// relocated dword is the QueryPerformanceCounter import it reads first.
const DWORD kEngineRenderRva = 0x143fe0;
const char kEngineRenderName[] = "?Render@Engine@GAME@@QAEXXZ";
const BYTE kEngineRenderBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xc0,        // push ebp; mov ebp,esp; and esp,-64
    0x83, 0xec, 0x78,                          // sub esp,0x78
    0x56, 0x57,                                // push esi,edi
    0x8b, 0x3d, 0, 0, 0, 0,                    // mov edi,ds:[QueryPerformanceCounter]
    0x8d, 0x44, 0x24, 0x40,                    // lea eax,[esp+0x40]
    0x50,                                      // push eax
    0x8b, 0xf1                                 // mov esi,ecx
};
const Relocation kEngineRenderRelocs[] = {{13, 0x2ac1b8}};

// --- GraphicsDeferredRendererX::Render pass partition.  Run 69 leaves
// 97.007 ms of the transition frame's GPU interval outside every existing
// named GPU region, followed by 97.651 ms blocked in Draw/DrawIndexed on the
// next frame.  These are the direct, ordered children of the exported DX11
// deferred-renderer entry.  We patch their E8 sites rather than any callee
// entry, so other renderer call paths remain byte-identical.
//
// Several sites are closer than a 16-byte verification window.  As with the
// seven sweep calls below, every original window is therefore verified before
// the first write, and patchCall then rechecks the exact E8 plus its resolved
// target.  Each wide window is 16 bytes; the renderer entry is independently
// identified by its decorated export, RVA, and 24-byte opening (including the
// relocated exception handler).
const DWORD kDeferredRenderRva = 0x166130;
const char kDeferredRenderName[] =
    "?Render@GraphicsDeferredRendererX@GAME@@QAEPAVRenderTexture@2@"
    "AAVGraphicsCanvas@2@MPAV32@_N2PAVRenderSurface@2@2@Z";
const BYTE kDeferredRenderBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x6a, 0xff,
    0x68, 0, 0, 0, 0, 0x64, 0xa1, 0x00,
    0x00, 0x00, 0x00, 0x50, 0x81, 0xec, 0xa8, 0x00
};
const Relocation kDeferredRenderRelocs[] = {{9, 0x298f0d}};

// FUN_1017ead0 is the sole direct caller in the complete Engine.dll callgraph.
// Patch its E8 instead of the shared renderer prologue. The 24-byte window
// proves the last two arguments and ECX setup around the call; the independent
// 20-byte callee tail proves that the wrapper must pop seven stack arguments.
const DWORD kDeferredOwnerCallWindowRva = 0x17fc8b;
const unsigned kDeferredOwnerCallOffset = 16;
const BYTE kDeferredOwnerCallWindowBytes[] = {
    0x51, 0x8d, 0x8c, 0x24, 0x34, 0x07, 0x00, 0x00,
    0xf3, 0x0f, 0x11, 0x04, 0x24, 0xff, 0x70, 0x04,
    0xe8, 0x90, 0x64, 0xfe, 0xff, 0x8d, 0x8c, 0x24
};
const DWORD kDeferredRenderTailRva = 0x1665cf;
const BYTE kDeferredRenderTailBytes[] = {
    0x8b, 0x8c, 0x24, 0xa0, 0x00, 0x00, 0x00, 0x33,
    0xcc, 0xe8, 0x93, 0x4b, 0xf9, 0xff, 0x8b, 0xe5,
    0x5d, 0xc2, 0x1c, 0x00
};

// The callee-cleaned x86 ABI is part of each wrapper's correctness.  These
// independently re-read 16-byte epilogues and make the `ret` immediate agree
// with the explicit-argument count below.  This is the check that prevents a
// decompiler's implicit-this count from becoming another stack imbalance.
const BYTE kDeferredGeometrySetupTailBytes[] = {
    0x8b, 0xcf, 0xe8, 0x6a, 0x2e, 0x02, 0x00, 0x5f,
    0x5e, 0x5b, 0x83, 0xc4, 0x38, 0xc2, 0x08, 0x00
};
const BYTE kDeferredSceneListTailBytes[] = {
    0x0d, 0x00, 0x00, 0x00, 0x00, 0x59, 0x5f, 0x5e,
    0x5d, 0x5b, 0x83, 0xc4, 0x3c, 0xc2, 0x14, 0x00
};
const BYTE kDeferredShadowsTailBytes[] = {
    0x00, 0x00, 0x59, 0x5f, 0x5e, 0x5d, 0x5b, 0x81,
    0xc4, 0x18, 0x02, 0x00, 0x00, 0xc2, 0x08, 0x00
};
const BYTE kDeferredLightingTailBytes[] = {
    0x33, 0xcc, 0xe8, 0x3f, 0x5e, 0xf9, 0xff, 0x81,
    0xc4, 0xc4, 0x00, 0x00, 0x00, 0xc2, 0x08, 0x00
};
const BYTE kDeferredResolveTailBytes[] = {
    0x00, 0xe8, 0x28, 0x53, 0x02, 0x00, 0x5f, 0x5e,
    0x5d, 0x5b, 0x83, 0xc4, 0x18, 0xc2, 0x0c, 0x00
};
const BYTE kDeferredAoTailBytes[] = {
    0x4f, 0x08, 0xe8, 0x33, 0xf5, 0x02, 0x00, 0x5f,
    0x5e, 0x5b, 0x8b, 0xe5, 0x5d, 0xc2, 0x04, 0x00
};
const BYTE kDeferredLateSceneATailBytes[] = {
    0x0d, 0x00, 0x00, 0x00, 0x00, 0x59, 0x5f, 0x5e,
    0x5d, 0x5b, 0x83, 0xc4, 0x30, 0xc2, 0x08, 0x00
};
const BYTE kDeferredLateSceneBTailBytes[] = {
    0x8f, 0x69, 0x02, 0x00, 0xc6, 0x87, 0x69, 0x09,
    0x00, 0x00, 0x00, 0x5f, 0x5e, 0xc2, 0x04, 0x00
};
const BYTE kDeferredPostHighlightTailBytes[] = {
    0x00, 0x00, 0x59, 0x5f, 0x5e, 0x5d, 0x5b, 0x81,
    0xc4, 0xac, 0x00, 0x00, 0x00, 0xc2, 0x04, 0x00
};
const BYTE kDeferredPostFogTailBytes[] = {
    0x33, 0xcc, 0xe8, 0x49, 0x50, 0xf9, 0xff, 0x81,
    0xc4, 0xc8, 0x00, 0x00, 0x00, 0xc2, 0x08, 0x00
};
const BYTE kDeferredPostMaskTailBytes[] = {
    0x40, 0xc2, 0x04, 0x00, 0x5f, 0x5e, 0x5d, 0x33,
    0xc0, 0x5b, 0x83, 0xc4, 0x40, 0xc2, 0x04, 0x00
};
const BYTE kDeferredPostCompositeTailBytes[] = {
    0xce, 0xe8, 0xa1, 0x9d, 0xff, 0xff, 0x5f, 0x5e,
    0x5d, 0x5b, 0x83, 0xc4, 0x30, 0xc2, 0x14, 0x00
};
const BYTE kDeferredPostDebugTailBytes[] = {
    0x0f, 0x82, 0xbb, 0xfe, 0xff, 0xff, 0x5f, 0x5e,
    0x5d, 0x5b, 0x83, 0xc4, 0x58, 0xc2, 0x04, 0x00
};

struct DeferredTargetAbi {
    DWORD targetRva;
    DWORD tailRva;
    const BYTE* bytes;
    unsigned arguments;
};
const DeferredTargetAbi kDeferredTargetAbis[] = {
    {0x1653a0, 0x16557f, kDeferredGeometrySetupTailBytes, 2},
    {0x1883f0, 0x1885ec, kDeferredSceneListTailBytes, 5},
    {0x164050, 0x16458b, kDeferredShadowsTailBytes, 2},
    {0x164640, 0x16532a, kDeferredLightingTailBytes, 2},
    {0x166800, 0x166bd2, kDeferredResolveTailBytes, 3},
    {0x15c8e0, 0x15c9c6, kDeferredAoTailBytes, 1},
    {0x161c80, 0x16212d, kDeferredLateSceneATailBytes, 2},
    {0x161a00, 0x161a5d, kDeferredLateSceneBTailBytes, 1},
    {0x161a70, 0x161c6b, kDeferredPostHighlightTailBytes, 1},
    {0x165aa0, 0x166120, kDeferredPostFogTailBytes, 2},
    {0x162200, 0x1625b0, kDeferredPostMaskTailBytes, 1},
    {0x1657b0, 0x165a89, kDeferredPostCompositeTailBytes, 5},
    {0x161720, 0x16193f, kDeferredPostDebugTailBytes, 1},
};
const unsigned kDeferredTargetAbiCount =
    sizeof(kDeferredTargetAbis) / sizeof(kDeferredTargetAbis[0]);

// CSV creation/load bins. "Other" is deliberately not called a gap: it can
// be owner code between direct children or one of the other named children.
enum DeferredOwnerBin {
    DeferredOwnerBinNone,
    DeferredOwnerBinI1Other,
    DeferredOwnerBinI1GeometrySetup,
    DeferredOwnerBinI1GeometryScene,
    DeferredOwnerBinI2Other,
    DeferredOwnerBinI2GeometrySetup,
    DeferredOwnerBinI2GeometryScene,
    DeferredOwnerBinCount
};

enum DeferredGeometryCell {
    DeferredGeometryCellNone,
    DeferredGeometryCellI1Setup,
    DeferredGeometryCellI1Scene,
    DeferredGeometryCellI2Setup,
    DeferredGeometryCellI2Scene,
    DeferredGeometryCellCount
};

// The loader fence. `push -1; push [g_fence]; call WaitForSingleObject`.
// §6.2 argues the event is normally already signalled, so a non-zero total
// here is exactly the evidence that argument is wrong.
const DWORD kFenceWindowRva = 0x14479a;
const BYTE kFenceWindowBytes[] = {
    0x6a, 0xff,                                // push -1  (INFINITE)
    0xff, 0x35, 0, 0, 0, 0,                    // push dword [fence]
    0xff, 0x15, 0, 0, 0, 0,                    // call [WaitForSingleObject]
    0xe8, 0x63, 0xab, 0xfd, 0xff               // call the fence close
};
const DWORD kWaitForSingleObjectSlotRva = 0x2ac188;
const Relocation kFenceWindowRelocs[] = {{4, 0x370258},
                                         {10, kWaitForSingleObjectSlotRva}};
const unsigned kFenceCallOffset = 8;

// The seven UnloadUnreferencedResources sweeps, in two contiguous runs.
const DWORD kSweepTargetRva = 0x120250;
const char kSweepTargetName[] =
    "?UnloadUnreferencedResources@BaseResourceManager@GAME@@QAEXXZ";
const BYTE kSweepWindowABytes[] = {
    0x8b, 0x4e, 0x28, 0xe8, 0xc7, 0xbd, 0xfd, 0xff,
    0x8b, 0x4e, 0x2c, 0xe8, 0xbf, 0xbd, 0xfd, 0xff,
    0x8b, 0x4e, 0x30, 0xe8, 0xb7, 0xbd, 0xfd, 0xff,
    0x8b, 0x4e, 0x24, 0xe8, 0xaf, 0xbd, 0xfd, 0xff
};
const BYTE kSweepWindowBBytes[] = {
    0x8b, 0x4e, 0x1c, 0xe8, 0xa7, 0xbd, 0xfd, 0xff,
    0x8b, 0x8b, 0x58, 0x01, 0x00, 0x00, 0xe8, 0x9c, 0xbd, 0xfd, 0xff,
    0x8b, 0x4b, 0x38, 0x8d, 0x89, 0x18, 0x05, 0x00, 0x00,
    0xe8, 0x8e, 0xbd, 0xfd, 0xff
};
const DWORD kSweepWindowARva = 0x144481;
const DWORD kSweepWindowBRva = 0x1444a1;

// Seven calls in two windows, so the window cannot be the thing each patch
// verifies: retargeting the first call rewrites four of the bytes the next
// one would compare. The window is checked once, whole, and then each call is
// patched against its own five bytes -- which is not a weaker check, because
// patchCall also requires the displacement to resolve to the sweep itself.
struct SweepSite {
    DWORD windowRva;
    const BYTE* window;
    unsigned windowLength;
    unsigned callOffset;
};
const SweepSite kSweepSites[kSweepCount] = {
    {kSweepWindowARva, kSweepWindowABytes, sizeof(kSweepWindowABytes), 3},
    {kSweepWindowARva, kSweepWindowABytes, sizeof(kSweepWindowABytes), 11},
    {kSweepWindowARva, kSweepWindowABytes, sizeof(kSweepWindowABytes), 19},
    {kSweepWindowARva, kSweepWindowABytes, sizeof(kSweepWindowABytes), 27},
    {kSweepWindowBRva, kSweepWindowBBytes, sizeof(kSweepWindowBBytes), 3},
    {kSweepWindowBRva, kSweepWindowBBytes, sizeof(kSweepWindowBBytes), 14},
    {kSweepWindowBRva, kSweepWindowBBytes, sizeof(kSweepWindowBBytes), 28},
};
const DWORD kGameUpdateRva = 0x19a230;
const char kGameUpdateName[] = "?Update@GameEngine@GAME@@QAEXH@Z";
const BYTE kGameUpdateBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,
    0x6a, 0xff,
    0x68, 0, 0, 0, 0,                          // push <SEH handler>
    0x64, 0xa1, 0x00, 0x00, 0x00, 0x00,
    0x50,
    0x81, 0xec, 0x70, 0x04                     // sub esp,0x470
};
const Relocation kGameUpdateRelocs[] = {{9, 0x2f1112}};

// ---------------------------------------------------------------------------
// Configuration. Bit 0 means "everything"; the rest select groups, so a run
// that misbehaves can be narrowed from the INI rather than from a rebuild.
const unsigned kGroupAll = 0x01;

unsigned g_traceMask = 1;
// Rejected behavior experiments remain compile-time off while their trace
// machinery and CSV columns stay available for reading the archived runs.
constexpr unsigned g_timerPeriodMs = 0;
constexpr unsigned g_pumpTimerMinMs = 0;
LONG g_pumpLastFullTick;
bool g_terrainTracing;
constexpr bool g_reflectionDeferAdmissionMesh = false;
bool g_reflectionDeferAdmissionMeshActive;
constexpr bool g_reflectionDeferAdmissionAll = false;
bool g_reflectionDeferAdmissionAllActive;
bool g_reflectionAdmissionBuildActive;
unsigned g_reflectionAdmissionBuildBuffers;
bool g_reflectionAdmissionPending;
bool g_reflectionAdmissionRenderActive;
// Whether this install() is installing the trace at all. archive_cache_mb can
// reach install() with the performance probe off, and without this every
// wants() below would read the trace mask -- which defaults to 1 -- and put
// the whole instrument in on a boot that asked only for the cache.
bool g_tracing;
bool g_pumpTracing;
// True only when the load, frame, and directional-shadow trace brackets all
// installed. Without all three, "outside directional" or update/render phase
// would be an inference from a missing hook rather than an observed class.
bool g_outsideDirResourceTracing;
// True only when the shared load hook, directional bracket, and Resource
// state/name accessors all installed. Retains rare cold directional mesh
// loads until F12; it never formats on the candidate frame.
bool g_shadowMeshResourceTracing;

bool reflectionAdmissionThresholdReached(unsigned buffers) {
    return buffers >= kReflectionAdmissionBufferThreshold;
}

LoadLevelFn g_loadLevel;
GuaranteedGetLevelFn g_guaranteedGetLevel;
LoadResourceFn g_loadResource;
ResourceFileNameFn g_resourceFileName;
SetTextureParameterFn g_setTextureParameter;


TerrainPreloadFn g_terrainPreload;
TerrainSetShaderParamsFn g_terrainSetShaderParams;
TerrainSetGrassShaderParamsFn g_terrainSetGrassShaderParams;
TerrainRenderGroundFn g_terrainRenderGround;
TerrainRtLoadFn g_terrainRtLoad;
TerrainRtLoadRenderDataFn g_terrainRtLoadRenderData;
TerrainRtPreloadFn g_terrainRtPreload;
TerrainRtNumLayersFn g_terrainRtNumLayers;
TerrainRtLayerTypeFn g_terrainRtLayerType;
DeferredRenderFn g_deferredRender;
DeferredFn2 g_deferredGeometrySetup;
DeferredFn5 g_deferredGeometryScene;
DeferredFn2 g_deferredShadows;
DeferredFn2 g_deferredLighting;
DeferredFn3 g_deferredResolve;
DeferredFn1 g_deferredAo;
DeferredFn2 g_deferredLateSceneA;
DeferredFn1 g_deferredLateSceneB;
DeferredFn5 g_deferredLateSceneList;
DeferredFn1 g_deferredPostHighlight;
DeferredFn2 g_deferredPostFog;
DeferredFn1 g_deferredPostMask;
DeferredFn5 g_deferredPostComposite;
DeferredFn1 g_deferredPostDebug;
ReflectionManagerFn g_reflectionManager;
ReflectionPlaneFn g_reflectionPlane;
ReflectionBuildSceneFn g_reflectionBuildScene;
UnloadLevelFn g_unloadLevel;
EnqueueFn g_enqueue;
ReadFromFileFn g_readFromFile;
SweepFn g_sweep;
EngineUpdateFn g_engineUpdate;
EngineRenderFn g_engineRender;
GameUpdateFn g_gameUpdate;
NewArrayFn g_newArray;
DeleteArrayFn g_deleteArray;
SetFilePointerExFn g_setFilePointerEx;
ReadFileFn g_readFile;
WaitFn g_engineWait;
WaitMultipleFn g_engineWaitMultiple;

CallPatch g_deferredCallPatches[kDeferredCallSiteCount];
CallPatch g_deferredOwnerPatch;
CallPatch g_reflectionManagerPatch;
CallPatch g_reflectionPlanePatch;
CallPatch g_reflectionBuildScenePatch;
bool g_deferredPassTracing;
volatile LONG g_deferredPass;
volatile LONG g_deferredGeometrySite;
volatile LONG g_deferredOwnerInvocation;
unsigned g_deferredOwnerFrame;
unsigned g_deferredOwnerCallsThisFrame;
bool g_reflectionTracing;
bool g_gpuChunkTracing;
volatile LONG g_reflectionManagerInvocation;
volatile LONG g_reflectionPlaneInvocation;
volatile LONG g_reflectionChild;
unsigned g_reflectionManagerFrame;
unsigned g_reflectionManagerCallsThisFrame;
unsigned g_reflectionPlaneCallsThisManager;
SleepFn g_engineSleep;
unsigned g_renderTicks;

Detour g_loadLevelDetour;
Detour g_guaranteedDetour;
Detour g_loadResourceDetour;
Detour g_unloadLevelDetour;
Detour g_enqueueDetour;
Detour g_readFromFileDetour;
Detour g_waitForLoadingDetour;
Detour g_engineUpdateDetour;
Detour g_engineRenderDetour;
Detour g_gameUpdateDetour;
Detour g_terrainPreloadDetour;
Detour g_terrainSetShaderParamsDetour;
Detour g_terrainSetGrassShaderParamsDetour;
Detour g_terrainRenderGroundDetour;
Detour g_terrainRtLoadDetour;
Detour g_terrainRtLoadRenderDataDetour;
Detour g_terrainRtPreloadDetour;
CallPatch g_newArrayPatch;
CallPatch g_deleteArrayPatch;
CallPatch g_seekPatch;
CallPatch g_readFilePatch;
CallPatch g_csPatch;
CallPatch g_objWaitPatch;
CallPatch g_objWaitMultiplePatch;
CallPatch g_enginesleepPatch;
CallPatch g_lockPatches[kLockSiteCount];
CallPatch g_forceLoadPatches[kForceLoadSiteCount];
CallPatch g_fencePatch;
CallPatch g_sweepPatches[kSweepCount];
CallPatch g_shadowMeshEnsurePatch;
CallPatch g_shadowTextureParameterPatch;
LONG g_insideEngineUpdate;
LONG g_insideEngineRender;
void* g_lastShadowRegion;
void* g_cachedShadowSurface;
DWORD g_cachedShadowMatrix[kShadowMatrixDwords];
int g_cachedShadowResult;
bool g_cachedShadowValid;
bool g_reusedLastShadow;
bool g_shadowTracing;
bool g_crossPassTracing;
bool g_reflectionChildTracing;
bool g_resourceFileNameVerified;
bool g_nameHashLayoutVerified;
bool g_shadowMaterialTexturePending;
bool g_shadowMaterialTextureHooked;
bool g_shadowTextureParameterHooked;
bool g_shadowMeshParameterHooked;
bool g_shadowTextureCallerSitesVerified;
bool g_insideShadowMaterialTexture;
uint32_t g_shadowMaterialTexturePendingUs;
uint32_t g_shadowMaterialPendingNameHash;
LONG g_shadowMaterialReports;
LONG g_shadowTextureChainReports;

const tq::probe::Counter kDeferredPassCountCounters[] = {
    tq::probe::CounterCount,
    tq::probe::CounterEngineDeferredGeometry,
    tq::probe::CounterEngineDeferredShadows,
    tq::probe::CounterEngineDeferredLighting,
    tq::probe::CounterEngineDeferredResolve,
    tq::probe::CounterEngineDeferredLateScene,
    tq::probe::CounterEngineDeferredPost,
};
const tq::probe::Counter kDeferredPassDurationCounters[] = {
    tq::probe::CounterCount,
    tq::probe::CounterEngineDeferredGeometryUs,
    tq::probe::CounterEngineDeferredShadowsUs,
    tq::probe::CounterEngineDeferredLightingUs,
    tq::probe::CounterEngineDeferredResolveUs,
    tq::probe::CounterEngineDeferredLateSceneUs,
    tq::probe::CounterEngineDeferredPostUs,
};
const tq::probe::Counter kDeferredPassDrawCounters[] = {
    tq::probe::CounterCount,
    tq::probe::CounterEngineDeferredGeometryDrawUs,
    tq::probe::CounterEngineDeferredShadowsDrawUs,
    tq::probe::CounterEngineDeferredLightingDrawUs,
    tq::probe::CounterEngineDeferredResolveDrawUs,
    tq::probe::CounterEngineDeferredLateSceneDrawUs,
    tq::probe::CounterEngineDeferredPostDrawUs,
};
const tq::probe::Counter kDeferredGeometryCountCounters[] = {
    tq::probe::CounterCount,
    tq::probe::CounterEngineDeferredI1GeometrySetup,
    tq::probe::CounterEngineDeferredI1GeometryScene,
    tq::probe::CounterEngineDeferredI2GeometrySetup,
    tq::probe::CounterEngineDeferredI2GeometryScene,
};
const tq::probe::Counter kDeferredGeometryDurationCounters[] = {
    tq::probe::CounterCount,
    tq::probe::CounterEngineDeferredI1GeometrySetupUs,
    tq::probe::CounterEngineDeferredI1GeometrySceneUs,
    tq::probe::CounterEngineDeferredI2GeometrySetupUs,
    tq::probe::CounterEngineDeferredI2GeometrySceneUs,
};
const tq::probe::Counter kDeferredGeometryDrawCounters[] = {
    tq::probe::CounterCount,
    tq::probe::CounterEngineDeferredI1GeometrySetupDrawUs,
    tq::probe::CounterEngineDeferredI1GeometrySceneDrawUs,
    tq::probe::CounterEngineDeferredI2GeometrySetupDrawUs,
    tq::probe::CounterEngineDeferredI2GeometrySceneDrawUs,
};
const tq::probe::GpuPhase kDeferredGeometryGpuPhases[] = {
    tq::probe::GpuPhaseCount,
    tq::probe::GpuDeferredI1GeometrySetup,
    tq::probe::GpuDeferredI1GeometryScene,
    tq::probe::GpuDeferredI2GeometrySetup,
    tq::probe::GpuDeferredI2GeometryScene,
};

struct DeferredOwnerBinCounters {
    tq::probe::Counter resourceCount;
    tq::probe::Counter resourceUs;
    tq::probe::Counter textureCount;
    tq::probe::Counter textureUs;
    tq::probe::Counter bufferCount;
    tq::probe::Counter bufferUs;
};

const DeferredOwnerBinCounters kDeferredOwnerBinCounters[] = {
    {tq::probe::CounterCount, tq::probe::CounterCount,
     tq::probe::CounterCount, tq::probe::CounterCount,
     tq::probe::CounterCount, tq::probe::CounterCount},
    {tq::probe::CounterEngineDeferredI1OtherResLoad,
     tq::probe::CounterEngineDeferredI1OtherResLoadUs,
     tq::probe::CounterEngineDeferredI1OtherTexCreate,
     tq::probe::CounterEngineDeferredI1OtherTexCreateUs,
     tq::probe::CounterEngineDeferredI1OtherBufCreate,
     tq::probe::CounterEngineDeferredI1OtherBufCreateUs},
    {tq::probe::CounterEngineDeferredI1GeometrySetupResLoad,
     tq::probe::CounterEngineDeferredI1GeometrySetupResLoadUs,
     tq::probe::CounterEngineDeferredI1GeometrySetupTexCreate,
     tq::probe::CounterEngineDeferredI1GeometrySetupTexCreateUs,
     tq::probe::CounterEngineDeferredI1GeometrySetupBufCreate,
     tq::probe::CounterEngineDeferredI1GeometrySetupBufCreateUs},
    {tq::probe::CounterEngineDeferredI1GeometrySceneResLoad,
     tq::probe::CounterEngineDeferredI1GeometrySceneResLoadUs,
     tq::probe::CounterEngineDeferredI1GeometrySceneTexCreate,
     tq::probe::CounterEngineDeferredI1GeometrySceneTexCreateUs,
     tq::probe::CounterEngineDeferredI1GeometrySceneBufCreate,
     tq::probe::CounterEngineDeferredI1GeometrySceneBufCreateUs},
    {tq::probe::CounterEngineDeferredI2OtherResLoad,
     tq::probe::CounterEngineDeferredI2OtherResLoadUs,
     tq::probe::CounterEngineDeferredI2OtherTexCreate,
     tq::probe::CounterEngineDeferredI2OtherTexCreateUs,
     tq::probe::CounterEngineDeferredI2OtherBufCreate,
     tq::probe::CounterEngineDeferredI2OtherBufCreateUs},
    {tq::probe::CounterEngineDeferredI2GeometrySetupResLoad,
     tq::probe::CounterEngineDeferredI2GeometrySetupResLoadUs,
     tq::probe::CounterEngineDeferredI2GeometrySetupTexCreate,
     tq::probe::CounterEngineDeferredI2GeometrySetupTexCreateUs,
     tq::probe::CounterEngineDeferredI2GeometrySetupBufCreate,
     tq::probe::CounterEngineDeferredI2GeometrySetupBufCreateUs},
    {tq::probe::CounterEngineDeferredI2GeometrySceneResLoad,
     tq::probe::CounterEngineDeferredI2GeometrySceneResLoadUs,
     tq::probe::CounterEngineDeferredI2GeometrySceneTexCreate,
     tq::probe::CounterEngineDeferredI2GeometrySceneTexCreateUs,
     tq::probe::CounterEngineDeferredI2GeometrySceneBufCreate,
     tq::probe::CounterEngineDeferredI2GeometrySceneBufCreateUs},
};
static_assert(sizeof(kDeferredPassCountCounters)
                  / sizeof(kDeferredPassCountCounters[0])
                  == DeferredPassCount,
              "every deferred pass needs a call counter");
static_assert(sizeof(kDeferredPassDurationCounters)
                  / sizeof(kDeferredPassDurationCounters[0])
                  == DeferredPassCount,
              "every deferred pass needs a duration counter");
static_assert(sizeof(kDeferredPassDrawCounters)
                  / sizeof(kDeferredPassDrawCounters[0])
                  == DeferredPassCount,
              "every deferred pass needs a draw-duration counter");
static_assert(sizeof(kDeferredGeometryCountCounters)
                  / sizeof(kDeferredGeometryCountCounters[0])
                  == DeferredGeometryCellCount,
              "every exact geometry cell needs a call counter");
static_assert(sizeof(kDeferredGeometryDurationCounters)
                  / sizeof(kDeferredGeometryDurationCounters[0])
                  == DeferredGeometryCellCount,
              "every exact geometry cell needs a duration counter");
static_assert(sizeof(kDeferredGeometryDrawCounters)
                  / sizeof(kDeferredGeometryDrawCounters[0])
                  == DeferredGeometryCellCount,
              "every exact geometry cell needs a draw counter");
static_assert(sizeof(kDeferredGeometryGpuPhases)
                  / sizeof(kDeferredGeometryGpuPhases[0])
                  == DeferredGeometryCellCount,
              "every exact geometry cell needs a GPU phase");
static_assert(sizeof(kDeferredOwnerBinCounters)
                  / sizeof(kDeferredOwnerBinCounters[0])
                  == DeferredOwnerBinCount,
              "every owner bin needs Resource and D3D counters");

const tq::probe::Counter kReflectionManagerCountCounters[] = {
    tq::probe::CounterCount,
    tq::probe::CounterEngineReflectionI1,
    tq::probe::CounterEngineReflectionI2,
};
const tq::probe::Counter kReflectionManagerDurationCounters[] = {
    tq::probe::CounterCount,
    tq::probe::CounterEngineReflectionI1Us,
    tq::probe::CounterEngineReflectionI2Us,
};
const tq::probe::Counter kReflectionManagerDrawCounters[] = {
    tq::probe::CounterCount,
    tq::probe::CounterEngineReflectionI1DrawUs,
    tq::probe::CounterEngineReflectionI2DrawUs,
};
const tq::probe::GpuPhase kReflectionManagerGpuPhases[] = {
    tq::probe::GpuPhaseCount,
    tq::probe::GpuReflectionI1,
    tq::probe::GpuReflectionI2,
};
const tq::probe::GpuPhase kReflectionCellGpuPhases[] = {
    tq::probe::GpuPhaseCount,
    tq::probe::GpuReflectionI1P1,
    tq::probe::GpuReflectionI1P2,
    tq::probe::GpuReflectionI2P1,
    tq::probe::GpuReflectionI2P2,
};
static_assert(sizeof(kReflectionCellCounters)
                  / sizeof(kReflectionCellCounters[0])
                  == ReflectionCellCount,
              "every reflection plane cell needs all counters");
static_assert(sizeof(kReflectionCellGpuPhases)
                  / sizeof(kReflectionCellGpuPhases[0])
                  == ReflectionCellCount,
              "every reflection plane cell needs a GPU phase");
static_assert(sizeof(kReflectionChildGpuPhases)
                  / sizeof(kReflectionChildGpuPhases[0])
                  == ReflectionCellCount,
              "every reflection plane cell needs both child GPU phases");
static_assert(sizeof(kReflectionManagerCountCounters)
                  / sizeof(kReflectionManagerCountCounters[0]) == 3
              && sizeof(kReflectionManagerDurationCounters)
                  / sizeof(kReflectionManagerDurationCounters[0]) == 3
              && sizeof(kReflectionManagerDrawCounters)
                  / sizeof(kReflectionManagerDrawCounters[0]) == 3
              && sizeof(kReflectionManagerGpuPhases)
                  / sizeof(kReflectionManagerGpuPhases[0]) == 3,
              "reflection manager invocation arrays cover i1 and i2");

ReflectionCell reflectionCell(unsigned manager, unsigned plane) {
    if (manager == 1 && plane == 1) return ReflectionCellI1P1;
    if (manager == 1 && plane == 2) return ReflectionCellI1P2;
    if (manager == 2 && plane == 1) return ReflectionCellI2P1;
    if (manager == 2 && plane == 2) return ReflectionCellI2P2;
    return ReflectionCellNone;
}

ReflectionLocation currentReflectionLocation() {
    TQ_ENGINE_PROBE_ENTER();
    ReflectionLocation result = {};
    if (!g_reflectionTracing || !onMainThread()) return result;
    const LONG manager = InterlockedCompareExchange(
        &g_reflectionManagerInvocation, 0, 0);
    const LONG plane = InterlockedCompareExchange(
        &g_reflectionPlaneInvocation, 0, 0);
    if (manager <= 0) return result;
    result.manager = (unsigned)manager;
    result.plane = plane > 0 ? (unsigned)plane : 0;
    result.cell = reflectionCell(result.manager, result.plane);
    return result;
}
const unsigned kGpuChunkDraws = 20;
const unsigned kGpuChunkStartDraw = 1;
const unsigned kGpuChunkMarkerFrames = 120;
const unsigned kGpuChunkRenderableHotCpuUs = 250;

GpuChunkEvent g_gpuChunkEvents[kGpuChunkEventSlots];
unsigned g_gpuChunkEventSequence;
ActiveGpuChunkEvent g_activeGpuChunks[GpuChunkClassCount];
unsigned g_gpuChunkLastFrame[GpuChunkClassCount];
bool g_reflectionGpuChunkPending;
unsigned g_reflectionGpuChunkTriggerUs;
GpuChunkRenderableCall* g_activeGpuChunkRenderableCall;

tq::probe::GpuPhase gpuChunkPhase(unsigned chunk) {
    if (chunk >= kGpuChunkCount) return tq::probe::GpuPhaseCount;
    return (tq::probe::GpuPhase)(tq::probe::GpuChunkReflection00 + chunk);
}

void openGpuChunk(ActiveGpuChunkEvent& active) {
    if (!active.event || !active.recording
        || active.chunk >= kGpuChunkCount || active.opened
        || active.drawsSeen + 1 < active.event->startDraw)
        return;
    if (!active.context) return;
    tq::probe::gpuBegin(active.context, gpuChunkPhase(active.chunk));
    active.opened = true;
}

void refreshGpuChunkDrawGate() {
    const ActiveGpuChunkEvent& reflection =
        g_activeGpuChunks[GpuChunkReflection];
    const bool active = reflection.event && reflection.recording;
    InterlockedExchange(&tq::engineprobe::detail::gpuChunkDrawActive, active ? 1 : 0);
}

void armGpuChunks(const ReflectionLocation& reflection, unsigned triggerUs) {
    if (!g_gpuChunkTracing) return;
    const unsigned frame = tq::probe::currentFrameIndex();
    ActiveGpuChunkEvent& active = g_activeGpuChunks[GpuChunkReflection];
    if (active.event
        || g_gpuChunkLastFrame[GpuChunkReflection] == frame + 1) {
        tq::probe::engineCount(
            tq::probe::CounterEngineGpuChunkReflectionCollision);
        return;
    }
    GpuChunkEvent& event =
        g_gpuChunkEvents[g_gpuChunkEventSequence++ % kGpuChunkEventSlots];
    memset(&event, 0, sizeof(event));
    event.framePlusOne = frame + 1;
    event.kind = GpuChunkReflection;
    event.startDraw = kGpuChunkStartDraw;
    event.triggerUs = triggerUs;
    event.manager = reflection.manager;
    event.plane = reflection.plane;
    active.event = &event;
    active.context = tq::probe::currentGpuContext();
    active.chunk = 0;
    active.drawsSeen = 0;
    active.opened = false;
    active.recording = true;
    g_gpuChunkLastFrame[GpuChunkReflection] = frame + 1;
    tq::probe::engineCount(tq::probe::CounterEngineGpuChunkReflectionArm);
    tq::probe::engineCount(
        tq::probe::CounterEngineGpuChunkReflectionStartDraw,
        kGpuChunkStartDraw);
    refreshGpuChunkDrawGate();
}

void closeGpuChunks() {
    ActiveGpuChunkEvent& active = g_activeGpuChunks[GpuChunkReflection];
    if (!active.event) return;
    if (active.opened)
        tq::probe::gpuEnd(active.context, gpuChunkPhase(active.chunk));
    if (active.chunk < kGpuChunkCount)
        active.event->chunks = active.chunk
            + (active.event->bins[active.chunk].draws || active.opened
               ? 1u : 0u);
    else
        active.event->chunks = kGpuChunkCount;
    active = {};
    g_activeGpuChunkRenderableCall = nullptr;
    refreshGpuChunkDrawGate();
}

void beginGpuChunkDrawInternal(ID3D11DeviceContext* context) {
    if (!g_gpuChunkTracing || !onMainThread()) return;
    ActiveGpuChunkEvent& active = g_activeGpuChunks[GpuChunkReflection];
    if (!active.event || !active.recording) return;
    if (!active.context) active.context = context;
    openGpuChunk(active);
}

void finishGpuChunkDrawInternal(
    bool indexed, unsigned count,
    const tq::engineprobe::DeferredDrawBindings* bindings) {
    if (!g_gpuChunkTracing || !onMainThread()) return;
    ActiveGpuChunkEvent& active = g_activeGpuChunks[GpuChunkReflection];
    if (!active.event || !active.recording) return;
    const unsigned ordinal = ++active.drawsSeen;
    tq::probe::engineCount(tq::probe::CounterEngineGpuChunkReflectionDraw);
    if (ordinal < active.event->startDraw) {
        // Open immediately after the preceding draw so bin zero also includes
        // resource and setup commands issued before the start draw reaches
        // the D3D hook.
        if (ordinal + 1 == active.event->startDraw) openGpuChunk(active);
        return;
    }
    if (active.chunk >= kGpuChunkCount) {
        active.event->overflow = true;
        tq::probe::engineCount(
            tq::probe::CounterEngineGpuChunkReflectionOverflow);
        active.recording = false;
        refreshGpuChunkDrawGate();
        return;
    }
    GpuChunkBin& bin = active.event->bins[active.chunk];
        if (!bin.draws) {
            bin.firstDraw = ordinal;
            if (bindings) {
                bin.firstVertexShader = bindings->vertexShader;
                bin.firstPixelShader = bindings->pixelShader;
                bin.firstResource0 = bindings->pixelResources[0];
                bin.firstVertexBuffer0 = bindings->vertexBuffers[0];
                bin.firstIndexBuffer = bindings->indexBuffer;
            }
        } else if (bindings
                   && (bin.lastVertexShader != bindings->vertexShader
                       || bin.lastPixelShader != bindings->pixelShader
                       || bin.lastResource0 != bindings->pixelResources[0]
                       || bin.lastVertexBuffer0 != bindings->vertexBuffers[0]
                       || bin.lastIndexBuffer != bindings->indexBuffer)) {
            ++bin.bindingChanges;
        }
        ++bin.draws;
        if (indexed) ++bin.indexedDraws;
        bin.elements += count;
        if (bindings) {
            if (!bindings->pixelShader) ++bin.pixelShaderNullDraws;
            if (!bindings->pixelResources[0]) ++bin.resource0NullDraws;
            bin.lastVertexShader = bindings->vertexShader;
            bin.lastPixelShader = bindings->pixelShader;
            bin.lastResource0 = bindings->pixelResources[0];
            bin.lastVertexBuffer0 = bindings->vertexBuffers[0];
            bin.lastIndexBuffer = bindings->indexBuffer;
        }
    if (bin.draws != kGpuChunkDraws) return;
    if (active.opened)
        tq::probe::gpuEnd(active.context, gpuChunkPhase(active.chunk));
    active.opened = false;
    ++active.chunk;
    active.event->chunks = active.chunk;
    // Open the next interval now, not at the next Draw call, so work between
    // renderables belongs to the following draw range.
    if (active.chunk < kGpuChunkCount) openGpuChunk(active);
}

const char* gpuChunkRenderableName(GpuChunkRenderableKind kind) {
    return kind == GpuChunkTerrainPlug ? "TerrainPlug"
         : kind == GpuChunkTerrainBlock ? "TerrainBlock"
         : kind == GpuChunkMeshInstance ? "GraphicsMeshInstance" : "none";
}

void noteGpuChunkRenderableResource(unsigned elapsedUs,
                                    const void* terrainType,
                                    int materialIndex) {
    if (!g_activeGpuChunkRenderableCall || !onMainThread()) return;
    GpuChunkRenderableCall& call = *g_activeGpuChunkRenderableCall;
    ++call.resourceCount;
    call.resourceUs += elapsedUs;
    if (terrainType) {
        call.terrainType = terrainType;
        call.materialIndex = materialIndex;
    }
}

void noteGpuChunkRenderableCreation(bool texture, unsigned elapsedUs) {
    if (!g_activeGpuChunkRenderableCall || !onMainThread()) return;
    GpuChunkRenderableCall& call = *g_activeGpuChunkRenderableCall;
    if (texture) {
        ++call.textureCount;
        call.textureUs += elapsedUs;
    } else {
        ++call.bufferCount;
        call.bufferUs += elapsedUs;
    }
}

void reportGpuChunksAtMarker() {
    if (!g_gpuChunkTracing) return;
    const unsigned marker = tq::probe::currentFrameIndex();
    const unsigned count = g_gpuChunkEventSequence < kGpuChunkEventSlots
        ? g_gpuChunkEventSequence : kGpuChunkEventSlots;
    unsigned retained = 0;
    for (unsigned offset = 0; offset < count; ++offset) {
        const GpuChunkEvent& event = g_gpuChunkEvents[
            (g_gpuChunkEventSequence - count + offset)
                % kGpuChunkEventSlots];
        if (event.framePlusOne && event.framePlusOne - 1 <= marker
            && marker - (event.framePlusOne - 1) <= kGpuChunkMarkerFrames)
            ++retained;
    }
    tq::hdr::log("Engine trace: F12 frame %u, retained GPU chunk events %u"
                 " (preceding %u frames)\r\n",
                 marker, retained, kGpuChunkMarkerFrames);
    // Oldest first is deliberate. The human-reaction candidate normally ends
    // 0.3--0.8 seconds before F12; Run 79 printed a newer event first and its
    // per-call dump exhausted the session log before the older event appeared.
    for (unsigned offset = 0; offset < count; ++offset) {
        const GpuChunkEvent& event = g_gpuChunkEvents[
            (g_gpuChunkEventSequence - count + offset)
                % kGpuChunkEventSlots];
        if (!event.framePlusOne) continue;
        const unsigned frame = event.framePlusOne - 1;
        if (frame > marker || marker - frame > kGpuChunkMarkerFrames)
            continue;
        tq::hdr::log(
            "Engine trace: GPU chunks frame %u, %s, i%u/p%u,"
            " start draw %u, trigger %u us, chunks %u, overflow %u\r\n",
            frame, "reflection RenderLightStyle", event.manager, event.plane,
            event.startDraw, event.triggerUs, event.chunks,
            event.overflow ? 1u : 0u);
        for (unsigned i = 0; i < event.chunks && i < kGpuChunkCount; ++i) {
            const GpuChunkBin& bin = event.bins[i];
            tq::hdr::log(
                "Engine trace: GPU chunk frame %u, %s, bin %u,"
                " draws %u-%u (%u, indexed %u, elements %llu),"
                " ps-null %u, srv0-null %u, binding changes %u,"
                " vs %p/%p ps %p/%p srv0 %p/%p vb0 %p/%p ib %p/%p\r\n",
                frame, "reflection RenderLightStyle", i, bin.firstDraw,
                bin.firstDraw + (bin.draws ? bin.draws - 1 : 0), bin.draws,
                bin.indexedDraws, bin.elements, bin.pixelShaderNullDraws,
                bin.resource0NullDraws, bin.bindingChanges,
                bin.firstVertexShader, bin.lastVertexShader,
                bin.firstPixelShader, bin.lastPixelShader,
                bin.firstResource0, bin.lastResource0,
                bin.firstVertexBuffer0, bin.lastVertexBuffer0,
                bin.firstIndexBuffer, bin.lastIndexBuffer);
            unsigned classCalls[GpuChunkMeshInstance + 1] = {};
            unsigned classDraws[GpuChunkMeshInstance + 1] = {};
            const unsigned binLast = bin.firstDraw
                + (bin.draws ? bin.draws - 1 : 0);
            for (unsigned j = 0;
                 j < event.renderableCalls
                     && j < kGpuChunkRenderableCallSlots; ++j) {
                const GpuChunkRenderableCall& call = event.renderables[j];
                if (call.kind <= GpuChunkRenderableNone
                    || call.kind > GpuChunkMeshInstance
                    || !bin.draws || call.lastDraw < bin.firstDraw
                    || call.firstDraw > binLast)
                    continue;
                const unsigned first = call.firstDraw > bin.firstDraw
                    ? call.firstDraw : bin.firstDraw;
                const unsigned last = call.lastDraw < binLast
                    ? call.lastDraw : binLast;
                ++classCalls[call.kind];
                classDraws[call.kind] += last - first + 1;
            }
            tq::hdr::log(
                "Engine trace: GPU chunk classes frame %u, bin %u,"
                " TerrainPlug %u/%u, TerrainBlock %u/%u,"
                " GraphicsMeshInstance %u/%u (calls/draws)\r\n",
                frame, i,
                classCalls[GpuChunkTerrainPlug],
                classDraws[GpuChunkTerrainPlug],
                classCalls[GpuChunkTerrainBlock],
                classDraws[GpuChunkTerrainBlock],
                classCalls[GpuChunkMeshInstance],
                classDraws[GpuChunkMeshInstance]);
        }
        tq::hdr::log(
            "Engine trace: reflection renderable calls frame %u, retained %u,"
            " overflow %u\r\n",
            frame, event.renderableCalls,
            event.renderableCallOverflow ? 1u : 0u);
        unsigned kindCalls[GpuChunkMeshInstance + 1] = {};
        unsigned kindDraws[GpuChunkMeshInstance + 1] = {};
        unsigned kindCpuUs[GpuChunkMeshInstance + 1] = {};
        unsigned kindResourceCount[GpuChunkMeshInstance + 1] = {};
        unsigned kindResourceUs[GpuChunkMeshInstance + 1] = {};
        unsigned hotCalls = 0;
        for (unsigned i = 0;
             i < event.renderableCalls
                 && i < kGpuChunkRenderableCallSlots; ++i) {
            const GpuChunkRenderableCall& call = event.renderables[i];
            const unsigned draws = call.lastDraw >= call.firstDraw
                ? call.lastDraw - call.firstDraw + 1 : 0;
            if (call.kind > GpuChunkRenderableNone
                && call.kind <= GpuChunkMeshInstance) {
                ++kindCalls[call.kind];
                kindDraws[call.kind] += draws;
                kindCpuUs[call.kind] += call.cpuUs;
                kindResourceCount[call.kind] += call.resourceCount;
                kindResourceUs[call.kind] += call.resourceUs;
            }
            if (!call.resourceCount && !call.textureCount
                && !call.bufferCount
                && call.cpuUs < kGpuChunkRenderableHotCpuUs)
                continue;
            ++hotCalls;
            tq::hdr::log(
                "Engine trace: reflection hot renderable frame %u, call %u,"
                " %s self %p, draws %u-%u (%u), cpu %u us, Resource %u/%u"
                " us, texture create %u/%u us, buffer create %u/%u us,"
                " TerrainType %p material %d\r\n",
                frame, i, gpuChunkRenderableName(call.kind), call.object,
                call.firstDraw, call.lastDraw, draws, call.cpuUs,
                call.resourceCount, call.resourceUs,
                call.textureCount, call.textureUs,
                call.bufferCount, call.bufferUs,
                call.terrainType, call.materialIndex);
        }
        for (unsigned kind = GpuChunkTerrainPlug;
             kind <= GpuChunkMeshInstance; ++kind) {
            tq::hdr::log(
                "Engine trace: reflection renderable class frame %u, %s,"
                " calls %u, draws %u, cpu %u us, Resource %u/%u us\r\n",
                frame,
                gpuChunkRenderableName((GpuChunkRenderableKind)kind),
                kindCalls[kind], kindDraws[kind], kindCpuUs[kind],
                kindResourceCount[kind], kindResourceUs[kind]);
        }
        tq::hdr::log(
            "Engine trace: reflection renderable frame %u emitted %u hot"
            " calls; cheap calls represented by class totals\r\n",
            frame, hotCalls);
    }
}

void countReflectionResource(unsigned elapsedUs) {
    const ReflectionLocation location = currentReflectionLocation();
    if (!location.manager) return;
    tq::probe::engineCount(tq::probe::CounterEngineReflectionManagerResLoad);
    tq::probe::engineCount(
        tq::probe::CounterEngineReflectionManagerResLoadUs, elapsedUs);
    if (location.cell <= ReflectionCellNone
        || location.cell >= ReflectionCellCount) return;
    const ReflectionCellCounters& counters =
        kReflectionCellCounters[location.cell];
    tq::probe::engineCount(counters.resourceCount);
    tq::probe::engineCount(counters.resourceUs, elapsedUs);
}

void countReflectionCreation(bool texture, unsigned elapsedUs) {
    noteGpuChunkRenderableCreation(texture, elapsedUs);
    const ReflectionLocation location = currentReflectionLocation();
    if (!location.manager) return;
    tq::probe::engineCount(
        texture ? tq::probe::CounterEngineReflectionManagerTexCreate
                : tq::probe::CounterEngineReflectionManagerBufCreate);
    tq::probe::engineCount(
        texture ? tq::probe::CounterEngineReflectionManagerTexCreateUs
                : tq::probe::CounterEngineReflectionManagerBufCreateUs,
        elapsedUs);
    if (location.cell <= ReflectionCellNone
        || location.cell >= ReflectionCellCount) return;
    const ReflectionCellCounters& counters =
        kReflectionCellCounters[location.cell];
    tq::probe::engineCount(texture ? counters.textureCount
                                   : counters.bufferCount);
    tq::probe::engineCount(texture ? counters.textureUs : counters.bufferUs,
                           elapsedUs);
}

void countReflectionDraw(unsigned elapsedUs) {
    if (!elapsedUs) return;
    const ReflectionLocation location = currentReflectionLocation();
    if (!location.manager) return;
    tq::probe::engineCount(
        tq::probe::CounterEngineReflectionManagerDrawUs, elapsedUs);
    if (location.manager <= 2)
        tq::probe::engineCount(
            kReflectionManagerDrawCounters[location.manager], elapsedUs);
    if (location.cell <= ReflectionCellNone
        || location.cell >= ReflectionCellCount) return;
    tq::probe::engineCount(kReflectionCellCounters[location.cell].drawUs,
                           elapsedUs);
}

DeferredGeometryCell deferredGeometryCell(unsigned invocation,
                                          DeferredGeometrySite site) {
    if (invocation == 1 && site == DeferredGeometrySiteSetup)
        return DeferredGeometryCellI1Setup;
    if (invocation == 1 && site == DeferredGeometrySiteScene)
        return DeferredGeometryCellI1Scene;
    if (invocation == 2 && site == DeferredGeometrySiteSetup)
        return DeferredGeometryCellI2Setup;
    if (invocation == 2 && site == DeferredGeometrySiteScene)
        return DeferredGeometryCellI2Scene;
    return DeferredGeometryCellNone;
}

DeferredOwnerBin deferredOwnerBin(unsigned invocation,
                                  DeferredGeometrySite site) {
    if (invocation == 1) {
        if (site == DeferredGeometrySiteSetup)
            return DeferredOwnerBinI1GeometrySetup;
        if (site == DeferredGeometrySiteScene)
            return DeferredOwnerBinI1GeometryScene;
        return DeferredOwnerBinI1Other;
    }
    if (invocation == 2) {
        if (site == DeferredGeometrySiteSetup)
            return DeferredOwnerBinI2GeometrySetup;
        if (site == DeferredGeometrySiteScene)
            return DeferredOwnerBinI2GeometryScene;
        return DeferredOwnerBinI2Other;
    }
    return DeferredOwnerBinNone;
}

struct DeferredLocation {
    unsigned invocation;
    DeferredPass pass;
    DeferredGeometrySite site;
    DeferredOwnerBin bin;
};

DeferredLocation currentDeferredLocation() {
    DeferredLocation result = {};
    if (!g_deferredPassTracing || !onMainThread()) return result;
    const LONG invocation = InterlockedCompareExchange(
        &g_deferredOwnerInvocation, 0, 0);
    const LONG pass = InterlockedCompareExchange(&g_deferredPass, 0, 0);
    const LONG site = InterlockedCompareExchange(&g_deferredGeometrySite, 0, 0);
    if (invocation <= 0) return result;
    result.invocation = (unsigned)invocation;
    result.pass = pass > DeferredPassNone && pass < DeferredPassCount
        ? (DeferredPass)pass : DeferredPassNone;
    result.site = site > DeferredGeometrySiteNone
               && site < DeferredGeometrySiteCount
        ? (DeferredGeometrySite)site : DeferredGeometrySiteNone;
    result.bin = deferredOwnerBin(result.invocation, result.site);
    return result;
}

struct AdmissionConsumerCounters {
    tq::probe::Counter draw;
    tq::probe::Counter renderable[GpuChunkMeshInstance + 1];
    tq::probe::Counter first[GpuChunkMeshInstance + 1];
};

const AdmissionConsumerCounters
    kAdmissionConsumerCounters[AdmissionConsumerCount] = {
    {},
    {tq::probe::CounterEngineAdmissionReflectionI2P1Draw,
     {tq::probe::CounterCount,
      tq::probe::CounterEngineAdmissionReflectionI2P1TerrainPlug,
      tq::probe::CounterEngineAdmissionReflectionI2P1TerrainBlock,
      tq::probe::CounterEngineAdmissionReflectionI2P1Mesh},
     {tq::probe::CounterCount,
      tq::probe::CounterEngineAdmissionReflectionI2P1TerrainPlugFirst,
      tq::probe::CounterEngineAdmissionReflectionI2P1TerrainBlockFirst,
      tq::probe::CounterEngineAdmissionReflectionI2P1MeshFirst}},
    {tq::probe::CounterEngineAdmissionDeferredI2SetupDraw,
     {tq::probe::CounterCount,
      tq::probe::CounterEngineAdmissionDeferredI2SetupTerrainPlug,
      tq::probe::CounterEngineAdmissionDeferredI2SetupTerrainBlock,
      tq::probe::CounterEngineAdmissionDeferredI2SetupMesh},
     {tq::probe::CounterCount,
      tq::probe::CounterEngineAdmissionDeferredI2SetupTerrainPlugFirst,
      tq::probe::CounterEngineAdmissionDeferredI2SetupTerrainBlockFirst,
      tq::probe::CounterEngineAdmissionDeferredI2SetupMeshFirst}},
    {tq::probe::CounterEngineAdmissionDeferredI2SceneDraw,
     {tq::probe::CounterCount,
      tq::probe::CounterEngineAdmissionDeferredI2SceneTerrainPlug,
      tq::probe::CounterEngineAdmissionDeferredI2SceneTerrainBlock,
      tq::probe::CounterEngineAdmissionDeferredI2SceneMesh},
     {tq::probe::CounterCount,
      tq::probe::CounterEngineAdmissionDeferredI2SceneTerrainPlugFirst,
      tq::probe::CounterEngineAdmissionDeferredI2SceneTerrainBlockFirst,
      tq::probe::CounterEngineAdmissionDeferredI2SceneMeshFirst}},
    {tq::probe::CounterEngineAdmissionShadowDirectionalDraw,
     {tq::probe::CounterCount,
      tq::probe::CounterEngineAdmissionShadowDirectionalTerrainPlug,
      tq::probe::CounterEngineAdmissionShadowDirectionalTerrainBlock,
      tq::probe::CounterEngineAdmissionShadowDirectionalMesh},
     {tq::probe::CounterCount,
      tq::probe::CounterEngineAdmissionShadowDirectionalTerrainPlugFirst,
      tq::probe::CounterEngineAdmissionShadowDirectionalTerrainBlockFirst,
      tq::probe::CounterEngineAdmissionShadowDirectionalMeshFirst}}
};

void countSecondaryAdmission(SecondaryAdmissionContext context,
                             bool admitted) {
    TQ_ENGINE_PROBE_ENTER();
    const tq::probe::Counter counter =
        context == SecondaryAdmissionContextReflection
            ? (admitted
                ? tq::probe::CounterEngineSecondaryAdmissionReflectionAdmitted
                : tq::probe::CounterEngineSecondaryAdmissionReflectionDeferred)
            : (admitted
                ? tq::probe::CounterEngineSecondaryAdmissionShadowAdmitted
                : tq::probe::CounterEngineSecondaryAdmissionShadowDeferred);
    tq::probe::engineCount(counter);
}

AdmissionConsumer currentAdmissionConsumer() {
    if (!g_tracing || !onMainThread()) return AdmissionConsumerNone;
    const ReflectionLocation reflection = currentReflectionLocation();
    if (reflection.cell == ReflectionCellI2P1)
        return AdmissionConsumerReflectionI2P1;
    if (InterlockedCompareExchange(&g_insideDirectional, 0, 0) > 0)
        return AdmissionConsumerShadowDirectional;
    const DeferredLocation deferred = currentDeferredLocation();
    if (deferred.invocation != 2) return AdmissionConsumerNone;
    if (deferred.site == DeferredGeometrySiteSetup)
        return AdmissionConsumerDeferredI2Setup;
    if (deferred.site == DeferredGeometrySiteScene)
        return AdmissionConsumerDeferredI2Scene;
    return AdmissionConsumerNone;
}

void countAdmissionDraw() {
    const AdmissionConsumer consumer = currentAdmissionConsumer();
    if (consumer <= AdmissionConsumerNone || consumer >= AdmissionConsumerCount)
        return;
    tq::probe::engineCount(kAdmissionConsumerCounters[consumer].draw);
}

void countAdmissionRenderable(GpuChunkRenderableKind kind,
                              const void* object) {
    TQ_ENGINE_PROBE_ENTER();
    const AdmissionConsumer consumer = currentAdmissionConsumer();
    if (consumer <= AdmissionConsumerNone || consumer >= AdmissionConsumerCount
        || kind <= GpuChunkRenderableNone || kind > GpuChunkMeshInstance)
        return;
    const AdmissionConsumerCounters& counters =
        kAdmissionConsumerCounters[consumer];
    tq::probe::engineCount(counters.renderable[kind]);
    if (admissionRenderableFirst(object, kind, consumer))
        tq::probe::engineCount(counters.first[kind]);
}

const char* deferredPassName(DeferredPass pass) {
    switch (pass) {
    case DeferredPassGeometry: return "geometry";
    case DeferredPassShadows: return "shadows";
    case DeferredPassLighting: return "lighting";
    case DeferredPassResolve: return "resolve";
    case DeferredPassLateScene: return "late";
    case DeferredPassPost: return "post";
    default: return "gap";
    }
}

const char* deferredSiteName(DeferredGeometrySite site) {
    return site == DeferredGeometrySiteSetup ? "gsetup"
         : site == DeferredGeometrySiteScene ? "gscene" : "none";
}

void countDeferredOwnerResource(unsigned elapsedUs) {
    const DeferredLocation location = currentDeferredLocation();
    if (location.bin <= DeferredOwnerBinNone
        || location.bin >= DeferredOwnerBinCount) return;
    const DeferredOwnerBinCounters& counters =
        kDeferredOwnerBinCounters[location.bin];
    tq::probe::engineCount(counters.resourceCount);
    tq::probe::engineCount(counters.resourceUs, elapsedUs);
}

enum CrossPassFamily {
    CrossPassNone = 0,
    CrossPassReflection = 1,
    CrossPassShadow = 2,
    CrossPassDeferred = 4
};
const unsigned kCrossPassIndexProbe = 16;
const unsigned kCrossPassFreshFrames = 120;
const unsigned kCrossPassMarkerReportLimit = 128;
CrossPassBufferRecord g_crossPassBuffers[kCrossPassBufferSlots];
CrossPassIndexEntry g_crossPassIndex[kCrossPassIndexSlots];
unsigned g_crossPassBufferSequence;
unsigned g_crossPassIndexOverflows;
unsigned g_crossPassRecentEvictions;

const void* const kCrossPassIndexTombstone = (const void*)(uintptr_t)1;

unsigned crossPassIndexStart(const void* object) {
    uintptr_t value = (uintptr_t)object;
    value ^= value >> 13;
    value ^= value >> 7;
    return (unsigned)((value >> 3) & (kCrossPassIndexSlots - 1));
}

void removeCrossPassIndex(const void* object, unsigned sequence) {
    if (!object || object == kCrossPassIndexTombstone) return;
    const unsigned start = crossPassIndexStart(object);
    for (unsigned i = 0; i < kCrossPassIndexProbe; ++i) {
        CrossPassIndexEntry& entry =
            g_crossPassIndex[(start + i) & (kCrossPassIndexSlots - 1)];
        if (!entry.object) return;
        if (entry.object == object && entry.sequence == sequence) {
            entry.object = kCrossPassIndexTombstone;
            entry.sequence = 0;
            return;
        }
    }
}

bool insertCrossPassIndex(const void* object, unsigned sequence) {
    if (!object || object == kCrossPassIndexTombstone) return false;
    const unsigned start = crossPassIndexStart(object);
    unsigned spare = kCrossPassIndexSlots;
    for (unsigned i = 0; i < kCrossPassIndexProbe; ++i) {
        const unsigned at = (start + i) & (kCrossPassIndexSlots - 1);
        CrossPassIndexEntry& entry = g_crossPassIndex[at];
        if (entry.object == object) {
            entry.sequence = sequence;
            return true;
        }
        if ((!entry.object || entry.object == kCrossPassIndexTombstone)
            && spare == kCrossPassIndexSlots)
            spare = at;
        if (!entry.object) break;
    }
    if (spare == kCrossPassIndexSlots) return false;
    g_crossPassIndex[spare].object = object;
    g_crossPassIndex[spare].sequence = sequence;
    return true;
}

void noteCrossPassBufferCreated(const void* object, unsigned byteWidth,
                                unsigned bindFlags) {
    if (!g_crossPassTracing || !object || !onMainThread()) return;
    const unsigned frame = tq::probe::currentFrameIndex();
    const unsigned sequence = ++g_crossPassBufferSequence;
    CrossPassBufferRecord& record =
        g_crossPassBuffers[(sequence - 1) % kCrossPassBufferSlots];
    if (record.object && frame >= record.createdFrame
        && frame - record.createdFrame <= kCrossPassFreshFrames) {
        ++g_crossPassRecentEvictions;
        tq::probe::engineCount(
            tq::probe::CounterEngineCrossPassRecentEviction);
    }
    removeCrossPassIndex(record.object, record.sequence);
    memset(&record, 0, sizeof(record));
    record.object = object;
    record.sequence = sequence;
    record.createdFrame = frame;
    record.byteWidth = byteWidth;
    record.bindFlags = bindFlags;
    const ReflectionLocation reflection = currentReflectionLocation();
    record.createdReflectionManager = reflection.manager;
    record.createdReflectionPlane = reflection.plane;
    const DeferredLocation deferred = currentDeferredLocation();
    record.createdDeferredInvocation = deferred.invocation;
    record.createdDeferredPass = deferred.pass;
    record.createdDeferredSite = deferred.site;
    tq::probe::engineCount(tq::probe::CounterEngineCrossPassBufferCreated);
    tq::probe::engineCount(
        tq::probe::CounterEngineCrossPassBufferCreatedBytes, byteWidth);
    if (!insertCrossPassIndex(object, sequence)) {
        ++g_crossPassIndexOverflows;
        tq::probe::engineCount(
            tq::probe::CounterEngineCrossPassIndexOverflow);
    }
}
DeferredCreationRecord g_deferredCreations[kDeferredCreationSlots];
unsigned g_deferredCreationSequence;
const unsigned kOffMainTextureMarkerFrames = 120;
const unsigned kOffMainTextureReportLimit = 192;
OffMainTextureRecord g_offMainTextures[kOffMainTextureSlots];
volatile LONG g_offMainTextureSequence;

void reportOffMainTexturesAtMarker() {
    if (!g_deferredPassTracing) return;
    const unsigned marker = tq::probe::currentFrameIndex();
    const LONG snapshot = InterlockedCompareExchange(
        &g_offMainTextureSequence, 0, 0);
    const LONG first = snapshot > (LONG)kOffMainTextureSlots
        ? snapshot - (LONG)kOffMainTextureSlots + 1 : 1;
    unsigned retained = 0;
    unsigned elapsedUs = 0;
    unsigned crossedFrames = 0;
    for (LONG sequence = first; sequence <= snapshot; ++sequence) {
        const OffMainTextureRecord& record =
            g_offMainTextures[(sequence - 1) % kOffMainTextureSlots];
        if (InterlockedCompareExchange(
                const_cast<volatile LONG*>(&record.publishedSequence), 0, 0)
                != sequence)
            continue;
        if (record.finishFrame > marker
            || marker - record.finishFrame > kOffMainTextureMarkerFrames)
            continue;
        ++retained;
        elapsedUs += record.elapsedUs;
        if (record.startFrame != record.finishFrame) ++crossedFrames;
    }
    tq::hdr::log(
        "Engine trace: F12 frame %u retained %u off-main texture creations"
        " / %u us from the preceding %u frames; crossed-frame %u,"
        " report limit %u\r\n",
        marker, retained, elapsedUs, kOffMainTextureMarkerFrames,
        crossedFrames, kOffMainTextureReportLimit);
    unsigned emitted = 0;
    for (LONG sequence = first; sequence <= snapshot; ++sequence) {
        const OffMainTextureRecord& record =
            g_offMainTextures[(sequence - 1) % kOffMainTextureSlots];
        if (InterlockedCompareExchange(
                const_cast<volatile LONG*>(&record.publishedSequence), 0, 0)
                != sequence)
            continue;
        if (record.finishFrame > marker
            || marker - record.finishFrame > kOffMainTextureMarkerFrames)
            continue;
        if (emitted >= kOffMainTextureReportLimit) continue;
        tq::hdr::log(
            "Engine trace: off-main texture seq %ld, frames %u-%u, thread %u,"
            " %u us, %ux%u mips %u fmt %u bind %#x misc %#x initial %u\r\n",
            sequence, record.startFrame, record.finishFrame, record.threadId,
            record.elapsedUs, record.width, record.height, record.mipLevels,
            record.format, record.bindFlags, record.miscFlags,
            record.hasInitialData ? 1u : 0u);
        ++emitted;
    }
    tq::hdr::log(
        "Engine trace: F12 frame %u emitted %u off-main texture records;"
        " omitted %u\r\n",
        marker, emitted, retained - emitted);
}

void noteDeferredCreationInternal(DeferredCreationKind kind,
                                  const void* object, unsigned elapsedUs,
                                  unsigned a, unsigned b, unsigned c,
                                  unsigned d, unsigned e, unsigned f) {
    if (object)
        countReflectionCreation(kind == DeferredCreationTexture, elapsedUs);
    if (kind == DeferredCreationBuffer)
        noteCrossPassBufferCreated(object, a, b);
    const DeferredLocation location = currentDeferredLocation();
    if (!object || location.bin <= DeferredOwnerBinNone
        || location.bin >= DeferredOwnerBinCount) return;
    const DeferredOwnerBinCounters& counters =
        kDeferredOwnerBinCounters[location.bin];
    tq::probe::engineCount(kind == DeferredCreationTexture
                               ? counters.textureCount : counters.bufferCount);
    tq::probe::engineCount(kind == DeferredCreationTexture
                               ? counters.textureUs : counters.bufferUs,
                           elapsedUs);
    DeferredCreationRecord& record =
        g_deferredCreations[g_deferredCreationSequence++
                            % kDeferredCreationSlots];
    record.object = object;
    record.frame = tq::probe::currentFrameIndex();
    record.invocation = location.invocation;
    record.pass = location.pass;
    record.site = location.site;
    record.kind = kind;
    record.elapsedUs = elapsedUs;
    record.a = a; record.b = b; record.c = c; record.d = d; record.e = e;
    record.f = f;
}

const DeferredCreationRecord* findDeferredBufferCreation(const void* object,
                                                          unsigned atFrame) {
    if (!object) return nullptr;
    const unsigned count = g_deferredCreationSequence < kDeferredCreationSlots
        ? g_deferredCreationSequence : kDeferredCreationSlots;
    for (unsigned back = 0; back < count; ++back) {
        const DeferredCreationRecord& record = g_deferredCreations[
            (g_deferredCreationSequence - 1 - back) % kDeferredCreationSlots];
        if (record.kind == DeferredCreationBuffer && record.object == object
            && record.frame <= atFrame)
            return &record;
    }
    return nullptr;
}

CrossPassBufferRecord* findCrossPassBuffer(const void* object,
                                            unsigned atFrame) {
    if (!object || object == kCrossPassIndexTombstone) return nullptr;
    const unsigned start = crossPassIndexStart(object);
    for (unsigned i = 0; i < kCrossPassIndexProbe; ++i) {
        const CrossPassIndexEntry& entry =
            g_crossPassIndex[(start + i) & (kCrossPassIndexSlots - 1)];
        if (!entry.object) return nullptr;
        if (entry.object != object) continue;
        CrossPassBufferRecord& record = g_crossPassBuffers[
            (entry.sequence - 1) % kCrossPassBufferSlots];
        if (record.object != object || record.sequence != entry.sequence
            || record.createdFrame > atFrame
            || atFrame - record.createdFrame > kCrossPassFreshFrames)
            return nullptr;
        return &record;
    }
    return nullptr;
}

void countCrossPassDraw(
    const tq::engineprobe::DeferredDrawBindings* bindings) {
    if (!g_crossPassTracing || !bindings || !onMainThread()) return;
    const ReflectionLocation reflection = currentReflectionLocation();
    const bool directional =
        InterlockedCompareExchange(&g_insideDirectional, 0, 0) > 0;
    const DeferredLocation deferred = currentDeferredLocation();
    CrossPassFamily family = CrossPassNone;
    if (reflection.cell > ReflectionCellNone
        && reflection.cell < ReflectionCellCount)
        family = CrossPassReflection;
    else if (directional)
        family = CrossPassShadow;
    else if (deferred.invocation)
        family = CrossPassDeferred;
    if (family == CrossPassNone) return;

    tq::probe::engineCount(
        family == CrossPassReflection
            ? tq::probe::CounterEngineCrossPassReflectionDraw
        : family == CrossPassShadow
            ? tq::probe::CounterEngineCrossPassShadowDraw
            : tq::probe::CounterEngineCrossPassDeferredDraw);
    if (family == CrossPassShadow)
        tq::probe::engineCount(
            tq::probe::CounterEngineShadowDirectionalDraw);

    const void* objects[tq::engineprobe::DeferredTraceVertexBufferSlots + 1];
    unsigned objectCount = 0;
    for (unsigned i = 0;
         i < tq::engineprobe::DeferredTraceVertexBufferSlots; ++i) {
        const void* object = bindings->vertexBuffers[i];
        if (!object) continue;
        bool duplicate = false;
        for (unsigned j = 0; j < objectCount; ++j)
            duplicate |= objects[j] == object;
        if (!duplicate) objects[objectCount++] = object;
    }
    if (bindings->indexBuffer) {
        bool duplicate = false;
        for (unsigned j = 0; j < objectCount; ++j)
            duplicate |= objects[j] == bindings->indexBuffer;
        if (!duplicate) objects[objectCount++] = bindings->indexBuffer;
    }

    const unsigned frame = tq::probe::currentFrameIndex();
    for (unsigned i = 0; i < objectCount; ++i) {
        CrossPassBufferRecord* record =
            findCrossPassBuffer(objects[i], frame);
        if (!record) continue;
        const unsigned before = record->useMask;
        if (!(before & family)) {
            tq::probe::engineCount(
                family == CrossPassReflection
                    ? tq::probe::CounterEngineCrossPassFreshReflectionBuffer
                : family == CrossPassShadow
                    ? tq::probe::CounterEngineCrossPassFreshShadowBuffer
                    : tq::probe::CounterEngineCrossPassFreshDeferredBuffer);
            record->useMask |= family;
            if (family == CrossPassReflection) {
                record->reflectionFirstFrame = frame;
                record->reflectionManager = reflection.manager;
                record->reflectionPlane = reflection.plane;
            } else if (family == CrossPassShadow) {
                record->shadowFirstFrame = frame;
            } else {
                record->deferredFirstFrame = frame;
                record->deferredInvocation = deferred.invocation;
                record->deferredPass = deferred.pass;
                record->deferredSite = deferred.site;
            }
        }
        if (family == CrossPassReflection) ++record->reflectionDraws;
        else if (family == CrossPassShadow) ++record->shadowDraws;
        else ++record->deferredDraws;

        const unsigned after = record->useMask;
#define TQ_COUNT_CROSS_JOIN(mask, counter) \
        if ((after & (mask)) == (mask) && (before & (mask)) != (mask)) \
            tq::probe::engineCount(tq::probe::counter)
        TQ_COUNT_CROSS_JOIN(
            CrossPassReflection | CrossPassShadow,
            CounterEngineCrossPassJoinReflectionShadow);
        TQ_COUNT_CROSS_JOIN(
            CrossPassReflection | CrossPassDeferred,
            CounterEngineCrossPassJoinReflectionDeferred);
        TQ_COUNT_CROSS_JOIN(
            CrossPassShadow | CrossPassDeferred,
            CounterEngineCrossPassJoinShadowDeferred);
        TQ_COUNT_CROSS_JOIN(
            CrossPassReflection | CrossPassShadow | CrossPassDeferred,
            CounterEngineCrossPassJoinAllThree);
#undef TQ_COUNT_CROSS_JOIN
    }
}

void reportCrossPassBuffersAtMarker() {
    if (!g_crossPassTracing) return;
    const unsigned marker = tq::probe::currentFrameIndex();
    const unsigned count = g_crossPassBufferSequence < kCrossPassBufferSlots
        ? g_crossPassBufferSequence : kCrossPassBufferSlots;
    unsigned retained = 0, joined = 0, allThree = 0, emitted = 0;
    for (unsigned back = 0; back < count; ++back) {
        const CrossPassBufferRecord& record = g_crossPassBuffers[
            (g_crossPassBufferSequence - 1 - back) % kCrossPassBufferSlots];
        if (!record.object || record.createdFrame > marker
            || marker - record.createdFrame > kCrossPassFreshFrames)
            continue;
        ++retained;
        const bool multi = record.useMask == 3 || record.useMask == 5
                        || record.useMask == 6 || record.useMask == 7;
        if (!multi) continue;
        ++joined;
        if (record.useMask == 7) ++allThree;
        if (emitted >= kCrossPassMarkerReportLimit) continue;
        tq::hdr::log(
            "Engine trace: cross-pass buffer %p created f%u %uB bind %#x"
            " reflection i%u/p%u deferred i%u/%s/%s; uses %#x:"
            " reflection f%u i%u/p%u draws%u, shadow f%u draws%u,"
            " deferred f%u i%u/%s/%s draws%u\r\n",
            record.object, record.createdFrame, record.byteWidth,
            record.bindFlags, record.createdReflectionManager,
            record.createdReflectionPlane, record.createdDeferredInvocation,
            deferredPassName(record.createdDeferredPass),
            deferredSiteName(record.createdDeferredSite), record.useMask,
            record.reflectionFirstFrame, record.reflectionManager,
            record.reflectionPlane, record.reflectionDraws,
            record.shadowFirstFrame, record.shadowDraws,
            record.deferredFirstFrame, record.deferredInvocation,
            deferredPassName(record.deferredPass),
            deferredSiteName(record.deferredSite), record.deferredDraws);
        ++emitted;
    }
    tq::hdr::log(
        "Engine trace: F12 frame %u cross-pass window %u frames: retained"
        " %u created buffers, joined %u, all-three %u, emitted %u, omitted"
        " %u, index overflows %u, recent ring evictions %u, ring capacity"
        " %u\r\n",
        marker, kCrossPassFreshFrames, retained, joined, allThree, emitted,
        joined - emitted, g_crossPassIndexOverflows,
        g_crossPassRecentEvictions,
        kCrossPassBufferSlots);
}
const unsigned kDeferredSlowMarkerFrames = 120;
const unsigned kDeferredSlowReportFrames = 8;
const unsigned kDeferredSlowFrameMinUs = 15000;
DeferredSlowFrame g_deferredSlowFrames[kDeferredSlowFrameSlots];

void rememberDeferredDraw(unsigned elapsedUs, bool indexed, unsigned count,
                          unsigned start, int base,
                          unsigned invocation, DeferredGeometrySite site,
                          const tq::engineprobe::DeferredDrawBindings* bindings) {
    if (!bindings) return;
    const unsigned frame = tq::probe::currentFrameIndex();
    DeferredSlowFrame& slot =
        g_deferredSlowFrames[frame % kDeferredSlowFrameSlots];
    if (slot.framePlusOne != frame + 1) {
        memset(&slot, 0, sizeof(slot));
        slot.framePlusOne = frame + 1;
    }
    if (UINT_MAX - slot.drawUs < elapsedUs) slot.drawUs = UINT_MAX;
    else slot.drawUs += elapsedUs;
    const unsigned ordinal = ++slot.drawCount;
    if (slot.recordCount == kDeferredTopDrawsPerFrame
        && elapsedUs <= slot.records[slot.recordCount - 1].elapsedUs)
        return;
    unsigned insert = slot.recordCount;
    if (insert < kDeferredTopDrawsPerFrame) ++slot.recordCount;
    else insert = kDeferredTopDrawsPerFrame - 1;
    while (insert && elapsedUs > slot.records[insert - 1].elapsedUs) {
        if (insert < kDeferredTopDrawsPerFrame)
            slot.records[insert] = slot.records[insert - 1];
        --insert;
    }
    DeferredSlowDrawRecord& record = slot.records[insert];
    record.elapsedUs = elapsedUs;
    record.ordinal = ordinal;
    record.indexed = indexed;
    record.count = count;
    record.start = start;
    record.base = base;
    record.invocation = invocation;
    record.site = site;
    record.bindings = *bindings;
}

void countDeferredDrawInternal(
    unsigned elapsedUs, bool indexed, unsigned count, unsigned start, int base,
    const tq::engineprobe::DeferredDrawBindings* bindings) {
    countCrossPassDraw(bindings);
    countReflectionDraw(elapsedUs);
    countAdmissionDraw();
    if (!g_deferredPassTracing || !elapsedUs) return;
    const LONG pass = InterlockedCompareExchange(&g_deferredPass, 0, 0);
    if (pass <= DeferredPassNone || pass >= DeferredPassCount) return;
    tq::probe::engineCount(kDeferredPassDrawCounters[pass], elapsedUs);
    const unsigned invocation = (unsigned)InterlockedCompareExchange(
        &g_deferredOwnerInvocation, 0, 0);
    const DeferredGeometrySite site = (DeferredGeometrySite)
        InterlockedCompareExchange(&g_deferredGeometrySite, 0, 0);
    const DeferredGeometryCell cell =
        deferredGeometryCell(invocation, site);
    if (cell <= DeferredGeometryCellNone
        || cell >= DeferredGeometryCellCount) return;
    tq::probe::engineCount(kDeferredGeometryDrawCounters[cell], elapsedUs);
    rememberDeferredDraw(elapsedUs, indexed, count, start, base,
                         invocation, site, bindings);
}

void reportDeferredSlowDrawsAtMarker() {
    if (!g_deferredPassTracing) return;
    const unsigned marker = tq::probe::currentFrameIndex();
    const DeferredSlowFrame* selected[kDeferredSlowReportFrames] = {};
    unsigned selectedCount = 0;
    for (unsigned back = 0; back <= kDeferredSlowMarkerFrames
                            && back <= marker; ++back) {
        const unsigned frame = marker - back;
        const DeferredSlowFrame& candidate =
            g_deferredSlowFrames[frame % kDeferredSlowFrameSlots];
        if (candidate.framePlusOne != frame + 1
            || candidate.drawUs < kDeferredSlowFrameMinUs)
            continue;
        unsigned insert = selectedCount;
        if (insert < kDeferredSlowReportFrames) ++selectedCount;
        else if (candidate.drawUs <= selected[insert - 1]->drawUs) continue;
        else insert = kDeferredSlowReportFrames - 1;
        while (insert && candidate.drawUs > selected[insert - 1]->drawUs) {
            if (insert < kDeferredSlowReportFrames)
                selected[insert] = selected[insert - 1];
            --insert;
        }
        selected[insert] = &candidate;
    }

    tq::hdr::log("Engine trace: F12 frame %u retained %u geometry-draw"
                 " frames >= %u us from the preceding %u frames\r\n",
                 marker, selectedCount, kDeferredSlowFrameMinUs,
                 kDeferredSlowMarkerFrames);
    for (unsigned i = 0; i < selectedCount; ++i) {
        const DeferredSlowFrame& frame = *selected[i];
        const unsigned frameIndex = frame.framePlusOne - 1;
        tq::hdr::log("Engine trace: geometry slow frame %u total %u us,"
                     " draws %u, retained top %u\r\n",
                     frameIndex, frame.drawUs, frame.drawCount,
                     frame.recordCount);
        for (unsigned j = 0; j < frame.recordCount; ++j) {
            const DeferredSlowDrawRecord& draw = frame.records[j];
            const DeferredCreationRecord* vb = findDeferredBufferCreation(
                draw.bindings.vertexBuffers[0], frameIndex);
            const DeferredCreationRecord* ib = findDeferredBufferCreation(
                draw.bindings.indexBuffer, frameIndex);
            tq::hdr::log(
                "Engine trace: geometry slow draw frame %u rank %u, %u us,"
                " i%u/%s, %s ordinal %u count %u start %u base %d,"
                " vb0 %p new %d/%uB, vb1 %p, ib %p new %d/%uB/fmt%u,"
                " vs %p ps %p, srv %p/%p/%p/%p/%p/%p/%p/%p\r\n",
                frameIndex, j + 1, draw.elapsedUs, draw.invocation,
                deferredSiteName(draw.site),
                draw.indexed ? "indexed" : "draw", draw.ordinal,
                draw.count, draw.start, draw.base,
                draw.bindings.vertexBuffers[0], vb ? (int)vb->frame : -1,
                vb ? vb->a : 0,
                draw.bindings.vertexBuffers[1], draw.bindings.indexBuffer,
                ib ? (int)ib->frame : -1, ib ? ib->a : 0,
                draw.bindings.indexFormat,
                draw.bindings.vertexShader, draw.bindings.pixelShader,
                draw.bindings.pixelResources[0],
                draw.bindings.pixelResources[1],
                draw.bindings.pixelResources[2],
                draw.bindings.pixelResources[3],
                draw.bindings.pixelResources[4],
                draw.bindings.pixelResources[5],
                draw.bindings.pixelResources[6],
                draw.bindings.pixelResources[7]);
        }

        unsigned textures = 0;
        const unsigned creationCount =
            g_deferredCreationSequence < kDeferredCreationSlots
            ? g_deferredCreationSequence : kDeferredCreationSlots;
        for (unsigned back = 0; back < creationCount && textures < 32;
             ++back) {
            const DeferredCreationRecord& creation = g_deferredCreations[
                (g_deferredCreationSequence - 1 - back)
                % kDeferredCreationSlots];
            if (creation.kind != DeferredCreationTexture
                || creation.frame != frameIndex) continue;
            tq::hdr::log(
                "Engine trace: geometry texture frame %u, %u us,"
                " i%u/%s/%s, object %p, %ux%u mips %u fmt %u"
                " bind %#x misc %#x\r\n",
                frameIndex, creation.elapsedUs, creation.invocation,
                deferredPassName(creation.pass),
                deferredSiteName(creation.site), creation.object,
                creation.a, creation.b, creation.c, creation.d,
                creation.e, creation.f);
            ++textures;
        }
    }
}

struct ReflectionManagerScope {
    LONG priorManager;
    LONG priorPlane;
    unsigned priorPlaneCalls;
    unsigned invocation;
    int64_t started;
    ID3D11DeviceContext* context;
    bool active;

    ReflectionManagerScope()
        : priorManager(0), priorPlane(0), priorPlaneCalls(0), invocation(0),
          started(0), context(nullptr),
          active(g_reflectionTracing && onMainThread()) {
        if (!active) return;
        const unsigned frame = tq::probe::currentFrameIndex();
        if (g_reflectionManagerFrame != frame) {
            g_reflectionManagerFrame = frame;
            g_reflectionManagerCallsThisFrame = 0;
        }
        invocation = ++g_reflectionManagerCallsThisFrame;
        priorManager = InterlockedExchange(
            &g_reflectionManagerInvocation, (LONG)invocation);
        priorPlane = InterlockedExchange(&g_reflectionPlaneInvocation, 0);
        priorPlaneCalls = g_reflectionPlaneCallsThisManager;
        g_reflectionPlaneCallsThisManager = 0;
        started = tq::probe::now();
        if (invocation <= 2) {
            context = tq::probe::currentGpuContext();
            tq::probe::gpuBegin(
                context, kReflectionManagerGpuPhases[invocation]);
        }
    }

    ~ReflectionManagerScope() {
        if (!active) return;
        if (invocation <= 2)
            tq::probe::gpuEnd(
                context, kReflectionManagerGpuPhases[invocation]);
        const unsigned elapsed = tq::probe::microsecondsSince(started);
        tq::probe::engineCount(tq::probe::CounterEngineReflectionManager);
        tq::probe::engineCount(
            tq::probe::CounterEngineReflectionManagerUs, elapsed);
        if (invocation <= 2) {
            tq::probe::engineCount(
                kReflectionManagerCountCounters[invocation]);
            tq::probe::engineCount(
                kReflectionManagerDurationCounters[invocation], elapsed);
        } else {
            tq::probe::engineCount(
                tq::probe::CounterEngineReflectionManagerOverflow);
        }
        g_reflectionPlaneCallsThisManager = priorPlaneCalls;
        InterlockedExchange(&g_reflectionPlaneInvocation, priorPlane);
        InterlockedExchange(&g_reflectionManagerInvocation, priorManager);
    }
};

struct ReflectionPlaneScope {
    LONG priorPlane;
    ReflectionCell cell;
    int64_t started;
    ID3D11DeviceContext* context;
    bool active;

    ReflectionPlaneScope()
        : priorPlane(0), cell(ReflectionCellNone), started(0), context(nullptr),
          active(g_reflectionTracing && onMainThread()
                 && InterlockedCompareExchange(
                        &g_reflectionManagerInvocation, 0, 0) > 0) {
        if (!active) return;
        g_reflectionGpuChunkPending = false;
        g_reflectionGpuChunkTriggerUs = 0;
        const unsigned manager = (unsigned)InterlockedCompareExchange(
            &g_reflectionManagerInvocation, 0, 0);
        const unsigned plane = ++g_reflectionPlaneCallsThisManager;
        priorPlane = InterlockedExchange(
            &g_reflectionPlaneInvocation, (LONG)plane);
        cell = reflectionCell(manager, plane);
        if (cell == ReflectionCellNone)
            tq::probe::engineCount(
                tq::probe::CounterEngineReflectionPlaneOverflow);
        started = tq::probe::now();
        if (cell != ReflectionCellNone) {
            context = tq::probe::currentGpuContext();
            tq::probe::gpuBegin(context, kReflectionCellGpuPhases[cell]);
        }
    }

    ~ReflectionPlaneScope() {
        if (!active) return;
        if (cell != ReflectionCellNone)
            tq::probe::gpuEnd(context, kReflectionCellGpuPhases[cell]);
        const unsigned elapsed = tq::probe::microsecondsSince(started);
        if (cell != ReflectionCellNone) {
            const ReflectionCellCounters& counters =
                kReflectionCellCounters[cell];
            tq::probe::engineCount(counters.count);
            tq::probe::engineCount(counters.durationUs, elapsed);
        }
        InterlockedExchange(&g_reflectionPlaneInvocation, priorPlane);
    }
};

int __fastcall hookReflectionManager(
    void* self, void* edx, uintptr_t canvas, uintptr_t renderSet) {
    ReflectionManagerScope scope;
    return g_reflectionManager
        ? g_reflectionManager(self, edx, canvas, renderSet) : 0;
}

uintptr_t __stdcall hookReflectionPlane(
    uintptr_t record, uintptr_t canvas, uintptr_t renderSet) {
    ReflectionPlaneScope scope;
    return g_reflectionPlane
        ? g_reflectionPlane(record, canvas, renderSet) : 0;
}

void __fastcall hookReflectionBuildScene(
    void* self, void* edx, int includeHidden) {
    const bool countAdmission = (g_reflectionDeferAdmissionMeshActive
        || g_reflectionDeferAdmissionAllActive
        || (g_tracing && tq::probe::drawTimingEnabled()))
        && onMainThread();
    const bool priorBuildActive = g_reflectionAdmissionBuildActive;
    const unsigned priorBuildBuffers = g_reflectionAdmissionBuildBuffers;
    if (countAdmission) {
        g_reflectionAdmissionBuildBuffers = 0;
        g_reflectionAdmissionBuildActive = true;
    }
    {
        ReflectionChildScope scope(ReflectionChildBuildScene);
        if (g_reflectionBuildScene)
            g_reflectionBuildScene(self, edx, includeHidden);
    }
    if (countAdmission) {
        const unsigned buffers = g_reflectionAdmissionBuildBuffers;
        g_reflectionAdmissionBuildActive = priorBuildActive;
        g_reflectionAdmissionBuildBuffers = priorBuildBuffers;
        g_reflectionAdmissionPending =
            reflectionAdmissionThresholdReached(buffers);
        if (g_reflectionAdmissionPending)
            tq::probe::engineCount(
                tq::probe::CounterEngineReflectionAdmissionDeferred);
    }
}

struct DeferredOwnerScope {
    LONG priorInvocation;
    LONG priorPass;
    LONG priorSite;
    bool active;

    DeferredOwnerScope()
        : priorInvocation(0), priorPass(DeferredPassNone),
          priorSite(DeferredGeometrySiteNone),
          active(g_deferredPassTracing && onMainThread()) {
        if (!active) return;
        const unsigned frame = tq::probe::currentFrameIndex();
        if (g_deferredOwnerFrame != frame) {
            g_deferredOwnerFrame = frame;
            g_deferredOwnerCallsThisFrame = 0;
        }
        const unsigned invocation = ++g_deferredOwnerCallsThisFrame;
        priorInvocation = InterlockedExchange(
            &g_deferredOwnerInvocation, (LONG)invocation);
        priorPass = InterlockedExchange(&g_deferredPass, DeferredPassNone);
        priorSite = InterlockedExchange(
            &g_deferredGeometrySite, DeferredGeometrySiteNone);
        tq::probe::engineCount(tq::probe::CounterEngineDeferredOwner);
        if (invocation > 2)
            tq::probe::engineCount(
                tq::probe::CounterEngineDeferredOwnerOverflow);
    }

    ~DeferredOwnerScope() {
        if (!active) return;
        InterlockedExchange(&g_deferredGeometrySite, priorSite);
        InterlockedExchange(&g_deferredPass, priorPass);
        InterlockedExchange(&g_deferredOwnerInvocation, priorInvocation);
    }
};

struct DeferredPassScope {
    DeferredPass pass;
    LONG prior;
    LONG priorSite;
    DeferredGeometryCell cell;
    int64_t started;
    ID3D11DeviceContext* context;
    bool active;

    explicit DeferredPassScope(
        DeferredPass value,
        DeferredGeometrySite geometrySite = DeferredGeometrySiteNone)
        : pass(value), prior(DeferredPassNone),
          priorSite(DeferredGeometrySiteNone),
          cell(DeferredGeometryCellNone), started(0), context(nullptr),
          active(g_deferredPassTracing && onMainThread()
                 && InterlockedCompareExchange(
                        &g_deferredOwnerInvocation, 0, 0) > 0) {
        if (!active) return;
        prior = InterlockedExchange(&g_deferredPass, pass);
        priorSite = InterlockedExchange(&g_deferredGeometrySite, geometrySite);
        const unsigned invocation = (unsigned)InterlockedCompareExchange(
            &g_deferredOwnerInvocation, 0, 0);
        cell = deferredGeometryCell(invocation, geometrySite);
        started = tq::probe::now();
        if (cell != DeferredGeometryCellNone) {
            context = tq::probe::currentGpuContext();
            tq::probe::gpuBegin(context, kDeferredGeometryGpuPhases[cell]);
        }
    }

    ~DeferredPassScope() {
        if (!active) return;
        if (cell != DeferredGeometryCellNone)
            tq::probe::gpuEnd(context, kDeferredGeometryGpuPhases[cell]);
        const unsigned elapsed = tq::probe::microsecondsSince(started);
        tq::probe::engineCount(kDeferredPassCountCounters[pass]);
        tq::probe::engineCount(kDeferredPassDurationCounters[pass], elapsed);
        if (cell != DeferredGeometryCellNone) {
            tq::probe::engineCount(kDeferredGeometryCountCounters[cell]);
            tq::probe::engineCount(kDeferredGeometryDurationCounters[cell],
                                   elapsed);
        }
        InterlockedExchange(&g_deferredGeometrySite, priorSite);
        InterlockedExchange(&g_deferredPass, prior);
    }
};

uintptr_t __fastcall hookDeferredRender(
    void* self, void* edx, uintptr_t a, uintptr_t b, uintptr_t c,
    uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g) {
    DeferredOwnerScope scope;
    return g_deferredRender
        ? g_deferredRender(self, edx, a, b, c, d, e, f, g) : 0;
}

uintptr_t __fastcall hookDeferredGeometrySetup(
    void* self, void* edx, uintptr_t a, uintptr_t b) {
    DeferredPassScope scope(DeferredPassGeometry,
                            DeferredGeometrySiteSetup);
    return g_deferredGeometrySetup
        ? g_deferredGeometrySetup(self, edx, a, b) : 0;
}

uintptr_t __fastcall hookDeferredGeometryScene(
    void* self, void* edx, uintptr_t a, uintptr_t b, uintptr_t c,
    uintptr_t d, uintptr_t e) {
    DeferredPassScope scope(DeferredPassGeometry,
                            DeferredGeometrySiteScene);
    return g_deferredGeometryScene
        ? g_deferredGeometryScene(self, edx, a, b, c, d, e) : 0;
}

uintptr_t __fastcall hookDeferredShadows(
    void* self, void* edx, uintptr_t a, uintptr_t b) {
    DeferredPassScope scope(DeferredPassShadows);
    return g_deferredShadows ? g_deferredShadows(self, edx, a, b) : 0;
}

uintptr_t __fastcall hookDeferredLighting(
    void* self, void* edx, uintptr_t a, uintptr_t b) {
    DeferredPassScope scope(DeferredPassLighting);
    return g_deferredLighting ? g_deferredLighting(self, edx, a, b) : 0;
}

uintptr_t __fastcall hookDeferredResolve(
    void* self, void* edx, uintptr_t a, uintptr_t b, uintptr_t c) {
    DeferredPassScope scope(DeferredPassResolve);
    return g_deferredResolve ? g_deferredResolve(self, edx, a, b, c) : 0;
}

uintptr_t __fastcall hookDeferredAo(
    void* self, void* edx, uintptr_t a) {
    DeferredPassScope scope(DeferredPassResolve);
    return g_deferredAo ? g_deferredAo(self, edx, a) : 0;
}

uintptr_t __fastcall hookDeferredLateSceneA(
    void* self, void* edx, uintptr_t a, uintptr_t b) {
    DeferredPassScope scope(DeferredPassLateScene);
    return g_deferredLateSceneA ? g_deferredLateSceneA(self, edx, a, b) : 0;
}

uintptr_t __fastcall hookDeferredLateSceneB(
    void* self, void* edx, uintptr_t a) {
    DeferredPassScope scope(DeferredPassLateScene);
    return g_deferredLateSceneB ? g_deferredLateSceneB(self, edx, a) : 0;
}

uintptr_t __fastcall hookDeferredLateSceneList(
    void* self, void* edx, uintptr_t a, uintptr_t b, uintptr_t c,
    uintptr_t d, uintptr_t e) {
    DeferredPassScope scope(DeferredPassLateScene);
    return g_deferredLateSceneList
        ? g_deferredLateSceneList(self, edx, a, b, c, d, e) : 0;
}

uintptr_t __fastcall hookDeferredPostHighlight(
    void* self, void* edx, uintptr_t a) {
    DeferredPassScope scope(DeferredPassPost);
    return g_deferredPostHighlight ? g_deferredPostHighlight(self, edx, a) : 0;
}

uintptr_t __fastcall hookDeferredPostFog(
    void* self, void* edx, uintptr_t a, uintptr_t b) {
    DeferredPassScope scope(DeferredPassPost);
    return g_deferredPostFog ? g_deferredPostFog(self, edx, a, b) : 0;
}

uintptr_t __fastcall hookDeferredPostMask(
    void* self, void* edx, uintptr_t a) {
    DeferredPassScope scope(DeferredPassPost);
    return g_deferredPostMask ? g_deferredPostMask(self, edx, a) : 0;
}

uintptr_t __fastcall hookDeferredPostComposite(
    void* self, void* edx, uintptr_t a, uintptr_t b, uintptr_t c,
    uintptr_t d, uintptr_t e) {
    DeferredPassScope scope(DeferredPassPost);
    return g_deferredPostComposite
        ? g_deferredPostComposite(self, edx, a, b, c, d, e) : 0;
}

uintptr_t __fastcall hookDeferredPostDebug(
    void* self, void* edx, uintptr_t a) {
    DeferredPassScope scope(DeferredPassPost);
    return g_deferredPostDebug ? g_deferredPostDebug(self, edx, a) : 0;
}
ShadowMeshParameterContext g_shadowMaterialPendingContext;
const void* g_shadowMaterialPendingTexture;

// The builder already obtains the exact mesh-instance class/style for the
// run-51 omission decision. Keep that result only for this directional call,
// keyed by the instance and pass that the later parameter call exposes. This
// avoids calling back into the engine's mesh/resource path from inside
// GraphicsMesh::SetShaderParameters -- run 52 froze on the first cold event
// when it did that. The generation makes each reset O(1), and values are used
// only for identity comparison while RenderDirectional is still on stack.
const unsigned kShadowRecordContextSlots = 4096;
static_assert((kShadowRecordContextSlots & (kShadowRecordContextSlots - 1)) == 0,
              "shadow record context table must be a power of two");
struct ShadowRecordContextEntry {
    unsigned generation;
    void* instance;
    int pass;
    unsigned style;
    bool styleKnown;
    bool baseKnown;
    const void* baseTexture;
};
ShadowRecordContextEntry g_shadowRecordContexts[kShadowRecordContextSlots];
unsigned g_shadowRecordContextGeneration;
ShadowMeshContextPatchStatus g_shadowMeshContextPatchStatus =
    ShadowMeshContextPatchDependencyMissing;

void resetShadowRecordContexts() {
    if (++g_shadowRecordContextGeneration) return;
    memset(g_shadowRecordContexts, 0, sizeof(g_shadowRecordContexts));
    g_shadowRecordContextGeneration = 1;
}

unsigned shadowRecordContextHash(const void* instance, int pass) {
    return (unsigned)(((uintptr_t)instance >> 4)
        ^ ((unsigned)pass * 0x9e3779b9u));
}

bool rememberShadowRecordContext(void* instance, int pass, unsigned style,
                                 bool styleKnown, bool baseKnown,
                                 const void* baseTexture) {
    if (!instance || !g_shadowRecordContextGeneration) return false;
    unsigned slot = shadowRecordContextHash(instance, pass)
        & (kShadowRecordContextSlots - 1);
    for (unsigned probe = 0; probe < kShadowRecordContextSlots; ++probe) {
        ShadowRecordContextEntry& entry = g_shadowRecordContexts[slot];
        if (entry.generation != g_shadowRecordContextGeneration
            || (entry.instance == instance && entry.pass == pass)) {
            entry.generation = g_shadowRecordContextGeneration;
            entry.instance = instance;
            entry.pass = pass;
            entry.style = style;
            entry.styleKnown = styleKnown;
            entry.baseKnown = baseKnown;
            entry.baseTexture = baseTexture;
            return true;
        }
        slot = (slot + 1) & (kShadowRecordContextSlots - 1);
    }
    tq::probe::engineCount(tq::probe::CounterEngineShadowContextTableOverflow);
    return false;
}

bool findShadowRecordContext(void* instance, int pass,
                             ShadowMeshParameterContext* out) {
    if (!instance || !out || !g_shadowRecordContextGeneration) return false;
    unsigned slot = shadowRecordContextHash(instance, pass)
        & (kShadowRecordContextSlots - 1);
    for (unsigned probe = 0; probe < kShadowRecordContextSlots; ++probe) {
        const ShadowRecordContextEntry& entry = g_shadowRecordContexts[slot];
        if (entry.generation != g_shadowRecordContextGeneration) return false;
        if (entry.instance == instance && entry.pass == pass) {
            out->active = true;
            out->styleKnown = entry.styleKnown;
            out->match = entry.styleKnown
                ? ShadowContextExact : ShadowContextClassOther;
            out->instance = instance;
            out->style = entry.style;
            out->pass = pass;
            out->baseKnown = entry.baseKnown;
            out->baseTexture = entry.baseTexture;
            return true;
        }
        slot = (slot + 1) & (kShadowRecordContextSlots - 1);
    }
    return false;
}

void explainShadowRecordMiss(ShadowMeshParameterContext* context) {
    if (!context || context->active) return;
    // A zero-initialized enum used to spell Exact here even though no adapter
    // had supplied an instance. Run 54 exposed that impossible combination:
    // lookup_exact beside pass_unknown. Missing call context is an explicit
    // instance-missing result, not a successful join.
    if (!context->instance) {
        context->match = ShadowContextInstanceMissing;
        return;
    }
    for (unsigned slot = 0; slot < kShadowRecordContextSlots; ++slot) {
        const ShadowRecordContextEntry& entry = g_shadowRecordContexts[slot];
        if (entry.generation == g_shadowRecordContextGeneration
            && entry.instance == context->instance) {
            context->match = ShadowContextPassMismatch;
            return;
        }
    }
    context->match = ShadowContextInstanceMissing;
}

// Which call site the expensive forced loads actually come from -- the one
// fact runs 27 and 28 left missing, and the reason Stage 5.1 was aimed wrong.
//
// Five `Region::LoadLevel` calls are 99.7% of a session's main-thread level
// loading, and neither `AddElementsInBox` site is one of them: on the
// zone-transition frame the two patched sites are not reached at all. There
// are thirty-eight `E8` sites in Engine.dll that reach this function and
// guessing between them is what produced §27, so this measures it.
//
// `hookLoadLevel` is a trampoline detour, so the return address here is the
// game's. The game's `CALL` pushed it; the six-byte `push imm32; ret` written
// over the entry consumes only its own. Nothing between that and this frame
// pushes another.
//
// A call has to cost a millisecond to be recorded, which is three orders of
// magnitude above the ~3 us the resident path takes, so this is a handful of
// events a session against ~206,000 calls. The cost on every other call is
// one comparison against a value already in a register.
const uint32_t kSlowLoadUs = 1000;
const unsigned kLoadCallerSlots = 16;

// One table per instrumented callee. Run 29 answered "which call site" for
// Region::LoadLevel and the answer was another function, so the same question
// has to be asked one level up; sharing the aggregator means the second
// answer costs a table and a hook rather than a second implementation.
//
// RVA 0 is the DOS header and can never be a call site, so it doubles as the
// empty marker.
struct CallerTable {
    DWORD rva[kLoadCallerSlots];
    LONG calls[kLoadCallerSlots];
    LONG main[kLoadCallerSlots];
    LONG us[kLoadCallerSlots];
    LONG worstUs[kLoadCallerSlots];
    LONG lost;
};
CallerTable g_loadLevelCallers;
CallerTable g_guaranteedCallers;

void recordSlowCall(CallerTable& table, const void* caller, uint32_t elapsed,
                    bool main) {
    if (!g_engineBase) return;
    const uintptr_t address = (uintptr_t)caller;
    const uintptr_t base = (uintptr_t)g_engineBase;
    // A caller outside Engine.dll is a different question from the one being
    // asked, and its RVA would be meaningless, so it is counted and dropped
    // rather than recorded against a wrong module.
    if (address < base || address - base >= kEngineImageSize) {
        InterlockedIncrement(&table.lost);
        return;
    }
    const DWORD rva = (DWORD)(address - base);
    for (unsigned i = 0; i < kLoadCallerSlots; ++i) {
        if (table.rva[i] != rva) {
            if (table.rva[i]) continue;
            // Two threads can reach an empty slot together; the loser either
            // finds its own RVA already there and shares the slot, or moves on.
            if (InterlockedCompareExchange((LONG*)&table.rva[i], (LONG)rva, 0)
                    != 0
                && table.rva[i] != rva)
                continue;
        }
        InterlockedIncrement(&table.calls[i]);
        if (main) InterlockedIncrement(&table.main[i]);
        InterlockedExchangeAdd(&table.us[i], (LONG)elapsed);
        // Deliberately not interlocked. A lost update to a running maximum
        // costs at most one sample, and a maximum is the one statistic that
        // survives that; paying a lock here would be pricing the instrument
        // rather than the load.
        if ((LONG)elapsed > table.worstUs[i])
            table.worstUs[i] = (LONG)elapsed;
        return;
    }
    InterlockedIncrement(&table.lost);
}

// Written from the Engine::Render bracket rather than from shutdown(), for
// the same reason the message histogram is: the game exits without unloading.
// --- The call chain, because hooking one function per boot resolves one link
// per boot and the chain is at least four deep. Runs 29 and 30 walked it from
// Region::LoadLevel up to WorldVec3::TranslateToFloor and it is still not at
// the top; §29 has the reasoning for changing technique here.
//
// The game keeps no frame pointers -- Region::GuaranteedGetLevel opens
// `push esi; mov esi,ecx; push edi` -- so a proper frame walk is not
// available. What replaces it is a raw upward scan of the stack keeping only
// values that look like an address a CALL pushed. Stale slots survive that,
// so the output is a superset; the real chain is a subsequence of it in
// increasing stack order, which three events a session makes readable by eye.
const unsigned kChainSlots = 8;
const unsigned kChainDepth = 32;
// 8 KiB. Engine::Render's own frame is 0x870 bytes, so a chain that reaches
// it needs room for several hundred words of one frame alone.
const unsigned kStackWords = 2048;

// Three modules, not one. Run 31 scanned Engine.dll only and came back with
// both ends of the chain and nothing in between: TQ.exe and Game.dll both
// import WorldVec3::TranslateToFloor, so the frames between Display::Update
// and it were dropped by construction rather than absent. §30.
//
// Each is admitted only if it is the audited build, so a chain frame's RVA
// means the same thing as every other RVA in this file.
const unsigned kChainModules = 3;
struct ChainModule {
    char tag;                  // 'E' Engine.dll, 'G' Game.dll, 'T' TQ.exe
    const BYTE* base;
    const BYTE* text;
    SIZE_T textSize;
};
ChainModule g_chainModules[kChainModules];
unsigned g_chainModuleCount;
struct StackChain {
    LONG ready;
    uint32_t us;
    unsigned depth;
    ChainFrame frame[kChainDepth];
};
StackChain g_chains[kChainSlots];
LONG g_chainsUsed;

const ChainModule* moduleOf(const BYTE* address) {
    for (unsigned i = 0; i < g_chainModuleCount; ++i) {
        const ChainModule& m = g_chainModules[i];
        if (address >= m.text && address < m.text + m.textSize) return &m;
    }
    return nullptr;
}

// Whether `ret` looks like the address a CALL pushed, checked against the
// .text of the module it landed in. All five encodings the engine actually
// uses, because the render path is full of virtual calls and dropping those
// would lose exactly the frames worth having.
//
// Every read is inside that module's .text, which is mapped, so the bound
// check before each read is the whole guard -- no VirtualQuery, which matters
// when this runs a couple of thousand times per event.
bool precededByCall(const BYTE* ret, const ChainModule& m) {
    const BYTE* const begin = m.text;
    const BYTE* const end = m.text + m.textSize;
    if (ret - 6 >= begin) {
        // E8 rel32, with the destination required to land in the same .text.
        // That second half is what makes this the strongest of the five: a
        // stale dword would have to be preceded by a byte that happens to be
        // E8 *and* carry a displacement that happens to point at code.
        if (ret[-5] == 0xe8) {
            int32_t rel = 0;
            memcpy(&rel, ret - 4, sizeof(rel));
            const BYTE* target = ret + rel;
            if (target >= begin && target < end) return true;
        }
        // FF 15 disp32, an import slot; FF 90 disp32, call [reg+disp32].
        if (ret[-6] == 0xff && (ret[-5] == 0x15 || (ret[-5] & 0xf8) == 0x90))
            return true;
    }
    // call [reg+disp8]
    if (ret - 3 >= begin && ret[-3] == 0xff && (ret[-2] & 0xf8) == 0x50)
        return true;
    // call reg, and call [reg] -- excluding the two rm encodings that mean a
    // SIB byte or a disp32 follows, since those are longer forms handled above.
    if (ret - 2 >= begin && ret[-2] == 0xff
        && (((ret[-1] & 0xf8) == 0xd0)
            || ((ret[-1] & 0xf8) == 0x10 && (ret[-1] & 7) != 4
                && (ret[-1] & 7) != 5)))
        return true;
    return false;
}

void captureChain(const void* from, uint32_t us) {
    if (!g_chainModuleCount) return;
    const LONG slot = InterlockedIncrement(&g_chainsUsed) - 1;
    if (slot < 0 || slot >= (LONG)kChainSlots) return;

    // One probe for the whole walk. The stack's committed pages above this
    // frame are a single region -- the guard page is below it, not above --
    // so what this returns is a safe upper bound and the scan needs no
    // further VirtualQuery. Which is the point: readable() is a syscall, and
    // paying one per word would price the instrument above the load.
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(from, &info, sizeof(info)) || info.State != MEM_COMMIT)
        return;
    const uintptr_t* const stack = (const uintptr_t*)from;
    const uintptr_t* const limit =
        (const uintptr_t*)((const BYTE*)info.BaseAddress + info.RegionSize);

    StackChain& chain = g_chains[slot];
    chain.us = us;
    unsigned depth = 0;
    for (unsigned i = 0;
         i < kStackWords && stack + i < limit && depth < kChainDepth; ++i) {
        const BYTE* const value = (const BYTE*)stack[i];
        const ChainModule* const m = moduleOf(value);
        if (!m || !precededByCall(value, *m)) continue;
        const DWORD rva = (DWORD)(value - m->base);
        // Consecutive repeats are one site spilled twice, not two frames.
        if (depth && chain.frame[depth - 1].rva == rva
            && chain.frame[depth - 1].tag == m->tag)
            continue;
        chain.frame[depth].rva = rva;
        chain.frame[depth].tag = m->tag;
        ++depth;
    }
    chain.depth = depth;
    // Published last, so the render thread never reads a half-filled chain.
    InterlockedExchange(&chain.ready, 1);
}

char* appendFrame(char* at, char* const end, const ChainFrame& frame) {
    if (end - at < 14) return at;
    *at++ = ' ';
    *at++ = frame.tag;
    *at++ = '+'; *at++ = '0'; *at++ = 'x';
    bool started = false;
    for (int shift = 28; shift >= 0; shift -= 4) {
        const unsigned digit = (frame.rva >> shift) & 0xf;
        if (!digit && !started && shift) continue;
        started = true;
        *at++ = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
    }
    return at;
}

// The direct-call table cannot name a texture load reached through a function
// pointer or through a stack frame deeper than its deliberately short scan.
// For the first few such loads, write the exact call-shaped stack candidates
// immediately. The HDR logger flushes while the session is alive; Titan Quest
// does not unload this DLL on exit. This path is trace-only and bounded to the
// existing eight chain slots.
void reportUnresolvedShadowTextureChain(const void* from, const char* resource,
                                        uint32_t us) {
    if (!tq::hdr::readSettings().trace || !from || !g_chainModuleCount) return;
    const LONG report = InterlockedIncrement(&g_shadowTextureChainReports) - 1;
    if (report < 0 || report >= (LONG)kChainSlots) return;

    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(from, &info, sizeof(info)) || info.State != MEM_COMMIT)
        return;
    const uintptr_t* const stack = (const uintptr_t*)from;
    const uintptr_t* const limit =
        (const uintptr_t*)((const BYTE*)info.BaseAddress + info.RegionSize);
    ChainFrame frames[kChainDepth];
    unsigned depth = 0;
    for (unsigned i = 0;
         i < kStackWords && stack + i < limit && depth < kChainDepth; ++i) {
        const BYTE* const value = (const BYTE*)stack[i];
        const ChainModule* const module = moduleOf(value);
        if (!module || !precededByCall(value, *module)) continue;
        const DWORD rva = (DWORD)(value - module->base);
        if (depth && frames[depth - 1].rva == rva
            && frames[depth - 1].tag == module->tag)
            continue;
        frames[depth].rva = rva;
        frames[depth].tag = module->tag;
        ++depth;
    }

    char line[kChainDepth * 14 + 1];
    char* at = line;
    for (unsigned i = 0; i < depth; ++i)
        at = appendFrame(at, line + sizeof(line) - 1, frames[i]);
    *at = 0;
    tq::hdr::log("Engine trace: unresolved shadow texture %ld, %u us,"
                 " resource %.160s, %u frames:%s\r\n",
                 report, us, resource ? resource : "(unknown)", depth, line);
}

void reportChains() {
    for (unsigned s = 0; s < kChainSlots; ++s) {
        if (!g_chains[s].ready) continue;
        char line[kChainDepth * 14 + 1];
        char* at = line;
        for (unsigned i = 0; i < g_chains[s].depth; ++i)
            at = appendFrame(at, line + sizeof(line) - 1, g_chains[s].frame[i]);
        *at = 0;
        tq::hdr::log("Engine trace: chain %u, %u us, %u frames:%s\r\n",
                     s, g_chains[s].us, g_chains[s].depth, line);
    }
}

void reportCallers(const CallerTable& table, const char* what) {
    for (unsigned i = 0; i < kLoadCallerSlots; ++i) {
        if (!table.rva[i]) continue;
        tq::hdr::log("Engine trace: slow %s from Engine+%#lx  x%ld"
                     " (%ld main) total %ld us worst %ld us\r\n", what,
                     (unsigned long)table.rva[i], table.calls[i],
                     table.main[i], table.us[i], table.worstUs[i]);
    }
    if (table.lost)
        tq::hdr::log("Engine trace: %ld slow %s calls had no slot left"
                     " or no caller inside Engine.dll\r\n", table.lost, what);
}

void reportSlowLoads() {
    reportCallers(g_loadLevelCallers, "LoadLevel");
    reportCallers(g_guaranteedCallers, "GuaranteedGetLevel");
    reportChains();
}

int __fastcall hookLoadLevel(void* self, void* edx, int background) {
    const void* caller = __builtin_return_address(0);
    if (!g_loadLevel) return 0;
    const int64_t started = tq::probe::now();
    const int result = g_loadLevel(self, edx, background);
    const uint32_t elapsed = tq::probe::microsecondsSince(started);
    tq::probe::engineCount(tq::probe::CounterEngineLevelLoad);
    tq::probe::engineCount(tq::probe::CounterEngineLevelLoadUs, elapsed);
    const bool main = onMainThread();
    if (main) {
        tq::probe::engineCount(tq::probe::CounterEngineLevelLoadMain);
        tq::probe::engineCount(tq::probe::CounterEngineLevelLoadMainUs, elapsed);
    }
    if (elapsed >= kSlowLoadUs) {
        recordSlowCall(g_loadLevelCallers, caller, elapsed, main);
        // From a local in this frame, so the walk starts below the return
        // address the game pushed and the first hit should be `caller` --
        // which is the scan's own cross-check against the table above.
        captureChain(&result, elapsed);
    }
    return result;
}

int __fastcall hookPortalLoadLevel(void* self, void* edx, int background) {
    return deferLoad(self, edx, background,
                     tq::probe::CounterEnginePortalAsyncLoad,
                     tq::probe::CounterEnginePortalAsyncSync);
}

// The same question as hookLoadLevel, one level up. Seventeen call sites in
// Engine.dll reach this function and only some of them are rendering -- the
// rest place entities and build paths -- so which one produced the 402 ms
// decides whether Stage 5.1 can be pointed here at all.
//
// This times but does not count: every call it makes reaches Region::LoadLevel
// unconditionally, so engine_level_load already counts the population and a
// CSV column would only restate it.
void* __fastcall hookGuaranteedGetLevel(void* self, void* edx, int flag) {
    const void* caller = __builtin_return_address(0);
    if (!g_guaranteedGetLevel) return nullptr;
    const int64_t started = tq::probe::now();
    void* const level = g_guaranteedGetLevel(self, edx, flag);
    const uint32_t elapsed = tq::probe::microsecondsSince(started);
    if (elapsed >= kSlowLoadUs)
        recordSlowCall(g_guaranteedCallers, caller, elapsed, onMainThread());
    return level;
}

void countShadowResourceState(unsigned state, bool inQueue,
                              uint32_t elapsed) {
    tq::probe::Counter count = tq::probe::CounterEngineShadowResStateOther;
    tq::probe::Counter duration =
        tq::probe::CounterEngineShadowResStateOtherUs;
    if (state == 0) {
        count = tq::probe::CounterEngineShadowResState0;
        duration = tq::probe::CounterEngineShadowResState0Us;
    } else if (state == 1) {
        count = tq::probe::CounterEngineShadowResState1;
        duration = tq::probe::CounterEngineShadowResState1Us;
    } else if (state == 2) {
        count = tq::probe::CounterEngineShadowResState2;
        duration = tq::probe::CounterEngineShadowResState2Us;
    }
    tq::probe::engineCount(count);
    tq::probe::engineCount(duration, elapsed);
    if (inQueue) {
        tq::probe::engineCount(tq::probe::CounterEngineShadowResInQueue);
        tq::probe::engineCount(tq::probe::CounterEngineShadowResInQueueUs,
                               elapsed);
    }
}

char asciiLower(char value) {
    return value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : value;
}

ShadowResourceType shadowResourceType(const char* name) {
    if (!name) return ShadowResourceOther;
    unsigned length = 0;
    // Resource::GetFileName returns a NUL-terminated engine-owned string.
    // Bound the read anyway: a corrupt resource should land in `other`, not
    // turn a diagnostic partition into an unbounded scan.
    while (length < 512 && name[length]) ++length;
    if (length < 4 || length == 512 || name[length - 4] != '.')
        return ShadowResourceOther;
    const char a = asciiLower(name[length - 3]);
    const char b = asciiLower(name[length - 2]);
    const char c = asciiLower(name[length - 1]);
    if (a == 'm' && b == 's' && c == 'h') return ShadowResourceMesh;
    if (a == 's' && b == 's' && c == 'h') return ShadowResourceShader;
    if (a == 't' && b == 'e' && c == 'x') return ShadowResourceTexture;
    return ShadowResourceOther;
}

void countShadowResourceType(ShadowResourceType type, uint32_t elapsed) {
    tq::probe::Counter count = tq::probe::CounterEngineShadowResTypeOther;
    tq::probe::Counter duration =
        tq::probe::CounterEngineShadowResTypeOtherUs;
    switch (type) {
    case ShadowResourceMesh:
        count = tq::probe::CounterEngineShadowResMesh;
        duration = tq::probe::CounterEngineShadowResMeshUs;
        break;
    case ShadowResourceShader:
        count = tq::probe::CounterEngineShadowResShader;
        duration = tq::probe::CounterEngineShadowResShaderUs;
        break;
    case ShadowResourceTexture:
        count = tq::probe::CounterEngineShadowResTexture;
        duration = tq::probe::CounterEngineShadowResTextureUs;
        break;
    case ShadowResourceOther:
        break;
    }
    tq::probe::engineCount(count);
    tq::probe::engineCount(duration, elapsed);
}

OutsideDirResourcePhase outsideDirResourcePhase() {
    if (InterlockedCompareExchange(&g_insideEngineRender, 0, 0) > 0)
        return OutsideDirResourceRender;
    if (InterlockedCompareExchange(&g_insideEngineUpdate, 0, 0) > 0)
        return OutsideDirResourceUpdate;
    return OutsideDirResourceOther;
}

void countOutsideDirResource(ShadowResourceType type,
                             OutsideDirResourcePhase phase,
                             uint32_t elapsed) {
    tq::probe::engineCount(tq::probe::CounterEngineResOutsideDir);
    tq::probe::engineCount(tq::probe::CounterEngineResOutsideDirUs, elapsed);

    tq::probe::Counter phaseCount =
        tq::probe::CounterEngineResOutsideDirOther;
    tq::probe::Counter phaseDuration =
        tq::probe::CounterEngineResOutsideDirOtherUs;
    if (phase == OutsideDirResourceRender) {
        phaseCount = tq::probe::CounterEngineResOutsideDirRender;
        phaseDuration = tq::probe::CounterEngineResOutsideDirRenderUs;
    } else if (phase == OutsideDirResourceUpdate) {
        phaseCount = tq::probe::CounterEngineResOutsideDirUpdate;
        phaseDuration = tq::probe::CounterEngineResOutsideDirUpdateUs;
    }
    tq::probe::engineCount(phaseCount);
    tq::probe::engineCount(phaseDuration, elapsed);

    tq::probe::Counter typeCount =
        tq::probe::CounterEngineResOutsideDirTypeOther;
    tq::probe::Counter typeDuration =
        tq::probe::CounterEngineResOutsideDirTypeOtherUs;
    if (type == ShadowResourceMesh) {
        typeCount = tq::probe::CounterEngineResOutsideDirMesh;
        typeDuration = tq::probe::CounterEngineResOutsideDirMeshUs;
    } else if (type == ShadowResourceShader) {
        typeCount = tq::probe::CounterEngineResOutsideDirShader;
        typeDuration = tq::probe::CounterEngineResOutsideDirShaderUs;
    } else if (type == ShadowResourceTexture) {
        typeCount = tq::probe::CounterEngineResOutsideDirTexture;
        typeDuration = tq::probe::CounterEngineResOutsideDirTextureUs;
    }
    tq::probe::engineCount(typeCount);
    tq::probe::engineCount(typeDuration, elapsed);
}
static_assert((kTerrainPreloadStateSlots
               & (kTerrainPreloadStateSlots - 1)) == 0,
              "terrain preload table must be a power of two");

TerrainPreloadState g_terrainPreloadStates[kTerrainPreloadStateSlots];
const void* g_activeTerrainType;
DWORD g_activeTerrainThread;
TerrainParameterPath g_activeTerrainPath;
int g_activeTerrainMaterialIndex;

unsigned terrainPreloadHash(const void* terrain) {
    uintptr_t value = (uintptr_t)terrain;
    value ^= value >> 11;
    value ^= value >> 21;
    return (unsigned)value & (kTerrainPreloadStateSlots - 1);
}

TerrainPreloadState* terrainPreloadState(const void* terrain, bool create) {
    if (!terrain) return nullptr;
    unsigned slot = terrainPreloadHash(terrain);
    for (unsigned probe = 0; probe < kTerrainPreloadStateSlots; ++probe) {
        TerrainPreloadState& state = g_terrainPreloadStates[slot];
        void* const found = (void*)InterlockedCompareExchangePointer(
            &state.terrain, create ? (void*)terrain : nullptr, nullptr);
        if (!found || found == terrain) return &state;
        slot = (slot + 1) & (kTerrainPreloadStateSlots - 1);
    }
    return nullptr;
}

void rememberTerrainPreloadAtFrame(const void* terrain, bool includeTextures,
                                   unsigned frame) {
    TerrainPreloadState* const state = terrainPreloadState(terrain, true);
    if (!state) {
        tq::probe::engineCount(
            tq::probe::CounterEngineTerrainPreloadTableOverflow);
        return;
    }
    const LONG framePlusOne = (LONG)frame + 1;
    if (includeTextures) {
        InterlockedExchange(&state->lastTrueFramePlusOne, framePlusOne);
        InterlockedIncrement(&state->trueCount);
    } else {
        InterlockedExchange(&state->lastFalseFramePlusOne, framePlusOne);
        InterlockedIncrement(&state->falseCount);
    }
}

void rememberTerrainPreload(const void* terrain, bool includeTextures) {
    rememberTerrainPreloadAtFrame(terrain, includeTextures,
                                  tq::probe::currentFrameIndex());
}

void rememberTerrainRtEventAtFrame(const void* terrain, TerrainRtEvent event,
                                   unsigned frame) {
    TerrainPreloadState* const state = terrainPreloadState(terrain, true);
    if (!state) {
        tq::probe::engineCount(
            tq::probe::CounterEngineTerrainPreloadTableOverflow);
        return;
    }
    LONG* count = nullptr;
    LONG* first = nullptr;
    LONG* last = nullptr;
    if (event == TerrainRtLoadAttach) {
        count = &state->rtLoadAttachCount;
        first = &state->rtLoadAttachFirstFramePlusOne;
        last = &state->rtLoadAttachLastFramePlusOne;
    } else if (event == TerrainRtLoadTextures) {
        count = &state->rtLoadTexturesCount;
        first = &state->rtLoadTexturesFirstFramePlusOne;
        last = &state->rtLoadTexturesLastFramePlusOne;
    } else {
        count = &state->rtOwnerPreloadCount;
        first = &state->rtOwnerPreloadFirstFramePlusOne;
        last = &state->rtOwnerPreloadLastFramePlusOne;
    }
    const LONG framePlusOne = (LONG)frame + 1;
    InterlockedCompareExchange(first, framePlusOne, 0);
    InterlockedExchange(last, framePlusOne);
    InterlockedIncrement(count);
}

void rememberTerrainRtEvent(const void* terrain, TerrainRtEvent event) {
    TQ_ENGINE_PROBE_ENTER();
    rememberTerrainRtEventAtFrame(terrain, event,
                                  tq::probe::currentFrameIndex());
}

TerrainPreloadSnapshot terrainPreloadSnapshot(const void* terrain) {
    TerrainPreloadSnapshot result = {};
    TerrainPreloadState* const state = terrainPreloadState(terrain, false);
    if (!state) return result;
    result.trueCount = (unsigned)InterlockedCompareExchange(
        &state->trueCount, 0, 0);
    result.falseCount = (unsigned)InterlockedCompareExchange(
        &state->falseCount, 0, 0);
    result.lastTrueFramePlusOne = (unsigned)InterlockedCompareExchange(
        &state->lastTrueFramePlusOne, 0, 0);
    result.lastFalseFramePlusOne = (unsigned)InterlockedCompareExchange(
        &state->lastFalseFramePlusOne, 0, 0);
    result.rtLoadAttachCount = (unsigned)InterlockedCompareExchange(
        &state->rtLoadAttachCount, 0, 0);
    result.rtLoadAttachFirstFramePlusOne = (unsigned)InterlockedCompareExchange(
        &state->rtLoadAttachFirstFramePlusOne, 0, 0);
    result.rtLoadAttachLastFramePlusOne = (unsigned)InterlockedCompareExchange(
        &state->rtLoadAttachLastFramePlusOne, 0, 0);
    result.rtLoadTexturesCount = (unsigned)InterlockedCompareExchange(
        &state->rtLoadTexturesCount, 0, 0);
    result.rtLoadTexturesFirstFramePlusOne = (unsigned)InterlockedCompareExchange(
        &state->rtLoadTexturesFirstFramePlusOne, 0, 0);
    result.rtLoadTexturesLastFramePlusOne = (unsigned)InterlockedCompareExchange(
        &state->rtLoadTexturesLastFramePlusOne, 0, 0);
    result.rtOwnerPreloadCount = (unsigned)InterlockedCompareExchange(
        &state->rtOwnerPreloadCount, 0, 0);
    result.rtOwnerPreloadFirstFramePlusOne =
        (unsigned)InterlockedCompareExchange(
            &state->rtOwnerPreloadFirstFramePlusOne, 0, 0);
    result.rtOwnerPreloadLastFramePlusOne =
        (unsigned)InterlockedCompareExchange(
            &state->rtOwnerPreloadLastFramePlusOne, 0, 0);
    return result;
}

unsigned rememberTerrainRtOwnerLayers(const void* owner,
                                      TerrainRtEvent event) {
    if (!owner || !g_terrainRtNumLayers || !g_terrainRtLayerType) return 0;
    const unsigned count = g_terrainRtNumLayers(owner, nullptr);
    const unsigned bounded = count < kTerrainRtLayerLimit
        ? count : kTerrainRtLayerLimit;
    if (count > kTerrainRtLayerLimit)
        tq::probe::engineCount(
            tq::probe::CounterEngineTerrainRtLayerOverflow);
    for (unsigned layer = 0; layer < bounded; ++layer)
        rememberTerrainRtEvent(
            g_terrainRtLayerType(owner, nullptr, layer), event);
    return bounded;
}
const unsigned kOutsideDirResourceMarkerFrames = 120;

OutsideDirResourceReport
    g_outsideDirResourceReports[kOutsideDirResourceReportSlots];
LONG g_outsideDirResourceSequence;
LONG g_outsideDirResourceReportedThrough;

void copyResourceName(char out[kOutsideDirResourceNameChars + 1],
                      const char* name) {
    unsigned length = 0;
    if (name) {
        while (length < kOutsideDirResourceNameChars && name[length]) {
            out[length] = name[length];
            ++length;
        }
    }
    out[length] = 0;
}

void rememberOutsideDirResource(const void* caller, const void* stack,
                                const char* resource,
                                bool stateKnown, unsigned state,
                                ShadowResourceType type,
                                OutsideDirResourcePhase phase,
                                unsigned frame, uint32_t elapsed,
                                const void* terrainType,
                                TerrainParameterPath terrainPath,
                                int terrainMaterialIndex,
                                TerrainPreloadSnapshot terrainPreload,
                                DeferredLocation deferredLocation,
                                ReflectionLocation reflectionLocation) {
    const LONG sequence = InterlockedIncrement(&g_outsideDirResourceSequence)
        - 1;
    if (sequence < 0) return;
    OutsideDirResourceReport& report = g_outsideDirResourceReports[
        (unsigned)sequence % kOutsideDirResourceReportSlots];
    InterlockedExchange(&report.ready, 0);
    report.frame = frame;
    report.us = elapsed;
    report.state = state;
    report.phase = phase;
    report.type = type;
    report.stateKnown = stateKnown;
    report.callerVerified = false;
    report.callerTag = '?';
    report.callerRva = 0;
    report.callerDepth = 0;
    report.terrainType = terrainType;
    report.terrainPath = terrainPath;
    report.terrainMaterialIndex = terrainMaterialIndex;
    report.terrainPreload = terrainPreload;
    report.deferredInvocation = deferredLocation.invocation;
    report.deferredPass = deferredLocation.pass;
    report.deferredSite = deferredLocation.site;
    report.reflectionManager = reflectionLocation.manager;
    report.reflectionPlane = reflectionLocation.plane;
    if (caller) {
        const BYTE* const address = (const BYTE*)caller;
        const ChainModule* const module = moduleOf(address);
        if (module && precededByCall(address, *module)) {
            report.callerVerified = true;
            report.callerTag = module->tag;
            report.callerRva = (DWORD)(address - module->base);
        }
    }
    if (stack && g_chainModuleCount) {
        MEMORY_BASIC_INFORMATION info = {};
        if (VirtualQuery(stack, &info, sizeof(info))
            && info.State == MEM_COMMIT) {
            const uintptr_t* const words = (const uintptr_t*)stack;
            const uintptr_t* const limit = (const uintptr_t*)(
                (const BYTE*)info.BaseAddress + info.RegionSize);
            for (unsigned i = 0;
                 i < kStackWords && words + i < limit
                 && report.callerDepth < kOutsideDirResourceCallerDepth;
                 ++i) {
                const BYTE* const value = (const BYTE*)words[i];
                const ChainModule* const module = moduleOf(value);
                if (!module || !precededByCall(value, *module)) continue;
                const DWORD rva = (DWORD)(value - module->base);
                if (report.callerDepth
                    && report.callerFrames[report.callerDepth - 1].rva == rva
                    && report.callerFrames[report.callerDepth - 1].tag
                        == module->tag)
                    continue;
                report.callerFrames[report.callerDepth].rva = rva;
                report.callerFrames[report.callerDepth].tag = module->tag;
                ++report.callerDepth;
            }
        }
    }
    copyResourceName(report.resource, resource);
    // sequence+1 makes zero an unambiguous unpublished slot. The marker runs
    // on this same main thread, but publishing last also makes the invariant
    // explicit if the game's threading changes.
    InterlockedExchange(&report.ready, sequence + 1);
}

const char* outsideDirResourcePhaseName(OutsideDirResourcePhase phase) {
    return phase == OutsideDirResourceRender ? "render"
         : phase == OutsideDirResourceUpdate ? "update" : "other";
}

const char* outsideDirResourceTypeName(ShadowResourceType type) {
    return type == ShadowResourceMesh ? "mesh"
         : type == ShadowResourceShader ? "shader"
         : type == ShadowResourceTexture ? "texture" : "other";
}

const char* terrainParameterPathName(TerrainParameterPath path) {
    return path == TerrainParameterMaterial ? "material"
         : path == TerrainParameterGrass ? "grass" : "none";
}

// F12 is deliberately a reaction anchor. Keep the loads in memory while the
// slow frame is running, then emit them only when the user marks what they
// felt. This avoids putting formatting or log-lock work into the candidate
// frame. The 120-frame horizon comfortably covers the measured 0.3--0.7 s
// reaction delay; the CSV remains authoritative for time-based candidate
// selection.
struct OutsideDirResourceWindow {
    LONG first;
    LONG total;
    bool truncated;
};

OutsideDirResourceWindow outsideDirResourceWindow(unsigned markerFrame) {
    OutsideDirResourceWindow window = {};
    window.total = InterlockedCompareExchange(
        &g_outsideDirResourceSequence, 0, 0);
    window.first = window.total > (LONG)kOutsideDirResourceReportSlots
        ? window.total - (LONG)kOutsideDirResourceReportSlots : 0;
    const LONG reported = InterlockedCompareExchange(
        &g_outsideDirResourceReportedThrough, 0, 0);
    if (reported < window.first && window.first < window.total) {
        const OutsideDirResourceReport& oldest = g_outsideDirResourceReports[
            (unsigned)window.first % kOutsideDirResourceReportSlots];
        if (oldest.ready == window.first + 1 && oldest.frame <= markerFrame
            && markerFrame - oldest.frame <= kOutsideDirResourceMarkerFrames)
            window.truncated = true;
    }
    if (reported > window.first) window.first = reported;
    return window;
}

bool outsideDirResourceInWindow(const OutsideDirResourceReport& report,
                                LONG sequence, unsigned markerFrame) {
    return report.ready == sequence + 1 && report.frame <= markerFrame
        && markerFrame - report.frame <= kOutsideDirResourceMarkerFrames;
}

void reportOutsideDirResourcesAtMarker() {
    if (!g_outsideDirResourceTracing) return;
    const unsigned markerFrame = tq::probe::currentFrameIndex();
    const OutsideDirResourceWindow window =
        outsideDirResourceWindow(markerFrame);

    unsigned emitted = 0;
    for (LONG sequence = window.first; sequence < window.total; ++sequence) {
        const OutsideDirResourceReport& report = g_outsideDirResourceReports[
            (unsigned)sequence % kOutsideDirResourceReportSlots];
        if (!outsideDirResourceInWindow(report, sequence, markerFrame))
            continue;
        char state[16];
        if (report.stateKnown)
            snprintf(state, sizeof(state), "%u", report.state);
        else
            lstrcpyA(state, "unknown");
        state[sizeof(state) - 1] = 0;
        if (report.callerVerified) {
            tq::hdr::log("Engine trace: outside-dir Resource %ld, frame %u,"
                         " %u us, phase %s, owner i%u/%s/%s,"
                         " reflection i%u/p%u, state %s, type %s,"
                         " caller %c+%#lx, resource %.*s\r\n",
                         sequence, report.frame, report.us,
                         outsideDirResourcePhaseName(report.phase),
                         report.deferredInvocation,
                         deferredPassName(report.deferredPass),
                         deferredSiteName(report.deferredSite),
                         report.reflectionManager, report.reflectionPlane,
                         state,
                         outsideDirResourceTypeName(report.type),
                         report.callerTag, (unsigned long)report.callerRva,
                         (int)kOutsideDirResourceNameChars,
                         report.resource[0] ? report.resource : "(unknown)");
        } else {
            tq::hdr::log("Engine trace: outside-dir Resource %ld, frame %u,"
                         " %u us, phase %s, owner i%u/%s/%s,"
                         " reflection i%u/p%u, state %s, type %s,"
                         " caller unverified, resource %.*s\r\n",
                         sequence, report.frame, report.us,
                         outsideDirResourcePhaseName(report.phase),
                         report.deferredInvocation,
                         deferredPassName(report.deferredPass),
                         deferredSiteName(report.deferredSite),
                         report.reflectionManager, report.reflectionPlane,
                         state,
                         outsideDirResourceTypeName(report.type),
                         (int)kOutsideDirResourceNameChars,
                         report.resource[0] ? report.resource : "(unknown)");
        }
        char chain[kOutsideDirResourceCallerDepth * 14 + 1];
        char* at = chain;
        for (unsigned i = 0; i < report.callerDepth; ++i)
            at = appendFrame(at, chain + sizeof(chain) - 1,
                             report.callerFrames[i]);
        *at = 0;
        tq::hdr::log("Engine trace: outside-dir Resource %ld upstream %u"
                     " frames:%s\r\n",
                     sequence, report.callerDepth, chain);
        const long lastTrue = report.terrainPreload.lastTrueFramePlusOne
            ? (long)report.terrainPreload.lastTrueFramePlusOne - 1 : -1;
        const long lastFalse = report.terrainPreload.lastFalseFramePlusOne
            ? (long)report.terrainPreload.lastFalseFramePlusOne - 1 : -1;
        tq::hdr::log("Engine trace: outside-dir Resource %ld TerrainType %p,"
                     " path %s, material %d, preload true %u last %ld,"
                     " false %u last %ld\r\n",
                     sequence, report.terrainType,
                     terrainParameterPathName(report.terrainPath),
                     report.terrainMaterialIndex,
                     report.terrainPreload.trueCount, lastTrue,
                     report.terrainPreload.falseCount, lastFalse);
        const long attachFirst =
            report.terrainPreload.rtLoadAttachFirstFramePlusOne
            ? (long)report.terrainPreload.rtLoadAttachFirstFramePlusOne - 1
            : -1;
        const long attachLast =
            report.terrainPreload.rtLoadAttachLastFramePlusOne
            ? (long)report.terrainPreload.rtLoadAttachLastFramePlusOne - 1
            : -1;
        const long texturesFirst =
            report.terrainPreload.rtLoadTexturesFirstFramePlusOne
            ? (long)report.terrainPreload.rtLoadTexturesFirstFramePlusOne - 1
            : -1;
        const long texturesLast =
            report.terrainPreload.rtLoadTexturesLastFramePlusOne
            ? (long)report.terrainPreload.rtLoadTexturesLastFramePlusOne - 1
            : -1;
        const long ownerFirst =
            report.terrainPreload.rtOwnerPreloadFirstFramePlusOne
            ? (long)report.terrainPreload.rtOwnerPreloadFirstFramePlusOne - 1
            : -1;
        const long ownerLast =
            report.terrainPreload.rtOwnerPreloadLastFramePlusOne
            ? (long)report.terrainPreload.rtOwnerPreloadLastFramePlusOne - 1
            : -1;
        tq::hdr::log("Engine trace: outside-dir Resource %ld TerrainRT"
                     " attach %u first %ld last %ld, LoadTextures %u"
                     " first %ld last %ld, owner PreLoad %u first %ld"
                     " last %ld\r\n",
                     sequence, report.terrainPreload.rtLoadAttachCount,
                     attachFirst, attachLast,
                     report.terrainPreload.rtLoadTexturesCount,
                     texturesFirst, texturesLast,
                     report.terrainPreload.rtOwnerPreloadCount,
                     ownerFirst, ownerLast);
        ++emitted;
    }
    InterlockedExchange(&g_outsideDirResourceReportedThrough, window.total);
    if (window.truncated)
        tq::probe::engineCount(
            tq::probe::CounterEngineResOutsideDirMarkerTruncated);
    tq::hdr::log("Engine trace: F12 frame %u emitted %u outside-dir Resource"
                 " loads from the preceding %u frames; truncated=%u\r\n",
                 markerFrame, emitted, kOutsideDirResourceMarkerFrames,
                 window.truncated ? 1u : 0u);
}
const unsigned kShadowMeshResourceMarkerFrames = 120;

ShadowMeshResourceReport
    g_shadowMeshResourceReports[kShadowMeshResourceReportSlots];
LONG g_shadowMeshResourceSequence;
LONG g_shadowMeshResourceReportedThrough;

void rememberShadowMeshResource(const void* caller, const void* stack,
                                const char* resource, bool inQueue,
                                unsigned frame, uint32_t elapsed) {
    const LONG sequence = InterlockedIncrement(&g_shadowMeshResourceSequence)
        - 1;
    if (sequence < 0) return;
    ShadowMeshResourceReport& report = g_shadowMeshResourceReports[
        (unsigned)sequence % kShadowMeshResourceReportSlots];
    InterlockedExchange(&report.ready, 0);
    report.frame = frame;
    report.us = elapsed;
    report.inQueue = inQueue;
    report.callerVerified = false;
    report.callerTag = '?';
    report.callerRva = 0;
    report.callerDepth = 0;
    if (caller) {
        const BYTE* const address = (const BYTE*)caller;
        const ChainModule* const module = moduleOf(address);
        if (module && precededByCall(address, *module)) {
            report.callerVerified = true;
            report.callerTag = module->tag;
            report.callerRva = (DWORD)(address - module->base);
        }
    }
    if (stack && g_chainModuleCount) {
        MEMORY_BASIC_INFORMATION info = {};
        if (VirtualQuery(stack, &info, sizeof(info))
            && info.State == MEM_COMMIT) {
            const uintptr_t* const words = (const uintptr_t*)stack;
            const uintptr_t* const limit = (const uintptr_t*)(
                (const BYTE*)info.BaseAddress + info.RegionSize);
            for (unsigned i = 0;
                 i < kStackWords && words + i < limit
                 && report.callerDepth < kOutsideDirResourceCallerDepth;
                 ++i) {
                const BYTE* const value = (const BYTE*)words[i];
                const ChainModule* const module = moduleOf(value);
                if (!module || !precededByCall(value, *module)) continue;
                const DWORD rva = (DWORD)(value - module->base);
                if (report.callerDepth
                    && report.callerFrames[report.callerDepth - 1].rva == rva
                    && report.callerFrames[report.callerDepth - 1].tag
                        == module->tag)
                    continue;
                report.callerFrames[report.callerDepth].rva = rva;
                report.callerFrames[report.callerDepth].tag = module->tag;
                ++report.callerDepth;
            }
        }
    }
    copyResourceName(report.resource, resource);
    InterlockedExchange(&report.ready, sequence + 1);
}

struct ShadowMeshResourceWindow {
    LONG first;
    LONG total;
    bool truncated;
};

ShadowMeshResourceWindow shadowMeshResourceWindow(unsigned markerFrame) {
    ShadowMeshResourceWindow window = {};
    window.total = InterlockedCompareExchange(
        &g_shadowMeshResourceSequence, 0, 0);
    window.first = window.total > (LONG)kShadowMeshResourceReportSlots
        ? window.total - (LONG)kShadowMeshResourceReportSlots : 0;
    const LONG reported = InterlockedCompareExchange(
        &g_shadowMeshResourceReportedThrough, 0, 0);
    if (reported < window.first && window.first < window.total) {
        const ShadowMeshResourceReport& oldest = g_shadowMeshResourceReports[
            (unsigned)window.first % kShadowMeshResourceReportSlots];
        if (oldest.ready == window.first + 1 && oldest.frame <= markerFrame
            && markerFrame - oldest.frame <= kShadowMeshResourceMarkerFrames)
            window.truncated = true;
    }
    if (reported > window.first) window.first = reported;
    return window;
}

bool shadowMeshResourceInWindow(const ShadowMeshResourceReport& report,
                                LONG sequence, unsigned markerFrame) {
    return report.ready == sequence + 1 && report.frame <= markerFrame
        && markerFrame - report.frame <= kShadowMeshResourceMarkerFrames;
}

void reportShadowMeshResourcesAtMarker() {
    if (!g_shadowMeshResourceTracing) return;
    const unsigned markerFrame = tq::probe::currentFrameIndex();
    const ShadowMeshResourceWindow window =
        shadowMeshResourceWindow(markerFrame);
    unsigned emitted = 0;
    for (LONG sequence = window.first; sequence < window.total; ++sequence) {
        const ShadowMeshResourceReport& report = g_shadowMeshResourceReports[
            (unsigned)sequence % kShadowMeshResourceReportSlots];
        if (!shadowMeshResourceInWindow(report, sequence, markerFrame))
            continue;
        if (report.callerVerified) {
            tq::hdr::log("Engine trace: directional mesh Resource %ld,"
                         " frame %u, %u us, state 0, queued %u, caller"
                         " %c+%#lx, resource %.*s\r\n",
                         sequence, report.frame, report.us,
                         report.inQueue ? 1u : 0u, report.callerTag,
                         (unsigned long)report.callerRva,
                         (int)kOutsideDirResourceNameChars,
                         report.resource[0] ? report.resource : "(unknown)");
        } else {
            tq::hdr::log("Engine trace: directional mesh Resource %ld,"
                         " frame %u, %u us, state 0, queued %u, caller"
                         " unverified, resource %.*s\r\n",
                         sequence, report.frame, report.us,
                         report.inQueue ? 1u : 0u,
                         (int)kOutsideDirResourceNameChars,
                         report.resource[0] ? report.resource : "(unknown)");
        }
        char chain[kOutsideDirResourceCallerDepth * 14 + 1];
        char* at = chain;
        for (unsigned i = 0; i < report.callerDepth; ++i)
            at = appendFrame(at, chain + sizeof(chain) - 1,
                             report.callerFrames[i]);
        *at = 0;
        tq::hdr::log("Engine trace: directional mesh Resource %ld upstream"
                     " %u frames:%s\r\n",
                     sequence, report.callerDepth, chain);
        ++emitted;
    }
    InterlockedExchange(&g_shadowMeshResourceReportedThrough, window.total);
    tq::hdr::log("Engine trace: F12 frame %u emitted %u directional state-0"
                 " mesh Resource loads from the preceding %u frames;"
                 " truncated=%u\r\n",
                 markerFrame, emitted, kShadowMeshResourceMarkerFrames,
                 window.truncated ? 1u : 0u);
}

enum ShadowTextureCaller {
    ShadowTextureMeshMaterial,
    ShadowTextureFun1155b0,
    ShadowTextureBillboard,
    ShadowTextureFun12fa30,
    ShadowTextureForwardRenderer,
    ShadowTextureStateParameter,
    ShadowTextureLineEffect,
    ShadowTexturePieOmatic,
    ShadowTextureFun23e1e0,
    ShadowTextureWater,
    ShadowTextureUnresolved,
    ShadowTextureCallerCount
};

const ShadowTextureCaller kShadowTextureDirectCallers[] = {
    ShadowTextureFun1155b0,
    ShadowTextureBillboard,
    ShadowTextureFun12fa30,
    ShadowTextureForwardRenderer,
    ShadowTextureStateParameter,
    ShadowTextureLineEffect,
    ShadowTexturePieOmatic,
    ShadowTextureFun23e1e0,
    ShadowTextureWater
};
static_assert(sizeof(kShadowTextureDirectCallerRvas)
                  / sizeof(*kShadowTextureDirectCallerRvas)
              == sizeof(kShadowTextureDirectCallers)
                  / sizeof(*kShadowTextureDirectCallers),
              "every direct texture caller needs a semantic bucket");

const tq::probe::Counter kShadowTextureCallerCounters[] = {
    tq::probe::CounterEngineShadowTexFromMeshMaterial,
    tq::probe::CounterEngineShadowTexFromFun1155b0,
    tq::probe::CounterEngineShadowTexFromBillboard,
    tq::probe::CounterEngineShadowTexFromFun12fa30,
    tq::probe::CounterEngineShadowTexFromForwardRenderer,
    tq::probe::CounterEngineShadowTexFromStateParameter,
    tq::probe::CounterEngineShadowTexFromLineEffect,
    tq::probe::CounterEngineShadowTexFromPieOmatic,
    tq::probe::CounterEngineShadowTexFromFun23e1e0,
    tq::probe::CounterEngineShadowTexFromWater,
    tq::probe::CounterEngineShadowTexFromUnresolved
};
const tq::probe::Counter kShadowTextureCallerDurationCounters[] = {
    tq::probe::CounterEngineShadowTexFromMeshMaterialUs,
    tq::probe::CounterEngineShadowTexFromFun1155b0Us,
    tq::probe::CounterEngineShadowTexFromBillboardUs,
    tq::probe::CounterEngineShadowTexFromFun12fa30Us,
    tq::probe::CounterEngineShadowTexFromForwardRendererUs,
    tq::probe::CounterEngineShadowTexFromStateParameterUs,
    tq::probe::CounterEngineShadowTexFromLineEffectUs,
    tq::probe::CounterEngineShadowTexFromPieOmaticUs,
    tq::probe::CounterEngineShadowTexFromFun23e1e0Us,
    tq::probe::CounterEngineShadowTexFromWaterUs,
    tq::probe::CounterEngineShadowTexFromUnresolvedUs
};
static_assert(sizeof(kShadowTextureCallerCounters)
                  / sizeof(*kShadowTextureCallerCounters)
              == ShadowTextureCallerCount,
              "every texture caller needs a count column");
static_assert(sizeof(kShadowTextureCallerDurationCounters)
                  / sizeof(*kShadowTextureCallerDurationCounters)
              == ShadowTextureCallerCount,
              "every texture caller needs a duration column");

ShadowTextureCaller shadowTextureCallerFromWords(
    const uintptr_t* words, unsigned count) {
    if (!g_engineBase || !words) return ShadowTextureUnresolved;
    for (unsigned word = 0; word < count; ++word) {
        const BYTE* const value = (const BYTE*)words[word];
        for (unsigned site = 0;
             site < sizeof(kShadowTextureDirectCallerRvas)
                        / sizeof(*kShadowTextureDirectCallerRvas);
             ++site) {
            if (value == g_engineBase + kShadowTextureDirectCallerRvas[site] + 5)
                return kShadowTextureDirectCallers[site];
        }
    }
    return ShadowTextureUnresolved;
}

ShadowTextureCaller shadowTextureCallerFromStack(const void* from) {
    if (g_insideShadowMaterialTexture) return ShadowTextureMeshMaterial;
    if (!g_shadowTextureCallerSitesVerified || !from)
        return ShadowTextureUnresolved;
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(from, &info, sizeof(info)) || info.State != MEM_COMMIT)
        return ShadowTextureUnresolved;
    const uintptr_t* const words = (const uintptr_t*)from;
    const uintptr_t* const limit = (const uintptr_t*)(
        (const BYTE*)info.BaseAddress + info.RegionSize);
    const SIZE_T available = limit > words ? (SIZE_T)(limit - words) : 0;
    const unsigned scan = (unsigned)(available < 128 ? available : 128);
    return shadowTextureCallerFromWords(words, scan);
}

void countShadowTextureCaller(ShadowTextureCaller caller, uint32_t elapsed) {
    if ((unsigned)caller >= ShadowTextureCallerCount)
        caller = ShadowTextureUnresolved;
    tq::probe::engineCount(kShadowTextureCallerCounters[caller]);
    tq::probe::engineCount(kShadowTextureCallerDurationCounters[caller],
                           elapsed);
}

void countShadowMaterialUsedContext(
    const ShadowMeshParameterContext& context, const void* texture,
    uint32_t elapsed) {
    static const tq::probe::Counter lookupCounts[] = {
        tq::probe::CounterEngineShadowMaterialLookupExact,
        tq::probe::CounterEngineShadowMaterialLookupClassOther,
        tq::probe::CounterEngineShadowMaterialLookupPassMismatch,
        tq::probe::CounterEngineShadowMaterialLookupInstanceMissing
    };
    static const tq::probe::Counter lookupDurations[] = {
        tq::probe::CounterEngineShadowMaterialLookupExactUs,
        tq::probe::CounterEngineShadowMaterialLookupClassOtherUs,
        tq::probe::CounterEngineShadowMaterialLookupPassMismatchUs,
        tq::probe::CounterEngineShadowMaterialLookupInstanceMissingUs
    };
    const unsigned match = (unsigned)context.match < 4
        ? (unsigned)context.match : (unsigned)ShadowContextInstanceMissing;
    tq::probe::engineCount(lookupCounts[match]);
    tq::probe::engineCount(lookupDurations[match], elapsed);

    static const tq::probe::Counter styleCounts[] = {
        tq::probe::CounterEngineShadowMaterialUsedStyle0,
        tq::probe::CounterEngineShadowMaterialUsedStyle1,
        tq::probe::CounterEngineShadowMaterialUsedStyle2,
        tq::probe::CounterEngineShadowMaterialUsedStyle3,
        tq::probe::CounterEngineShadowMaterialUsedStyle4,
        tq::probe::CounterEngineShadowMaterialUsedStyle5
    };
    static const tq::probe::Counter styleDurations[] = {
        tq::probe::CounterEngineShadowMaterialUsedStyle0Us,
        tq::probe::CounterEngineShadowMaterialUsedStyle1Us,
        tq::probe::CounterEngineShadowMaterialUsedStyle2Us,
        tq::probe::CounterEngineShadowMaterialUsedStyle3Us,
        tq::probe::CounterEngineShadowMaterialUsedStyle4Us,
        tq::probe::CounterEngineShadowMaterialUsedStyle5Us
    };
    if (!context.active || !context.styleKnown || context.style > 5) {
        tq::probe::engineCount(
            tq::probe::CounterEngineShadowMaterialUsedContextUnknown);
        tq::probe::engineCount(
            tq::probe::CounterEngineShadowMaterialUsedContextUnknownUs,
            elapsed);
    } else {
        tq::probe::engineCount(styleCounts[context.style]);
        tq::probe::engineCount(styleDurations[context.style], elapsed);
    }

    tq::probe::Counter baseCount =
        tq::probe::CounterEngineShadowMaterialUsedBaseUnknown;
    tq::probe::Counter baseDuration =
        tq::probe::CounterEngineShadowMaterialUsedBaseUnknownUs;
    if (context.active && context.baseKnown) {
        const bool base = texture && texture == context.baseTexture;
        baseCount = base
            ? tq::probe::CounterEngineShadowMaterialUsedBaseMatch
            : tq::probe::CounterEngineShadowMaterialUsedBaseOther;
        baseDuration = base
            ? tq::probe::CounterEngineShadowMaterialUsedBaseMatchUs
            : tq::probe::CounterEngineShadowMaterialUsedBaseOtherUs;
    }
    tq::probe::engineCount(baseCount);
    tq::probe::engineCount(baseDuration, elapsed);

    tq::probe::Counter passCount =
        tq::probe::CounterEngineShadowMaterialUsedPassUnknown;
    tq::probe::Counter passDuration =
        tq::probe::CounterEngineShadowMaterialUsedPassUnknownUs;
    // The adapter supplies the material call's pass even when no accepted
    // record matches it. `active` means a table match; instance means the
    // call context itself is known. Keep those facts independent.
    if (context.instance) {
        passCount = context.pass == 0
            ? tq::probe::CounterEngineShadowMaterialUsedPass0
            : tq::probe::CounterEngineShadowMaterialUsedPassOther;
        passDuration = context.pass == 0
            ? tq::probe::CounterEngineShadowMaterialUsedPass0Us
            : tq::probe::CounterEngineShadowMaterialUsedPassOtherUs;
    }
    tq::probe::engineCount(passCount);
    tq::probe::engineCount(passDuration, elapsed);

    const tq::probe::Counter outerCount = context.outerInstanceSite
        ? tq::probe::CounterEngineShadowMaterialOuterInstanceSite
        : tq::probe::CounterEngineShadowMaterialOuterOtherSite;
    const tq::probe::Counter outerDuration = context.outerInstanceSite
        ? tq::probe::CounterEngineShadowMaterialOuterInstanceSiteUs
        : tq::probe::CounterEngineShadowMaterialOuterOtherSiteUs;
    tq::probe::engineCount(outerCount);
    tq::probe::engineCount(outerDuration, elapsed);
}

void countShadowMeshContextPatchStatus() {
    static const tq::probe::Counter counters[] = {
        tq::probe::CounterEngineShadowContextPatchActive,
        tq::probe::CounterEngineShadowContextPatchDependencyMissing,
        tq::probe::CounterEngineShadowContextPatchFrameMismatch,
        tq::probe::CounterEngineShadowContextPatchEntryMismatch,
        tq::probe::CounterEngineShadowContextPatchContextMismatch,
        tq::probe::CounterEngineShadowContextPatchCallFailed,
        tq::probe::CounterEngineShadowContextPatchReverted
    };
    static_assert(sizeof(counters) / sizeof(*counters)
                      == ShadowMeshContextPatchStatusCount,
                  "every mesh-context patch result needs a counter");
    const unsigned status = (unsigned)g_shadowMeshContextPatchStatus
                                < ShadowMeshContextPatchStatusCount
        ? (unsigned)g_shadowMeshContextPatchStatus
        : (unsigned)ShadowMeshContextPatchDependencyMissing;
    tq::probe::engineCount(counters[status]);
}

void countShadowMaterialTexture(bool known, bool used, uint32_t elapsed) {
    tq::probe::engineCount(tq::probe::CounterEngineShadowMaterialTex);
    tq::probe::engineCount(tq::probe::CounterEngineShadowMaterialTexUs,
                           elapsed);
    tq::probe::Counter count = known
        ? (used ? tq::probe::CounterEngineShadowMaterialTexUsed
                : tq::probe::CounterEngineShadowMaterialTexUnused)
        : tq::probe::CounterEngineShadowMaterialTexUnknown;
    tq::probe::Counter duration = known
        ? (used ? tq::probe::CounterEngineShadowMaterialTexUsedUs
                : tq::probe::CounterEngineShadowMaterialTexUnusedUs)
        : tq::probe::CounterEngineShadowMaterialTexUnknownUs;
    tq::probe::engineCount(count);
    tq::probe::engineCount(duration, elapsed);
}

void reportShadowMaterialDependency(const ShadowMeshParameterContext& context,
                                    const void* texture, uint32_t nameHash,
                                    uint32_t elapsed) {
    if (!tq::hdr::readSettings().trace) return;
    const LONG report = InterlockedIncrement(&g_shadowMaterialReports) - 1;
    if (report < 0 || report >= (LONG)kChainSlots) return;
    const char* const resource = g_resourceFileNameVerified
        && g_resourceFileName && texture
        ? g_resourceFileName(const_cast<void*>(texture), nullptr) : nullptr;
    const char* base = "unknown";
    if (context.active && context.baseKnown)
        base = texture == context.baseTexture ? "match" : "other";
    tq::hdr::log("Engine trace: cold used shadow material %ld, %u us,"
                 " Name::Hash=%#lx resource %.160s style=%d pass=%d"
                 " base=%s match=%u\r\n",
                 report, elapsed, (unsigned long)nameHash,
                 resource ? resource : "(unknown)",
                 context.active && context.styleKnown
                     ? (int)context.style : -1,
                 context.instance ? context.pass : -1, base,
                 (unsigned)context.match);
}

void flushPendingShadowMaterialTexture(bool known, bool used) {
    TQ_ENGINE_PROBE_ENTER();
    if (!g_shadowMaterialTexturePending) return;
    const uint32_t elapsed = g_shadowMaterialTexturePendingUs;
    const uint32_t nameHash = g_shadowMaterialPendingNameHash;
    const ShadowMeshParameterContext context =
        g_shadowMaterialPendingContext;
    const void* const texture = g_shadowMaterialPendingTexture;
    g_shadowMaterialTexturePending = false;
    g_shadowMaterialTexturePendingUs = 0;
    g_shadowMaterialPendingNameHash = 0;
    g_shadowMaterialPendingContext = {};
    g_shadowMaterialPendingTexture = nullptr;
    countShadowMaterialTexture(known, used, elapsed);
    if (known && used) {
        countShadowMaterialUsedContext(context, texture, elapsed);
        reportShadowMaterialDependency(context, texture, nameHash, elapsed);
    }
}

int __fastcall hookShadowTextureParameter(
    void* shader, void* edx, const void* name, unsigned index,
    void* reserved, void* textureValue) {
    if (g_shadowMaterialTexturePending) {
        const bool canClassify = onMainThread()
            && InterlockedCompareExchange(&g_insideDirectional, 0, 0) > 0
            && g_shaderHasParameterVerified && g_shaderHasParameter
            && g_resourceStateVerified && shader && name
            && *(const unsigned*)((const BYTE*)shader
                                 + kResourceLoadedStateOffset) == 2;
        const bool used = canClassify
            && g_shaderHasParameter(shader, nullptr, name);
        flushPendingShadowMaterialTexture(canClassify, used);
    }
    return g_setTextureParameter
        ? g_setTextureParameter(shader, edx, name, index, reserved, textureValue)
        : 0;
}

void countDeferredShadowAlpha(unsigned state, bool enqueued, bool failed) {
    TQ_ENGINE_PROBE_ENTER();
    if (!g_shadowTracing) return;
    tq::probe::engineCount(tq::probe::CounterEngineShadowAlphaOmitted);
    tq::probe::engineCount(state == 0
        ? tq::probe::CounterEngineShadowAlphaState0
        : tq::probe::CounterEngineShadowAlphaState1);
    if (enqueued)
        tq::probe::engineCount(tq::probe::CounterEngineShadowAlphaEnqueued);
    if (failed)
        tq::probe::engineCount(
            tq::probe::CounterEngineShadowAlphaEnqueueFailed);
}

void countDeferredShadowMesh(unsigned state, bool enqueued, bool failed) {
    TQ_ENGINE_PROBE_ENTER();
    if (!g_shadowTracing) return;
    tq::probe::engineCount(tq::probe::CounterEngineShadowMeshOmitted);
    tq::probe::engineCount(state == 0
        ? tq::probe::CounterEngineShadowMeshOmittedState0
        : tq::probe::CounterEngineShadowMeshOmittedState1);
    if (enqueued)
        tq::probe::engineCount(
            tq::probe::CounterEngineShadowMeshOmittedEnqueued);
    if (failed)
        tq::probe::engineCount(
            tq::probe::CounterEngineShadowMeshOmittedEnqueueFailed);
}

void countDeferredShadowActorPose(unsigned state, bool enqueued, bool failed) {
    TQ_ENGINE_PROBE_ENTER();
    if (!g_shadowTracing) return;
    tq::probe::engineCount(
        tq::probe::CounterEngineShadowActorPoseDeferred);
    tq::probe::engineCount(state == 0
        ? tq::probe::CounterEngineShadowActorPoseState0
        : tq::probe::CounterEngineShadowActorPoseState1);
    if (enqueued)
        tq::probe::engineCount(
            tq::probe::CounterEngineShadowActorPoseEnqueued);
    if (failed)
        tq::probe::engineCount(
            tq::probe::CounterEngineShadowActorPoseEnqueueFailed);
}

void countShadowActorPoseEnqueueFailure() {
    TQ_ENGINE_PROBE_ENTER();
    if (g_shadowTracing)
        tq::probe::engineCount(
            tq::probe::CounterEngineShadowActorPoseEnqueueFailed);
}

void __fastcall hookShadowMeshEnsure(void* resource, void* edx) {
    if (!g_ensureAvailable) return;
    const bool cold = onMainThread()
        && InterlockedCompareExchange(&g_insideDirectional, 0, 0) > 0
        && g_resourceStateVerified && resource
        && *(const unsigned*)((const BYTE*)resource
                             + kResourceLoadedStateOffset) == 0;
    if (!cold) {
        g_ensureAvailable(resource, edx);
        return;
    }
    const int64_t started = tq::probe::now();
    g_ensureAvailable(resource, edx);
    tq::probe::engineCount(tq::probe::CounterEngineShadowMeshCold);
    tq::probe::engineCount(tq::probe::CounterEngineShadowMeshColdUs,
                           tq::probe::microsecondsSince(started));
}

int __fastcall hookTerrainRtLoad(void* self, void* edx, void* reader,
                                 int version) {
    if (!g_terrainRtLoad) return 0;
    const int64_t started = tq::probe::now();
    const int result = g_terrainRtLoad(self, edx, reader, version);
    tq::probe::engineCount(tq::probe::CounterEngineTerrainRtLoad);
    tq::probe::engineCount(tq::probe::CounterEngineTerrainRtLoadUs,
                           tq::probe::microsecondsSince(started));
    if (result)
        rememberTerrainRtOwnerLayers(self, TerrainRtLoadAttach);
    return result;
}

int __fastcall hookTerrainRtLoadRenderData(void* self, void* edx) {
    if (!g_terrainRtLoadRenderData) return 0;
    const bool main = onMainThread();
    const int64_t started = tq::probe::now();
    int result = 0;
    {
        // LoadRenderData can run on the loader thread during a save load.
        // g_gpuCurrent and the immediate D3D context belong to the render
        // thread; issuing End(query) from the loader can deadlock the device.
        tq::probe::GpuScope gpu(main ? tq::probe::currentGpuContext() : nullptr,
                                tq::probe::GpuTerrainRtLoadRender);
        result = g_terrainRtLoadRenderData(self, edx);
    }
    const uint32_t elapsed = tq::probe::microsecondsSince(started);
    tq::probe::engineCount(tq::probe::CounterEngineTerrainRtLoadRender);
    tq::probe::engineCount(tq::probe::CounterEngineTerrainRtLoadRenderUs,
                           elapsed);
    tq::probe::engineCount(main
        ? tq::probe::CounterEngineTerrainRtLoadRenderMain
        : tq::probe::CounterEngineTerrainRtLoadRenderOther);
    tq::probe::engineCount(main
        ? tq::probe::CounterEngineTerrainRtLoadRenderMainUs
        : tq::probe::CounterEngineTerrainRtLoadRenderOtherUs, elapsed);
    return result;
}

void __fastcall hookTerrainRtPreload(void* self, void* edx, int priority,
                                     const void* frustum, unsigned flags) {
    if (!g_terrainRtPreload) return;
    const unsigned layers = rememberTerrainRtOwnerLayers(
        self, TerrainRtOwnerPreload);
    tq::probe::engineCount(tq::probe::CounterEngineTerrainRtPreloadLayers,
                           layers);
    const int64_t started = tq::probe::now();
    g_terrainRtPreload(self, edx, priority, frustum, flags);
    tq::probe::engineCount(tq::probe::CounterEngineTerrainRtPreload);
    tq::probe::engineCount(tq::probe::CounterEngineTerrainRtPreloadUs,
                           tq::probe::microsecondsSince(started));
}

void __fastcall hookTerrainPreload(void* self, void* edx,
                                   int includeTextures) {
    if (!g_terrainPreload) return;
    rememberTerrainPreload(self, includeTextures != 0);
    const int64_t started = tq::probe::now();
    g_terrainPreload(self, edx, includeTextures);
    tq::probe::engineCount(tq::probe::CounterEngineTerrainPreload);
    tq::probe::engineCount(tq::probe::CounterEngineTerrainPreloadUs,
                           tq::probe::microsecondsSince(started));
    tq::probe::engineCount(includeTextures
        ? tq::probe::CounterEngineTerrainPreloadTrue
        : tq::probe::CounterEngineTerrainPreloadFalse);
}

void __fastcall hookTerrainSetShaderParams(
    const void* self, void* edx, const void* shader, int materialIndex) {
    if (!g_terrainSetShaderParams) return;
    tq::probe::engineCount(tq::probe::CounterEngineTerrainShaderParams);
    if (!onMainThread()) {
        g_terrainSetShaderParams(self, edx, shader, materialIndex);
        return;
    }
    const void* const priorType = g_activeTerrainType;
    const DWORD priorThread = g_activeTerrainThread;
    const TerrainParameterPath priorPath = g_activeTerrainPath;
    const int priorMaterial = g_activeTerrainMaterialIndex;
    g_activeTerrainType = self;
    g_activeTerrainThread = GetCurrentThreadId();
    g_activeTerrainPath = TerrainParameterMaterial;
    g_activeTerrainMaterialIndex = materialIndex;
    g_terrainSetShaderParams(self, edx, shader, materialIndex);
    g_activeTerrainType = priorType;
    g_activeTerrainThread = priorThread;
    g_activeTerrainPath = priorPath;
    g_activeTerrainMaterialIndex = priorMaterial;
}

void __fastcall hookTerrainSetGrassShaderParams(
    const void* self, void* edx, const void* shader) {
    if (!g_terrainSetGrassShaderParams) return;
    tq::probe::engineCount(tq::probe::CounterEngineTerrainGrassParams);
    if (!onMainThread()) {
        g_terrainSetGrassShaderParams(self, edx, shader);
        return;
    }
    const void* const priorType = g_activeTerrainType;
    const DWORD priorThread = g_activeTerrainThread;
    const TerrainParameterPath priorPath = g_activeTerrainPath;
    const int priorMaterial = g_activeTerrainMaterialIndex;
    g_activeTerrainType = self;
    g_activeTerrainThread = GetCurrentThreadId();
    g_activeTerrainPath = TerrainParameterGrass;
    g_activeTerrainMaterialIndex = -1;
    g_terrainSetGrassShaderParams(self, edx, shader);
    g_activeTerrainType = priorType;
    g_activeTerrainThread = priorThread;
    g_activeTerrainPath = priorPath;
    g_activeTerrainMaterialIndex = priorMaterial;
}

void __fastcall hookTerrainRenderGround(
    const void* self, void* edx, const void* name, void* canvas,
    const void* sceneRenderer, const void* pass, int flag) {
    if (!g_terrainRenderGround) return;
    const int64_t started = tq::probe::now();
    {
        tq::probe::GpuScope gpu(tq::probe::currentGpuContext(),
                                tq::probe::GpuTerrainGround);
        g_terrainRenderGround(self, edx, name, canvas, sceneRenderer, pass,
                              flag);
    }
    tq::probe::engineCount(tq::probe::CounterEngineTerrainGround);
    tq::probe::engineCount(tq::probe::CounterEngineTerrainGroundUs,
                           tq::probe::microsecondsSince(started));
}

void __fastcall hookLoadResource(void* self, void* edx, void* resource) {
    if (!g_loadResource) return;
    // Sample before entering the game function: state 1 is the branch that
    // waits for the loader worker, while state 0 falls through to a direct
    // load on this thread. The resource is necessarily a live Resource -- the
    // original reads +0x30 before doing anything else -- and the two offsets
    // are enabled only after their exported accessors and bytes verify.
    const bool main = onMainThread();
    const bool inShadow = main
        && InterlockedCompareExchange(&g_insideDirectional, 0, 0) > 0;
    const bool outsideDir = g_outsideDirResourceTracing && main && !inShadow;
    const bool classify = (inShadow || outsideDir)
        && g_resourceStateVerified && resource;
    const unsigned state = classify
        ? *(const unsigned*)((const BYTE*)resource + kResourceLoadedStateOffset)
        : 0;
    const bool inQueue = inShadow && classify
        && *(void* const*)((const BYTE*)resource + kResourceInQueueOffset)
            != nullptr;
    const char* const resourceName = (inShadow || outsideDir)
        && g_resourceFileNameVerified && g_resourceFileName && resource
        ? g_resourceFileName(resource, nullptr) : nullptr;
    const ShadowResourceType resourceType = resourceName
        ? shadowResourceType(resourceName) : ShadowResourceOther;
    const bool shadowMeshReport = g_shadowMeshResourceTracing && inShadow
        && classify && state == 0 && resourceType == ShadowResourceMesh;
    const ShadowTextureCaller textureCaller =
        inShadow && resourceType == ShadowResourceTexture
        ? shadowTextureCallerFromStack(&resource)
        : ShadowTextureUnresolved;
    const void* const caller = outsideDir || shadowMeshReport
        ? __builtin_return_address(0) : nullptr;
    const OutsideDirResourcePhase outsidePhase = outsideDir
        ? outsideDirResourcePhase() : OutsideDirResourceOther;
    const DeferredLocation deferredLocation = main
        ? currentDeferredLocation() : DeferredLocation();
    const ReflectionLocation reflectionLocation = main
        ? currentReflectionLocation() : ReflectionLocation();
    const unsigned frame = outsideDir || shadowMeshReport
        ? tq::probe::currentFrameIndex() : 0;
    const bool terrainContext = outsideDir && g_activeTerrainType
        && g_activeTerrainThread == GetCurrentThreadId();
    const void* const terrainType = terrainContext
        ? g_activeTerrainType : nullptr;
    const TerrainParameterPath terrainPath = terrainContext
        ? g_activeTerrainPath : TerrainParameterNone;
    const int terrainMaterialIndex = terrainContext
        ? g_activeTerrainMaterialIndex : -1;
    const TerrainPreloadSnapshot terrainPreload =
        terrainPreloadSnapshot(terrainType);
    char resourceNameCopy[kOutsideDirResourceNameChars + 1] = {};
    if (outsideDir || shadowMeshReport)
        copyResourceName(resourceNameCopy, resourceName);
    const int64_t started = tq::probe::now();
    g_loadResource(self, edx, resource);
    const uint32_t elapsed = tq::probe::microsecondsSince(started);
    if (main)
        noteGpuChunkRenderableResource(elapsed, terrainType,
                                       terrainMaterialIndex);
    tq::probe::engineCount(tq::probe::CounterEngineResLoad);
    tq::probe::engineCount(tq::probe::CounterEngineResLoadUs, elapsed);
    if (main) {
        countReflectionResource(elapsed);
        countDeferredOwnerResource(elapsed);
        tq::probe::engineCount(tq::probe::CounterEngineResLoadMain);
        tq::probe::engineCount(tq::probe::CounterEngineResLoadMainUs, elapsed);
        if (outsideDir) {
            countOutsideDirResource(resourceType, outsidePhase, elapsed);
            rememberOutsideDirResource(
                caller, &resource, resourceNameCopy, classify, state, resourceType,
                outsidePhase, frame, elapsed, terrainType, terrainPath,
                terrainMaterialIndex, terrainPreload, deferredLocation,
                reflectionLocation);
        }
        if (inShadow) {
            tq::probe::engineCount(tq::probe::CounterEngineShadowResLoad);
            tq::probe::engineCount(tq::probe::CounterEngineShadowResLoadUs,
                                   elapsed);
            if (classify) countShadowResourceState(state, inQueue, elapsed);
            if (g_resourceFileNameVerified) {
                countShadowResourceType(resourceType, elapsed);
                if (shadowMeshReport)
                    rememberShadowMeshResource(
                        caller, &resource, resourceNameCopy, inQueue, frame,
                        elapsed);
                if (resourceType == ShadowResourceTexture) {
                    countShadowTextureCaller(textureCaller, elapsed);
                    if (textureCaller == ShadowTextureUnresolved)
                        reportUnresolvedShadowTextureChain(
                            &resource, resourceName, elapsed);
                }
            }
        }
    }
}

void __fastcall hookUnloadLevel(void* self, void* edx, int a, int b) {
    if (!g_unloadLevel) return;
    const int64_t started = tq::probe::now();
    g_unloadLevel(self, edx, a, b);
    tq::probe::engineCount(tq::probe::CounterEngineRegionUnload);
    tq::probe::engineCount(tq::probe::CounterEngineRegionUnloadUs,
                           tq::probe::microsecondsSince(started));
}

void __fastcall hookEnqueueResource(void* self, void* edx, const void* resource,
                                    int priority, int a, int b) {
    if (!g_enqueue) return;
    tq::probe::engineCount(tq::probe::CounterEngineResEnqueued);
    g_enqueue(self, edx, resource, priority, a, b);
}

int __fastcall hookReadFromFile(void* self, void* edx, int entry, BYTE* dest,
                                unsigned offset, unsigned size,
                                void* blockBuffer) {
    if (!g_readFromFile) return 0;
    // Counted, never timed. Rounded up so a short read still registers as one
    // KiB rather than disappearing.
    tq::probe::engineCount(tq::probe::CounterEngineArcRead);
    tq::probe::engineCount(tq::probe::CounterEngineArcKib, (size + 1023u) >> 10);
    return g_readFromFile(self, edx, entry, dest, offset, size, blockBuffer);
}

// Semantically what the seven replaced bytes did: spin while the region's
// loading flag is set. The null check is the one departure, and it turns a
// fault inside Engine.dll into a return -- a function a full .text scan finds
// no caller for is not worth crashing over.
void __fastcall hookWaitForLoadingToFinish(void* self, void* edx) {
    (void)edx;
    if (!self) return;
    tq::probe::engineCount(tq::probe::CounterEngineWaitLoading);
    const int64_t started = tq::probe::now();
    const volatile BYTE* loading = (const volatile BYTE*)self + 0x78;
    while (*loading == 1) {
        YieldProcessor();
        SwitchToThread();
    }
    tq::probe::engineCount(tq::probe::CounterEngineWaitLoadingUs,
                           tq::probe::microsecondsSince(started));
}

// Retargeted call sites reach these through the ordinary call frame, so they
// can be plain __stdcall functions: no hand-emitted thunk, and no chance of
// getting a callee-pop count wrong on a path that runs every frame.

// The uncontended case is one interlocked operation and a branch, and records
// nothing at all. That is what makes measuring a lock on a render-path call
// affordable; taking a timestamp per acquisition would not be.
void __stdcall hookEnterCriticalSection(LPCRITICAL_SECTION section) {
    if (TryEnterCriticalSection(section)) return;
    const int64_t started = tq::probe::now();
    EnterCriticalSection(section);
    tq::probe::engineCount(tq::probe::CounterEngineRegionLockHits);
    tq::probe::engineCount(tq::probe::CounterEngineRegionLockUs,
                           tq::probe::microsecondsSince(started));
}

DWORD __stdcall hookWaitForSingleObject(HANDLE handle, DWORD milliseconds) {
    const int64_t started = tq::probe::now();
    const DWORD result = WaitForSingleObject(handle, milliseconds);
    tq::probe::engineCount(tq::probe::CounterEngineFenceWait);
    tq::probe::engineCount(tq::probe::CounterEngineFenceWaitUs,
                           tq::probe::microsecondsSince(started));
    return result;
}

void __fastcall hookSweep(void* self, void* edx) {
    if (!g_sweep) return;
    const int64_t started = tq::probe::now();
    g_sweep(self, edx);
    tq::probe::engineCount(tq::probe::CounterEngineSweeps);
    tq::probe::engineCount(tq::probe::CounterEngineSweepUs,
                           tq::probe::microsecondsSince(started));
}

// The frame's two halves. Everything else in this file nests inside one of
// these, so their totals are the denominators the rest are read against.
void __fastcall hookEngineUpdate(void* self, void* edx, const void* frustum,
                                 int flag) {
    if (!g_engineUpdate) return;
    const int64_t started = tq::probe::now();
    if (g_outsideDirResourceTracing)
        InterlockedIncrement(&g_insideEngineUpdate);
    g_engineUpdate(self, edx, frustum, flag);
    if (g_outsideDirResourceTracing)
        InterlockedDecrement(&g_insideEngineUpdate);
    tq::probe::engineCount(tq::probe::CounterEngineUpdate);
    tq::probe::engineCount(tq::probe::CounterEngineUpdateUs,
                           tq::probe::microsecondsSince(started));
}

// What the window actually receives, so "the pump is slow" can be read
// against how much it is being asked to carry. Claimed with an interlocked
// exchange rather than a lock: the pump is single-threaded in practice, and
// a histogram is not worth a critical section on it.
const unsigned kMessageKinds = 64;
volatile LONG g_messageId[kMessageKinds];
volatile LONG g_messageCount[kMessageKinds];
volatile LONG g_messageSlow[kMessageKinds];
SetTimerFn g_setTimer;
CallPatch g_setTimerPatch;
LONG g_setTimerCalls;

// The game's timer, learned from the messages rather than from SetTimer.
// SetTimer is never called while we are installed -- run 19 hooked it and
// logged nothing -- because TQ.exe arms its timer during startup, long before
// the renderer exists and this module can be loaded. But every WM_TIMER
// carries the identity of the timer that produced it: hwnd, wParam = the
// timer id, lParam = its TIMERPROC or null. That is everything SetTimer needs
// to re-arm the same timer with a different period, which is the experiment.
HWND g_timerWindow;
UINT_PTR g_timerId;
LONG g_timerKnown;
LONG g_timerRearmed;
uintptr_t g_timerProc;
int64_t g_lastTimerTick;
uint32_t g_timerGapMinUs = 0xffffffffu;
LONG g_timerSamples;

void noteTimer(const MSG* message) {
    // The shortest gap between two WM_TIMER messages is the best estimate of
    // the period the game asked for: the message is synthesized only when the
    // queue is otherwise empty, so every other gap is inflated by coalescing.
    const int64_t now = tq::probe::now();
    if (g_lastTimerTick) {
        const uint32_t gap = tq::probe::microsecondsSince(g_lastTimerTick);
        if (gap && gap < g_timerGapMinUs) g_timerGapMinUs = gap;
        InterlockedIncrement(&g_timerSamples);
    }
    g_lastTimerTick = now;
    if (InterlockedCompareExchange(&g_timerKnown, 1, 0) == 0) {
        g_timerWindow = message->hwnd;
        g_timerId = message->wParam;
        g_timerProc = (uintptr_t)message->lParam;
    }
}

void noteMessage(UINT id, bool slow) {
    // +1 so a real WM_NULL (0) is distinguishable from an unclaimed slot.
    const LONG key = (LONG)id + 1;
    unsigned slot = (id * 2654435761u) % kMessageKinds;
    for (unsigned probe = 0; probe < kMessageKinds; ++probe) {
        volatile LONG* cell = &g_messageId[(slot + probe) % kMessageKinds];
        LONG held = InterlockedCompareExchange(cell, key, 0);
        if (held == 0 || held == key) {
            const unsigned at = (slot + probe) % kMessageKinds;
            InterlockedIncrement(&g_messageCount[at]);
            if (slow) InterlockedIncrement(&g_messageSlow[at]);
            return;
        }
    }
}

struct MessageName { UINT id; const char* name; };
const MessageName kMessageNames[] = {
    {0x0003, "MOVE"}, {0x0005, "SIZE"}, {0x0006, "ACTIVATE"},
    {0x0007, "SETFOCUS"}, {0x0008, "KILLFOCUS"}, {0x000f, "PAINT"},
    {0x0014, "ERASEBKGND"}, {0x001c, "ACTIVATEAPP"}, {0x0046, "WINPOSCHANGING"},
    {0x0047, "WINPOSCHANGED"}, {0x007e, "DISPLAYCHANGE"}, {0x0084, "NCHITTEST"},
    {0x00a0, "NCMOUSEMOVE"}, {0x00ff, "INPUT"}, {0x0100, "KEYDOWN"},
    {0x0101, "KEYUP"}, {0x0102, "CHAR"}, {0x0104, "SYSKEYDOWN"},
    {0x0112, "SYSCOMMAND"}, {0x0113, "TIMER"}, {0x0020, "SETCURSOR"},
    {0x0200, "MOUSEMOVE"}, {0x0201, "LBUTTONDOWN"}, {0x0202, "LBUTTONUP"},
    {0x0204, "RBUTTONDOWN"}, {0x0205, "RBUTTONUP"}, {0x020a, "MOUSEWHEEL"},
    {0x0219, "DEVICECHANGE"}, {0x0281, "IME_SETCONTEXT"},
};

void reportMessages() {
    unsigned total = 0;
    for (unsigned i = 0; i < kMessageKinds; ++i)
        total += (unsigned)g_messageCount[i];
    if (!total) return;
    // Most frequent first, at most twelve kinds; a game window sees far fewer
    // than that, so the list is the whole truth rather than a sample. Slots
    // are struck off as they are printed rather than filtered by a descending
    // count, which would silently drop ties.
    bool done[kMessageKinds] = {};
    for (unsigned printed = 0; printed < 12; ++printed) {
        int best = -1;
        for (unsigned i = 0; i < kMessageKinds; ++i)
            if (g_messageId[i] && !done[i]
                && (best < 0 || g_messageCount[i] > g_messageCount[best]))
                best = (int)i;
        if (best < 0) break;
        done[best] = true;
        const UINT id = (UINT)(g_messageId[best] - 1);
        const char* name = "?";
        for (unsigned k = 0; k < sizeof(kMessageNames) / sizeof(*kMessageNames); ++k)
            if (kMessageNames[k].id == id) { name = kMessageNames[k].name; break; }
        tq::hdr::log("Engine trace: pump message 0x%04x %-14s x%-7ld slow %ld\r\n",
                     id, name, g_messageCount[best], g_messageSlow[best]);
    }
    tq::hdr::log("Engine trace: %u window messages so far\r\n", total);
    if (g_timerKnown)
        tq::hdr::log("Engine trace: game timer hwnd=%p id=%u proc=%p"
                     " shortest gap %u us over %ld samples\r\n",
                     (void*)g_timerWindow, (unsigned)g_timerId,
                     (void*)g_timerProc, g_timerGapMinUs, g_timerSamples);
    // Re-arm once, and only once, and only when asked. SetTimer on an (hwnd,
    // id) pair that already exists replaces its period and leaves everything
    // else alone, so the TIMERPROC the message carried is passed straight
    // back rather than cleared -- passing null there would turn a callback
    // timer into a posted one and change what the game does, not just when.
    // It has to run on the thread that owns the window, and this does: the
    // pump is the main thread.
    // ...but only for a WINDOW timer. Run 20 found this one has hwnd = NULL,
    // which makes it a *thread* timer, and SetTimer ignores the id for those:
    // it would create a second timer rather than re-periodise this one, and
    // hand it the lParam we observed (0xFFFF0016) as a TIMERPROC to call.
    // That is not an experiment, it is a crash. Refuse rather than run it.
    if (g_timerPeriodMs && g_timerKnown && !g_timerWindow
        && InterlockedCompareExchange(&g_timerRearmed, 1, 0) == 0) {
        tq::hdr::log("Engine trace: timer_period_ms=%u ignored -- the game's"
                     " timer has no window, so SetTimer would create a second"
                     " one rather than change this one\r\n", g_timerPeriodMs);
    }
    if (g_timerPeriodMs && g_timerKnown && g_timerWindow && g_setTimer
        && InterlockedCompareExchange(&g_timerRearmed, 1, 0) == 0) {
        const UINT_PTR again = g_setTimer(g_timerWindow, g_timerId,
                                          g_timerPeriodMs,
                                          (TIMERPROC)g_timerProc);
        tq::hdr::log("Engine trace: re-armed the game timer at %u ms -> %s\r\n",
                     g_timerPeriodMs, again ? "ok" : "FAILED");
    }
}


void __fastcall hookEngineRender(void* self, void* edx) {
    if (!g_engineRender) return;
    // Engine::Render is once a frame, which makes it the cheapest clock this
    // module has. The histogram is written from here rather than from
    // shutdown() because Titan Quest exits without unloading: `reserved` is
    // set at DLL_PROCESS_DETACH, so only probe::flushOnExit runs and nothing
    // in this file's teardown is ever reached. Roughly every thirty seconds,
    // so the last snapshot covers almost the whole session.
    if (++g_renderTicks % 1800 == 0) {
        reportMessages();
        reportSlowLoads();
    }
    const int64_t started = tq::probe::now();
    if (g_outsideDirResourceTracing)
        InterlockedIncrement(&g_insideEngineRender);
    g_engineRender(self, edx);
    if (g_outsideDirResourceTracing)
        InterlockedDecrement(&g_insideEngineRender);
    tq::probe::engineCount(tq::probe::CounterEngineRender);
    tq::probe::engineCount(tq::probe::CounterEngineRenderUs,
                           tq::probe::microsecondsSince(started));
}
SleepFn g_loopSleep;
GetMessageFn g_loopGetMessage;
WaitFn g_loopWait;
CallPatch g_loopSleepPatch;
CallPatch g_loopMessagePatch;
CallPatch g_loopWaitPatch;

void __stdcall hookLoopSleep(DWORD milliseconds) {
    if (!g_loopSleep) return;
    const int64_t started = tq::probe::now();
    g_loopSleep(milliseconds);
    tq::probe::engineCount(tq::probe::CounterLoopSleep);
    // Saturating rather than wrapping, so an INFINITE or a very long request
    // reads as "longer than this can express" instead of as a short one.
    const uint32_t requested = milliseconds > 4294967u ? 0xffffffffu
                                                       : milliseconds * 1000u;
    tq::probe::engineCount(tq::probe::CounterLoopSleepRequestedUs, requested);
    tq::probe::engineCount(tq::probe::CounterLoopSleepUs,
                           tq::probe::microsecondsSince(started));
}

BOOL __stdcall hookLoopGetMessage(LPMSG message, HWND window, UINT first,
                                  UINT last) {
    if (!g_loopGetMessage) return FALSE;
    const int64_t started = tq::probe::now();
    const BOOL result = g_loopGetMessage(message, window, first, last);
    tq::probe::engineCount(tq::probe::CounterLoopMessage);
    tq::probe::engineCount(tq::probe::CounterLoopMessageUs,
                           tq::probe::microsecondsSince(started));
    return result;
}
PresentSurfaceFn g_presentSurface;
CollisionsFn g_collisions;
CallPatch g_presentSurfacePatch;
CallPatch g_collisionsPatch;

void __fastcall hookPresentSurface(void* self, void* edx) {
    if (!g_presentSurface) return;
    const int64_t started = tq::probe::now();
    g_presentSurface(self, edx);
    tq::probe::engineCount(tq::probe::CounterEnginePresentSurface);
    tq::probe::engineCount(tq::probe::CounterEnginePresentSurfaceUs,
                           tq::probe::microsecondsSince(started));
}
PlatformProcessFn g_platform;
ThisVoidFn g_gfxOptions;
ThisVoidFn g_jukebox;
SoundUpdateFn g_sound;
ThisVoidFn g_quests;
PumpFn g_pump;
CallPatch g_platformPatch, g_gfxOptionsPatch, g_jukeboxPatch;
CallPatch g_soundPatch, g_questsPatch, g_pumpPatch;

// The game's timer, logged and optionally re-periodised. Hooking SetTimer
// costs nothing -- it is called a handful of times in a session -- and the
// first few calls say what the period actually is, which nothing has ever
// reported.
UINT_PTR __stdcall hookSetTimer(HWND window, UINT_PTR id, UINT elapse,
                                TIMERPROC callback) {
    if (!g_setTimer) return 0;
    const UINT period = g_timerPeriodMs ? g_timerPeriodMs : elapse;
    if (InterlockedIncrement(&g_setTimerCalls) <= 8)
        tq::hdr::log("Engine trace: SetTimer(hwnd=%p id=%u elapse=%u proc=%p)"
                     " -> period %u\r\n", (void*)window, (unsigned)id, elapse,
                     (void*)callback, period);
    return g_setTimer(window, id, period, callback);
}

void __stdcall hookPlatform(void) {
    if (!g_platform) return;
    const int64_t started = tq::probe::now();
    g_platform();
    tq::probe::engineCount(tq::probe::CounterLoopPlatform);
    tq::probe::engineCount(tq::probe::CounterLoopPlatformUs,
                           tq::probe::microsecondsSince(started));
}

void __fastcall hookGfxOptions(void* self, void* edx) {
    if (!g_gfxOptions) return;
    const int64_t started = tq::probe::now();
    g_gfxOptions(self, edx);
    tq::probe::engineCount(tq::probe::CounterLoopGfxOptions);
    tq::probe::engineCount(tq::probe::CounterLoopGfxOptionsUs,
                           tq::probe::microsecondsSince(started));
}

void __fastcall hookJukebox(void* self, void* edx) {
    if (!g_jukebox) return;
    const int64_t started = tq::probe::now();
    g_jukebox(self, edx);
    tq::probe::engineCount(tq::probe::CounterLoopJukebox);
    tq::probe::engineCount(tq::probe::CounterLoopJukeboxUs,
                           tq::probe::microsecondsSince(started));
}

void __fastcall hookSound(void* self, void* edx, const void* frustum) {
    if (!g_sound) return;
    const int64_t started = tq::probe::now();
    g_sound(self, edx, frustum);
    tq::probe::engineCount(tq::probe::CounterLoopSound);
    tq::probe::engineCount(tq::probe::CounterLoopSoundUs,
                           tq::probe::microsecondsSince(started));
}

void __fastcall hookQuests(void* self, void* edx) {
    if (!g_quests) return;
    const int64_t started = tq::probe::now();
    g_quests(self, edx);
    tq::probe::engineCount(tq::probe::CounterLoopQuests);
    tq::probe::engineCount(tq::probe::CounterLoopQuestsUs,
                           tq::probe::microsecondsSince(started));
}

// The loop's own condition: it runs while this returns true, so returning
// anything else on a missing trampoline would end the game. There is no
// trampoline-missing case while installed, and the guard returns 1 rather
// than 0 for that reason.
int __fastcall hookPump(void* self, void* edx) {
    if (!g_pump) return 1;
    const int64_t started = tq::probe::now();
    const int result = g_pump(self, edx);
    tq::probe::engineCount(tq::probe::CounterLoopPump);
    tq::probe::engineCount(tq::probe::CounterLoopPumpUs,
                           tq::probe::microsecondsSince(started));
    return result;
}
PeekMessageFn g_peekMessage;
DispatchMessageFn g_dispatchMessage;
CallPatch g_peekPatch, g_dispatchPatch;

// The peek the game makes, with WM_TIMER kept out of range. Two calls rather
// than one because a message range is contiguous: everything below the timer,
// then everything above it. Both are misses in the common case and a miss
// measured 1 us, so the second call is not a second round trip in any sense
// that matters.
BOOL peekAroundTimer(LPMSG message, HWND window, UINT remove) {
    if (g_peekMessage(message, window, 0, WM_TIMER - 1, remove)) return TRUE;
    return g_peekMessage(message, window, WM_TIMER + 1, 0xffffffffu, remove);
}

BOOL __stdcall hookPeekMessage(LPMSG message, HWND window, UINT first,
                               UINT last, UINT remove) {
    if (!g_peekMessage) return FALSE;
    // The marker must reuse the game's input retrieval. Run 40 polled
    // GetAsyncKeyState separately at each Present and created a new class of
    // 100-230 ms stalls in the otherwise unbracketed part of the frame.
    const bool timePeek = g_pumpTracing;
    const int64_t started = timePeek ? tq::probe::now() : 0;
    BOOL result;
    // Only the unfiltered peek is touched. A caller that already asked for a
    // range knows what it wants, and splitting it would change which messages
    // it can see.
    if (g_pumpTimerMinMs && !first && !last) {
        const LONG now = (LONG)GetTickCount();
        if ((LONG)(now - g_pumpLastFullTick) >= (LONG)g_pumpTimerMinMs) {
            g_pumpLastFullTick = now;
            tq::probe::engineCount(tq::probe::CounterPumpTimerFull);
            result = g_peekMessage(message, window, first, last, remove);
        } else {
            tq::probe::engineCount(tq::probe::CounterPumpTimerSplit);
            result = peekAroundTimer(message, window, remove);
        }
    } else {
        result = g_peekMessage(message, window, first, last, remove);
    }
    if (result && message
        && (message->message == WM_KEYDOWN || message->message == WM_SYSKEYDOWN)
        && message->wParam == VK_F12
        && (message->lParam & (LPARAM(1) << 30)) == 0) {
        reportOutsideDirResourcesAtMarker();
        reportShadowMeshResourcesAtMarker();
        reportDeferredSlowDrawsAtMarker();
        reportCrossPassBuffersAtMarker();
        reportOffMainTexturesAtMarker();
        reportGpuChunksAtMarker();
        tq::probe::markStutter();
    }
    if (!timePeek) return result;

    const uint32_t elapsed = tq::probe::microsecondsSince(started);
    tq::probe::engineCount(tq::probe::CounterPumpPeek);
    tq::probe::engineCount(tq::probe::CounterPumpPeekUs, elapsed);
    if (!result) {
        // The queue was empty. Time spent here is the cost of asking.
        tq::probe::engineCount(tq::probe::CounterPumpPeekMiss);
        tq::probe::engineCount(tq::probe::CounterPumpPeekMissUs, elapsed);
    } else if (message) {
        noteMessage(message->message, elapsed > 5000u);
        if (message->message == WM_TIMER) noteTimer(message);
    }
    return result;
}

LRESULT __stdcall hookDispatchMessage(const MSG* message) {
    if (!g_dispatchMessage) return 0;
    const int64_t started = tq::probe::now();
    const LRESULT result = g_dispatchMessage(message);
    tq::probe::engineCount(tq::probe::CounterPumpDispatch);
    tq::probe::engineCount(tq::probe::CounterPumpDispatchUs,
                           tq::probe::microsecondsSince(started));
    return result;
}

void __fastcall hookCollisions(void* self, void* edx, const void* camera) {
    if (!g_collisions) return;
    const int64_t started = tq::probe::now();
    g_collisions(self, edx, camera);
    tq::probe::engineCount(tq::probe::CounterGameCollisions);
    tq::probe::engineCount(tq::probe::CounterGameCollisionsUs,
                           tq::probe::microsecondsSince(started));
}

DWORD __stdcall hookLoopWait(HANDLE handle, DWORD milliseconds) {
    if (!g_loopWait) return WAIT_FAILED;
    const int64_t started = tq::probe::now();
    const DWORD result = g_loopWait(handle, milliseconds);
    tq::probe::engineCount(tq::probe::CounterLoopWait);
    tq::probe::engineCount(tq::probe::CounterLoopWaitUs,
                           tq::probe::microsecondsSince(started));
    return result;
}

// Engine.dll's `operator new[]` and `operator delete[]`. The `!g_...` guard
// is unreachable by construction rather than defensive: redirectImport
// publishes the original before it patches the slot, and shutdown() restores
// the slot before it clears the pointer, so there is no window in which the
// hook is live and the target is null.
//
// Everything is counted; `_big` is timed separately as well, because the
// question is not "does the engine allocate" but "does it allocate a
// quarter-megabyte buffer thousands of times in one frame".
void* __cdecl hookNewArray(size_t bytes) {
    if (!g_newArray) return nullptr;
    const int64_t started = tq::probe::now();
    void* block = g_newArray(bytes);
    const uint32_t elapsed = tq::probe::microsecondsSince(started);
    tq::probe::engineCount(tq::probe::CounterEngineHeapAlloc);
    tq::probe::engineCount(tq::probe::CounterEngineHeapAllocUs, elapsed);
    tq::probe::engineCount(tq::probe::CounterEngineHeapAllocKib,
                           (uint32_t)((bytes + 1023) >> 10));
    if (bytes >= kHeapBigBytes) {
        tq::probe::engineCount(tq::probe::CounterEngineHeapBig);
        tq::probe::engineCount(tq::probe::CounterEngineHeapBigUs, elapsed);
    }
    return block;
}

void __cdecl hookDeleteArray(void* block) {
    if (!g_deleteArray) return;
    const int64_t started = tq::probe::now();
    g_deleteArray(block);
    tq::probe::engineCount(tq::probe::CounterEngineHeapFree);
    tq::probe::engineCount(tq::probe::CounterEngineHeapFreeUs,
                           tq::probe::microsecondsSince(started));
}

// The seek and the read the block routine makes, so that
// engine_arc_inflate_us -- which brackets all three -- can have the inflate
// recovered from it by subtraction.
BOOL WINAPI hookSetFilePointerEx(HANDLE file, LARGE_INTEGER distance,
                                 PLARGE_INTEGER newPointer, DWORD method) {
    if (!g_setFilePointerEx) return FALSE;
    const int64_t started = tq::probe::now();
    const BOOL result = g_setFilePointerEx(file, distance, newPointer, method);
    tq::probe::engineCount(tq::probe::CounterEngineIoSeek);
    tq::probe::engineCount(tq::probe::CounterEngineIoSeekUs,
                           tq::probe::microsecondsSince(started));
    return result;
}

BOOL WINAPI hookReadFile(HANDLE file, LPVOID buffer, DWORD bytes,
                         LPDWORD read, LPOVERLAPPED overlapped) {
    if (!g_readFile) return FALSE;
    const int64_t started = tq::probe::now();
    const BOOL result = g_readFile(file, buffer, bytes, read, overlapped);
    tq::probe::engineCount(tq::probe::CounterEngineIoRead);
    tq::probe::engineCount(tq::probe::CounterEngineIoReadUs,
                           tq::probe::microsecondsSince(started));
    tq::probe::engineCount(tq::probe::CounterEngineIoReadKib,
                           (bytes + 1023u) >> 10);
    return result;
}

// Every critical section in Engine.dll, not just the three render-path sites
// the region-lock group covers. Contended acquisitions only: TryEnter first,
// and a timestamp is taken solely when it fails, so an uncontended lock costs
// one interlocked operation and records nothing. That is what makes this
// affordable on a path the module takes constantly -- including the archive's
// own `archive+0x60`, held across every block read and never measured.
//
// Disjoint from engine_region_lock_* by construction: patchCall repointed
// those three sites at a mod-owned cell, so they no longer read this slot.
void __stdcall hookEngineEnterCriticalSection(LPCRITICAL_SECTION section) {
    if (TryEnterCriticalSection(section)) return;
    const int64_t started = tq::probe::now();
    EnterCriticalSection(section);
    const uint32_t elapsed = tq::probe::microsecondsSince(started);
    tq::probe::engineCount(tq::probe::CounterEngineCsWait);
    tq::probe::engineCount(tq::probe::CounterEngineCsWaitUs, elapsed);
    if (onMainThread()) {
        tq::probe::engineCount(tq::probe::CounterEngineCsWaitMain);
        tq::probe::engineCount(tq::probe::CounterEngineCsWaitMainUs, elapsed);
    }
}

// Both waits fold into one pair of columns, and both are split by thread:
// unattributed they sum across every thread in the process and read larger
// than wall clock, which is what run 25 found.
void countObjectWait(uint32_t elapsed) {
    tq::probe::engineCount(tq::probe::CounterEngineObjWait);
    tq::probe::engineCount(tq::probe::CounterEngineObjWaitUs, elapsed);
    if (onMainThread()) {
        tq::probe::engineCount(tq::probe::CounterEngineObjWaitMain);
        tq::probe::engineCount(tq::probe::CounterEngineObjWaitMainUs, elapsed);
    }
}

DWORD WINAPI hookEngineWait(HANDLE handle, DWORD milliseconds) {
    if (!g_engineWait) return WAIT_FAILED;
    const int64_t started = tq::probe::now();
    const DWORD result = g_engineWait(handle, milliseconds);
    countObjectWait(tq::probe::microsecondsSince(started));
    return result;
}

DWORD WINAPI hookEngineWaitMultiple(DWORD count, const HANDLE* handles,
                                    BOOL all, DWORD milliseconds) {
    if (!g_engineWaitMultiple) return WAIT_FAILED;
    const int64_t started = tq::probe::now();
    const DWORD result = g_engineWaitMultiple(count, handles, all, milliseconds);
    countObjectWait(tq::probe::microsecondsSince(started));
    return result;
}

// The requested total beside the actual, for the main thread only. A poll
// loop that asks for a millisecond four hundred times and is handed two and a
// half each time is a host-granularity problem and `timeBeginPeriod` can
// reach it; one that asks for four hundred milliseconds is the game's own and
// cannot be fixed from here. The two columns say which immediately, and this
// is the same shape as the loop_sleep_req_us pair added in run 13.
void WINAPI hookEngineSleep(DWORD milliseconds) {
    if (!g_engineSleep) return;
    const int64_t started = tq::probe::now();
    g_engineSleep(milliseconds);
    const uint32_t elapsed = tq::probe::microsecondsSince(started);
    tq::probe::engineCount(tq::probe::CounterEngineSleep);
    tq::probe::engineCount(tq::probe::CounterEngineSleepUs, elapsed);
    if (onMainThread()) {
        tq::probe::engineCount(tq::probe::CounterEngineSleepMain);
        tq::probe::engineCount(tq::probe::CounterEngineSleepMainUs, elapsed);
        tq::probe::engineCount(tq::probe::CounterEngineSleepMainReqUs,
                               milliseconds * 1000u);
    }
}

void __fastcall hookGameUpdate(void* self, void* edx, int delta) {
    if (!g_gameUpdate) return;
    const int64_t started = tq::probe::now();
    g_gameUpdate(self, edx, delta);
    tq::probe::engineCount(tq::probe::CounterGameUpdate);
    tq::probe::engineCount(tq::probe::CounterGameUpdateUs,
                           tq::probe::microsecondsSince(started));
}

// ---------------------------------------------------------------------------
// Install.

// The trace's own gate, decided once per install() so that wants() below
// cannot be asked the question before it has been answered.
void decideTracing() { g_tracing = tq::probe::enabled() && g_traceMask != 0; }

bool wants(unsigned group) {
    if (!g_tracing) return false;
    return (g_traceMask & kGroupAll) != 0 || (g_traceMask & group) != 0;
}

bool verifyShadowTextureDirectCallers(HMODULE engine, const void* getter) {
    if (!engine || !getter) return false;
    for (unsigned i = 0;
         i < sizeof(kShadowTextureDirectCallerRvas)
                    / sizeof(*kShadowTextureDirectCallerRvas);
         ++i) {
        const BYTE* const call =
            (const BYTE*)engine + kShadowTextureDirectCallerRvas[i];
        if (!tq::detour::readable(call, 5) || call[0] != 0xe8)
            return false;
        int32_t displacement = 0;
        memcpy(&displacement, call + 1, sizeof(displacement));
        if (call + 5 + displacement != getter) return false;
    }
    return true;
}

// Every attach here hands the detour the global the hook calls through, so the
// trampoline is published before the entry is patched rather than after.
bool installLoads(HMODULE engine) {
    g_resourceStateVerified = verifyResourceStateLayout(engine);
    void* target = resolve(engine, kLoadLevelName, kLoadLevelRva);
    if (target)
        tq::detour::attach(g_loadLevelDetour, engine, target,
                           signature(kLoadLevelBytes, sizeof(kLoadLevelBytes)),
                           6, (const void*)&hookLoadLevel,
                           (void**)&g_loadLevel);
    note("Region::LoadLevel", g_loadLevel != nullptr);

    target = resolve(engine, kGuaranteedGetLevelName, kGuaranteedGetLevelRva);
    if (target)
        tq::detour::attach(
            g_guaranteedDetour, engine, target,
            signature(kGuaranteedGetLevelBytes, sizeof(kGuaranteedGetLevelBytes)),
            6, (const void*)&hookGuaranteedGetLevel,
            (void**)&g_guaranteedGetLevel);
    note("Region::GuaranteedGetLevel", g_guaranteedGetLevel != nullptr);

    target = resolve(engine, kLoadResourceName, kLoadResourceRva);
    if (target)
        tq::detour::attach(
            g_loadResourceDetour, engine, target,
            signature(kLoadResourceBytes, sizeof(kLoadResourceBytes),
                      kLoadResourceRelocs, 1),
            7, (const void*)&hookLoadResource, (void**)&g_loadResource);
    note("ResourceLoader::LoadResource", g_loadResource != nullptr);

    target = resolve(engine, kUnloadLevelName, kUnloadLevelRva);
    if (target)
        tq::detour::attach(
            g_unloadLevelDetour, engine, target,
            signature(kUnloadLevelBytes, sizeof(kUnloadLevelBytes),
                      kUnloadLevelRelocs, 2),
            7, (const void*)&hookUnloadLevel, (void**)&g_unloadLevel);
    note("Region::UnloadLevel", g_unloadLevel != nullptr);

    target = resolve(engine, kEnqueueName, kEnqueueRva);
    if (target)
        tq::detour::attach(
            g_enqueueDetour, engine, target,
            signature(kEnqueueBytes, sizeof(kEnqueueBytes), kEnqueueRelocs, 1),
            7, (const void*)&hookEnqueueResource, (void**)&g_enqueue);
    note("ResourceLoader::EnqueueResource", g_enqueue != nullptr);
    return true;
}

// The one check that can fail for a reason other than "different build": the
// call sites go through the import table, and patchCall compares what that
// slot holds against what kernel32 exports today. If a loader ever bound the
// slot to a forwarder those two differ, the patch is refused, and the column
// reads zero -- so say which pointer disagreed rather than only that the hook
// was skipped.
void* importedKernelFunction(HMODULE engine, const char* name, DWORD slotRva) {
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    void* exported = kernel ? (void*)GetProcAddress(kernel, name) : nullptr;
    void* const* slot = (void* const*)((BYTE*)engine + slotRva);
    void* bound = tq::detour::readable(slot, sizeof(*slot)) ? *slot : nullptr;
    if (exported && exported == bound) return exported;
    tq::hdr::log("Engine trace: %s import slot holds %p, kernel32 exports %p\r\n",
                 name, bound, exported);
    return nullptr;
}

bool installFence(HMODULE engine) {
    void* wait = importedKernelFunction(engine, "WaitForSingleObject",
                                        kWaitForSingleObjectSlotRva);
    const bool ok = resolve(engine, kEngineUpdateName, kEngineUpdateRva) && wait
        && tq::detour::patchCall(
               g_fencePatch, engine, (BYTE*)engine + kFenceWindowRva,
               signature(kFenceWindowBytes, sizeof(kFenceWindowBytes),
                         kFenceWindowRelocs, 2),
               kFenceCallOffset, wait, (const void*)&hookWaitForSingleObject);
    note("Engine::Update loader fence", ok);
    return ok;
}

bool installRegionLock(HMODULE engine) {
    void* enter = importedKernelFunction(engine, "EnterCriticalSection",
                                         kEnterCriticalSectionSlotRva);
    unsigned installed = 0;
    for (unsigned i = 0; enter && i < kLockSiteCount; ++i) {
        const LockSite& site = kLockSites[i];
        if (!resolve(engine, site.owner, site.ownerRva)) continue;
        if (tq::detour::patchCall(
                g_lockPatches[i], engine, (BYTE*)engine + site.windowRva,
                signature(site.bytes, sizeof(kRegionLockEbxBytes),
                          kRegionLockRelocs, 2),
                kRegionLockCallOffset, enter,
                (const void*)&hookEnterCriticalSection))
            ++installed;
    }
    tq::hdr::log("Engine trace: region lock %u/%u sites installed\r\n",
                 installed, kLockSiteCount);
    if (installed) ++g_installedHooks;
    return installed != 0;
}

bool installSweeps(HMODULE engine) {
    void* target = resolve(engine, kSweepTargetName, kSweepTargetRva);
    if (!target || !resolve(engine, kEngineUpdateName, kEngineUpdateRva)) {
        note("Engine::Update resource sweeps", false);
        return false;
    }
    // Both windows whole, before anything is written to either.
    if (!tq::detour::matches(
            engine, (BYTE*)engine + kSweepWindowARva,
            signature(kSweepWindowABytes, sizeof(kSweepWindowABytes)))
        || !tq::detour::matches(
            engine, (BYTE*)engine + kSweepWindowBRva,
            signature(kSweepWindowBBytes, sizeof(kSweepWindowBBytes)))) {
        note("Engine::Update resource sweeps", false);
        return false;
    }
    g_sweep = (SweepFn)target;
    unsigned installed = 0;
    for (unsigned i = 0; i < kSweepCount; ++i) {
        const SweepSite& site = kSweepSites[i];
        BYTE* call = (BYTE*)engine + site.windowRva + site.callOffset;
        if (tq::detour::patchCall(g_sweepPatches[i], engine, call,
                                  signature(site.window + site.callOffset, 5), 0,
                                  target, (const void*)&hookSweep))
            ++installed;
    }
    tq::hdr::log("Engine trace: resource sweeps %u/%u sites installed\r\n",
                 installed, kSweepCount);
    if (installed) ++g_installedHooks;
    else g_sweep = nullptr;
    return installed != 0;
}

// A module the stack scan will name frames in. Admitted only if it is the
// audited build -- an RVA into an unrecognised binary would be a number
// without a meaning, and this file's whole discipline is that RVAs mean
// something. A module that is absent or unaudited is skipped, and the scan
// keeps working with the ones that are there.
void addChainModule(HMODULE module, DWORD expectedSize, char tag,
                    const char* what) {
    if (g_chainModuleCount >= kChainModules || !module) return;
    if (!auditedImage(module, expectedSize, what)) return;
    BYTE* text = nullptr;
    SIZE_T size = 0;
    if (!tq::detour::moduleText(module, &text, &size)) return;
    ChainModule& entry = g_chainModules[g_chainModuleCount++];
    entry.tag = tag;
    entry.base = (const BYTE*)module;
    entry.text = text;
    entry.textSize = size;
    tq::hdr::log("Engine trace: chain scan covers %c = %s, .text %p+%#lx\r\n",
                 tag, what, (void*)text, (unsigned long)size);
}

bool installGame() {
    HMODULE game = GetModuleHandleW(L"Game.dll");
    if (!game || !auditedImage(game, kGameImageSize, "Game.dll")) {
        note("GameEngine::Update", false);
        return false;
    }
    void* target = resolve(game, kGameUpdateName, kGameUpdateRva);
    if (target)
        tq::detour::attach(
            g_gameUpdateDetour, game, target,
            signature(kGameUpdateBytes, sizeof(kGameUpdateBytes),
                      kGameUpdateRelocs, 1),
            6, (const void*)&hookGameUpdate, (void**)&g_gameUpdate);
    note("GameEngine::Update", g_gameUpdate != nullptr);
    return g_gameUpdate != nullptr;
}

// One import slot, with the original captured before the slot is rewritten so
// the hook always has something to call through.
bool redirectImport(CallPatch& patch, HMODULE executable, const char* dll,
                    const char* name, void** original, const void* replacement) {
    HMODULE provider = GetModuleHandleA(dll);
    void* target = provider ? (void*)GetProcAddress(provider, name) : nullptr;
    if (!target) {
        tq::hdr::log("Engine trace: %s!%s is not resolvable\r\n", dll, name);
        return false;
    }
    *original = target;
    if (tq::detour::patchImport(patch, executable, dll, name, target,
                                replacement))
        return true;
    *original = nullptr;
    tq::hdr::log("Engine trace: the %s import does not hold %p\r\n",
                 name, target);
    return false;
}

bool installLoop() {
    HMODULE executable = GetModuleHandleW(nullptr);
    if (!executable
        || !auditedImage(executable, kExecutableImageSize, "TQ.exe")) {
        note("TQ.exe main loop", false);
        return false;
    }
    unsigned installed = 0;
    installed += redirectImport(g_loopSleepPatch, executable, "kernel32.dll",
                                "Sleep", (void**)&g_loopSleep,
                                (const void*)&hookLoopSleep) ? 1u : 0u;
    installed += redirectImport(g_loopMessagePatch, executable, "user32.dll",
                                "GetMessageA", (void**)&g_loopGetMessage,
                                (const void*)&hookLoopGetMessage) ? 1u : 0u;
    installed += redirectImport(g_loopWaitPatch, executable, "kernel32.dll",
                                "WaitForSingleObject", (void**)&g_loopWait,
                                (const void*)&hookLoopWait) ? 1u : 0u;
    // The two calls the loop makes that do real work and sit in none of the
    // brackets. PresentSurface is the one the loop's shape points at.
    installed += redirectImport(g_presentSurfacePatch, executable, "Engine.dll",
                                "?PresentSurface@Engine@GAME@@QAEXXZ",
                                (void**)&g_presentSurface,
                                (const void*)&hookPresentSurface) ? 1u : 0u;
    installed += redirectImport(
        g_collisionsPatch, executable, "Game.dll",
        "?FixupCharacterCollisions@InterpenetrationManager@GAME@@QAEXABV"
        "GameCamera@2@@Z",
        (void**)&g_collisions, (const void*)&hookCollisions) ? 1u : 0u;
    installed += redirectImport(g_platformPatch, executable, "thqno_api.dll",
                                "THQNO_Process", (void**)&g_platform,
                                (const void*)&hookPlatform) ? 1u : 0u;
    installed += redirectImport(
        g_gfxOptionsPatch, executable, "Engine.dll",
        "?UpdateFromOptions@GraphicsEngine@GAME@@QAEXXZ",
        (void**)&g_gfxOptions, (const void*)&hookGfxOptions) ? 1u : 0u;
    installed += redirectImport(g_jukeboxPatch, executable, "Engine.dll",
                                "?Update@Jukebox@GAME@@QAEXXZ",
                                (void**)&g_jukebox,
                                (const void*)&hookJukebox) ? 1u : 0u;
    installed += redirectImport(
        g_soundPatch, executable, "Engine.dll",
        "?Update@SoundManager@GAME@@QAEXPBVWorldFrustum@2@@Z",
        (void**)&g_sound, (const void*)&hookSound) ? 1u : 0u;
    installed += redirectImport(g_questsPatch, executable, "Game.dll",
                                "?FireTriggers@QuestRepository@GAME@@QAEXXZ",
                                (void**)&g_quests,
                                (const void*)&hookQuests) ? 1u : 0u;
    installed += redirectImport(g_pumpPatch, executable, "Engine.dll",
                                "?ProcessMessages@EWindow@GAME@@QAE_NXZ",
                                (void**)&g_pump,
                                (const void*)&hookPump) ? 1u : 0u;
    installed += redirectImport(g_setTimerPatch, executable, "user32.dll",
                                "SetTimer", (void**)&g_setTimer,
                                (const void*)&hookSetTimer) ? 1u : 0u;
    tq::hdr::log("Engine trace: TQ.exe main loop %u/12 imports redirected"
                 " (timer_period_ms=%u)\r\n", installed, g_timerPeriodMs);
    if (installed) ++g_installedHooks;
    return installed != 0;
}

bool installPump(HMODULE engine, bool tracePump) {
    g_pumpTracing = tracePump;
    unsigned installed = 0;
    // PeekMessageA carries the filter as well as the timing, so it goes in for
    // either reason; DispatchMessageA is pure instrument and stays behind the
    // trace. Engine.dll's slot is the one the game's pump calls through.
    installed += redirectImport(g_peekPatch, engine, "user32.dll",
                                "PeekMessageA", (void**)&g_peekMessage,
                                (const void*)&hookPeekMessage) ? 1u : 0u;
    if (tracePump)
        installed += redirectImport(g_dispatchPatch, engine, "user32.dll",
                                    "DispatchMessageA",
                                    (void**)&g_dispatchMessage,
                                    (const void*)&hookDispatchMessage)
                                        ? 1u : 0u;
    tq::hdr::log("Engine trace: message pump %u imports redirected"
                 " (pump_timer_min_ms=%u)\r\n", installed, g_pumpTimerMinMs);
    if (installed) ++g_installedHooks;
    return installed != 0;
}

// Both of these patch four bytes of a data table and no code at all, and are
// scoped to Engine.dll -- the mod's own allocations and the game's other
// modules keep the real functions. The RVA beside each name is an identity
// assertion: patchImport resolves by name, and this says the slot it found is
// the slot the audit recorded.
bool importedSlotHolds(HMODULE engine, const char* dll, const char* name,
                       DWORD slotRva) {
    HMODULE provider = GetModuleHandleA(dll);
    void* exported = provider ? (void*)GetProcAddress(provider, name) : nullptr;
    void* const* slot = (void* const*)((BYTE*)engine + slotRva);
    void* bound = tq::detour::readable(slot, sizeof(*slot)) ? *slot : nullptr;
    if (exported && exported == bound) return true;
    tq::hdr::log("Engine trace: %s slot at +%#lx holds %p, %s exports %p\r\n",
                 name, (unsigned long)slotRva, bound, dll, exported);
    return false;
}

bool installHeap(HMODULE engine) {
    unsigned installed = 0;
    if (importedSlotHolds(engine, "MSVCR110.dll", kNewArrayName,
                          kNewArraySlotRva))
        installed += redirectImport(g_newArrayPatch, engine, "MSVCR110.dll",
                                    kNewArrayName, (void**)&g_newArray,
                                    (const void*)&hookNewArray) ? 1u : 0u;
    if (importedSlotHolds(engine, "MSVCR110.dll", kDeleteArrayName,
                          kDeleteArraySlotRva))
        installed += redirectImport(g_deleteArrayPatch, engine, "MSVCR110.dll",
                                    kDeleteArrayName, (void**)&g_deleteArray,
                                    (const void*)&hookDeleteArray) ? 1u : 0u;
    tq::hdr::log("Engine trace: array allocator %u/2 imports redirected\r\n",
                 installed);
    if (installed) ++g_installedHooks;
    return installed != 0;
}

bool installArchiveIo(HMODULE engine) {
    unsigned installed = 0;
    if (importedSlotHolds(engine, "kernel32.dll", "SetFilePointerEx",
                          kSetFilePointerExSlotRva))
        installed += redirectImport(g_seekPatch, engine, "kernel32.dll",
                                    "SetFilePointerEx",
                                    (void**)&g_setFilePointerEx,
                                    (const void*)&hookSetFilePointerEx) ? 1u : 0u;
    if (importedSlotHolds(engine, "kernel32.dll", "ReadFile",
                          kReadFileSlotRva))
        installed += redirectImport(g_readFilePatch, engine, "kernel32.dll",
                                    "ReadFile", (void**)&g_readFile,
                                    (const void*)&hookReadFile) ? 1u : 0u;
    tq::hdr::log("Engine trace: archive I/O %u/2 imports redirected\r\n",
                 installed);
    if (installed) ++g_installedHooks;
    return installed != 0;
}

// Installs last, and that ordering is required rather than tidy: the region
// lock and the fence groups read this module's EnterCriticalSection and
// WaitForSingleObject import slots to check they still hold kernel32's
// exports, and would refuse if this group had already redirected them.
bool installBlocking(HMODULE engine) {
    unsigned installed = 0;
    if (importedSlotHolds(engine, "kernel32.dll", "EnterCriticalSection",
                          kEnterCriticalSectionSlotRva)
        && tq::detour::patchImport(
               g_csPatch, engine, "kernel32.dll", "EnterCriticalSection",
               (const void*)&EnterCriticalSection,
               (const void*)&hookEngineEnterCriticalSection))
        ++installed;
    if (importedSlotHolds(engine, "kernel32.dll", "WaitForSingleObject",
                          kWaitForSingleObjectSlotRva))
        installed += redirectImport(g_objWaitPatch, engine, "kernel32.dll",
                                    "WaitForSingleObject",
                                    (void**)&g_engineWait,
                                    (const void*)&hookEngineWait) ? 1u : 0u;
    if (importedSlotHolds(engine, "kernel32.dll", "WaitForMultipleObjects",
                          kWaitForMultipleObjectsSlotRva))
        installed += redirectImport(g_objWaitMultiplePatch, engine,
                                    "kernel32.dll", "WaitForMultipleObjects",
                                    (void**)&g_engineWaitMultiple,
                                    (const void*)&hookEngineWaitMultiple)
                                        ? 1u : 0u;
    if (importedSlotHolds(engine, "kernel32.dll", "Sleep", kSleepSlotRva))
        installed += redirectImport(g_enginesleepPatch, engine, "kernel32.dll",
                                    "Sleep", (void**)&g_engineSleep,
                                    (const void*)&hookEngineSleep) ? 1u : 0u;
    tq::hdr::log("Engine trace: blocking %u/4 imports redirected\r\n",
                 installed);
    if (installed) ++g_installedHooks;
    return installed != 0;
}

bool installFrame(HMODULE engine) {
    void* target = resolve(engine, kEngineUpdateName, kEngineUpdateRva);
    if (target)
        tq::detour::attach(
            g_engineUpdateDetour, engine, target,
            signature(kEngineUpdateBytes, sizeof(kEngineUpdateBytes),
                      kEngineUpdateRelocs, 1),
            6, (const void*)&hookEngineUpdate, (void**)&g_engineUpdate);
    note("Engine::Update", g_engineUpdate != nullptr);

    target = resolve(engine, kEngineRenderName, kEngineRenderRva);
    if (target)
        tq::detour::attach(
            g_engineRenderDetour, engine, target,
            signature(kEngineRenderBytes, sizeof(kEngineRenderBytes),
                      kEngineRenderRelocs, 1),
            6, (const void*)&hookEngineRender, (void**)&g_engineRender);
    note("Engine::Render", g_engineRender != nullptr);
    return true;
}

bool installDeferredPasses(HMODULE engine) {
    BYTE* const base = (BYTE*)engine;
    void* const owner = resolve(engine, kDeferredRenderName,
                                kDeferredRenderRva);
    bool verified = owner && tq::detour::matches(
        engine, owner,
        signature(kDeferredRenderBytes, sizeof(kDeferredRenderBytes),
                  kDeferredRenderRelocs, 1))
        && tq::detour::matches(
            engine, base + kDeferredOwnerCallWindowRva,
            signature(kDeferredOwnerCallWindowBytes,
                      sizeof(kDeferredOwnerCallWindowBytes)))
        && tq::detour::matches(
            engine, base + kDeferredRenderTailRva,
            signature(kDeferredRenderTailBytes,
                      sizeof(kDeferredRenderTailBytes)))
        && kDeferredRenderTailBytes[17] == 0xc2
        && kDeferredRenderTailBytes[18] == 7 * sizeof(uintptr_t)
        && kDeferredRenderTailBytes[19] == 0;

    // Validate every overlapping original window and every callee-cleaned
    // epilogue before changing the first displacement.
    for (unsigned i = 0; i < kDeferredCallSiteCount && verified; ++i) {
        const DeferredCallSite& site = kDeferredCallSites[i];
        verified = tq::detour::matches(
            engine, base + site.callRva,
            signature(site.bytes, 16, site.relocations,
                      site.relocationCount));
        const DeferredTargetAbi* abi = nullptr;
        for (unsigned j = 0; j < kDeferredTargetAbiCount; ++j)
            if (kDeferredTargetAbis[j].targetRva == site.targetRva) {
                abi = &kDeferredTargetAbis[j];
                break;
            }
        verified = verified && abi && abi->arguments == site.arguments
            && abi->bytes[13] == 0xc2
            && abi->bytes[14] == site.arguments * sizeof(uintptr_t)
            && abi->bytes[15] == 0
            && tq::detour::matches(
                engine, base + abi->tailRva,
                signature(abi->bytes, 16));
    }
    if (!verified) {
        note("GraphicsDeferredRendererX direct-pass windows", false);
        return false;
    }

    void** const originals[] = {
        (void**)&g_deferredGeometrySetup,
        (void**)&g_deferredGeometryScene,
        (void**)&g_deferredShadows,
        (void**)&g_deferredLighting,
        (void**)&g_deferredResolve,
        (void**)&g_deferredAo,
        (void**)&g_deferredLateSceneA,
        (void**)&g_deferredLateSceneB,
        (void**)&g_deferredLateSceneList,
        (void**)&g_deferredPostHighlight,
        (void**)&g_deferredPostFog,
        (void**)&g_deferredPostMask,
        (void**)&g_deferredPostComposite,
        (void**)&g_deferredPostDebug,
    };
    const void* const replacements[] = {
        (const void*)&hookDeferredGeometrySetup,
        (const void*)&hookDeferredGeometryScene,
        (const void*)&hookDeferredShadows,
        (const void*)&hookDeferredLighting,
        (const void*)&hookDeferredResolve,
        (const void*)&hookDeferredAo,
        (const void*)&hookDeferredLateSceneA,
        (const void*)&hookDeferredLateSceneB,
        (const void*)&hookDeferredLateSceneList,
        (const void*)&hookDeferredPostHighlight,
        (const void*)&hookDeferredPostFog,
        (const void*)&hookDeferredPostMask,
        (const void*)&hookDeferredPostComposite,
        (const void*)&hookDeferredPostDebug,
    };
    static_assert(sizeof(originals) / sizeof(originals[0])
                      == kDeferredCallSiteCount,
                  "every deferred call needs an original slot");
    static_assert(sizeof(replacements) / sizeof(replacements[0])
                      == kDeferredCallSiteCount,
                  "every deferred call needs a wrapper");

    for (unsigned i = 0; i < kDeferredCallSiteCount; ++i)
        *originals[i] = base + kDeferredCallSites[i].targetRva;

    g_deferredRender = (DeferredRenderFn)owner;
    if (!tq::detour::patchCall(
            g_deferredOwnerPatch, engine,
            base + kDeferredOwnerCallWindowRva,
            signature(kDeferredOwnerCallWindowBytes,
                      sizeof(kDeferredOwnerCallWindowBytes)),
            kDeferredOwnerCallOffset, owner,
            (const void*)&hookDeferredRender)) {
        g_deferredRender = nullptr;
        for (unsigned i = 0; i < kDeferredCallSiteCount; ++i)
            *originals[i] = nullptr;
        note("GraphicsDeferredRendererX owner call", false);
        return false;
    }

    unsigned installed = 0;
    for (; installed < kDeferredCallSiteCount; ++installed) {
        const DeferredCallSite& site = kDeferredCallSites[installed];
        // All 16-byte windows were checked above while still original.  The
        // five-byte signature is now safe even where a neighbouring call has
        // already changed, and patchCall additionally resolves its target.
        if (!tq::detour::patchCall(
                g_deferredCallPatches[installed], engine,
                base + site.callRva, signature(site.bytes, 5), 0,
                base + site.targetRva, replacements[installed]))
            break;
    }
    if (installed != kDeferredCallSiteCount) {
        while (installed)
            tq::detour::restoreCall(g_deferredCallPatches[--installed]);
        tq::detour::restoreCall(g_deferredOwnerPatch);
        g_deferredRender = nullptr;
        for (unsigned i = 0; i < kDeferredCallSiteCount; ++i)
            *originals[i] = nullptr;
        note("GraphicsDeferredRendererX direct-pass windows", false);
        return false;
    }

    InterlockedExchange(&g_deferredPass, DeferredPassNone);
    InterlockedExchange(&g_deferredGeometrySite, DeferredGeometrySiteNone);
    InterlockedExchange(&g_deferredOwnerInvocation, 0);
    g_deferredOwnerFrame = UINT_MAX;
    g_deferredOwnerCallsThisFrame = 0;
    memset(g_deferredCreations, 0, sizeof(g_deferredCreations));
    g_deferredCreationSequence = 0;
    memset(g_offMainTextures, 0, sizeof(g_offMainTextures));
    InterlockedExchange(&g_offMainTextureSequence, 0);
    memset(g_deferredSlowFrames, 0, sizeof(g_deferredSlowFrames));
    g_deferredPassTracing = true;
    ++g_installedHooks;
    note("GraphicsDeferredRendererX owner call", true);
    note("GraphicsDeferredRendererX direct-pass windows", true);
    return true;
}

bool installWait(HMODULE engine) {
    void* target = resolve(engine, kWaitForLoadingName, kWaitForLoadingRva);
    const bool ok = target
        && tq::detour::replace(
               g_waitForLoadingDetour, engine, target,
               signature(kWaitForLoadingBytes, sizeof(kWaitForLoadingBytes)),
               sizeof(kWaitForLoadingBytes),
               (const void*)&hookWaitForLoadingToFinish);
    note("Region::WaitForLoadingToFinish", ok);
    return ok;
}
void resetEngineTraceState() {
    g_outsideDirResourceTracing = false;
    g_shadowMeshResourceTracing = false;
    memset(g_outsideDirResourceReports, 0,
           sizeof(g_outsideDirResourceReports));
    InterlockedExchange(&g_outsideDirResourceSequence, 0);
    InterlockedExchange(&g_outsideDirResourceReportedThrough, 0);
    memset(g_shadowMeshResourceReports, 0,
           sizeof(g_shadowMeshResourceReports));
    InterlockedExchange(&g_shadowMeshResourceSequence, 0);
    InterlockedExchange(&g_shadowMeshResourceReportedThrough, 0);
    memset(g_terrainPreloadStates, 0, sizeof(g_terrainPreloadStates));
    g_activeTerrainType = nullptr;
    g_activeTerrainThread = 0;
    g_activeTerrainPath = TerrainParameterNone;
    g_activeTerrainMaterialIndex = -1;
    g_terrainTracing = false;
    g_deferredPassTracing = false;
    InterlockedExchange(&g_deferredPass, DeferredPassNone);
    InterlockedExchange(&g_deferredGeometrySite, DeferredGeometrySiteNone);
    InterlockedExchange(&g_deferredOwnerInvocation, 0);
    g_deferredOwnerFrame = UINT_MAX;
    g_deferredOwnerCallsThisFrame = 0;
    g_reflectionTracing = false;
    g_reflectionChildTracing = false;
    g_reflectionDeferAdmissionMeshActive = false;
    g_reflectionDeferAdmissionAllActive = false;
    g_reflectionAdmissionBuildActive = false;
    g_reflectionAdmissionBuildBuffers = 0;
    g_reflectionAdmissionPending = false;
    g_reflectionAdmissionRenderActive = false;
    g_crossPassTracing = false;
    g_gpuChunkTracing = false;
    InterlockedExchange(&tq::engineprobe::detail::gpuChunkDrawActive, 0);
    InterlockedExchange(&g_reflectionManagerInvocation, 0);
    InterlockedExchange(&g_reflectionPlaneInvocation, 0);
    InterlockedExchange(&g_reflectionChild, 0);
    g_reflectionGpuChunkPending = false;
    g_reflectionGpuChunkTriggerUs = 0;
    g_activeGpuChunkRenderableCall = nullptr;
    g_reflectionManagerFrame = UINT_MAX;
    g_reflectionManagerCallsThisFrame = 0;
    g_reflectionPlaneCallsThisManager = 0;
    memset(g_deferredCreations, 0, sizeof(g_deferredCreations));
    g_deferredCreationSequence = 0;
    memset(g_offMainTextures, 0, sizeof(g_offMainTextures));
    InterlockedExchange(&g_offMainTextureSequence, 0);
    memset(g_deferredSlowFrames, 0, sizeof(g_deferredSlowFrames));
    memset(g_crossPassBuffers, 0, sizeof(g_crossPassBuffers));
    memset(g_crossPassIndex, 0, sizeof(g_crossPassIndex));
    g_crossPassBufferSequence = 0;
    g_crossPassIndexOverflows = 0;
    g_crossPassRecentEvictions = 0;
    memset(g_gpuChunkEvents, 0, sizeof(g_gpuChunkEvents));
    memset(g_activeGpuChunks, 0, sizeof(g_activeGpuChunks));
    memset(g_gpuChunkLastFrame, 0, sizeof(g_gpuChunkLastFrame));
    g_gpuChunkEventSequence = 0;
}

} } }

namespace tq { namespace engineprobe {
using namespace tq::engine::detail;


bool deferredDrawTraceRequested() {
    return tq::probe::drawTimingEnabled() && g_traceMask != 0
        && ((g_traceMask & kGroupAll) != 0
            || (g_traceMask & kGroupDeferredPasses) != 0
            || (g_traceMask & kGroupReflections) != 0);
}


void beginGpuChunkDraw(ID3D11DeviceContext* context) {
    beginGpuChunkDrawInternal(context);
}


void finishGpuChunkDraw(
    bool indexed, unsigned count, const DeferredDrawBindings* bindings) {
    finishGpuChunkDrawInternal(indexed, count, bindings);
}


void countDeferredDraw(unsigned elapsedUs, bool indexed, unsigned count,
                       unsigned start, int base,
                       const DeferredDrawBindings* bindings) {
    countDeferredDrawInternal(elapsedUs, indexed, count, start, base,
                              bindings);
}


void noteDeferredTextureCreated(
    const void* texture, unsigned elapsedUs, unsigned width, unsigned height,
    unsigned mipLevels, unsigned format, unsigned bindFlags,
    unsigned miscFlags) {
    noteDeferredCreationInternal(
        DeferredCreationTexture, texture, elapsedUs, width, height, mipLevels,
        format, bindFlags, miscFlags);
}


void noteOffMainTextureCreated(
    unsigned startFrame, unsigned finishFrame, unsigned elapsedUs,
    unsigned threadId, unsigned width, unsigned height, unsigned mipLevels,
    unsigned format, unsigned bindFlags, unsigned miscFlags,
    bool hasInitialData) {
    if (!g_deferredPassTracing) return;
    const LONG sequence = InterlockedIncrement(&g_offMainTextureSequence);
    OffMainTextureRecord& record =
        g_offMainTextures[(sequence - 1) % kOffMainTextureSlots];
    InterlockedExchange(&record.publishedSequence, 0);
    record.startFrame = startFrame;
    record.finishFrame = finishFrame;
    record.elapsedUs = elapsedUs;
    record.threadId = threadId;
    record.width = width;
    record.height = height;
    record.mipLevels = mipLevels;
    record.format = format;
    record.bindFlags = bindFlags;
    record.miscFlags = miscFlags;
    record.hasInitialData = hasInitialData;
    MemoryBarrier();
    InterlockedExchange(&record.publishedSequence, sequence);
}


void noteDeferredBufferCreated(
    const void* buffer, unsigned elapsedUs, unsigned byteWidth,
    unsigned bindFlags, unsigned usage, unsigned cpuAccessFlags,
    unsigned miscFlags) {
    if (buffer && g_reflectionAdmissionBuildActive && onMainThread())
        ++g_reflectionAdmissionBuildBuffers;
    noteDeferredCreationInternal(
        DeferredCreationBuffer, buffer, elapsedUs, byteWidth, bindFlags,
        usage, cpuAccessFlags, miscFlags, 0);
}


bool reflectionAdmissionBufferTrackingRequested() {
    return false;
}
} }

#ifdef TQ_SELFTEST

namespace tq { namespace engineprobe {
using namespace tq::engine::detail;
bool asyncLevelLoadForTest() { return g_asyncLevelLoad; }

bool shadowTransitionReuseForTest() { return g_shadowTransitionReuse; }

unsigned installedForTest() { return g_installedHooks; }

void enterCriticalSectionForTest(LPCRITICAL_SECTION section) {
    hookEnterCriticalSection(section);
}

void setTraceMaskForTest(unsigned mask) { g_traceMask = mask; }

bool shadowDeferColdResourcesForTest() { return g_shadowDeferColdResources; }

bool shadowDeferColdActorPoseForTest() {
    return g_shadowDeferColdActorPose;
}

bool terrainPreloadLayersForTest() { return g_terrainPreloadLayers; }

bool reflectionDeferAdmissionMeshForTest() {
    return g_reflectionDeferAdmissionMesh;
}

bool reflectionDeferAdmissionAllForTest() {
    return g_reflectionDeferAdmissionAll;
}

unsigned secondaryPassAdmissionBudgetForTest() {
    return g_secondaryPassAdmissionBudget;
}

bool reflectionAdmissionTriggeredForTest(unsigned buffers) {
    (void)buffers;
    return false;
}

void resetAdmissionRenderableIdentitiesForTest() {
    memset(g_admissionRenderableIdentities, 0,
           sizeof(g_admissionRenderableIdentities));
}

bool admissionRenderableFirstForTest(const void* object, unsigned kind,
                                     unsigned consumer) {
    return admissionRenderableFirst(
        object, (GpuChunkRenderableKind)kind, (AdmissionConsumer)consumer);
}

void resetSecondaryAdmissionForTest(unsigned budget, bool armed) {
    static DWORD testMainThread;
    testMainThread = GetCurrentThreadId();
    g_mainThreadId = &testMainThread;
    memset(g_admissionRenderableIdentities, 0,
           sizeof(g_admissionRenderableIdentities));
    g_secondaryPassAdmissionBudget = budget;
    g_secondaryPassAdmissionActive = budget != 0;
    g_secondaryAdmissionArmed = armed;
    g_secondaryAdmissionDrawHooksReady = budget != 0;
    g_insideReflectionRenderLight = false;
    InterlockedExchange(&g_insideDirectional, 0);
    g_secondaryAdmissionFrameSerial = 1;
    g_secondaryAdmissionBudgetFrame = UINT_MAX;
    g_secondaryAdmissionUsedThisFrame = 0;
    InterlockedExchange(&tq::secondaryadmission::detail::secondaryAdmissionDrawSuppressDepth, 0);
}

bool secondaryAdmissionArmedForTest() {
    return g_secondaryAdmissionArmed;
}

bool secondaryAdmissionRenderableDeferredForTest(
    const void* object, unsigned kind, bool reflection, bool directional) {
    g_insideReflectionRenderLight = reflection;
    InterlockedExchange(&g_insideDirectional, directional ? 1 : 0);
    const bool deferred = shouldDeferSecondaryAdmission(
        (GpuChunkRenderableKind)kind, object);
    g_insideReflectionRenderLight = false;
    InterlockedExchange(&g_insideDirectional, 0);
    return deferred;
}

void setDeferredPassForTest(unsigned pass) {
    g_deferredPassTracing = pass > DeferredPassNone
        && pass < DeferredPassCount;
    InterlockedExchange(
        &g_deferredPass,
        g_deferredPassTracing ? (LONG)pass : (LONG)DeferredPassNone);
}

void setDeferredOwnerContextForTest(unsigned invocation, unsigned site) {
    static DWORD testMainThread;
    testMainThread = GetCurrentThreadId();
    g_mainThreadId = &testMainThread;
    g_deferredPassTracing = true;
    InterlockedExchange(&g_deferredOwnerInvocation, (LONG)invocation);
    InterlockedExchange(
        &g_deferredGeometrySite,
        site < DeferredGeometrySiteCount ? (LONG)site
                                         : (LONG)DeferredGeometrySiteNone);
}

void setReflectionContextForTest(unsigned manager, unsigned plane) {
    static DWORD testMainThread;
    testMainThread = GetCurrentThreadId();
    g_mainThreadId = &testMainThread;
    g_reflectionTracing = manager != 0;
    InterlockedExchange(&g_reflectionManagerInvocation, (LONG)manager);
    InterlockedExchange(&g_reflectionPlaneInvocation, (LONG)plane);
}

void setCrossPassTracingForTest(bool enabled) {
    static DWORD testMainThread;
    testMainThread = GetCurrentThreadId();
    g_mainThreadId = &testMainThread;
    g_crossPassTracing = enabled;
    memset(g_crossPassBuffers, 0, sizeof(g_crossPassBuffers));
    memset(g_crossPassIndex, 0, sizeof(g_crossPassIndex));
    g_crossPassBufferSequence = 0;
    g_crossPassIndexOverflows = 0;
    g_crossPassRecentEvictions = 0;
}

void setGpuChunkTracingForTest(bool enabled) {
    static DWORD testMainThread;
    testMainThread = GetCurrentThreadId();
    g_mainThreadId = &testMainThread;
    g_gpuChunkTracing = enabled;
    InterlockedExchange(&tq::engineprobe::detail::gpuChunkDrawActive, 0);
    memset(g_gpuChunkEvents, 0, sizeof(g_gpuChunkEvents));
    memset(g_activeGpuChunks, 0, sizeof(g_activeGpuChunks));
    memset(g_gpuChunkLastFrame, 0, sizeof(g_gpuChunkLastFrame));
    g_gpuChunkEventSequence = 0;
    g_activeGpuChunkRenderableCall = nullptr;
}

void armGpuChunksForTest() {
    ReflectionLocation location = {};
    location.manager = 2;
    location.plane = 1;
    armGpuChunks(location, 2000);
}

void closeGpuChunksForTest() {
    closeGpuChunks();
}

unsigned gpuChunkBinDrawsForTest(unsigned bin) {
    if (!g_gpuChunkEventSequence || bin >= kGpuChunkCount) return 0;
    for (unsigned back = 0; back < g_gpuChunkEventSequence
                            && back < kGpuChunkEventSlots; ++back) {
        const GpuChunkEvent& event = g_gpuChunkEvents[
            (g_gpuChunkEventSequence - 1 - back) % kGpuChunkEventSlots];
        if (event.kind == GpuChunkReflection) return event.bins[bin].draws;
    }
    return 0;
}

void recordGpuChunkTerrainCallForTest(bool block, const void* object,
                                      unsigned cpuUs, unsigned resourceUs,
                                      unsigned textureUs) {
    GpuChunkRenderableCallScope call(
        block ? GpuChunkTerrainBlock : GpuChunkTerrainPlug, object);
    noteGpuChunkRenderableResource(resourceUs, (const void*)2, 3);
    noteGpuChunkRenderableCreation(true, textureUs);
    for (unsigned i = 0; i < 2; ++i) {
        beginGpuChunkDrawInternal(nullptr);
        finishGpuChunkDrawInternal(true, 3, nullptr);
    }
    call.finish(cpuUs);
}

void recordGpuChunkMeshCallForTest(const void* object, unsigned cpuUs) {
    GpuChunkRenderableCallScope call(GpuChunkMeshInstance, object);
    for (unsigned i = 0; i < 2; ++i) {
        beginGpuChunkDrawInternal(nullptr);
        finishGpuChunkDrawInternal(true, 3, nullptr);
    }
    call.finish(cpuUs);
}

unsigned gpuChunkRenderableKindForTest(unsigned index) {
    if (!g_gpuChunkEventSequence) return 0;
    const GpuChunkEvent& event = g_gpuChunkEvents[
        (g_gpuChunkEventSequence - 1) % kGpuChunkEventSlots];
    return index < event.renderableCalls
        ? (unsigned)event.renderables[index].kind : 0;
}

bool gpuChunkTerrainCallForTest(unsigned index, bool* block,
                               unsigned* firstDraw, unsigned* lastDraw,
                               unsigned* cpuUs, unsigned* resourceCount,
                               unsigned* resourceUs,
                               unsigned* textureCount,
                               unsigned* textureUs) {
    if (!g_gpuChunkEventSequence) return false;
    const GpuChunkEvent& event = g_gpuChunkEvents[
        (g_gpuChunkEventSequence - 1) % kGpuChunkEventSlots];
    if (index >= event.renderableCalls) return false;
    const GpuChunkRenderableCall& call = event.renderables[index];
    if (block) *block = call.kind == GpuChunkTerrainBlock;
    if (firstDraw) *firstDraw = call.firstDraw;
    if (lastDraw) *lastDraw = call.lastDraw;
    if (cpuUs) *cpuUs = call.cpuUs;
    if (resourceCount) *resourceCount = call.resourceCount;
    if (resourceUs) *resourceUs = call.resourceUs;
    if (textureCount) *textureCount = call.textureCount;
    if (textureUs) *textureUs = call.textureUs;
    return true;
}

void setDirectionalContextForTest(bool enabled) {
    InterlockedExchange(&g_insideDirectional, enabled ? 1 : 0);
}

void noteCrossPassBufferForTest(const void* buffer, unsigned bytes) {
    noteCrossPassBufferCreated(buffer, bytes, D3D11_BIND_VERTEX_BUFFER);
}

void countCrossPassDrawForTest(const void* buffer) {
    DeferredDrawBindings bindings = {};
    bindings.vertexBuffers[0] = buffer;
    countCrossPassDraw(&bindings);
}

void countDeferredDrawForTest(unsigned elapsedUs, bool indexed,
                              unsigned count) {
    DeferredDrawBindings bindings = {};
    countDeferredDrawInternal(elapsedUs, indexed, count, 0, 0, &bindings);
}

void noteDeferredCreationForTest(bool texture, unsigned elapsedUs) {
    noteDeferredCreationInternal(
        texture ? DeferredCreationTexture : DeferredCreationBuffer,
        (const void*)1, elapsedUs, 1, 2, 3, 4, 5, 6);
}

void resetOffMainTexturesForTest() {
    memset(g_offMainTextures, 0, sizeof(g_offMainTextures));
    InterlockedExchange(&g_offMainTextureSequence, 0);
}

bool latestOffMainTextureForTest(
    unsigned* startFrame, unsigned* finishFrame, unsigned* elapsedUs,
    unsigned* threadId, unsigned* width, unsigned* height,
    unsigned* mipLevels, bool* hasInitialData) {
    const LONG sequence = InterlockedCompareExchange(
        &g_offMainTextureSequence, 0, 0);
    if (sequence <= 0) return false;
    const OffMainTextureRecord& record =
        g_offMainTextures[(sequence - 1) % kOffMainTextureSlots];
    if (InterlockedCompareExchange(
            const_cast<volatile LONG*>(&record.publishedSequence), 0, 0)
            != sequence)
        return false;
    if (startFrame) *startFrame = record.startFrame;
    if (finishFrame) *finishFrame = record.finishFrame;
    if (elapsedUs) *elapsedUs = record.elapsedUs;
    if (threadId) *threadId = record.threadId;
    if (width) *width = record.width;
    if (height) *height = record.height;
    if (mipLevels) *mipLevels = record.mipLevels;
    if (hasInitialData) *hasInitialData = record.hasInitialData;
    return true;
}

bool shouldDeferShadowAlphaForTest(unsigned style, unsigned state) {
    return shouldDeferShadowAlpha(style, state);
}

bool shouldDeferShadowMeshForTest(unsigned state) {
    return shouldDeferShadowMesh(state);
}

bool shadowActorPoseQueueConfirmedForTest(unsigned state, bool inQueue) {
    return shadowActorPoseQueueConfirmed(state, inQueue);
}

void countDeferredShadowAlphaForTest(unsigned state, bool enqueued,
                                     bool failed) {
    const bool tracing = g_shadowTracing;
    g_shadowTracing = true;
    countDeferredShadowAlpha(state, enqueued, failed);
    g_shadowTracing = tracing;
}

void countDeferredShadowMeshForTest(unsigned state, bool enqueued,
                                    bool failed) {
    const bool tracing = g_shadowTracing;
    g_shadowTracing = true;
    countDeferredShadowMesh(state, enqueued, failed);
    g_shadowTracing = tracing;
}

void countDeferredShadowActorPoseForTest(unsigned state, bool enqueued,
                                         bool failed) {
    const bool tracing = g_shadowTracing;
    g_shadowTracing = true;
    countDeferredShadowActorPose(state, enqueued, failed);
    g_shadowTracing = tracing;
}

void primeShadowReuseForTest(void* region, void* surface, const void* matrix) {
    g_lastShadowRegion = region;
    rememberShadow(surface, matrix, 1);
}

bool reuseShadowForTest(void* region, void* surface, void* matrix) {
    const bool changed =
        region && g_lastShadowRegion && region != g_lastShadowRegion;
    if (region) g_lastShadowRegion = region;
    return reusePreviousShadow(changed, surface, matrix);
}

void countShadowResourceStateForTest(unsigned state, bool inQueue,
                                     unsigned elapsedUs) {
    countShadowResourceState(state, inQueue, elapsedUs);
}

void countShadowResourceTypeForTest(const char* name, unsigned elapsedUs) {
    countShadowResourceType(shadowResourceType(name), elapsedUs);
}

void countOutsideDirResourceForTest(unsigned type, unsigned phase,
                                    unsigned elapsedUs) {
    const ShadowResourceType resourceType = type <= ShadowResourceTexture
        ? (ShadowResourceType)type : ShadowResourceOther;
    const OutsideDirResourcePhase resourcePhase =
        phase < OutsideDirResourcePhaseCount
            ? (OutsideDirResourcePhase)phase : OutsideDirResourceOther;
    countOutsideDirResource(resourceType, resourcePhase, elapsedUs);
}

void outsideDirResourceResetForTest() {
    memset(g_outsideDirResourceReports, 0,
           sizeof(g_outsideDirResourceReports));
    InterlockedExchange(&g_outsideDirResourceSequence, 0);
    InterlockedExchange(&g_outsideDirResourceReportedThrough, 0);
}

void outsideDirResourceRememberForTest(unsigned frame) {
    rememberOutsideDirResource(nullptr, nullptr, "test.msh", true, 0,
                               ShadowResourceMesh, OutsideDirResourceRender,
                               frame, 1000, nullptr, TerrainParameterNone, -1,
                               TerrainPreloadSnapshot(), DeferredLocation(),
                               ReflectionLocation());
}

unsigned outsideDirResourceWindowForTest(unsigned markerFrame,
                                         bool* truncated) {
    const OutsideDirResourceWindow window =
        outsideDirResourceWindow(markerFrame);
    unsigned count = 0;
    for (LONG sequence = window.first; sequence < window.total; ++sequence) {
        const OutsideDirResourceReport& report = g_outsideDirResourceReports[
            (unsigned)sequence % kOutsideDirResourceReportSlots];
        if (outsideDirResourceInWindow(report, sequence, markerFrame)) ++count;
    }
    if (truncated) *truncated = window.truncated;
    return count;
}

void shadowMeshResourceResetForTest() {
    memset(g_shadowMeshResourceReports, 0,
           sizeof(g_shadowMeshResourceReports));
    InterlockedExchange(&g_shadowMeshResourceSequence, 0);
    InterlockedExchange(&g_shadowMeshResourceReportedThrough, 0);
}

void shadowMeshResourceRememberForTest(unsigned frame) {
    rememberShadowMeshResource(nullptr, nullptr, "test.msh", false, frame,
                               1000);
}

unsigned shadowMeshResourceWindowForTest(unsigned markerFrame,
                                         bool* truncated) {
    const ShadowMeshResourceWindow window =
        shadowMeshResourceWindow(markerFrame);
    unsigned count = 0;
    for (LONG sequence = window.first; sequence < window.total; ++sequence) {
        const ShadowMeshResourceReport& report = g_shadowMeshResourceReports[
            (unsigned)sequence % kShadowMeshResourceReportSlots];
        if (shadowMeshResourceInWindow(report, sequence, markerFrame)) ++count;
    }
    if (truncated) *truncated = window.truncated;
    return count;
}

void terrainPreloadResetForTest() {
    memset(g_terrainPreloadStates, 0, sizeof(g_terrainPreloadStates));
}

void terrainPreloadRememberForTest(const void* terrain, bool includeTextures,
                                   unsigned frame) {
    rememberTerrainPreloadAtFrame(terrain, includeTextures, frame);
}

void terrainPreloadSnapshotForTest(const void* terrain, unsigned* trueCount,
                                   unsigned* falseCount,
                                   unsigned* lastTrueFramePlusOne,
                                   unsigned* lastFalseFramePlusOne) {
    const TerrainPreloadSnapshot snapshot = terrainPreloadSnapshot(terrain);
    if (trueCount) *trueCount = snapshot.trueCount;
    if (falseCount) *falseCount = snapshot.falseCount;
    if (lastTrueFramePlusOne)
        *lastTrueFramePlusOne = snapshot.lastTrueFramePlusOne;
    if (lastFalseFramePlusOne)
        *lastFalseFramePlusOne = snapshot.lastFalseFramePlusOne;
}

void terrainRtEventRememberForTest(const void* terrain, unsigned event,
                                   unsigned frame) {
    const TerrainRtEvent kind = event == 0 ? TerrainRtLoadAttach
        : event == 1 ? TerrainRtLoadTextures : TerrainRtOwnerPreload;
    rememberTerrainRtEventAtFrame(terrain, kind, frame);
}

void terrainRtEventSnapshotForTest(
    const void* terrain, unsigned* attachCount, unsigned* attachFirst,
    unsigned* attachLast, unsigned* texturesCount, unsigned* texturesFirst,
    unsigned* texturesLast, unsigned* preloadCount, unsigned* preloadFirst,
    unsigned* preloadLast) {
    const TerrainPreloadSnapshot snapshot = terrainPreloadSnapshot(terrain);
    if (attachCount) *attachCount = snapshot.rtLoadAttachCount;
    if (attachFirst) *attachFirst = snapshot.rtLoadAttachFirstFramePlusOne;
    if (attachLast) *attachLast = snapshot.rtLoadAttachLastFramePlusOne;
    if (texturesCount) *texturesCount = snapshot.rtLoadTexturesCount;
    if (texturesFirst)
        *texturesFirst = snapshot.rtLoadTexturesFirstFramePlusOne;
    if (texturesLast)
        *texturesLast = snapshot.rtLoadTexturesLastFramePlusOne;
    if (preloadCount) *preloadCount = snapshot.rtOwnerPreloadCount;
    if (preloadFirst)
        *preloadFirst = snapshot.rtOwnerPreloadFirstFramePlusOne;
    if (preloadLast)
        *preloadLast = snapshot.rtOwnerPreloadLastFramePlusOne;
}

void countShadowMaterialTextureForTest(bool known, bool used,
                                       unsigned elapsedUs) {
    countShadowMaterialTexture(known, used, elapsedUs);
}

void countShadowMaterialUsedContextForTest(bool callKnown, bool context,
                                           bool styleKnown,
                                           unsigned match, unsigned style,
                                           bool baseKnown, bool baseMatch, int pass,
                                           bool outerInstanceSite,
                                           unsigned elapsedUs) {
    ShadowMeshParameterContext value = {};
    value.active = context;
    value.instance = callKnown ? (void*)0x2468 : nullptr;
    value.styleKnown = styleKnown;
    value.match = match < 4
        ? (ShadowContextMatch)match : ShadowContextInstanceMissing;
    value.style = style;
    value.pass = pass;
    value.baseKnown = baseKnown;
    value.outerInstanceSite = outerInstanceSite;
    const void* const loaded = (const void*)0x1234;
    value.baseTexture = baseMatch ? loaded : (const void*)0x5678;
    countShadowMaterialUsedContext(value, loaded, elapsedUs);
}

void resetShadowRecordContextsForTest() {
    resetShadowRecordContexts();
}

void rememberShadowRecordContextForTest(void* instance, int pass,
                                        unsigned style, bool styleKnown,
                                        bool baseKnown,
                                        const void* baseTexture) {
    rememberShadowRecordContext(
        instance, pass, style, styleKnown, baseKnown, baseTexture);
}

bool findShadowRecordContextForTest(void* instance, int pass,
                                    unsigned* style, bool* styleKnown,
                                    bool* baseKnown,
                                    const void** baseTexture) {
    ShadowMeshParameterContext value = {};
    if (!findShadowRecordContext(instance, pass, &value)) return false;
    if (style) *style = value.style;
    if (styleKnown) *styleKnown = value.styleKnown;
    if (baseKnown) *baseKnown = value.baseKnown;
    if (baseTexture) *baseTexture = value.baseTexture;
    return true;
}

unsigned explainShadowRecordMissForTest(void* instance, int pass) {
    ShadowMeshParameterContext value = {};
    value.instance = instance;
    value.pass = pass;
    explainShadowRecordMiss(&value);
    return (unsigned)value.match;
}

void countShadowMeshContextPatchStatusForTest(unsigned status) {
    const ShadowMeshContextPatchStatus prior = g_shadowMeshContextPatchStatus;
    g_shadowMeshContextPatchStatus = status < ShadowMeshContextPatchStatusCount
        ? (ShadowMeshContextPatchStatus)status
        : ShadowMeshContextPatchDependencyMissing;
    countShadowMeshContextPatchStatus();
    g_shadowMeshContextPatchStatus = prior;
}

void countShadowTextureCallerForTest(unsigned caller, unsigned elapsedUs) {
    countShadowTextureCaller((ShadowTextureCaller)caller, elapsedUs);
}

unsigned shadowTextureCallerFromWordsForTest(const void* const* words,
                                             unsigned count,
                                             const void* engineBase) {
    const BYTE* const prior = g_engineBase;
    g_engineBase = (const BYTE*)engineBase;
    const ShadowTextureCaller result = shadowTextureCallerFromWords(
        (const uintptr_t*)words, count);
    g_engineBase = prior;
    return (unsigned)result;
}

void slowLoadResetForTest(const void* base) {
    g_engineBase = (const BYTE*)base;
    memset(&g_loadLevelCallers, 0, sizeof(g_loadLevelCallers));
    memset(&g_guaranteedCallers, 0, sizeof(g_guaranteedCallers));
    memset(g_chains, 0, sizeof(g_chains));
    g_chainsUsed = 0;
}

void chainTextForTest(BYTE* begin, SIZE_T size, char tag) {
    if (!begin) { g_chainModuleCount = 0; return; }
    if (g_chainModuleCount >= kChainModules) return;
    ChainModule& entry = g_chainModules[g_chainModuleCount++];
    entry.tag = tag;
    entry.base = begin;
    entry.text = begin;
    entry.textSize = size;
}

bool precededByCallForTest(const BYTE* ret) {
    const ChainModule* m = moduleOf(ret);
    return m && precededByCall(ret, *m);
}

unsigned captureChainForTest(const void* from, unsigned* depth, char* tags) {
    const LONG before = g_chainsUsed;
    captureChain(from, 1234);
    if (before < 0 || before >= (LONG)kChainSlots || !g_chains[before].ready)
        return 0;
    *depth = g_chains[before].depth;
    for (unsigned i = 0; i < g_chains[before].depth; ++i)
        tags[i] = g_chains[before].frame[i].tag;
    return (unsigned)g_chains[before].frame[0].rva;
}

void slowLoadRecordForTest(const void* caller, unsigned us, bool main) {
    recordSlowCall(g_loadLevelCallers, caller, us, main);
}

bool slowLoadSlotForTest(unsigned slot, unsigned long* rva, long* calls,
                         long* main, long* us, long* worst) {
    if (slot >= kLoadCallerSlots || !g_loadLevelCallers.rva[slot]) return false;
    *rva = g_loadLevelCallers.rva[slot];
    *calls = g_loadLevelCallers.calls[slot];
    *main = g_loadLevelCallers.main[slot];
    *us = g_loadLevelCallers.us[slot];
    *worst = g_loadLevelCallers.worstUs[slot];
    return true;
}

long slowLoadLostForTest() { return g_loadLevelCallers.lost; }

bool wantsForTest(unsigned group) {
    decideTracing();
    return wants(group);
}
} }
#endif

#ifdef TQ_SELFTEST
namespace tq { namespace engineprobe {
unsigned runtimeEntriesForTest() { return tq::engine::detail::g_runtimeEntriesForTest; }
} }
#endif
