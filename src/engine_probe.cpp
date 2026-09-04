#include "engine_probe.h"

#include "arc_cache.h"
#include "detour.h"
#include "hdr.h"
#include "probe.h"

#include <stdint.h>
#include <string.h>

namespace tq {
namespace engineprobe {
namespace {

using tq::detour::CallPatch;
using tq::detour::Detour;
using tq::detour::Relocation;
using tq::detour::Signature;

// ---------------------------------------------------------------------------
// What the audit recorded, and what this file re-reads before it writes.
//
// research/streaming/findings.md §7 lists these, but that file is a record and
// not a licence: every byte below was read back out of the pinned Engine.dll
// (SHA-256 0aedbb18...f694f6) while this was written. The addresses are RVAs
// against the preferred base 0x10000000, and each exported target is resolved
// by decorated name first and then asserted against its RVA -- so the name is
// the lookup and the RVA is the identity check, which is the way round that
// survives a rebased image.

const DWORD kEngineImageSize = 0x0044b000;

// The thread the engine recorded as its own. `CMP EAX,[0x1041a5dc]` at
// 0x1014476b is what the engine itself compares against, so reading it costs
// one load and needs no hook.
const DWORD kMainThreadIdRva = 0x41a5dc;

// --- Region::LoadLevel. Returns immediately when the level is resident
// (`MOV EAX,[EBX+0x50]` / `TEST` / the AL=1 epilogue), so the duration this
// records *is* the cost of a load the renderer forced.
const DWORD kLoadLevelRva = 0x20bec0;
const char kLoadLevelName[] = "?LoadLevel@Region@GAME@@QAE_N_N@Z";
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

// Resource::GetLoadedState and Resource::GetInLoadingQueue derive the two
// fields the shadow-resource lifecycle trace reads. These accessors are not
// patched; their 16-byte windows and exports make the layout a runtime-checked
// fact before hookLoadResource dereferences either offset.
const DWORD kResourceLoadedStateRva = 0x213180;
const DWORD kResourceLoadedStateOffset = 0x30;
const char kResourceLoadedStateName[] =
    "?GetLoadedState@Resource@GAME@@QBE?AW4LoadState@12@XZ";
const BYTE kResourceLoadedStateBytes[] = {
    0x8b, 0x41, 0x30,                          // mov eax,[ecx+0x30]
    0xc3,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0xcc, 0xcc, 0xcc
};
const DWORD kResourceInQueueRva = 0x212d20;
const DWORD kResourceInQueueOffset = 0x60;
const char kResourceInQueueName[] =
    "?GetInLoadingQueue@Resource@GAME@@QBE_NXZ";
const BYTE kResourceInQueueBytes[] = {
    0x33, 0xc0,                                // xor eax,eax
    0x39, 0x41, 0x60,                          // cmp [ecx+0x60],eax
    0x0f, 0x95, 0xc0,                          // setne al
    0xc3,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc
};

// Resource::GetFileName is the engine's own type discriminator for this
// trace. It returns the c_str() of Resource+0xc, using the MSVC small-string
// capacity at +0x20 to choose inline storage or the heap pointer. We call the
// verified export only for the small population already entering
// ResourceLoader::LoadResource inside the directional-shadow bracket, then
// partition its .msh/.ssh/.tex suffix. No filename is retained or written.
const DWORD kResourceFileNameRva = 0x2130e0;
const char kResourceFileNameName[] =
    "?GetFileName@Resource@GAME@@QBEPBDXZ";
const BYTE kResourceFileNameBytes[] = {
    0x83, 0x79, 0x20, 0x10,                    // cmp dword [ecx+0x20],0x10
    0x8d, 0x41, 0x0c,                          // lea eax,[ecx+0xc]
    0x72, 0x02,                                // jb inline
    0x8b, 0x00,                                // mov eax,[eax]
    0xc3,
    0xcc, 0xcc, 0xcc, 0xcc
};

// GraphicsShadowMapRenderer asks each GraphicsRenderable for its number of
// shadow passes before shader selection and before the draw-list record is
// constructed. For GraphicsMeshInstance, the first operation is
// EnsureAvailable(this+4), where this+4 is its GraphicsMesh resource. The
// complete 24-byte function is verified and only its E8 displacement is
// changed. This is both the earliest exact cold-mesh boundary for a per-caster
// omission and the point option 2 would have to make resident earlier.
const DWORD kShadowMeshPassCountRva = 0x173440;
const char kShadowMeshPassCountName[] =
    "?GetNumShadowRenderPasses@GraphicsMeshInstance@GAME@@UBEHXZ";
const DWORD kEnsureAvailableRva = 0x2130f0;
const char kEnsureAvailableName[] =
    "?EnsureAvailable@Resource@GAME@@QBEXXZ";
const unsigned kShadowMeshEnsureCallOffset = 10;
const BYTE kShadowMeshPassCountBytes[] = {
    0x56,                                      // push esi
    0x8b, 0x71, 0x04,                          // mov esi,[ecx+4] mesh
    0x85, 0xf6,                                // test esi,esi
    0x74, 0x0c,                                // jz no mesh
    0x8b, 0xce,                                // mov ecx,esi
    0xe8, 0xa1, 0xfc, 0x09, 0x00,              // call EnsureAvailable
    0x8b, 0x46, 0x7c,                          // mov eax,[esi+0x7c] pass count
    0x5e, 0xc3,                                // pop esi; ret
    0x33, 0xc0,                                // xor eax,eax
    0x5e, 0xc3                                 // pop esi; ret
};

// The DX11 shadow RenderPass reaches the mesh's generic material-parameter
// loop. For every type-7 (texture) entry that loop calls
// GraphicsTexture::GetTexture *before* asking FUN_10035ea0 to bind the value;
// the latter is where a missing parameter in the active shadow shader is
// finally discovered. Run 50 wraps both adjacent calls. The first measures a
// cold material texture, and the second asks the verified public
// GraphicsShader2::HasParameter about the same Name before forwarding to the
// original setter. The windows do not overlap, so each remains independently
// verifiable after the other is patched.
const DWORD kGraphicsMeshSetShaderParametersRva = 0x169c40;
const char kGraphicsMeshSetShaderParametersName[] =
    "?SetShaderParameters@GraphicsMesh@GAME@@QBEX"
    "PBVGraphicsShader2@2@H@Z";
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
const DWORD kShadowMaterialTextureWindowRva = 0x169ca8;
const unsigned kShadowMaterialTextureCallOffset = 3;
// At the patched getter's entry, the caller's active GraphicsShader2* is this
// far above ESP: it was at caller ESP+0x1c and E8 pushed one return address.
const unsigned kShadowMaterialShaderStackOffset = 0x20;
const unsigned kShadowMaterialOuterCallerStackOffset = 0x1c;
const BYTE kShadowMaterialTextureWindowBytes[] = {
    0x8b, 0x4e, 0x14,                          // mov ecx,[esi+0x14] texture
    0xe8, 0x00, 0xac, 0x02, 0x00,              // call GetTexture()
    0x89, 0x44, 0x24, 0x10,                    // mov [esp+0x10],eax
    0x8d, 0x44, 0x24, 0x10                     // lea eax,[esp+0x10]
};
const DWORD kGraphicsTextureGetTextureRva = 0x1948b0;
const char kGraphicsTextureGetTextureName[] =
    "?GetTexture@GraphicsTexture@GAME@@QBEPBVRenderTexture@2@XZ";

// Run 51 proved that a base-texture-only record gate leaves cold, shader-used
// texture work on the following directional build. This direct call inside
// GraphicsMeshInstance::SetShaderParameters is where the exact instance and
// pass still coexist with GraphicsMesh::SetShaderParameters. A wrapper around
// only this E8 supplies that context while the existing material getter runs.
// All windows are 16-24 bytes. They prove ESI=this and EBX=shader. EBP starts
// as arg3/pass but is multiplied by the 0x34 MeshRenderInfo stride before the
// call, so it is emphatically not a pass there. The original arg3 remains at
// adapter ESP+0xbc after the adapter's first two pushes; that offset includes
// the 0x8c local frame, four saved registers, two original call arguments,
// the E8 return address, and those two pushes.
const DWORD kGraphicsMeshInstanceSetShaderParametersRva = 0x173480;
const char kGraphicsMeshInstanceSetShaderParametersName[] =
    "?SetShaderParameters@GraphicsMeshInstance@GAME@@UBEX"
    "PBVGraphicsShader2@2@HHABVMeshRenderInfo@2@@Z";
const DWORD kShadowMeshParameterFrameRva = 0x173480;
const BYTE kShadowMeshParameterFrameBytes[] = {
    0x81, 0xec, 0x8c, 0x00, 0x00, 0x00,       // sub esp,0x8c
    0xa1, 0x44, 0xb0, 0x41, 0x10,             // load static guard
    0x53,                                      // save ebx
    0x8b, 0x9c, 0x24, 0x94, 0x00, 0x00, 0x00 // ebx = arg1 shader
};
const Relocation kShadowMeshParameterFrameRelocs[] = {
    {7, 0x41b044}                             // A1 absolute static guard
};
const DWORD kShadowMeshParameterEntryRva = 0x17348b;
const BYTE kShadowMeshParameterEntryBytes[] = {
    0x53,                                      // push ebx
    0x8b, 0x9c, 0x24, 0x94, 0x00, 0x00, 0x00, // ebx = arg1 shader
    0x55,                                      // push ebp
    0x8b, 0xac, 0x24, 0xa0, 0x00, 0x00, 0x00, // ebp = arg3 pass
    0x56                                       // push esi
};
const DWORD kShadowMeshParameterContextRva = 0x173494;
const BYTE kShadowMeshParameterContextBytes[] = {
    0x8b, 0xac, 0x24, 0xa0, 0x00, 0x00, 0x00, // ebp = arg3 pass
    0x56, 0x57,                                // preserve esi/edi
    0x8b, 0xbc, 0x24, 0xac, 0x00, 0x00, 0x00, // edi = arg4 render info
    0x8b, 0xf1,                                // esi = this instance
    0x89, 0x7c, 0x24, 0x14                     // preserve render-info pointer
};
const DWORD kShadowMeshParameterArgsRva = 0x173857;
const BYTE kShadowMeshParameterArgsBytes[] = {
    0xff, 0xb4, 0x24, 0xa4, 0x00, 0x00, 0x00, // push arg2 material index
    0x6b, 0xed, 0x34,                          // pass * 0x34
    0x03, 0x6f, 0x1c,                          // + MeshRenderInfo base
    0x8b, 0x4e, 0x04                           // ecx = instance+4 mesh
};
const DWORD kShadowMeshParameterCallRva = 0x17385e;
const unsigned kShadowMeshParameterCallOffset = 14;
const unsigned kShadowMeshParameterAdapterPassOffset = 0xbc;
const BYTE kShadowMeshParameterCallBytes[] = {
    0x6b, 0xed, 0x34,                          // pass * mesh-info stride
    0x03, 0x6f, 0x1c,
    0x8b, 0x4e, 0x04,                          // ecx = instance+4 mesh
    0x53,                                      // push ebx shader
    0x89, 0x6c, 0x24, 0x20,
    0xe8, 0xcf, 0x63, 0xff, 0xff               // GraphicsMesh setter
};

// GraphicsMeshInstance applies two optional texture overrides after the base
// mesh material. The +0x18 override is named bumpTexture. Stock code ensures
// that Resource before its setter asks whether the active shader has such a
// parameter, so a directional-shadow shader that does not use bump mapping
// can synchronously load it for no rendered result. Patch only the E8: EBX is
// the already verified shader from the enclosing function, and ECX is the
// bump texture Resource. The wrapper forwards every non-directional or used
// case unchanged.
const DWORD kShadowInstanceBumpEnsureWindowRva = 0x173b3f;
const unsigned kShadowInstanceBumpEnsureCallOffset = 9;
const BYTE kShadowInstanceBumpEnsureWindowBytes[] = {
    0x8b, 0x7e, 0x18,                          // edi = instance+0x18 texture
    0x85, 0xff,                                // test edi,edi
    0x74, 0x6b,                                // null: skip the whole block
    0x8b, 0xcf,                                // ecx = texture Resource
    0xe8, 0xa3, 0xf5, 0x09, 0x00,              // EnsureAvailable
    0x8b, 0x47, 0x74,                          // render-texture vector begin
    0x8b, 0x4f, 0x78,                          // render-texture vector end
    0x2b, 0xc8                                 // end - begin
};
const DWORD kShadowInstanceBumpSetterWindowRva = 0x173b99;
const DWORD kBumpTextureNameRva = 0x41b078;
const DWORD kBumpTextureLiteralRva = 0x2d6644;
const BYTE kShadowInstanceBumpSetterWindowBytes[] = {
    0x89, 0x44, 0x24, 0x20,                    // store chosen texture
    0x8d, 0x44, 0x24, 0x20,
    0x50, 0x51, 0x6a, 0x00,
    0x68, 0x78, 0xb0, 0x41, 0x10,              // Name("bumpTexture")
    0x8b, 0xcb,
    0xe8, 0xef, 0x22, 0xec, 0xff               // texture-param setter
};
const Relocation kShadowInstanceBumpSetterWindowRelocs[] = {
    {13, kBumpTextureNameRva}
};
const DWORD kBumpTextureNameInitWindowRva = 0x173595;
const BYTE kBumpTextureNameInitWindowBytes[] = {
    0x68, 0x78, 0xb0, 0x41, 0x10,              // destination Name object
    0x83, 0xc8, 0x08,
    0x6a, 0x0b,                                // strlen("bumpTexture")
    0x68, 0x44, 0x66, 0x2d, 0x10,              // source string
    0xa3, 0x44, 0xb0, 0x41, 0x10               // initialized-bit guard
};
const Relocation kBumpTextureNameInitWindowRelocs[] = {
    {1, kBumpTextureNameRva}, {11, kBumpTextureLiteralRva}, {16, 0x41b044}
};
const DWORD kSetTextureParameterMissingWindowRva = 0x35eb8;
const BYTE kSetTextureParameterMissingWindowBytes[] = {
    0x8b, 0x44, 0x24, 0x0c,                    // Name lookup result
    0x3b, 0x87, 0xa0, 0x00, 0x00, 0x00,       // compare end/sentinel
    0x74, 0x76,                                // absent: return success
    0x8b, 0x40, 0x18,
    0x83, 0xf8, 0xff,
    0x74, 0x6e,
    0x8d, 0x0c, 0xc0
};
const DWORD kSetTextureParameterMissingReturnRva = 0x35f3a;
const BYTE kSetTextureParameterMissingReturnBytes[] = {
    0x5f, 0xb0, 0x01, 0x5e, 0xc2, 0x10, 0x00,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc
};

// The base GraphicsMesh material is applied first, then an instance+0x14
// override is ensured and bound to the same baseTexture Name before any draw.
// A material getter inside the base call can therefore omit only a different
// baseTexture Resource when this exact enclosing instance has a non-null
// override. These windows independently prove the field, ensure, Name, and
// ordering; the existing context E8 proves the base material call came first.
const DWORD kShadowInstanceBaseEnsureWindowRva = 0x173acd;
const unsigned kShadowInstanceBaseEnsureCallOffset = 9;
const BYTE kShadowInstanceBaseEnsureWindowBytes[] = {
    0x8b, 0x7e, 0x14,                          // instance+0x14 override
    0x85, 0xff,
    0x74, 0x6b,
    0x8b, 0xcf,
    0xe8, 0x15, 0xf6, 0x09, 0x00,              // EnsureAvailable
    0x8b, 0x47, 0x74,
    0x8b, 0x4f, 0x78,
    0x2b, 0xc8
};
const DWORD kShadowInstanceBaseSetterWindowRva = 0x173b27;
const DWORD kBaseTextureNameRva = 0x41b068;
const DWORD kBaseTextureLiteralRva = 0x2d6638;
const BYTE kShadowInstanceBaseSetterWindowBytes[] = {
    0x89, 0x44, 0x24, 0x28,
    0x8d, 0x44, 0x24, 0x28,
    0x50, 0x51, 0x6a, 0x00,
    0x68, 0x68, 0xb0, 0x41, 0x10,              // Name("baseTexture")
    0x8b, 0xcb,
    0xe8, 0x61, 0x23, 0xec, 0xff
};
const Relocation kShadowInstanceBaseSetterWindowRelocs[] = {
    {13, kBaseTextureNameRva}
};
const DWORD kBaseTextureNameInitWindowRva = 0x173548;
const BYTE kBaseTextureNameInitWindowBytes[] = {
    0x68, 0x68, 0xb0, 0x41, 0x10,
    0x83, 0xc8, 0x04,
    0x6a, 0x0b,                                // strlen("baseTexture")
    0x68, 0x38, 0x66, 0x2d, 0x10,
    0xa3, 0x44, 0xb0, 0x41, 0x10
};
const Relocation kBaseTextureNameInitWindowRelocs[] = {
    {1, kBaseTextureNameRva}, {11, kBaseTextureLiteralRva}, {16, 0x41b044}
};

// Every other direct Engine.dll caller of GraphicsTexture::GetTexture. At a
// nested texture ResourceLoader::LoadResource call the original caller's E8
// return address is still on the stack. Run 52 scans only that committed stack
// region and compares exact return RVAs; indirect or unrecognized paths remain
// in an explicit bucket. The existing material E8 at 0x169cab is represented
// by a dynamic bracket because shadow_defer_cold_alpha has already retargeted
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

const DWORD kShadowTextureParameterWindowRva = 0x169cb8;
const unsigned kShadowTextureParameterCallOffset = 9;
const DWORD kSetTextureParameterRva = 0x35ea0;
const BYTE kShadowTextureParameterWindowBytes[] = {
    0x50,                                      // push eax (texture output)
    0x51,                                      // push ecx (reserved)
    0x8b, 0x4c, 0x24, 0x24,                    // mov ecx,[esp+0x24] shader
    0x6a, 0x00,                                // push 0
    0x56,                                      // push esi (material Name)
    0xe8, 0xda, 0xc1, 0xec, 0xff,              // call texture-param setter
    0xeb, 0x46,                                // jmp next material entry
    0xf3, 0x0f, 0x10, 0x46, 0x14               // movss xmm0,[esi+0x14]
};

const DWORD kShaderHasParameterRva = 0x18ba70;
const char kShaderHasParameterName[] =
    "?HasParameter@GraphicsShader2@GAME@@QBE_NABVName@2@@Z";
const BYTE kShaderHasParameterBytes[] = {
    0x51, 0x56, 0x8b, 0xf1,
    0xe8, 0x77, 0x76, 0x08, 0x00,              // call EnsureAvailable
    0xff, 0x74, 0x24, 0x0c,
    0x8d, 0x44, 0x24, 0x10,
    0x50,
    0x8d, 0x8e, 0xa0, 0x00, 0x00, 0x00
};

// A material parameter Name is a 16-byte digest, but this diagnostic only
// needs its stable first dword. Prove that Name::Hash reads exactly that field
// before copying it out of the verified material entry; no engine call is
// added to the hot material path.
const DWORD kNameHashRva = 0x13f0;
const char kNameHashName[] = "?Hash@Name@GAME@@QBEIXZ";
const BYTE kNameHashBytes[] = {
    0x8b, 0x01, 0xc3,                         // mov eax,[ecx]; ret
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc
};

// The shadow renderer builds one 0x88-byte record per accepted caster/pass.
// This direct call is the last decision point before it appends that record:
// EDI is the pass, ESI is the renderable entry, and EAX is the output record.
// Returning false therefore omits only this caster/pass from the shadow map;
// the normal colour render path never sees the decision.
const DWORD kShadowRecordCallWindowRva = 0x18c8f5;
const unsigned kShadowRecordCallOffset = 9;
const DWORD kBuildShadowRecordRva = 0x18c650;
const BYTE kShadowRecordCallWindowBytes[] = {
    0x57,                                      // push edi (pass)
    0x56,                                      // push esi (renderable entry)
    0x8d, 0x44, 0x24, 0x30,                    // lea eax,[esp+0x30] output
    0x50,                                      // push eax
    0x8b, 0xcd,                                // mov ecx,ebp (renderer)
    0xe8, 0x4d, 0xfd, 0xff, 0xff,              // call build-record helper
    0x84, 0xc0,                                // test al,al
    0x0f, 0x84, 0x8c, 0x00, 0x00, 0x00         // false: skip append
};
const BYTE kBuildShadowRecordBytes[] = {
    0x53, 0x8b, 0x5c, 0x24, 0x0c, 0x55, 0x8b, 0xe9,
    0x8b, 0x0b, 0x56, 0x8b, 0x01, 0x57, 0x8b, 0x40,
    0x24, 0xff, 0xd0, 0x84, 0xc0
};

// GraphicsMeshInstance is identified by the exact virtual function in slot 2
// that the build-record helper calls. Its shadow style partitions opaque
// (0-2) from alpha-tested (3-5). The function obtains the base GraphicsTexture
// resource through virtual GetTexture; that getter ensures only the already
// accepted mesh, not the returned texture resource.
const DWORD kMeshShadowStyleRva = 0x1733b0;
const char kMeshShadowStyleName[] =
    "?GetShadowRenderStyle@GraphicsMeshInstance@GAME@@UBE?AW4"
    "RenderShadowStyle@2@H@Z";
const DWORD kNameNoNameRva = 0x41a55c;
const BYTE kMeshShadowStyleBytes[] = {
    0x53, 0x56, 0x8b, 0xf1,
    0x68, 0x5c, 0xa5, 0x41, 0x10,              // push Name::noName
    0x8b, 0x06, 0xff, 0x74, 0x24, 0x10,
    0x32, 0xdb, 0xff, 0x50, 0x1c,
    0x85, 0xc0, 0x74, 0x11
};
const Relocation kMeshShadowStyleRelocs[] = {{5, kNameNoNameRva}};
const DWORD kMeshShadowStyleAlphaRva = 0x1733c8;
const BYTE kMeshShadowStyleAlphaBytes[] = {
    0x80, 0xb8, 0x81, 0x00, 0x00, 0x00, 0x02,
    0xb3, 0x01, 0x7c, 0x06,
    0x8a, 0x98, 0x80, 0x00, 0x00, 0x00
};
const DWORD kMeshShadowStyleSkinnedRva = 0x1733f3;
const BYTE kMeshShadowStyleSkinnedBytes[] = {
    0xd1, 0xe9, 0xf6, 0xc1, 0x01, 0x74, 0x14, 0x84, 0xdb,
    0xb8, 0x01, 0x00, 0x00, 0x00,              // opaque skinned
    0xb9, 0x04, 0x00, 0x00, 0x00,              // alpha skinned
    0x5e, 0x0f, 0x45, 0xc1
};
const DWORD kMeshShadowStyleFoliageRva = 0x173415;
const BYTE kMeshShadowStyleFoliageBytes[] = {
    0x74, 0x14, 0x84, 0xdb,
    0xb8, 0x02, 0x00, 0x00, 0x00,              // opaque foliage
    0xb9, 0x05, 0x00, 0x00, 0x00,              // alpha foliage
    0x5e, 0x0f, 0x45, 0xc1, 0x5b, 0xc2, 0x04, 0x00
};
const DWORD kMeshShadowStyleStaticRva = 0x17342b;
const BYTE kMeshShadowStyleStaticBytes[] = {
    0x33, 0xc0,                                // opaque static = 0
    0x84, 0xdb,
    0xb9, 0x03, 0x00, 0x00, 0x00,              // alpha static = 3
    0x5e, 0x0f, 0x45, 0xc1, 0x5b, 0xc2, 0x04, 0x00,
    0xcc, 0xcc, 0xcc
};

const DWORD kMeshGetTextureRva = 0x1731a0;
const char kMeshGetTextureName[] =
    "?GetTexture@GraphicsMeshInstance@GAME@@UBEPBVGraphicsTexture@2@"
    "HABVName@2@@Z";
const BYTE kMeshGetTextureBytes[] = {
    0x51,
    0xa1, 0x08, 0xb1, 0x41, 0x10,
    0x57, 0x8b, 0xf9, 0xa8, 0x01, 0x75, 0x44,
    0x68, 0x0c, 0xb1, 0x41, 0x10,
    0x83, 0xc8, 0x01, 0x6a, 0x0b
};
const Relocation kMeshGetTextureRelocs[] = {
    {2, 0x41b108}, {14, 0x41b10c}
};
const DWORD kMeshGetTextureMeshRva = 0x173204;
const unsigned kMeshGetTextureEnsureCallOffset = 8;
const BYTE kMeshGetTextureMeshBytes[] = {
    0x0f, 0x84, 0x8b, 0x00, 0x00, 0x00,
    0x8b, 0xce,
    0xe8, 0xdf, 0xfe, 0x09, 0x00,              // ensure mesh
    0x8b, 0x4c, 0x24, 0x18,
    0x8b, 0x86, 0x80, 0x00, 0x00, 0x00
};
const DWORD kMeshGetTextureReturnRva = 0x17329f;
const BYTE kMeshGetTextureReturnBytes[] = {
    0x8b, 0x44, 0x24, 0x10,
    0x6b, 0xc9, 0x54,
    0x8b, 0x44, 0x01, 0x14,                    // return entry+0x14 resource
    0x5e, 0x5d, 0x5b, 0x5f, 0x59,
    0xc2, 0x08, 0x00
};

const DWORD kResourceLoaderAccessorRva = 0x212dc0;
const char kResourceLoaderAccessorName[] =
    "?GetResourceLoader@Resource@GAME@@QAEPAVResourceLoader@2@XZ";
const BYTE kResourceLoaderAccessorBytes[] = {
    0x8b, 0x41, 0x24, 0xc3,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xcc, 0xcc, 0xcc, 0xcc
};

// --- The deferred renderer's one call to the DX11 directional-shadow build.
// This is a call-site patch, not another entry detour: only this orchestration
// path is timed, and the four-byte E8 displacement is the only code changed.
// The 23-byte window includes the final three arguments, the local shadow-map
// object's address, the call and its branch-around-success continuation.
const DWORD kShadowCallWindowRva = 0x1644ac;
const unsigned kShadowCallOffset = 16;
const DWORD kRenderDirectionalRva = 0x18db80;
const char kRenderDirectionalName[] =
    "?RenderDirectional@GraphicsShadowMapDx11@GAME@@QAE_N"
    "AAVGraphicsCanvas@2@ABVCamera@2@ABVFrustum@2@W4Algorithm@12@"
    "PAVRenderSurface@2@AAVMat4@2@@Z";
const BYTE kShadowCallWindowBytes[] = {
    0x50,                                      // push frustum
    0x8d, 0x43, 0x28,                          // lea eax,[ebx+0x28] camera
    0x50,                                      // push eax
    0xff, 0x74, 0x24, 0x3c,                    // push canvas
    0x8d, 0x8c, 0x24, 0xbc, 0x00, 0x00, 0x00,  // lea ecx,[esp+0xbc] shadow map
    0xe8, 0xbf, 0x96, 0x02, 0x00,              // call RenderDirectional
    0xeb, 0x3d                                 // skip the other algorithm
};

// The shadow-map constructor copies its region argument to self+0x6c. This
// separate 24-byte window makes the field an instruction-derived fact before
// the hook reads it; no object-layout document is trusted at runtime.
const DWORD kShadowRegionConstructorRva = 0x18d427;
const unsigned kShadowRegionOffset = 0x6c;
const BYTE kShadowRegionConstructorBytes[] = {
    0x8b, 0x74, 0x24, 0x0c,
    0x8b, 0x4d, 0x0c,
    0x83, 0x7e, 0x0c, 0x00,
    0x89, 0x4e, 0x6c,                          // mov [esi+0x6c],ecx
    0x74, 0x6c,
    0x85, 0xc9,
    0x74, 0x68,
    0x8d, 0x46, 0x10,
    0x50
};

// RenderDirectional saves its seventh argument (Mat4&) and later copies the
// completed matrix to it with `mov ecx,16; rep movsd`. Together these two
// windows prove both the output pointer and all 64 bytes the reuse path must
// restore. Neither table is patched.
const DWORD kShadowOutputArgumentRva = 0x18dbd5;
const BYTE kShadowOutputArgumentBytes[] = {
    0x8b, 0x45, 0x0c,
    0x89, 0x44, 0x24, 0x24,
    0x8b, 0x45, 0x1c,                          // mov eax,[ebp+0x1c] output
    0x89, 0xb4, 0x24, 0xc8, 0x00, 0x00, 0x00,
    0x89, 0x44, 0x24, 0x7c                     // save output at esp+0x7c
};
const DWORD kShadowOutputCopyRva = 0x18f0c0;
const unsigned kShadowMatrixDwords = 16;
const BYTE kShadowOutputCopyBytes[] = {
    0x8b, 0xbc, 0x24, 0x80, 0x00, 0x00, 0x00,  // output after one push
    0x8b, 0xf0,                                // source matrix
    0xb9, 0x10, 0x00, 0x00, 0x00,              // 16 dwords
    0xf3, 0xa5                                 // rep movsd
};

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

// --- ResourceLoader::EnqueueResource. Only the same seven-byte SEH prologue
// as the two above, so the handler it pushes is what tells them apart.
const DWORD kEnqueueRva = 0x2145c0;
const char kEnqueueName[] =
    "?EnqueueResource@ResourceLoader@GAME@@QAEXPBVResource@2@"
    "W4ResourcePriority@2@_N2@Z";
const BYTE kEnqueueBytes[] = {
    0x6a, 0xff,
    0x68, 0, 0, 0, 0,
    0x64, 0xa1, 0x00, 0x00, 0x00, 0x00,
    0x50,
    0x83, 0xec, 0x14,                          // sub esp,0x14
    0x53, 0x55, 0x56, 0x57,
    0xa1
};
const Relocation kEnqueueRelocs[] = {{3, 0x2a2e86}};

// BaseResourceManager's stock preload path supplies the exact tuple reused by
// the cold-alpha fix: priority 1, notify=true, immediate=false. This window is
// verified as behaviour, not patched.
const DWORD kPreloadResourceRva = 0x1200e0;
const char kPreloadResourceName[] =
    "?PreLoadResource@BaseResourceManager@GAME@@QAEXPBVResource@2@@Z";
const DWORD kPreloadEnqueueWindowRva = 0x120110;
const unsigned kPreloadEnqueueCallOffset = 10;
const BYTE kPreloadEnqueueWindowBytes[] = {
    0x8b, 0x4f, 0x10,                          // loader
    0x6a, 0x00,                                // immediate=false
    0x6a, 0x01,                                // notify=true
    0x6a, 0x01,                                // priority=1
    0x56,                                      // resource
    0xe8, 0xa1, 0x44, 0x0f, 0x00,              // EnqueueResource
    0x5e, 0x5f, 0xc2, 0x04, 0x00
};

// --- Archive::ReadFromFile. `?ReadFromFile@Archive@GAME@@QBE_NHPAEIIPAU
// BlockBuffer@12@@Z` and the disassembly agree on the argument layout, which
// settles the one thing the plan flagged as inferred: `size` is the fourth
// stack argument, read at [ebp+0x14].
const DWORD kReadFromFileRva = 0x11d320;
const char kReadFromFileName[] =
    "?ReadFromFile@Archive@GAME@@QBE_NHPAEIIPAUBlockBuffer@12@@Z";
const BYTE kReadFromFileBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,
    0x83, 0xec, 0x0c,
    0x83, 0x7d, 0x0c, 0x00,                    // cmp dword [ebp+0xc],0   dest
    0x53, 0x56,
    0x8b, 0xc1,                                // mov eax,ecx
    0x57,
    0x89, 0x44, 0x24, 0x10,
    0x0f, 0x84                                 // jz the bail-out
};

// --- One block read and inflated. Not exported, so identity rests entirely on
// the bytes: the prologue is the same six bytes as three other targets, and
// what makes this one itself is the call to zlib's uncompress 0xf6 in.
//
// The window runs to the end of the address arithmetic rather than stopping at
// the prologue, because that arithmetic is the whole structure the block cache
// keys on. Read it as the derivation it is:
//
//   this  = ecx                             Archive*
//   entry = [ebp+8]   block = [ebp+0xc]   blockBuffer = [ebp+0x10]
//   esi   = [ecx+0x2c] + entry*0x44        the entry record, stride 0x11*4
//   edi   = [esi+0x20] + block*0xc         its block descriptor, stride 3*4
//
// So `mov eax,[ecx+0x2c]` names the entry table, `shl edx,4 / add edx,[ebp+8] /
// lea esi,[eax+edx*4]` names the 0x44 stride, `mov eax,[esi+0x20]` names the
// descriptor array, and `lea ecx,[ebx+ebx*2] / lea edi,[eax+ecx*4]` names the
// 12-byte descriptor. None of those offsets is taken from a document.
const DWORD kArchiveBlockRva = 0x11d0e0;
const BYTE kArchiveBlockBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,
    0x83, 0xec, 0x0c,
    0x8b, 0x55, 0x08,                          // mov edx,[ebp+8]   entry index
    0x8b, 0x41, 0x2c,                          // mov eax,[ecx+0x2c] entry table
    0xc1, 0xe2, 0x04,                          // shl edx,4
    0x03, 0x55, 0x08,                          // add edx,[ebp+8]   -> index*0x11
    0x53,
    0x8b, 0x5d, 0x0c,                          // mov ebx,[ebp+0xc] block index
    0x56,
    0x8d, 0x34, 0x90,                          // lea esi,[eax+edx*4] entry, 0x44
    0x89, 0x4c, 0x24, 0x08,                    // [esp+8] = this
    0x8b, 0x46, 0x20,                          // mov eax,[esi+0x20] descriptors
    0x8d, 0x0c, 0x5b,                          // lea ecx,[ebx+ebx*2]
    0x57,
    0x8d, 0x3c, 0x88,                          // lea edi,[eax+ecx*4] desc, 0xc
    0x89, 0x7c, 0x24, 0x10                     // [esp+0x10] = descriptor
};

// The seek, which names two more of the key's fields and the syscall that
// consumes them: `push [edi]` is the descriptor's offset and `push [eax+0xc]`
// is the open `.arc` file HANDLE, with eax reloaded from the saved `this`. The
// trailing indirect call is SetFilePointerEx through Engine's IAT, and it is
// relocated, so it is compared against the slot rather than literally.
const DWORD kArchiveSeekWindowRva = 0x11d122;
const BYTE kArchiveSeekWindowBytes[] = {
    0x8b, 0x44, 0x24, 0x0c,                    // mov eax,[esp+0xc]  this
    0x6a, 0x00, 0x6a, 0x00, 0x6a, 0x00,
    0xff, 0x37,                                // push [edi]        desc.offset
    0xff, 0x70, 0x0c,                          // push [eax+0xc]    the HANDLE
    0xff, 0x15, 0, 0, 0, 0                     // call SetFilePointerEx
};
const DWORD kSetFilePointerExSlotRva = 0x2ac190;
const Relocation kArchiveSeekWindowRelocs[] = {{17, kSetFilePointerExSlotRva}};

// The read. `push [eax+4]` off the reloaded descriptor is the compressed size,
// `push [edi+4]` off the reloaded blockBuffer is the scratch the compressed
// bytes land in, and `push [eax+0xc]` is the handle again.
const DWORD kArchiveReadWindowRva = 0x11d13a;
const BYTE kArchiveReadWindowBytes[] = {
    0x6a, 0x00,
    0x8d, 0x44, 0x24, 0x18, 0x50,              // lea eax,[esp+0x18]; push  read
    0x8b, 0x44, 0x24, 0x18,                    // mov eax,[esp+0x18] descriptor
    0xff, 0x70, 0x04,                          // push [eax+4]      desc.csize
    0x8b, 0x44, 0x24, 0x18,                    // mov eax,[esp+0x18] this
    0xff, 0x77, 0x04,                          // push [edi+4]      bb.compressed
    0xff, 0x70, 0x0c,                          // push [eax+0xc]    the HANDLE
    0xff, 0x15, 0, 0, 0, 0                     // call ReadFile
};
const DWORD kReadFileSlotRva = 0x2ac1a4;
const Relocation kArchiveReadWindowRelocs[] = {{26, kReadFileSlotRva}};

// The inflate, and the single strongest table in this file. In twenty-five
// bytes it names every remaining field of the cache's key and its destination,
// in the operands of the call that actually consumes them:
//
//   push [ecx+4]        desc.compressedSize   -> uncompress's sourceLen
//   mov  eax,[ecx+8]    desc.uncompressedSize -> its destLen, written below
//   push [edi+4]        blockBuffer[1]        -> its source
//   mov  ecx,[edi+8]    blockBuffer[2]        -> its dest, which is the block
//   call 0x10065760     zlib uncompress, built __fastcall
//
// The displacement is relative and unrelocated, so comparing these bytes at
// this exact address is the same statement as "this call goes to the inflate",
// which is what distinguishes this function from the three others that open
// with `55 8b ec 83 e4 f8`.
const DWORD kArchiveInflateWindowRva = 0x11d1c2;
const BYTE kArchiveInflateWindowBytes[] = {
    0xff, 0x71, 0x04,                          // push [ecx+4]     desc.csize
    0x8b, 0x41, 0x08,                          // mov eax,[ecx+8]  desc.usize
    0xff, 0x77, 0x04,                          // push [edi+4]     bb.compressed
    0x8b, 0x4f, 0x08,                          // mov ecx,[edi+8]  bb.block
    0x8d, 0x54, 0x24, 0x14,                    // lea edx,[esp+0x14]
    0x89, 0x44, 0x24, 0x14,                    // [esp+0x14] = desc.usize
    0xe8, 0x85, 0x85, 0xf4, 0xff               // call 0x10065760
};

// The epilogue, which is the contract a cache hit has to reproduce exactly:
// store the block index into the caller's one-slot cache, return true in AL,
// and pop twelve bytes of arguments. `FUN_1011d240` never looks at the return
// value, but the stack discipline is not optional.
const DWORD kArchiveBlockTailRva = 0x11d230;
const BYTE kArchiveBlockTailBytes[] = {
    0x89, 0x1f,                                // mov [edi],ebx    cached = block
    0x5f, 0x5e,
    0xb0, 0x01,                                // mov al,1
    0x5b,
    0x8b, 0xe5, 0x5d,
    0xc2, 0x0c, 0x00                           // ret 0xc          three args
};

// The block size, which has exactly one writer in the whole image. A slot is
// one block, so this is what says a slot is 256 KiB; the cache also re-reads
// `archive[0x40]` at runtime and refuses to key anything whose archive says
// otherwise.
const DWORD kArchiveBlockSizeRva = 0x11ea94;
const BYTE kArchiveBlockSizeBytes[] = {
    0xc7, 0x46, 0x40, 0x00, 0x00, 0x04, 0x00   // mov [esi+0x40],0x40000
};

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

// Offsets the four windows above establish, named once so the code that reads
// them says where each came from.
const unsigned kArchiveEntryTableOffset = 0x2c;
const unsigned kArchiveHandleOffset = 0xc;
const unsigned kArchiveBlockSizeOffset = 0x40;
const unsigned kArchiveEntryStride = 0x44;
const unsigned kArchiveEntryDescriptorsOffset = 0x20;
const unsigned kArchiveDescriptorStride = 0xc;

// --- Region::WaitForLoadingToFinish. Seven bytes: `cmp byte [ecx+0x78],1`,
// `jz -6`, `ret`. The stolen bytes would contain that relative jump, so a
// trampoline is impossible and the replacement implements the spin itself.
const DWORD kWaitForLoadingRva = 0x20bde0;
const char kWaitForLoadingName[] = "?WaitForLoadingToFinish@Region@GAME@@QAEXXZ";
const BYTE kWaitForLoadingBytes[] = {0x80, 0x79, 0x78, 0x01, 0x74, 0xfa, 0xc3};

// --- The region lock, on the render path. Three sites with an identical
// twenty-eight byte shape:
//     ff 7? 08                push [region+8]
//     ff 15 <EnterCriticalSection>
//     8b 7? 50                mov esi,[region+0x50]
//     c7 4? 6c 00 00 00 00    mov dword [region+0x6c],0
//     ff 7? 08                push [region+8]
//     ff 15 <LeaveCriticalSection>
// The critical section covers two loads, so a hit here is the render thread
// waiting on whoever else holds the region -- which is the audit's §1b, and
// has never been measured.
const DWORD kEnterCriticalSectionSlotRva = 0x2ac17c;
const DWORD kLeaveCriticalSectionSlotRva = 0x2ac178;
const BYTE kRegionLockEbxBytes[] = {
    0xff, 0x73, 0x08,
    0xff, 0x15, 0, 0, 0, 0,
    0x8b, 0x73, 0x50,
    0xc7, 0x43, 0x6c, 0x00, 0x00, 0x00, 0x00,
    0xff, 0x73, 0x08,
    0xff, 0x15, 0, 0, 0, 0
};
const BYTE kRegionLockEdiBytes[] = {
    0xff, 0x77, 0x08,
    0xff, 0x15, 0, 0, 0, 0,
    0x8b, 0x77, 0x50,
    0xc7, 0x47, 0x6c, 0x00, 0x00, 0x00, 0x00,
    0xff, 0x77, 0x08,
    0xff, 0x15, 0, 0, 0, 0
};
const Relocation kRegionLockRelocs[] = {{5, kEnterCriticalSectionSlotRva},
                                        {24, kLeaveCriticalSectionSlotRva}};
static_assert(sizeof(kRegionLockEbxBytes) == sizeof(kRegionLockEdiBytes),
              "both region-lock windows are the same twenty-eight byte shape");
const unsigned kRegionLockCallOffset = 3;

struct LockSite {
    const char* owner;          // decorated name of the containing export
    DWORD ownerRva;             // asserted against what the name resolves to
    DWORD windowRva;
    const BYTE* bytes;
};
const LockSite kLockSites[] = {
    {"?GetEntitiesInFrustum@Region@GAME@@QBEXAAV?$vector@PAVEntity@GAME@@"
     "V?$allocator@PAVEntity@GAME@@@std@@@std@@ABVFrustum@2@_NPBV12@"
     "W4EntityListType@2@22@Z",
     0x209840, 0x209896, kRegionLockEbxBytes},
    {"?AddElementsInBox@GraphicsDeferredRendererX@GAME@@UAEXPAVRegion@2@"
     "ABVOBBox@2@ABVCoords@2@@Z",
     0x1677e0, 0x167869, kRegionLockEdiBytes},
    {"?AddElementsInBox@GraphicsForwardRenderer@GAME@@UAEXPAVRegion@2@"
     "ABVOBBox@2@ABVCoords@2@@Z",
     0x17d850, 0x17d8d9, kRegionLockEdiBytes},
};
const unsigned kLockSiteCount = sizeof(kLockSites) / sizeof(kLockSites[0]);

// The three deferral thunks, declared here because the site table below names
// them and they are defined with the other hooks.
int __fastcall hookAddElementsLoadLevel(void* self, void* edx, int background);
int __fastcall hookPortalLoadLevel(void* self, void* edx, int background);

// --- [performance] async_level_load: the call sites that force a level load
// synchronously, and the engine's own asynchronous entry point to send them
// to instead.
//
// Two are in the renderers and, as runs 27-32 measured, never defer anything:
// they find the region already resident every time, because the game's own
// RegionLoader keeps ahead of the player. The third is portal traversal and is
// the only synchronous level load that happens during play.
//
// The sites are the thirty-four bytes immediately *before* the two
// AddElementsInBox region-lock windows above -- 0x167847 + 34 == 0x167869 and
// 0x17d8b7 + 34 == 0x17d8d9, which are the two windowRvas in kLockSites. The
// two groups are adjacent and disjoint, and that arithmetic is a cross-check
// on both: each table ends exactly where the other begins.
//
//   85 ff                  test edi,edi          Region*, never null past here
//   0f 84 dd 00 00 00      jz  the epilogue
//   6a 00                  push 0                the flag both sites pass
//   8b cf                  mov ecx,edi
//   e8 <rel32>             call Region::LoadLevel          <- offset 12
//   80 7f 74 00            cmp byte [edi+0x74],0           the loading flag
//   c7 47 6c 00 00 00 00   mov dword [edi+0x6c],0          unload countdown
//   0f 85 c3 00 00 00      jnz the epilogue      skip the region this frame
//
// Three things that sentence-by-sentence justify the change:
//
// - The renderer's own skip test is `cmp byte [edi+0x74],0` / `jnz`, and
//   `[0x74]` is exactly the byte BackgroundLoadLevel sets. Deferring is
//   therefore not something bolted on: the call site is already written to
//   handle a region that did not load, and the flag it reads is the flag the
//   asynchronous path raises. verify-sites.py asserts the two offsets agree.
// - `MOV` does not touch flags, so `mov dword [edi+0x6c],0` runs on the skip
//   path as well: a deferred region still has its unload countdown reset and
//   cannot be evicted while its load is in flight.
// - EAX is dead across the call. The next instruction is the `CMP` above,
//   which overwrites the flags, and nothing between it and the `JNZ` reads
//   the register -- so what the thunk returns cannot be observed.
//
// The two tables differ only in the call displacement, which is relative and
// therefore not a Relocation; each site carries its own copy and patchCall
// re-derives the destination from it anyway.
const BYTE kForceLoadDeferredBytes[] = {
    0x85, 0xff,
    0x0f, 0x84, 0xdd, 0x00, 0x00, 0x00,
    0x6a, 0x00,
    0x8b, 0xcf,
    0xe8, 0x68, 0x46, 0x0a, 0x00,              // call 0x1020bec0
    0x80, 0x7f, 0x74, 0x00,
    0xc7, 0x47, 0x6c, 0x00, 0x00, 0x00, 0x00,
    0x0f, 0x85, 0xc3, 0x00, 0x00, 0x00
};
const BYTE kForceLoadForwardBytes[] = {
    0x85, 0xff,
    0x0f, 0x84, 0xdd, 0x00, 0x00, 0x00,
    0x6a, 0x00,
    0x8b, 0xcf,
    0xe8, 0xf8, 0xe5, 0x08, 0x00,              // call 0x1020bec0
    0x80, 0x7f, 0x74, 0x00,
    0xc7, 0x47, 0x6c, 0x00, 0x00, 0x00, 0x00,
    0x0f, 0x85, 0xc3, 0x00, 0x00, 0x00
};
static_assert(sizeof(kForceLoadDeferredBytes) == sizeof(kForceLoadForwardBytes),
              "both forced-load windows are the same thirty-four byte shape");
const unsigned kForceLoadCallOffset = 12;

// --- The third site, and the only synchronous level load that happens during
// play. Run 30 caught it once in 11,055 frames: 105.7 ms on an Engine::Update
// frame, where the two above only ever fire on a level change. Its containing
// function is not exported, so identity rests on the bytes -- which is why the
// window runs past the branch to the EnterCriticalSection import, whose
// relocated dword is compared against the slot rather than literally.
//
//   10117a8c  85 f6              test esi,esi     the far-side Region*
//   10117a8e  74 7b              jz  past
//   10117a90  6a 00              push 0
//   10117a92  8b ce              mov ecx,esi
//   10117a94  e8 <rel32>         call Region::LoadLevel        <- offset 8
//   10117a99  80 7e 74 00        cmp byte [esi+0x74],0
//   10117a9d  c7 46 6c 00..      mov dword [esi+0x6c],0
//   10117aa4  75 19              jnz 0x10117abf   still loading -> skip
//   10117aa6  ff 76 08           push [esi+8]
//   10117aa9  ff 15 <EnterCS>
//
// The call is at offset **8**, not 12: the guard here is the two-byte `jz`
// rather than the six-byte form the renderers use, which is why the site
// table carries the offset per entry.
//
// Its containing function -- `World::GetRegionsInFrustum`,
// `WorldFrustum::GetRelativeFrustum`, `Portal::GetConnectedRegion`, then this
// call, then `Region::GetPortal` and `Portal::GetChokePoint` -- is portal
// traversal: the region on the far side of a doorway, forced resident.
//
// It is the best-founded of the three, and by one thing the others cannot
// claim: the only call it makes on that region afterwards,
// `Region::GetPortal` at 0x10117ac8, has its result null-checked two
// instructions later (`test eax,eax / jz`). So the code past the branch
// already copes with a region whose level is not there.
const DWORD kPortalLoadWindowRva = 0x117a8c;
const BYTE kPortalLoadBytes[] = {
    0x85, 0xf6,
    0x74, 0x7b,
    0x6a, 0x00,
    0x8b, 0xce,
    0xe8, 0x27, 0x44, 0x0f, 0x00,              // call 0x1020bec0
    0x80, 0x7e, 0x74, 0x00,
    0xc7, 0x46, 0x6c, 0x00, 0x00, 0x00, 0x00,
    0x75, 0x19,
    0xff, 0x76, 0x08,
    0xff, 0x15, 0, 0, 0, 0                     // call EnterCriticalSection
};
const Relocation kPortalLoadRelocs[] = {{31, kEnterCriticalSectionSlotRva}};
const unsigned kPortalLoadCallOffset = 8;

struct ForceLoadSite {
    // The decorated name of the containing export, or null when the function
    // is not exported and the bytes are the whole identity.
    const char* owner;
    DWORD ownerRva;             // asserted against what the name resolves to
    DWORD windowRva;
    const BYTE* bytes;
    SIZE_T size;
    const Relocation* relocations;
    unsigned relocationCount;
    unsigned callOffset;        // 12 in the renderers, 8 here
    const void* replacement;    // its own thunk, so each site gets its own columns
};
// A full .text scan for `E8` displacements resolving to Region::LoadLevel
// finds thirty-eight call sites. These are the only two inside a renderer;
// GraphicsSceneRenderer::AddElementsInBox, the third of the three
// AddElementsInBox overrides, does not call it at all.
const ForceLoadSite kForceLoadSites[] = {
    {"?AddElementsInBox@GraphicsDeferredRendererX@GAME@@UAEXPAVRegion@2@"
     "ABVOBBox@2@ABVCoords@2@@Z",
     0x1677e0, 0x167847, kForceLoadDeferredBytes,
     sizeof(kForceLoadDeferredBytes), nullptr, 0, 12,
     (const void*)&hookAddElementsLoadLevel},
    {"?AddElementsInBox@GraphicsForwardRenderer@GAME@@UAEXPAVRegion@2@"
     "ABVOBBox@2@ABVCoords@2@@Z",
     0x17d850, 0x17d8b7, kForceLoadForwardBytes,
     sizeof(kForceLoadForwardBytes), nullptr, 0, 12,
     (const void*)&hookAddElementsLoadLevel},
    // Not exported, so no owner to resolve: the thirty-five bytes and the
    // relocated EnterCriticalSection slot are the identity.
    {nullptr, 0, kPortalLoadWindowRva, kPortalLoadBytes,
     sizeof(kPortalLoadBytes), kPortalLoadRelocs, 1, kPortalLoadCallOffset,
     (const void*)&hookPortalLoadLevel},
};
const unsigned kForceLoadSiteCount =
    sizeof(kForceLoadSites) / sizeof(kForceLoadSites[0]);

// --- Region::BackgroundLoadLevel, `__thiscall void(bool, bool)`, 85 bytes.
// Not detoured and not patched: it is resolved and called, so what has to be
// verified is not its identity but its *behaviour*, because the thunk works
// around one specific thing it does.
//
// Three windows, and the first is the one that carries the trap.
//
//   1020be60  8b 41 50        mov eax,[ecx+0x50]      the loaded Level*
//   1020be63  8a 54 24 04     mov dl,[esp+4]          only the FIRST bool
//   1020be67  83 ec 0c        sub esp,0xc
//   1020be6a  85 c0           test eax,eax
//   1020be6c  74 0d           jz  0x1020be7b          not resident -> proceed
//   1020be6e  84 d2           test dl,dl
//   1020be70  74 3d           jz  0x1020beaf          <-- DOES NOTHING
//
// With `region[0x50]` non-null and a false flag -- which is exactly what both
// call sites pass -- this function returns having done nothing. It does not
// set `[0x74]`, so the caller's `JNZ` would not fire and the renderer would
// draw an unloaded region. Those calls have to go to the original
// Region::LoadLevel, which is what the thunk's `region[0x50]` test is for,
// and it is the reason that test exists rather than a defensive habit.
//
// It costs nothing: `region[0x50]` non-null is the resident case, which
// Region::LoadLevel answers out of its own first three instructions.
const DWORD kBackgroundLoadLevelRva = 0x20be60;
const char kBackgroundLoadLevelName[] =
    "?BackgroundLoadLevel@Region@GAME@@QAEX_N0@Z";
const BYTE kBackgroundEntryBytes[] = {
    0x8b, 0x41, 0x50,                          // mov eax,[ecx+0x50]
    0x8a, 0x54, 0x24, 0x04,                    // mov dl,[esp+4]
    0x83, 0xec, 0x0c,
    0x85, 0xc0,                                // test eax,eax
    0x74, 0x0d,                                // jz  +0xd -> the flag window
    0x84, 0xd2,                                // test dl,dl
    0x74, 0x3d                                 // jz  the epilogue
};

// The second window is the re-entry guard and the flag the renderer reads,
// and it says the thunk needs no in-flight test of its own:
//
//   1020be7b  80 79 74 00     cmp byte [ecx+0x74],0
//   1020be7f  75 2e           jnz the epilogue      already loading -> bail
//   1020be81  80 79 75 00     cmp byte [ecx+0x75],0
//   1020be85  75 28           jnz the epilogue
//   1020be87  83 79 50 00     cmp dword [ecx+0x50],0
//   1020be8b  74 06           jz  0x1020be93
//   1020be8d  c6 41 75 01     mov byte [ecx+0x75],1
//   1020be91  eb 04           jmp past
//   1020be93  c6 41 74 01     mov byte [ecx+0x74],1   <- the renderer's flag
//
// The thunk only ever routes the `[0x50] == 0` case here, so the branch at
// 0x1020be8b is always taken and `[0x74]` -- not `[0x75]` -- is what gets
// set. That is the byte the call site's `cmp byte [edi+0x74],0` reads, so the
// region is skipped for this frame and picked up by RegionLoader::Update.
// Region::Update sets the same two flags in the same shape at +0x372, +0x3d9
// and +0x777, so this is the engine's own idiom and not a new state.
const DWORD kBackgroundFlagsRva = 0x20be7b;
const BYTE kBackgroundFlagsBytes[] = {
    0x80, 0x79, 0x74, 0x00,
    0x75, 0x2e,
    0x80, 0x79, 0x75, 0x00,
    0x75, 0x28,
    0x83, 0x79, 0x50, 0x00,                    // cmp dword [ecx+0x50],0
    0x74, 0x06,
    0xc6, 0x41, 0x75, 0x01,                    // mov byte [ecx+0x75],1
    0xeb, 0x04,
    0xc6, 0x41, 0x74, 0x01                     // mov byte [ecx+0x74],1
};

// The third is the epilogue, which is the ABI the thunk calls with: two stack
// arguments, callee popped. Every early exit above jumps here, so this is also
// what says the do-nothing path is a clean return and not a fall-through.
const DWORD kBackgroundTailRva = 0x20beaf;
const BYTE kBackgroundTailBytes[] = {
    0x83, 0xc4, 0x0c,                          // add esp,0xc
    0xc2, 0x08, 0x00                           // ret 8      two bools
};

// The one field of Region this file dereferences, taken from the operand of
// the instruction that uses it -- `mov eax,[ecx+0x50]`, the first instruction
// of both Region::LoadLevel and Region::BackgroundLoadLevel -- rather than
// from a layout table. Non-null means the level is resident.
const unsigned kRegionLevelOffset = 0x50;
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
const unsigned kSweepCount = 7;

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

// --- GameEngine::Update, in Game.dll rather than Engine.dll. It is here
// because run 11 closed the frame's accounting and found the answer was not
// in Engine.dll at all: 58% of the session is Engine::Render and 10% is
// Engine::Update, but 38% of the hitch time -- and eighteen of the thirty-two
// frames over 100 ms -- fall outside both, on frames that draw 400 to 1,600
// times with the mod completely idle. Against a normal frame's 0.21 ms
// outside, those spend 100 to 225 ms there. This is the one bracket large
// enough to hold it.
//
// `?Update@GameEngine@GAME@@QAEXH@Z` is __thiscall void(int): one stack
// argument, callee popped, confirmed by the `ret 4` that ends the body at
// +0x5ee.
const DWORD kGameImageSize = 0x0059a000;
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

// --- TQ.exe's main loop. Not a patch at all: three entries of the
// executable's own import address table, so the redirect is scoped to the one
// module that has the stall in it and every other caller in the process --
// including the mod's own -- keeps the real function.
//
// The loop's entire vocabulary for blocking is these three. It imports no
// PeekMessage, so its pump is the *blocking* GetMessageA; and it imports
// GameEngine::NeedsSleep beside Sleep, so it is a frame limiter. Either can
// hand back an arbitrary amount of time on a host that is momentarily busy,
// which is the shape run 12 measured.
const DWORD kExecutableImageSize = 0x0036a000;

// ---------------------------------------------------------------------------
// Configuration. Bit 0 means "everything"; the rest select groups, so a run
// that misbehaves can be narrowed from the INI rather than from a rebuild.
const unsigned kGroupAll = 0x01;
const unsigned kGroupLoads = 0x02;
const unsigned kGroupArchive = 0x04;
const unsigned kGroupFence = 0x08;
const unsigned kGroupLock = 0x10;
const unsigned kGroupSweeps = 0x20;
const unsigned kGroupWait = 0x40;
const unsigned kGroupFrame = 0x80;
const unsigned kGroupGame = 0x100;
const unsigned kGroupLoop = 0x200;
const unsigned kGroupPump = 0x400;
const unsigned kGroupHeap = 0x800;
const unsigned kGroupArcIo = 0x1000;
const unsigned kGroupBlocking = 0x2000;
const unsigned kGroupShadow = 0x4000;

unsigned g_traceMask = 1;
unsigned g_timerPeriodMs;   // 0 = leave the game's own period alone
// [performance] pump_timer_min_ms. A game-behaviour change rather than an
// instrument, so like archive_cache_mb and async_level_load it defaults off,
// installs nothing at 0, and reaches install() with the performance probe off.
//
// What it is for. Across runs 34-37 the message pump is the only in-play
// stutter class the GPU work does not touch: 7-21 frames a minute of 60-225 ms
// with Engine::Render under 15 ms, 1.3-3.2 seconds of stall per minute of play.
// The cost is not "asking the host" -- an empty poll is 1 us. It is the
// retrieval: run 35's frame 5303 spent 221,249 us in the two peeks that
// returned a message and 1 us in the one that did not. §16 found 76% of slow
// retrievals return WM_TIMER, which PeekMessage *synthesizes* when the queue is
// otherwise empty and a timer has expired, rather than dequeues.
//
// So the lever §17 did not try: keep WM_TIMER out of the peek's range on most
// calls, and let an unfiltered peek -- the only kind that can synthesize one --
// through no more than once every pump_timer_min_ms. The game still receives
// WM_TIMER, just not on every poll; at the 14.2 a second §16 measured, a floor
// of 50 ms leaves the cadence essentially intact while cutting unfiltered peeks
// from a couple of hundred a second to twenty.
//
// If the stalls follow the unfiltered peek, this is the lever. If they simply
// move to the other messages, the pump really is a host property and §17's
// closure stands -- which is worth one boot either way.
unsigned g_pumpTimerMinMs;  // 0 = never filter, the stock pump
LONG g_pumpLastFullTick;    // GetTickCount of the last unfiltered peek
// [performance] async_level_load. A game-behaviour change rather than an
// instrument, so like archive_cache_mb it defaults off, installs nothing at
// 0, and reaches install() with the performance probe off.
bool g_asyncLevelLoad;
// [performance] shadow_transition_reuse. On a directional shadow region
// change, preserve the previous global depth map and explicitly restore its
// matching matrix for one frame. A fix rather than an instrument: defaults
// off, reaches install() with the probe off, and does no timing in that mode.
bool g_shadowTransitionReuse;
// [performance] shadow_defer_cold_alpha. Alpha-tested shadow casters whose
// base texture is not resident are omitted from this directional build and
// explicitly handed to the engine's loader. They return on the first later
// build after the texture reaches loaded state 2. Opaque casters and colour
// rendering are untouched.
bool g_shadowDeferColdAlpha;
bool g_shadowDeferActive;
// Whether this install() is installing the trace at all. archive_cache_mb can
// reach install() with the performance probe off, and without this every
// wants() below would read the trace mask -- which defaults to 1 -- and put
// the whole instrument in on a boot that asked only for the cache.
bool g_tracing;
bool g_pumpTracing;

LONG g_installed;
unsigned g_installedHooks;

const volatile DWORD* g_mainThreadId;

bool onMainThread() {
    return g_mainThreadId && *g_mainThreadId == GetCurrentThreadId();
}

// ---------------------------------------------------------------------------
// Hooks. Titan Quest is __thiscall throughout; __fastcall with a second
// argument nobody reads matches it -- this in ecx, the rest on the stack,
// callee cleaned -- which is what src/grass.cpp already relies on. The bool
// arguments are declared int so the caller's whole pushed dword goes through
// unchanged.

typedef int (__fastcall* LoadLevelFn)(void* self, void* edx, int background);
typedef void (__fastcall* BackgroundLoadLevelFn)(void* self, void* edx,
                                                 int background, int second);
typedef void* (__fastcall* GuaranteedGetLevelFn)(void* self, void* edx,
                                                int flag);
typedef void (__fastcall* LoadResourceFn)(void* self, void* edx, void* resource);
typedef void (__fastcall* EnsureAvailableFn)(void* self, void* edx);
typedef const char* (__fastcall* ResourceFileNameFn)(void* self, void* edx);
typedef void* (__fastcall* GraphicsTextureGetTextureFn)(void* self, void* edx);
typedef void (__fastcall* GraphicsMeshSetShaderParametersFn)(
    void* self, void* edx, const void* shader, int materialIndex);
typedef int (__fastcall* SetTextureParameterFn)(
    void* shader, void* edx, const void* name, unsigned index,
    void* reserved, void* textureValue);
typedef bool (__fastcall* ShaderHasParameterFn)(
    void* shader, void* edx, const void* name);
typedef int (__fastcall* BuildShadowRecordFn)(
    void* renderer, void* edx, void* output, void* renderableEntry, int pass);
typedef int (__fastcall* ShadowEligibleFn)(void* self, void* edx);
typedef int (__fastcall* MeshShadowStyleFn)(void* self, void* edx, int pass);
typedef const void* (__fastcall* MeshGetTextureFn)(
    void* self, void* edx, int pass, const void* name);
typedef void* (__fastcall* ResourceLoaderAccessorFn)(void* self, void* edx);
typedef int (__fastcall* RenderDirectionalFn)(
    void* self, void* edx, void* canvas, const void* camera,
    const void* frustum, int algorithm, void* surface, void* matrix);
typedef void (__fastcall* UnloadLevelFn)(void* self, void* edx, int a, int b);
typedef void (__fastcall* EnqueueFn)(void* self, void* edx, const void* resource,
                                     int priority, int a, int b);
typedef int (__fastcall* ReadFromFileFn)(void* self, void* edx, int entry,
                                         BYTE* dest, unsigned offset,
                                         unsigned size, void* blockBuffer);
typedef int (__fastcall* ArchiveBlockFn)(void* self, void* edx, unsigned entry,
                                         unsigned block, void* blockBuffer);
typedef void (__fastcall* SweepFn)(void* self, void* edx);
typedef void (__fastcall* EngineUpdateFn)(void* self, void* edx,
                                          const void* frustum, int flag);
typedef void (__fastcall* EngineRenderFn)(void* self, void* edx);
typedef void (__fastcall* GameUpdateFn)(void* self, void* edx, int delta);
typedef void* (__cdecl* NewArrayFn)(size_t bytes);
typedef void (__cdecl* DeleteArrayFn)(void* block);
typedef BOOL (WINAPI* SetFilePointerExFn)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER,
                                          DWORD);
typedef BOOL (WINAPI* ReadFileFn)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef DWORD (WINAPI* WaitMultipleFn)(DWORD, const HANDLE*, BOOL, DWORD);
typedef void (WINAPI* SleepFn)(DWORD);
typedef DWORD (WINAPI* WaitFn)(HANDLE, DWORD);

LoadLevelFn g_loadLevel;
// Region::LoadLevel and Region::BackgroundLoadLevel as the module exports
// them, which is not the same thing as g_loadLevel above. g_loadLevel is the
// trace's trampoline and exists only when the loads group is installed;
// these two are resolved addresses and work with the probe off. Calling the
// export means that when the trace *is* installed the call still lands in
// hookLoadLevel and is counted, which is what we want.
LoadLevelFn g_regionLoadLevel;
BackgroundLoadLevelFn g_backgroundLoadLevel;
GuaranteedGetLevelFn g_guaranteedGetLevel;
LoadResourceFn g_loadResource;
EnsureAvailableFn g_ensureAvailable;
ResourceFileNameFn g_resourceFileName;
GraphicsTextureGetTextureFn g_graphicsTextureGetTexture;
GraphicsMeshSetShaderParametersFn g_graphicsMeshSetShaderParameters;
SetTextureParameterFn g_setTextureParameter;
ShaderHasParameterFn g_shaderHasParameter;
BuildShadowRecordFn g_buildShadowRecord;
MeshShadowStyleFn g_meshShadowStyle;
MeshGetTextureFn g_meshGetTexture;
ResourceLoaderAccessorFn g_resourceLoaderAccessor;
EnqueueFn g_shadowEnqueue;
RenderDirectionalFn g_renderDirectional;
UnloadLevelFn g_unloadLevel;
EnqueueFn g_enqueue;
ReadFromFileFn g_readFromFile;
ArchiveBlockFn g_archiveBlock;
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
SleepFn g_engineSleep;
unsigned g_renderTicks;

Detour g_loadLevelDetour;
Detour g_guaranteedDetour;
Detour g_loadResourceDetour;
Detour g_unloadLevelDetour;
Detour g_enqueueDetour;
Detour g_readFromFileDetour;
Detour g_archiveBlockDetour;
Detour g_waitForLoadingDetour;
Detour g_engineUpdateDetour;
Detour g_engineRenderDetour;
Detour g_gameUpdateDetour;
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
CallPatch g_shadowDirectionalPatch;
CallPatch g_shadowMeshEnsurePatch;
CallPatch g_shadowMaterialTexturePatch;
CallPatch g_shadowTextureParameterPatch;
CallPatch g_shadowMeshParameterPatch;
CallPatch g_shadowInstanceBumpEnsurePatch;
CallPatch g_shadowRecordPatch;
LONG g_insideDirectional;
void* g_lastShadowRegion;
void* g_cachedShadowSurface;
DWORD g_cachedShadowMatrix[kShadowMatrixDwords];
int g_cachedShadowResult;
bool g_cachedShadowValid;
bool g_reusedLastShadow;
bool g_shadowTracing;
bool g_resourceStateVerified;
bool g_resourceFileNameVerified;
bool g_shaderHasParameterVerified;
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

enum ShadowContextMatch {
    ShadowContextExact,
    ShadowContextClassOther,
    ShadowContextPassMismatch,
    ShadowContextInstanceMissing
};

enum ShadowMeshContextPatchStatus {
    ShadowMeshContextPatchActive,
    ShadowMeshContextPatchDependencyMissing,
    ShadowMeshContextPatchFrameMismatch,
    ShadowMeshContextPatchEntryMismatch,
    ShadowMeshContextPatchContextMismatch,
    ShadowMeshContextPatchCallFailed,
    ShadowMeshContextPatchReverted,
    ShadowMeshContextPatchStatusCount
};

struct ShadowMeshParameterContext {
    bool active;
    bool styleKnown;
    ShadowContextMatch match;
    void* instance;
    unsigned style;
    int pass;
    bool baseKnown;
    const void* baseTexture;
    bool outerInstanceSite;
};
ShadowMeshParameterContext g_shadowMeshParameterContext;
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

const BYTE* g_engineBase;

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

struct ChainFrame {
    DWORD rva;
    char tag;
};
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

// [performance] async_level_load. Reached only from the two AddElementsInBox
// call sites, which patchCall retargeted: Region::LoadLevel itself is
// untouched, so its other thirty-six callers are exactly as they were and
// there is no recursion to worry about.
//
// Same ABI as Region::LoadLevel -- __thiscall(bool) is GCC __fastcall with a
// dead edx, one stack argument, callee-pop -- which is the LoadLevelFn typedef
// the trace already uses.
//
// `self` needs no null check: the call site's own `test edi,edi / jz` two
// instructions earlier is what guarantees it, and reading `self + 0x50` is
// what the engine does unconditionally in the first instruction of both
// functions. The return value needs no thought either -- the caller branches
// on `[self+0x74]`, not on EAX. See kForceLoadDeferredBytes.
int deferLoad(void* self, void* edx, int background,
              tq::probe::Counter deferred, tq::probe::Counter fellThrough) {
    if (!g_regionLoadLevel) return 0;
    // Resident already, or nothing to defer to: BackgroundLoadLevel answers
    // the resident case by returning without setting [0x74], which would
    // leave the renderer drawing an unloaded region. The original answers it
    // out of its own first three instructions, so this costs nothing.
    if (!g_backgroundLoadLevel
        || *(void* const*)((BYTE*)self + kRegionLevelOffset) != nullptr) {
        tq::probe::engineCount(fellThrough);
        return g_regionLoadLevel(self, edx, background);
    }
    // The flag is forwarded rather than hardcoded. Both sites push 0 today --
    // it is in the byte tables -- so the two are the same call; forwarding is
    // what keeps that a fact about the sites rather than an assumption baked
    // in here. The second bool is never read: the function stores only the
    // first into the work item it queues.
    //
    // No in-flight check is needed. BackgroundLoadLevel guards its own
    // re-entry on [0x74] and [0x75] before it queues anything.
    g_backgroundLoadLevel(self, edx, background, 0);
    tq::probe::engineCount(deferred);
    return 1;
}

// One thunk per site, so each gets its own pair of columns. They are three
// instructions each; what they exist for is to name which site deferred.
int __fastcall hookAddElementsLoadLevel(void* self, void* edx, int background) {
    return deferLoad(self, edx, background, tq::probe::CounterEngineAsyncLoad,
                     tq::probe::CounterEngineAsyncSync);
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

enum ShadowResourceType {
    ShadowResourceMesh,
    ShadowResourceShader,
    ShadowResourceTexture,
    ShadowResourceOther,
};

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

extern "C" void* __cdecl shadowMaterialTextureFiltered(
    void* texture, const void* name, void* shader, const void* outerCaller) {
    if (!g_graphicsTextureGetTexture) return nullptr;
    const bool inShadow = onMainThread()
        && InterlockedCompareExchange(&g_insideDirectional, 0, 0) > 0
        && g_resourceStateVerified;
    const bool cold = inShadow && texture
        && *(const unsigned*)((const BYTE*)texture
                             + kResourceLoadedStateOffset) == 0;
    ShadowMeshParameterContext context = g_shadowMeshParameterContext;
    const void* const baseOverride = context.instance
        ? *(const void* const*)((const BYTE*)context.instance + 0x14)
        : nullptr;
    const bool overriddenBase = g_shadowDeferActive && inShadow && texture
        && name && g_engineBase && baseOverride
        && texture != baseOverride
        && memcmp(name, g_engineBase + kBaseTextureNameRva, 16) == 0;
    if (overriddenBase) {
        if (g_shadowTracing) {
            tq::probe::engineCount(
                tq::probe::CounterEngineShadowBaseOverrideSkipped);
            if (cold)
                tq::probe::engineCount(
                    tq::probe::CounterEngineShadowBaseOverrideSkippedCold);
        }
        // The verified enclosing GraphicsMeshInstance method ensures and
        // binds this exact non-null +0x14 override to baseTexture immediately
        // after the base material call returns, before any draw can observe
        // the temporary null binding.
        return nullptr;
    }
    const bool canFilter = g_shadowDeferActive && inShadow && texture
        && shader && name && g_shaderHasParameterVerified
        && g_shaderHasParameter
        && *(const unsigned*)((const BYTE*)shader
                             + kResourceLoadedStateOffset) == 2;
    if (canFilter && !g_shaderHasParameter(shader, nullptr, name)) {
        if (g_shadowTracing) {
            tq::probe::engineCount(
                tq::probe::CounterEngineShadowMaterialTexSkipped);
            if (cold)
                tq::probe::engineCount(
                    tq::probe::CounterEngineShadowMaterialTexSkippedCold);
        }
        // The adjacent original setter receives null, then makes the same
        // HasParameter decision and discards it. No declared shader input and
        // therefore no rendered value changes.
        return nullptr;
    }
    const int64_t started = cold ? tq::probe::now() : 0;
    // With the context patch active, the C wrapper is necessarily the
    // enclosing caller visible from GraphicsMesh::SetShaderParameters. The
    // original return address remains directly visible only when that patch
    // is absent, which was run 55. Either condition names the same verified
    // base GraphicsMeshInstance site.
    context.outerInstanceSite = context.instance
        || outerCaller == (const void*)(
            g_engineBase + kShadowMeshParameterCallRva
            + kShadowMeshParameterCallOffset + 5);
    if (cold && !context.active) explainShadowRecordMiss(&context);
    const bool priorMaterial = g_insideShadowMaterialTexture;
    if (inShadow) g_insideShadowMaterialTexture = true;
    void* const result = g_graphicsTextureGetTexture(texture, nullptr);
    g_insideShadowMaterialTexture = priorMaterial;
    if (cold && g_shadowTextureParameterHooked) {
        // The patched code has exactly one setter after each getter. Preserve
        // a complete partition even if an unexpected control flow violates
        // that relationship.
        flushPendingShadowMaterialTexture(false, false);
        g_shadowMaterialTexturePending = true;
        g_shadowMaterialTexturePendingUs =
            tq::probe::microsecondsSince(started);
        g_shadowMaterialPendingNameHash =
            g_nameHashLayoutVerified && name
                ? *(const uint32_t*)name : 0;
        g_shadowMaterialPendingContext = context;
        g_shadowMaterialPendingTexture = texture;
    }
    return result;
}

// The material Name lives in ESI and the active shadow shader at caller
// ESP+0x1c. The patched E8 adds a return address, making that ESP+0x20 here;
// the enclosing GraphicsMesh caller's return address is at ESP+0x1c. A naked
// adapter is the only way to forward these values without changing the game's
// call-site ABI; the C helper above preserves the nonvolatile registers.
void* __attribute__((naked)) __fastcall hookShadowMaterialTexture(
    void*, void*) {
    __asm__ __volatile__(
        "pushl 0x1c(%%esp)\n\t"              // enclosing caller return
        "pushl 0x24(%%esp)\n\t"              // shader after first push
        "pushl %%esi\n\t"
        "pushl %%ecx\n\t"
        "call _shadowMaterialTextureFiltered\n\t"
        "addl $16, %%esp\n\t"
        "ret\n\t"
        : : : "memory");
}

extern "C" void __cdecl shadowInstanceBumpEnsureFiltered(
    void* texture, const void* shader) {
    const bool inShadow = onMainThread()
        && InterlockedCompareExchange(&g_insideDirectional, 0, 0) > 0;
    const bool cold = inShadow && texture && g_resourceStateVerified
        && *(const unsigned*)((const BYTE*)texture
                             + kResourceLoadedStateOffset) == 0;
    const bool canFilter = g_shadowDeferActive && inShadow && shader
        && g_engineBase && g_shaderHasParameterVerified && g_shaderHasParameter
        && g_resourceStateVerified
        && *(const unsigned*)((const BYTE*)shader
                             + kResourceLoadedStateOffset) == 2;
    if (canFilter
        && !g_shaderHasParameter(
            const_cast<void*>(shader), nullptr,
            g_engineBase + kBumpTextureNameRva)) {
        if (g_shadowTracing) {
            tq::probe::engineCount(
                tq::probe::CounterEngineShadowBumpTexSkipped);
            if (cold)
                tq::probe::engineCount(
                    tq::probe::CounterEngineShadowBumpTexSkippedCold);
        }
        // The verified stock setter at the end of this block performs the
        // same Name lookup before touching the supplied texture value. With
        // no bumpTexture parameter it discards the empty vector result, so
        // omitting this EnsureAvailable changes no shader binding.
        return;
    }
    if (g_ensureAvailable) g_ensureAvailable(texture, nullptr);
}

// At the patched E8, ECX is the optional bump Resource and EBX is the active
// shader. Preserve the game's no-argument __thiscall shape while supplying
// both to the ordinary C helper.
void __attribute__((naked)) __fastcall hookShadowInstanceBumpEnsure(
    void*, void*) {
    __asm__ __volatile__(
        "pushl %%ebx\n\t"
        "pushl %%ecx\n\t"
        "call _shadowInstanceBumpEnsureFiltered\n\t"
        "addl $8, %%esp\n\t"
        "ret\n\t"
        : : : "memory");
}

extern "C" void __cdecl shadowMeshSetShaderParametersContext(
    void* mesh, void* instance, int pass, const void* shader,
    int materialIndex) {
    if (!g_graphicsMeshSetShaderParameters) return;
    // The adapter is patched globally, but its context is needed only by the
    // main-thread directional call. Do not let a concurrent colour/worker
    // invocation overwrite the bracket's live instance pointer.
    if (!onMainThread()
        || InterlockedCompareExchange(&g_insideDirectional, 0, 0) <= 0) {
        g_graphicsMeshSetShaderParameters(
            mesh, nullptr, shader, materialIndex);
        return;
    }
    const ShadowMeshParameterContext prior = g_shadowMeshParameterContext;
    ShadowMeshParameterContext current = {};
    current.instance = instance;
    current.pass = pass;
    current.match = ShadowContextInstanceMissing;
    if (g_shadowTracing)
        findShadowRecordContext(instance, pass, &current);
    g_shadowMeshParameterContext = current;
    g_graphicsMeshSetShaderParameters(
        mesh, nullptr, shader, materialIndex);
    g_shadowMeshParameterContext = prior;
}

// At this patched E8's entry ECX is GraphicsMesh*, ESI is the owning
// GraphicsMeshInstance, and the original two stack arguments are shader and
// material index. EBP is a MeshRenderInfo*, not the pass. Each push of [esp+8]
// is deliberate: after the first push, shader moves from old +4 to new +8;
// after both, the original arg3/pass is at ESP+0xbc.
void __attribute__((naked)) __fastcall hookShadowMeshSetShaderParameters(
    void*, void*) {
    __asm__ __volatile__(
        "pushl 8(%%esp)\n\t"                 // material index
        "pushl 8(%%esp)\n\t"                 // shader
        "pushl 0xbc(%%esp)\n\t"              // original arg3, pass
        "pushl %%esi\n\t"                    // instance
        "pushl %%ecx\n\t"                    // mesh
        "call _shadowMeshSetShaderParametersContext\n\t"
        "addl $20, %%esp\n\t"
        "ret $8\n\t"
        : : : "memory");
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

bool shouldDeferShadowAlpha(unsigned style, unsigned state) {
    // GetShadowRenderStyle's verified return classes are opaque 0-2 and
    // alpha-tested 3-5. Only the two cold Resource states are omitted.
    return style >= 3 && style <= 5 && state <= 1;
}

void countDeferredShadowAlpha(unsigned state, bool enqueued, bool failed) {
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

int __fastcall hookBuildShadowRecord(
    void* renderer, void* edx, void* output, void* renderableEntry, int pass) {
    if (!g_buildShadowRecord) return 0;

    void* contextRenderable = nullptr;
    unsigned contextStyle = 0;
    bool contextStyleKnown = false;
    const void* contextBaseTexture = nullptr;
    bool contextBaseKnown = false;

    // Decide before the original helper writes the temporary record. This
    // avoids constructing a record the caller will not append, and therefore
    // avoids depending on undocumented ownership inside that record.
    if (g_shadowDeferActive && onMainThread()
        && InterlockedCompareExchange(&g_insideDirectional, 0, 0) > 0
        && g_resourceStateVerified && renderableEntry && g_meshShadowStyle
        && g_meshGetTexture && g_resourceLoaderAccessor && g_shadowEnqueue) {
        void* const renderable = *(void**)renderableEntry;
        void** const vtable = renderable ? *(void***)renderable : nullptr;
        contextRenderable = renderable;
        // Slot 2 identifies the exact GraphicsMeshInstance implementation of
        // GetShadowRenderStyle. Other accepted renderables are recorded too,
        // but remain class_other: this gate never applies mesh layout or
        // behavior to an override it has not verified.
        if (vtable && vtable[2] == (void*)g_meshShadowStyle) {
            const unsigned style = (unsigned)g_meshShadowStyle(
                renderable, nullptr, pass);
            contextStyleKnown = true;
            const bool alpha = style >= 3 && style <= 5;
            const void* const texture = alpha
                ? g_meshGetTexture(renderable, nullptr, pass,
                    (const BYTE*)g_engineBase + kNameNoNameRva)
                : nullptr;
            contextStyle = style;
            contextBaseTexture = texture;
            contextBaseKnown = alpha && texture;
            if (texture) {
                const unsigned state = *(const unsigned*)(
                    (const BYTE*)texture + kResourceLoadedStateOffset);
                if (shouldDeferShadowAlpha(style, state)) {
                    // Preserve the original helper's first eligibility
                    // decision before queueing anything. This call is made
                    // only for candidates we will omit, so every normal
                    // caster still invokes the getter exactly once inside the
                    // original helper.
                    ShadowEligibleFn const eligible =
                        (ShadowEligibleFn)vtable[9];
                    if (!eligible)
                        return g_buildShadowRecord(
                            renderer, edx, output, renderableEntry, pass);
                    if (!eligible(renderable, nullptr)) return 0;
                    bool enqueued = false;
                    bool failed = false;
                    if (state == 0
                        && !*(void* const*)((const BYTE*)texture
                                           + kResourceInQueueOffset)) {
                        void* const loader = g_resourceLoaderAccessor(
                            const_cast<void*>(texture), nullptr);
                        if (loader) {
                            // This is the engine's own normal preload tuple:
                            // priority 1, notify=true, immediate=false.
                            g_shadowEnqueue(loader, nullptr, texture, 1, 1, 0);
                            const unsigned after = *(const unsigned*)(
                                (const BYTE*)texture
                                + kResourceLoadedStateOffset);
                            enqueued = after != 0
                                || *(void* const*)((const BYTE*)texture
                                                  + kResourceInQueueOffset);
                        }
                        failed = !enqueued;
                    }
                    countDeferredShadowAlpha(state, enqueued, failed);
                    return 0;
                }
            }
        }
    }
    const int result = g_buildShadowRecord(
        renderer, edx, output, renderableEntry, pass);
    if (result && g_shadowTracing && g_shadowMeshParameterHooked
        && contextRenderable)
        rememberShadowRecordContext(
            contextRenderable, pass, contextStyle, contextStyleKnown,
            contextBaseKnown, contextBaseTexture);
    return result;
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
    const bool classify = inShadow && g_resourceStateVerified && resource;
    const unsigned state = classify
        ? *(const unsigned*)((const BYTE*)resource + kResourceLoadedStateOffset)
        : 0;
    const bool inQueue = classify
        && *(void* const*)((const BYTE*)resource + kResourceInQueueOffset)
            != nullptr;
    const char* const resourceName = inShadow
        && g_resourceFileNameVerified && g_resourceFileName && resource
        ? g_resourceFileName(resource, nullptr) : nullptr;
    const ShadowResourceType resourceType = resourceName
        ? shadowResourceType(resourceName) : ShadowResourceOther;
    const ShadowTextureCaller textureCaller =
        resourceType == ShadowResourceTexture
        ? shadowTextureCallerFromStack(&resource)
        : ShadowTextureUnresolved;
    const int64_t started = tq::probe::now();
    g_loadResource(self, edx, resource);
    const uint32_t elapsed = tq::probe::microsecondsSince(started);
    tq::probe::engineCount(tq::probe::CounterEngineResLoad);
    tq::probe::engineCount(tq::probe::CounterEngineResLoadUs, elapsed);
    if (main) {
        tq::probe::engineCount(tq::probe::CounterEngineResLoadMain);
        tq::probe::engineCount(tq::probe::CounterEngineResLoadMainUs, elapsed);
        if (inShadow) {
            tq::probe::engineCount(tq::probe::CounterEngineShadowResLoad);
            tq::probe::engineCount(tq::probe::CounterEngineShadowResLoadUs,
                                   elapsed);
            if (classify) countShadowResourceState(state, inQueue, elapsed);
            if (g_resourceFileNameVerified) {
                countShadowResourceType(resourceType, elapsed);
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

bool reusePreviousShadow(bool regionChanged, void* surface, void* matrix) {
    if (!g_shadowTransitionReuse || !regionChanged || g_reusedLastShadow
        || !g_cachedShadowValid || surface != g_cachedShadowSurface || !matrix)
        return false;
    memcpy(matrix, g_cachedShadowMatrix, sizeof(g_cachedShadowMatrix));
    g_reusedLastShadow = true;
    if (g_shadowTracing)
        tq::probe::engineCount(tq::probe::CounterEngineShadowReuse);
    return true;
}

void rememberShadow(void* surface, const void* matrix, int result) {
    g_reusedLastShadow = false;
    if (!result || !surface || !matrix) return;
    memcpy(g_cachedShadowMatrix, matrix, sizeof(g_cachedShadowMatrix));
    g_cachedShadowSurface = surface;
    g_cachedShadowResult = result;
    g_cachedShadowValid = true;
}

int __fastcall hookRenderDirectional(
    void* self, void* edx, void* canvas, const void* camera,
    const void* frustum, int algorithm, void* surface, void* matrix) {
    if (!g_renderDirectional) return 0;

    void* const region = *(void**)((BYTE*)self + kShadowRegionOffset);
    const bool regionChanged =
        region && g_lastShadowRegion && region != g_lastShadowRegion;
    if (g_shadowTracing && regionChanged)
        tq::probe::engineCount(tq::probe::CounterEngineShadowRegionChange);
    if (region) g_lastShadowRegion = region;

    if (reusePreviousShadow(regionChanged, surface, matrix))
        return g_cachedShadowResult;

    const int64_t started = g_shadowTracing ? tq::probe::now() : 0;
    const bool bracketDirectional = g_shadowTracing || g_shadowDeferActive;
    if (bracketDirectional) {
        if (g_shadowTracing) {
            countShadowMeshContextPatchStatus();
            flushPendingShadowMaterialTexture(false, false);
            resetShadowRecordContexts();
        }
        InterlockedIncrement(&g_insideDirectional);
    }
    const int result = g_renderDirectional(self, edx, canvas, camera, frustum,
                                            algorithm, surface, matrix);
    if (bracketDirectional) {
        if (g_shadowTracing)
            flushPendingShadowMaterialTexture(false, false);
        InterlockedDecrement(&g_insideDirectional);
    }
    if (g_shadowTracing) {
        tq::probe::engineCount(tq::probe::CounterEngineShadowRender);
        tq::probe::engineCount(tq::probe::CounterEngineShadowRenderUs,
                               tq::probe::microsecondsSince(started));
    }
    rememberShadow(surface, matrix, result);
    return result;
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

// Walks the block routine's own address arithmetic -- the derivation spelled
// out above kArchiveBlockBytes -- to recover the five fields the cache keys on
// and the buffer the inflate would have written into.
//
// Every dereference is guarded, and the block size is checked against the
// archive rather than assumed, so a pointer that is not an Archive fails to
// describe rather than faulting. Refusing is free: the caller falls through to
// the engine's own read and inflate, which is what happens today.
bool describeBlock(void* self, unsigned entry, unsigned block,
                   void* blockBuffer, tq::arccache::Key& key, BYTE** dest) {
    const BYTE* archive = (const BYTE*)self;
    if (!tq::detour::readable(archive, kArchiveBlockSizeOffset + 4)) return false;
    if (*(const uint32_t*)(archive + kArchiveBlockSizeOffset)
        != tq::arccache::kSlotBytes)
        return false;

    // 0x400000 entries is sixty times the 67,873 the install actually has, and
    // it is what keeps entry * 0x44 inside 32 bits.
    if (entry >= 0x400000u || block >= 0x400000u) return false;

    const BYTE* entryTable =
        *(const BYTE* const*)(archive + kArchiveEntryTableOffset);
    const BYTE* record = entryTable + (SIZE_T)entry * kArchiveEntryStride;
    if (!tq::detour::readable(record, kArchiveEntryStride)) return false;

    const BYTE* descriptors =
        *(const BYTE* const*)(record + kArchiveEntryDescriptorsOffset);
    const BYTE* descriptor =
        descriptors + (SIZE_T)block * kArchiveDescriptorStride;
    if (!tq::detour::readable(descriptor, kArchiveDescriptorStride))
        return false;

    if (!tq::detour::readable(blockBuffer, 12)) return false;
    BYTE* destination = *(BYTE**)((BYTE*)blockBuffer + 8);

    key.archive = self;
    key.handle = *(void* const*)(archive + kArchiveHandleOffset);
    key.offset = ((const uint32_t*)descriptor)[0];
    key.compressed = ((const uint32_t*)descriptor)[1];
    key.uncompressed = ((const uint32_t*)descriptor)[2];
    if (!key.uncompressed || key.uncompressed > tq::arccache::kSlotBytes)
        return false;
    if (!tq::detour::readable(destination, key.uncompressed)) return false;
    *dest = destination;
    return true;
}

int __fastcall hookArchiveBlock(void* self, void* edx, unsigned entry,
                                unsigned block, void* blockBuffer) {
    if (!g_archiveBlock) return 0;

    // engine_arc_blocks counts what the engine asked for, hit or miss, so the
    // 1.8x and 2.3x amplification figures runs 10 and 17 measured stay the
    // same measurement in a cached run. engine_arc_inflate_us below then
    // covers only the blocks that were actually read and inflated.
    tq::probe::engineCount(tq::probe::CounterEngineArcBlocks);

    tq::arccache::Key key = {};
    BYTE* dest = nullptr;
    bool keyed = false;
    if (tq::arccache::running()) {
        keyed = describeBlock(self, entry, block, blockBuffer, key, &dest);
        // A refusal is safe -- the engine's own read and inflate run, exactly
        // as they do at archive_cache_mb=0 -- but it is not expected, so it
        // gets a column of its own rather than being silent.
        if (!keyed) tq::probe::engineCount(tq::probe::CounterArcCacheSkip);
    }
    if (keyed) {
        const int64_t looked = tq::probe::now();
        if (tq::arccache::lookup(key, dest)) {
            // Everything the original does that anyone downstream can observe:
            // FUN_1011d240's one-slot cache is told which block its scratch
            // buffer now holds, and AL comes back 1. What is skipped is the
            // seek, the read, the archive's own critical section and the
            // inflate -- see kArchiveBlockTailBytes.
            *(unsigned*)blockBuffer = block;
            tq::probe::engineCount(tq::probe::CounterArcCacheHitUs,
                                   tq::probe::microsecondsSince(looked));
            tq::arccache::report();
            return 1;
        }
    }

    const int64_t started = tq::probe::now();
    const int result = g_archiveBlock(self, edx, entry, block, blockBuffer);
    tq::probe::engineCount(tq::probe::CounterEngineArcInflateUs,
                           tq::probe::microsecondsSince(started));
    // The routine returns its bool in AL and leaves the rest of EAX holding
    // the inflated length, so this reads the byte the caller would.
    if (keyed && (result & 0xff)) tq::arccache::store(key, dest);
    if (tq::arccache::running()) tq::arccache::report();
    return result;
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
    g_engineUpdate(self, edx, frustum, flag);
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
volatile LONG g_messageSlow[kMessageKinds];   // peeks over 5 ms returning it

typedef UINT_PTR (WINAPI* SetTimerFn)(HWND, UINT_PTR, UINT, TIMERPROC);
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
    g_engineRender(self, edx);
    tq::probe::engineCount(tq::probe::CounterEngineRender);
    tq::probe::engineCount(tq::probe::CounterEngineRenderUs,
                           tq::probe::microsecondsSince(started));
}

typedef BOOL (WINAPI* GetMessageFn)(LPMSG, HWND, UINT, UINT);
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

// Both __thiscall, both reached through TQ.exe's import table, both once a
// frame. PresentSurface takes no arguments; the collision fixup takes one
// reference, so it pops four.
typedef void (__fastcall* PresentSurfaceFn)(void* self, void* edx);
typedef void (__fastcall* CollisionsFn)(void* self, void* edx, const void* camera);
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

// The remaining six, all __thiscall except the platform pump, which is a C
// entry point taking no arguments -- at zero arguments __stdcall and __cdecl
// emit the same epilogue, so the declaration cannot get the cleanup wrong.
// The call site confirms it: no pushes before, no stack adjustment after.
typedef void (__stdcall* PlatformProcessFn)(void);
typedef void (__fastcall* ThisVoidFn)(void* self, void* edx);
typedef void (__fastcall* SoundUpdateFn)(void* self, void* edx,
                                         const void* frustum);
typedef int (__fastcall* PumpFn)(void* self, void* edx);
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

// The pump, split. Both are USER32 imports of Engine.dll -- not of the
// executable -- because EWindow::ProcessMessages is Engine.dll's code.
typedef BOOL (WINAPI* PeekMessageFn)(LPMSG, HWND, UINT, UINT, UINT);
typedef LRESULT (WINAPI* DispatchMessageFn)(const MSG*);
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
        && (message->lParam & (LPARAM(1) << 30)) == 0)
        tq::probe::markStutter();
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

Signature signature(const BYTE* bytes, SIZE_T length,
                    const Relocation* relocations = nullptr,
                    unsigned relocationCount = 0) {
    Signature result = {bytes, length, relocations, relocationCount};
    return result;
}

// Resolves by decorated name and asserts the RVA. Either half alone would be
// weaker: the name survives a rebase but not a renamed build, the RVA survives
// a renamed build but not a rebase, and requiring both is what makes "this is
// the audited Engine.dll" a statement rather than a hope.
void* resolve(HMODULE engine, const char* name, DWORD rva) {
    void* address = (void*)GetProcAddress(engine, name);
    if (!address) {
        tq::hdr::log("Engine trace: %s is not exported\r\n", name);
        return nullptr;
    }
    if (address != (void*)((BYTE*)engine + rva)) {
        tq::hdr::log("Engine trace: %s resolved to %p, expected %p\r\n", name,
                     address, (void*)((BYTE*)engine + rva));
        return nullptr;
    }
    return address;
}

void note(const char* what, bool ok) {
    if (ok) ++g_installedHooks;
    tq::hdr::log("Engine trace: %s %s\r\n", what, ok ? "installed" : "skipped");
}

bool verifyResourceStateLayout(HMODULE engine) {
    void* const state = resolve(engine, kResourceLoadedStateName,
                                kResourceLoadedStateRva);
    void* const queue = resolve(engine, kResourceInQueueName,
                                kResourceInQueueRva);
    const bool ok = state && queue
        && tq::detour::matches(
               engine, state,
               signature(kResourceLoadedStateBytes,
                         sizeof(kResourceLoadedStateBytes)))
        && tq::detour::matches(
               engine, queue,
               signature(kResourceInQueueBytes,
                         sizeof(kResourceInQueueBytes)));
    tq::hdr::log("Engine trace: Resource loaded-state/queue layout %s\r\n",
                 ok ? "verified" : "unavailable");
    void* const fileName = resolve(engine, kResourceFileNameName,
                                   kResourceFileNameRva);
    g_resourceFileNameVerified = fileName
        && tq::detour::matches(
               engine, fileName,
               signature(kResourceFileNameBytes,
                         sizeof(kResourceFileNameBytes)));
    g_resourceFileName = g_resourceFileNameVerified
        ? (ResourceFileNameFn)fileName : nullptr;
    tq::hdr::log("Engine trace: Resource filename accessor %s\r\n",
                 g_resourceFileNameVerified ? "verified" : "unavailable");
    return ok;
}

// Resolve and verify every function the cold-alpha fix calls before any entry
// detour can replace their first bytes. The enqueue export is also used by the
// load trace, which installs earlier than the shadow call-site patch.
bool prepareShadowAlphaDefer(HMODULE engine) {
    g_resourceStateVerified = verifyResourceStateLayout(engine);
    void* const style = resolve(engine, kMeshShadowStyleName,
                                kMeshShadowStyleRva);
    void* const texture = resolve(engine, kMeshGetTextureName,
                                  kMeshGetTextureRva);
    void* const loader = resolve(engine, kResourceLoaderAccessorName,
                                 kResourceLoaderAccessorRva);
    void* const enqueue = resolve(engine, kEnqueueName, kEnqueueRva);
    void* const preload = resolve(engine, kPreloadResourceName,
                                  kPreloadResourceRva);
    void* const ensure = resolve(engine, kEnsureAvailableName,
                                 kEnsureAvailableRva);
    void* const materialOwner = resolve(
        engine, kGraphicsMeshSetShaderParametersName,
        kGraphicsMeshSetShaderParametersRva);
    void* const instanceMaterialOwner = resolve(
        engine, kGraphicsMeshInstanceSetShaderParametersName,
        kGraphicsMeshInstanceSetShaderParametersRva);
    void* const materialTexture = resolve(
        engine, kGraphicsTextureGetTextureName,
        kGraphicsTextureGetTextureRva);
    void* const hasParameter = resolve(
        engine, kShaderHasParameterName, kShaderHasParameterRva);
    void* const helper = (BYTE*)engine + kBuildShadowRecordRva;
    const bool ok = g_resourceStateVerified && style && texture && loader
        && enqueue && preload && ensure && materialOwner
        && instanceMaterialOwner && materialTexture && hasParameter
        && tq::detour::matches(
               engine, style,
               signature(kMeshShadowStyleBytes,
                         sizeof(kMeshShadowStyleBytes),
                         kMeshShadowStyleRelocs, 1))
        && tq::detour::matches(
               engine, (BYTE*)engine + kMeshShadowStyleAlphaRva,
               signature(kMeshShadowStyleAlphaBytes,
                         sizeof(kMeshShadowStyleAlphaBytes)))
        && tq::detour::matches(
               engine, (BYTE*)engine + kMeshShadowStyleSkinnedRva,
               signature(kMeshShadowStyleSkinnedBytes,
                         sizeof(kMeshShadowStyleSkinnedBytes)))
        && tq::detour::matches(
               engine, (BYTE*)engine + kMeshShadowStyleFoliageRva,
               signature(kMeshShadowStyleFoliageBytes,
                         sizeof(kMeshShadowStyleFoliageBytes)))
        && tq::detour::matches(
               engine, (BYTE*)engine + kMeshShadowStyleStaticRva,
               signature(kMeshShadowStyleStaticBytes,
                         sizeof(kMeshShadowStyleStaticBytes)))
        && tq::detour::matches(
               engine, texture,
               signature(kMeshGetTextureBytes, sizeof(kMeshGetTextureBytes),
                         kMeshGetTextureRelocs, 2))
        && tq::detour::matches(
               engine, (BYTE*)engine + kMeshGetTextureMeshRva,
               signature(kMeshGetTextureMeshBytes,
                         sizeof(kMeshGetTextureMeshBytes)))
        && tq::detour::matches(
               engine, (BYTE*)engine + kMeshGetTextureReturnRva,
               signature(kMeshGetTextureReturnBytes,
                         sizeof(kMeshGetTextureReturnBytes)))
        && tq::detour::matches(
               engine, loader,
               signature(kResourceLoaderAccessorBytes,
                         sizeof(kResourceLoaderAccessorBytes)))
        && tq::detour::matches(
               engine, enqueue,
               signature(kEnqueueBytes, sizeof(kEnqueueBytes),
                         kEnqueueRelocs, 1))
        && tq::detour::matches(
               engine, (BYTE*)engine + kPreloadEnqueueWindowRva,
               signature(kPreloadEnqueueWindowBytes,
                         sizeof(kPreloadEnqueueWindowBytes)))
        && tq::detour::matches(
               engine, helper,
               signature(kBuildShadowRecordBytes,
                         sizeof(kBuildShadowRecordBytes)))
        && tq::detour::matches(
               engine, (BYTE*)engine + kShadowMaterialTextureWindowRva,
               signature(kShadowMaterialTextureWindowBytes,
                         sizeof(kShadowMaterialTextureWindowBytes)))
        && tq::detour::matches(
               engine, (BYTE*)engine + kShadowMeshParameterFrameRva,
               signature(kShadowMeshParameterFrameBytes,
                         sizeof(kShadowMeshParameterFrameBytes),
                         kShadowMeshParameterFrameRelocs, 1))
        && tq::detour::matches(
               engine, (BYTE*)engine + kShadowMeshParameterEntryRva,
               signature(kShadowMeshParameterEntryBytes,
                         sizeof(kShadowMeshParameterEntryBytes)))
        && tq::detour::matches(
               engine, (BYTE*)engine + kShadowMeshParameterContextRva,
               signature(kShadowMeshParameterContextBytes,
                         sizeof(kShadowMeshParameterContextBytes)))
        && tq::detour::matches(
               engine, (BYTE*)engine + kShadowMeshParameterCallRva,
               signature(kShadowMeshParameterCallBytes,
                         sizeof(kShadowMeshParameterCallBytes)))
        && tq::detour::matches(
               engine, (BYTE*)engine + kShadowInstanceBumpEnsureWindowRva,
               signature(kShadowInstanceBumpEnsureWindowBytes,
                         sizeof(kShadowInstanceBumpEnsureWindowBytes)))
        && tq::detour::matches(
               engine, (BYTE*)engine + kShadowInstanceBumpSetterWindowRva,
               signature(kShadowInstanceBumpSetterWindowBytes,
                         sizeof(kShadowInstanceBumpSetterWindowBytes),
                         kShadowInstanceBumpSetterWindowRelocs, 1))
        && tq::detour::matches(
               engine, (BYTE*)engine + kBumpTextureNameInitWindowRva,
               signature(kBumpTextureNameInitWindowBytes,
                         sizeof(kBumpTextureNameInitWindowBytes),
                         kBumpTextureNameInitWindowRelocs, 3))
        && tq::detour::matches(
               engine, (BYTE*)engine + kSetTextureParameterMissingWindowRva,
               signature(kSetTextureParameterMissingWindowBytes,
                         sizeof(kSetTextureParameterMissingWindowBytes)))
        && tq::detour::matches(
               engine, (BYTE*)engine + kSetTextureParameterMissingReturnRva,
               signature(kSetTextureParameterMissingReturnBytes,
                         sizeof(kSetTextureParameterMissingReturnBytes)))
        && tq::detour::matches(
               engine, (BYTE*)engine + kShadowInstanceBaseEnsureWindowRva,
               signature(kShadowInstanceBaseEnsureWindowBytes,
                         sizeof(kShadowInstanceBaseEnsureWindowBytes)))
        && tq::detour::matches(
               engine, (BYTE*)engine + kShadowInstanceBaseSetterWindowRva,
               signature(kShadowInstanceBaseSetterWindowBytes,
                         sizeof(kShadowInstanceBaseSetterWindowBytes),
                         kShadowInstanceBaseSetterWindowRelocs, 1))
        && tq::detour::matches(
               engine, (BYTE*)engine + kBaseTextureNameInitWindowRva,
               signature(kBaseTextureNameInitWindowBytes,
                         sizeof(kBaseTextureNameInitWindowBytes),
                         kBaseTextureNameInitWindowRelocs, 3))
        && tq::detour::matches(
               engine, hasParameter,
               signature(kShaderHasParameterBytes,
                         sizeof(kShaderHasParameterBytes)));
    g_meshShadowStyle = ok ? (MeshShadowStyleFn)style : nullptr;
    g_meshGetTexture = ok ? (MeshGetTextureFn)texture : nullptr;
    g_resourceLoaderAccessor = ok ? (ResourceLoaderAccessorFn)loader : nullptr;
    g_shadowEnqueue = ok ? (EnqueueFn)enqueue : nullptr;
    g_ensureAvailable = ok ? (EnsureAvailableFn)ensure : nullptr;
    g_buildShadowRecord = ok ? (BuildShadowRecordFn)helper : nullptr;
    g_graphicsTextureGetTexture = ok
        ? (GraphicsTextureGetTextureFn)materialTexture : nullptr;
    g_graphicsMeshSetShaderParameters = ok
        ? (GraphicsMeshSetShaderParametersFn)materialOwner : nullptr;
    g_shaderHasParameterVerified = ok;
    g_shaderHasParameter = ok ? (ShaderHasParameterFn)hasParameter : nullptr;
    tq::hdr::log("Engine trace: cold alpha-shadow dependencies %s\r\n",
                 ok ? "verified" : "unavailable");
    return ok;
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

// The three windows a cache hit's correctness rests on, over and above the two
// that establish the hook's identity: the seek and the read, which name the
// handle and the descriptor's three fields in the operands of the syscalls
// that consume them, and the epilogue, which is the contract a hit reproduces.
// The block size has exactly one writer in the image, and that is checked too.
//
// These are required only when the cache is on. The instrument on its own
// needs the function to *be* the block routine; the cache additionally needs
// every offset it reads to be the offset the routine reads.
bool archiveStructureVerified(HMODULE engine) {
    struct Window {
        const char* what;
        DWORD rva;
        const BYTE* bytes;
        SIZE_T size;
        const Relocation* relocations;
        unsigned relocationCount;
    };
    const Window windows[] = {
        {"seek", kArchiveSeekWindowRva, kArchiveSeekWindowBytes,
         sizeof(kArchiveSeekWindowBytes), kArchiveSeekWindowRelocs, 1},
        {"read", kArchiveReadWindowRva, kArchiveReadWindowBytes,
         sizeof(kArchiveReadWindowBytes), kArchiveReadWindowRelocs, 1},
        {"epilogue", kArchiveBlockTailRva, kArchiveBlockTailBytes,
         sizeof(kArchiveBlockTailBytes), nullptr, 0},
        {"block size", kArchiveBlockSizeRva, kArchiveBlockSizeBytes,
         sizeof(kArchiveBlockSizeBytes), nullptr, 0},
    };
    for (unsigned i = 0; i < sizeof(windows) / sizeof(*windows); ++i) {
        const Window& w = windows[i];
        if (tq::detour::matches(engine, (BYTE*)engine + w.rva,
                                signature(w.bytes, w.size, w.relocations,
                                          w.relocationCount)))
            continue;
        tq::hdr::log("Archive cache: the %s window at %p does not match --"
                     " refusing to cache\r\n", w.what,
                     (void*)((BYTE*)engine + w.rva));
        return false;
    }
    return true;
}

bool installArchive(HMODULE engine, bool trace, bool cache) {
    if (trace) {
        void* target = resolve(engine, kReadFromFileName, kReadFromFileRva);
        if (target)
            tq::detour::attach(
                g_readFromFileDetour, engine, target,
                signature(kReadFromFileBytes, sizeof(kReadFromFileBytes)), 6,
                (const void*)&hookReadFromFile, (void**)&g_readFromFile);
        note("Archive::ReadFromFile", g_readFromFile != nullptr);
    }

    // The block routine is not exported, so identity rests entirely on the
    // bytes: its own forty-seven byte prologue and address arithmetic, and the
    // call to zlib's uncompress that is the only thing distinguishing it from
    // three functions with the same opening.
    BYTE* block = (BYTE*)engine + kArchiveBlockRva;
    const bool anchored = tq::detour::matches(
        engine, (BYTE*)engine + kArchiveInflateWindowRva,
        signature(kArchiveInflateWindowBytes,
                  sizeof(kArchiveInflateWindowBytes)));
    if (anchored)
        tq::detour::attach(
            g_archiveBlockDetour, engine, block,
            signature(kArchiveBlockBytes, sizeof(kArchiveBlockBytes)), 6,
            (const void*)&hookArchiveBlock, (void**)&g_archiveBlock);
    note("archive block inflate", g_archiveBlock != nullptr);

    // Nothing is committed until the site is proven. A refusal anywhere here
    // leaves the game byte-identical to archive_cache_mb=0, which is the
    // default, and says which check refused.
    if (cache) {
        if (!g_archiveBlock)
            tq::hdr::log("Archive cache: the block routine is not hooked --"
                         " nothing to cache in front of\r\n");
        else if (archiveStructureVerified(engine))
            tq::arccache::start();
    }
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

// The same assertion install() makes about Engine.dll, for whichever module
// is being patched: a build with a different SizeOfImage is a different build,
// and one log line beats nine failed signature matches.
bool auditedImage(HMODULE module, DWORD expectedSize, const char* what) {
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)module;
    if (!module || !tq::detour::readable(dos, sizeof(*dos))
        || dos->e_lfanew <= 0)
        return false;
    const IMAGE_NT_HEADERS* nt =
        (const IMAGE_NT_HEADERS*)((const BYTE*)module + dos->e_lfanew);
    if (tq::detour::readable(nt, sizeof(*nt))
        && nt->Signature == IMAGE_NT_SIGNATURE
        && nt->OptionalHeader.SizeOfImage == expectedSize)
        return true;
    tq::hdr::log("Engine trace: %s is not the audited build, nothing"
                 " installed from it\r\n", what);
    return false;
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

// Brackets the renderer's single directional-shadow build and tags resource
// loads made synchronously inside it. The object field is verified separately
// because patchCall's window proves the caller and callee, not the layout of
// the temporary GraphicsShadowMapDx11 object passed in ECX.
bool installShadow(HMODULE engine, bool trace) {
    void* target = resolve(engine, kRenderDirectionalName,
                           kRenderDirectionalRva);
    const bool regionVerified = tq::detour::matches(
        engine, (BYTE*)engine + kShadowRegionConstructorRva,
        signature(kShadowRegionConstructorBytes,
                  sizeof(kShadowRegionConstructorBytes)));
    const bool outputArgumentVerified = tq::detour::matches(
        engine, (BYTE*)engine + kShadowOutputArgumentRva,
        signature(kShadowOutputArgumentBytes,
                  sizeof(kShadowOutputArgumentBytes)));
    const bool outputCopyVerified = tq::detour::matches(
        engine, (BYTE*)engine + kShadowOutputCopyRva,
        signature(kShadowOutputCopyBytes, sizeof(kShadowOutputCopyBytes)));
    if (!target || !regionVerified || !outputArgumentVerified
        || !outputCopyVerified) {
        if (!regionVerified)
            tq::hdr::log("Engine trace: GraphicsShadowMapDx11 region field"
                         " does not match -- leaving the call alone\r\n");
        if (!outputArgumentVerified || !outputCopyVerified)
            tq::hdr::log("Engine trace: GraphicsShadowMapDx11 matrix output"
                         " does not match -- leaving the call alone\r\n");
        note("GraphicsShadowMapDx11::RenderDirectional", false);
        return false;
    }

    g_renderDirectional = (RenderDirectionalFn)target;
    const bool ok = tq::detour::patchCall(
        g_shadowDirectionalPatch, engine,
        (BYTE*)engine + kShadowCallWindowRva,
        signature(kShadowCallWindowBytes, sizeof(kShadowCallWindowBytes)),
        kShadowCallOffset, target, (const void*)&hookRenderDirectional);
    if (ok) {
        g_shadowTracing = trace;
    } else {
        g_renderDirectional = nullptr;
    }
    note("GraphicsShadowMapDx11::RenderDirectional", ok);
    if (ok && g_shadowDeferColdAlpha) {
        const bool recordOk = g_buildShadowRecord
            && tq::detour::patchCall(
                g_shadowRecordPatch, engine,
                (BYTE*)engine + kShadowRecordCallWindowRva,
                signature(kShadowRecordCallWindowBytes,
                          sizeof(kShadowRecordCallWindowBytes)),
                kShadowRecordCallOffset, (const void*)g_buildShadowRecord,
                (const void*)&hookBuildShadowRecord);
        const bool contextOk = recordOk && g_graphicsMeshSetShaderParameters
            && tq::detour::patchCall(
                g_shadowMeshParameterPatch, engine,
                (BYTE*)engine + kShadowMeshParameterCallRva,
                signature(kShadowMeshParameterCallBytes,
                          sizeof(kShadowMeshParameterCallBytes)),
                kShadowMeshParameterCallOffset,
                (const void*)g_graphicsMeshSetShaderParameters,
                (const void*)&hookShadowMeshSetShaderParameters);
        const bool filterOk = contextOk && g_graphicsTextureGetTexture
            && tq::detour::patchCall(
                g_shadowMaterialTexturePatch, engine,
                (BYTE*)engine + kShadowMaterialTextureWindowRva,
                signature(kShadowMaterialTextureWindowBytes,
                          sizeof(kShadowMaterialTextureWindowBytes)),
                kShadowMaterialTextureCallOffset,
                (const void*)g_graphicsTextureGetTexture,
                (const void*)&hookShadowMaterialTexture);
        const bool bumpOk = filterOk && g_ensureAvailable
            && tq::detour::patchCall(
                g_shadowInstanceBumpEnsurePatch, engine,
                (BYTE*)engine + kShadowInstanceBumpEnsureWindowRva,
                signature(kShadowInstanceBumpEnsureWindowBytes,
                          sizeof(kShadowInstanceBumpEnsureWindowBytes)),
                kShadowInstanceBumpEnsureCallOffset,
                (const void*)g_ensureAvailable,
                (const void*)&hookShadowInstanceBumpEnsure);
        const bool deferOk = recordOk && contextOk && filterOk && bumpOk;
        if (!deferOk) {
            tq::detour::restoreCall(g_shadowInstanceBumpEnsurePatch);
            tq::detour::restoreCall(g_shadowMaterialTexturePatch);
            tq::detour::restoreCall(g_shadowMeshParameterPatch);
            tq::detour::restoreCall(g_shadowRecordPatch);
            g_buildShadowRecord = nullptr;
            g_meshShadowStyle = nullptr;
            g_meshGetTexture = nullptr;
            g_resourceLoaderAccessor = nullptr;
            g_shadowEnqueue = nullptr;
            g_graphicsTextureGetTexture = nullptr;
            g_graphicsMeshSetShaderParameters = nullptr;
            g_shaderHasParameter = nullptr;
            g_shaderHasParameterVerified = false;
        }
        const bool contextActive = deferOk && contextOk;
        g_shadowMeshParameterHooked = contextActive;
        g_shadowMeshContextPatchStatus = contextActive
            ? ShadowMeshContextPatchActive
            : contextOk ? ShadowMeshContextPatchReverted
                        : ShadowMeshContextPatchCallFailed;
        g_shadowMaterialTextureHooked = deferOk && filterOk;
        g_shadowDeferActive = deferOk;
        note("GraphicsMeshInstance base-override context", contextActive);
        note("opaque texture-free / cold alpha shadow mitigation", deferOk);
        note("unused directional bump-texture omission", deferOk && bumpOk);
    }
    if (ok && trace && g_resourceStateVerified) {
        void* const owner = resolve(engine, kShadowMeshPassCountName,
                                    kShadowMeshPassCountRva);
        void* const ensure = resolve(engine, kEnsureAvailableName,
                                     kEnsureAvailableRva);
        g_ensureAvailable = (EnsureAvailableFn)ensure;
        const bool meshOk = owner && ensure && tq::detour::patchCall(
            g_shadowMeshEnsurePatch, engine, owner,
            signature(kShadowMeshPassCountBytes,
                      sizeof(kShadowMeshPassCountBytes)),
            kShadowMeshEnsureCallOffset, ensure,
            (const void*)&hookShadowMeshEnsure);
        // The fix's bump wrapper also forwards through this exact export.
        // A diagnostic-boundary mismatch must disable only that diagnostic,
        // never remove the forwarding target under an installed fix.
        if (!meshOk && !g_shadowDeferActive) g_ensureAvailable = nullptr;
        note("GraphicsMeshInstance cold shadow-mesh boundary", meshOk);

        void* const materialOwner = resolve(
            engine, kGraphicsMeshSetShaderParametersName,
            kGraphicsMeshSetShaderParametersRva);
        void* const instanceMaterialOwner = resolve(
            engine, kGraphicsMeshInstanceSetShaderParametersName,
            kGraphicsMeshInstanceSetShaderParametersRva);
        void* const getTexture = resolve(
            engine, kGraphicsTextureGetTextureName,
            kGraphicsTextureGetTextureRva);
        void* const hasParameter = resolve(
            engine, kShaderHasParameterName, kShaderHasParameterRva);
        void* const nameHash = resolve(
            engine, kNameHashName, kNameHashRva);
        if (!g_shaderHasParameterVerified) {
            g_shaderHasParameterVerified = hasParameter
                && tq::detour::matches(
                    engine, hasParameter,
                    signature(kShaderHasParameterBytes,
                              sizeof(kShaderHasParameterBytes)));
            g_shaderHasParameter = g_shaderHasParameterVerified
                ? (ShaderHasParameterFn)hasParameter : nullptr;
        }
        g_nameHashLayoutVerified = nameHash
            && tq::detour::matches(
                engine, nameHash,
                signature(kNameHashBytes, sizeof(kNameHashBytes)));
        tq::hdr::log("Engine trace: material Name hash layout %s\r\n",
                     g_nameHashLayoutVerified ? "verified" : "unavailable");
        if (!g_graphicsTextureGetTexture)
            g_graphicsTextureGetTexture =
                (GraphicsTextureGetTextureFn)getTexture;
        g_shadowTextureCallerSitesVerified =
            verifyShadowTextureDirectCallers(engine, getTexture);
        note("direct GraphicsTexture caller attribution",
             g_shadowTextureCallerSitesVerified);

        const bool meshContextAlready = g_shadowMeshParameterHooked;
        const bool meshContextDependencies = meshContextAlready
            || (instanceMaterialOwner
                && materialOwner && g_meshShadowStyle && g_meshGetTexture);
        const bool meshContextFrame = meshContextAlready
            || (meshContextDependencies
            && tq::detour::matches(
                engine, (BYTE*)engine + kShadowMeshParameterFrameRva,
                signature(kShadowMeshParameterFrameBytes,
                          sizeof(kShadowMeshParameterFrameBytes),
                          kShadowMeshParameterFrameRelocs, 1)));
        const bool meshContextEntry = meshContextAlready
            || (meshContextFrame
            && tq::detour::matches(
                engine, (BYTE*)engine + kShadowMeshParameterEntryRva,
                signature(kShadowMeshParameterEntryBytes,
                          sizeof(kShadowMeshParameterEntryBytes))));
        const bool meshContextContext = meshContextAlready
            || (meshContextEntry
            && tq::detour::matches(
                engine, (BYTE*)engine + kShadowMeshParameterContextRva,
                signature(kShadowMeshParameterContextBytes,
                          sizeof(kShadowMeshParameterContextBytes))));
        const bool meshContextOk = meshContextAlready
            || (meshContextContext && tq::detour::patchCall(
                g_shadowMeshParameterPatch, engine,
                (BYTE*)engine + kShadowMeshParameterCallRva,
                signature(kShadowMeshParameterCallBytes,
                          sizeof(kShadowMeshParameterCallBytes)),
                kShadowMeshParameterCallOffset, materialOwner,
                (const void*)&hookShadowMeshSetShaderParameters));
        g_shadowMeshContextPatchStatus = !meshContextDependencies
            ? ShadowMeshContextPatchDependencyMissing
            : !meshContextFrame ? ShadowMeshContextPatchFrameMismatch
            : !meshContextEntry ? ShadowMeshContextPatchEntryMismatch
            : !meshContextContext ? ShadowMeshContextPatchContextMismatch
            : !meshContextOk ? ShadowMeshContextPatchCallFailed
            : ShadowMeshContextPatchActive;
        g_shadowMeshParameterHooked = meshContextOk;
        g_graphicsMeshSetShaderParameters = meshContextOk
            ? (GraphicsMeshSetShaderParametersFn)materialOwner : nullptr;
        if (meshContextAlready)
            tq::hdr::log("Engine trace: GraphicsMeshInstance shadow material"
                         " context installed by fix\r\n");
        else
            note("GraphicsMeshInstance shadow material context", meshContextOk);

        g_setTextureParameter = (SetTextureParameterFn)(
            (BYTE*)engine + kSetTextureParameterRva);
        const bool setterOk = materialOwner && getTexture
            && g_shaderHasParameterVerified
            && tq::detour::patchCall(
                g_shadowTextureParameterPatch, engine,
                (BYTE*)engine + kShadowTextureParameterWindowRva,
                signature(kShadowTextureParameterWindowBytes,
                          sizeof(kShadowTextureParameterWindowBytes)),
                kShadowTextureParameterCallOffset,
                (const void*)g_setTextureParameter,
                (const void*)&hookShadowTextureParameter);
        g_shadowTextureParameterHooked = setterOk;
        const bool getterOk = g_shadowMaterialTextureHooked
            || (setterOk && tq::detour::patchCall(
                g_shadowMaterialTexturePatch, engine,
                (BYTE*)engine + kShadowMaterialTextureWindowRva,
                signature(kShadowMaterialTextureWindowBytes,
                          sizeof(kShadowMaterialTextureWindowBytes)),
                kShadowMaterialTextureCallOffset, getTexture,
                (const void*)&hookShadowMaterialTexture));
        g_shadowMaterialTextureHooked = getterOk;
        const bool materialOk = setterOk && getterOk;
        if (!materialOk) {
            tq::detour::restoreCall(g_shadowTextureParameterPatch);
            g_shadowTextureParameterHooked = false;
            g_setTextureParameter = nullptr;
            if (!g_shadowDeferActive) {
                tq::detour::restoreCall(g_shadowMeshParameterPatch);
                g_shadowMeshParameterHooked = false;
                g_graphicsMeshSetShaderParameters = nullptr;
                if (meshContextOk)
                    g_shadowMeshContextPatchStatus =
                        ShadowMeshContextPatchReverted;
            }
            if (!g_shadowDeferActive) {
                tq::detour::restoreCall(g_shadowMaterialTexturePatch);
                g_shadowMaterialTextureHooked = false;
                g_graphicsTextureGetTexture = nullptr;
                g_shaderHasParameter = nullptr;
                g_shaderHasParameterVerified = false;
            }
        }
        note("cold shadow material-texture use", materialOk);
    }
    return ok;
}

// [performance] async_level_load. The three windows the thunk's correctness
// rests on, over and above the two call-site windows patchCall checks itself.
//
// Nothing here establishes identity -- BackgroundLoadLevel is exported and
// resolve() has already asserted its name and its RVA. What these check is
// behaviour, because the thunk is built around three specific things this
// function does: it returns without doing anything when the level is resident
// and the flag is false, it guards its own re-entry, and it raises the byte
// the renderer tests. A build where any of those changed needs a different
// thunk, not this one.
bool backgroundLoadVerified(HMODULE engine) {
    struct Window {
        const char* what;
        DWORD rva;
        const BYTE* bytes;
        SIZE_T size;
    };
    const Window windows[] = {
        {"entry", kBackgroundLoadLevelRva, kBackgroundEntryBytes,
         sizeof(kBackgroundEntryBytes)},
        {"loading flags", kBackgroundFlagsRva, kBackgroundFlagsBytes,
         sizeof(kBackgroundFlagsBytes)},
        {"epilogue", kBackgroundTailRva, kBackgroundTailBytes,
         sizeof(kBackgroundTailBytes)},
    };
    for (unsigned i = 0; i < sizeof(windows) / sizeof(*windows); ++i) {
        const Window& w = windows[i];
        if (tq::detour::matches(engine, (BYTE*)engine + w.rva,
                                signature(w.bytes, w.size)))
            continue;
        tq::hdr::log("Async level load: Region::BackgroundLoadLevel's %s window"
                     " at %p does not match -- leaving the load synchronous\r\n",
                     w.what, (void*)((BYTE*)engine + w.rva));
        return false;
    }
    return true;
}

// Retargets the two forced loads at the thunk. Nothing is written until both
// the asynchronous entry point and the original resolve and every window
// matches, and a refusal anywhere leaves the game byte-identical to
// async_level_load=0, which is the default.
//
// Ordering: this reads no import slot and patches no function entry, so it is
// independent of every group above it. It does depend on Region::LoadLevel
// still being the call sites' destination -- which is true whether or not the
// loads group detoured it, because a detour rewrites the function's entry and
// not the displacements that reach it.
bool installAsyncLoad(HMODULE engine) {
    void* background =
        resolve(engine, kBackgroundLoadLevelName, kBackgroundLoadLevelRva);
    void* original = resolve(engine, kLoadLevelName, kLoadLevelRva);
    if (!background || !original || !backgroundLoadVerified(engine)) {
        note("async level load", false);
        return false;
    }
    g_backgroundLoadLevel = (BackgroundLoadLevelFn)background;
    g_regionLoadLevel = (LoadLevelFn)original;

    unsigned installed = 0;
    for (unsigned i = 0; i < kForceLoadSiteCount; ++i) {
        const ForceLoadSite& site = kForceLoadSites[i];
        if (site.owner && !resolve(engine, site.owner, site.ownerRva)) continue;
        if (tq::detour::patchCall(
                g_forceLoadPatches[i], engine, (BYTE*)engine + site.windowRva,
                signature(site.bytes, site.size, site.relocations,
                          site.relocationCount),
                site.callOffset, original, site.replacement))
            ++installed;
    }
    tq::hdr::log("Async level load: %u/%u forced loads retargeted at"
                 " Region::BackgroundLoadLevel\r\n", installed,
                 kForceLoadSiteCount);
    if (installed) {
        ++g_installedHooks;
        return true;
    }
    // Half a patch is not a state this can be left in, and neither is a pair
    // of live function pointers nothing calls.
    g_backgroundLoadLevel = nullptr;
    g_regionLoadLevel = nullptr;
    return false;
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

}  // namespace

void readOptions(const wchar_t* iniPath) {
    // No INI is the shipping configuration, and the shipping configuration has
    // the probe off, so the default here only decides what a trace run gets.
    g_traceMask = iniPath && iniPath[0]
        ? (unsigned)GetPrivateProfileIntW(L"debug", L"engine_trace", 1, iniPath)
        : 1u;
    // A game-behaviour change, so it lives under [performance] and defaults
    // to leaving the game alone. Clamped: a period of zero would be a request
    // for the system minimum, and anything over a second would stop whatever
    // the timer drives rather than slow it.
    const int period = iniPath && iniPath[0]
        ? GetPrivateProfileIntW(L"performance", L"timer_period_ms", 0, iniPath)
        : 0;
    g_timerPeriodMs = period > 0 && period <= 1000 ? (unsigned)period : 0u;
    // Makes the two renderer-forced level loads asynchronous. The same kind
    // of key as timer_period_ms and archive_cache_mb -- a game-behaviour
    // change under [performance], defaulting to leaving the game alone -- but
    // like archive_cache_mb and unlike timer_period_ms it is a fix rather
    // than an experiment, so install() lets it in without the trace.
    g_asyncLevelLoad = iniPath && iniPath[0]
        && GetPrivateProfileIntW(L"performance", L"async_level_load", 0,
                                 iniPath) != 0;
    // Keeps WM_TIMER out of the game's unfiltered peek except once every this
    // many milliseconds. Like archive_cache_mb and async_level_load it is a
    // fix rather than an experiment, so install() lets it in without the
    // trace. Clamped to a second: beyond that the game's timer cadence is
    // being changed rather than its polling, which is a different experiment
    // and one §17 already refused.
    const int floorMs = iniPath && iniPath[0]
        ? GetPrivateProfileIntW(L"performance", L"pump_timer_min_ms", 0,
                                iniPath)
        : 0;
    g_pumpTimerMinMs = floorMs > 0 && floorMs <= 1000 ? (unsigned)floorMs : 0u;
    g_pumpLastFullTick = (LONG)GetTickCount();
    // Reuses the last complete directional depth-map/matrix pair for exactly
    // one call when the shadow context's region pointer changes. Unlike the
    // trace group, this is a game-behaviour change and must work with the
    // performance probe off.
    g_shadowTransitionReuse = iniPath && iniPath[0]
        && GetPrivateProfileIntW(L"performance", L"shadow_transition_reuse", 0,
                                 iniPath) != 0;
    // Omits only alpha-tested GraphicsMeshInstance shadow records while their
    // base texture is in loaded state 0 or 1, explicitly queueing state 0.
    // This is a behaviour fix, not an instrument, so it also works with the
    // performance probe off and defaults to leaving the engine untouched.
    g_shadowDeferColdAlpha = iniPath && iniPath[0]
        && GetPrivateProfileIntW(L"performance", L"shadow_defer_cold_alpha", 0,
                                 iniPath) != 0;
    // The block cache rides on this file's one hook into the archive path, so
    // it reads its option here -- but it is a fix rather than an instrument,
    // and install() lets it in without the trace.
    tq::arccache::readOptions(iniPath);
}

bool install(HMODULE engine) {
    if (!engine) return false;
    // The trace has two gates, and none of the independently requested paths
    // opens them:
    // the trace still needs the probe on and a non-zero mask, and stays
    // byte-identical to a build without this file otherwise. What
    // archive_cache_mb, async_level_load, shadow_transition_reuse and
    // shadow_defer_cold_alpha add
    // independent ways in
    // that install their own hooks and no instrumentation -- because they are
    // game-behaviour changes and have to work on a boot with the probe off.
    // wants() below refuses every trace group when g_tracing is false, so
    // none of them brings the rest of the instrument along. The marker also
    // installs only the existing PeekMessage import wrapper.
    const bool cache = tq::arccache::configured();
    const bool async = g_asyncLevelLoad;
    const bool pumpFilter = g_pumpTimerMinMs != 0;
    const bool shadowReuse = g_shadowTransitionReuse;
    const bool shadowDefer = g_shadowDeferColdAlpha;
    const bool marker = tq::probe::stutterMarkerEnabled();
    decideTracing();
    if (!g_tracing && !cache && !async && !pumpFilter && !shadowReuse
        && !shadowDefer
        && !marker)
        return false;
    if (InterlockedCompareExchange(&g_installed, 1, 0)) return false;

    if (!auditedImage(engine, kEngineImageSize, "Engine.dll")) {
        InterlockedExchange(&g_installed, 0);
        return false;
    }

    // The base every recorded caller is reported as an RVA against, and the
    // .text bounds the stack scan filters on. Cached here so neither costs a
    // VirtualQuery per event.
    g_engineBase = (const BYTE*)engine;
    g_chainModuleCount = 0;
    addChainModule(engine, kEngineImageSize, 'E', "Engine.dll");
    addChainModule(GetModuleHandleW(L"Game.dll"), kGameImageSize, 'G',
                   "Game.dll");
    addChainModule(GetModuleHandleW(nullptr), kExecutableImageSize, 'T',
                   "TQ.exe");

    const volatile DWORD* mainThread =
        (const volatile DWORD*)((BYTE*)engine + kMainThreadIdRva);
    g_mainThreadId = tq::detour::readable((const void*)mainThread,
                                          sizeof(DWORD)) ? mainThread : nullptr;

    g_installedHooks = 0;
    const bool shadowDeferReady = shadowDefer
        && prepareShadowAlphaDefer(engine);
    if (wants(kGroupLoads)) installLoads(engine);
    if (wants(kGroupArchive) || cache)
        installArchive(engine, wants(kGroupArchive), cache);
    if (wants(kGroupFence)) installFence(engine);
    if (wants(kGroupLock)) installRegionLock(engine);
    if (wants(kGroupSweeps)) installSweeps(engine);
    if (wants(kGroupWait)) installWait(engine);
    if (wants(kGroupFrame)) installFrame(engine);
    if (wants(kGroupGame)) installGame();
    if (wants(kGroupLoop)) installLoop();
    const bool tracePump = wants(kGroupPump);
    if (tracePump || pumpFilter || marker) installPump(engine, tracePump);
    if (wants(kGroupHeap)) installHeap(engine);
    if (wants(kGroupArcIo)) installArchiveIo(engine);
    if (wants(kGroupBlocking)) installBlocking(engine);
    const bool traceShadow = wants(kGroupShadow);
    if (traceShadow || shadowReuse || shadowDeferReady)
        installShadow(engine, traceShadow);
    if (async) installAsyncLoad(engine);

    tq::hdr::log("Engine trace: %s, mask=0x%x, cache %s, async load %s,"
                 " pump timer floor %u ms, shadow transition reuse %s,"
                 " cold alpha-shadow defer %s,"
                 " hooks=%u, main thread id at %p\r\n",
                 g_tracing ? "on" : "off", g_traceMask,
                 cache ? "requested" : "off", async ? "requested" : "off",
                 g_pumpTimerMinMs, shadowReuse ? "requested" : "off",
                 shadowDefer ? "requested" : "off",
                 g_installedHooks,
                 (const void*)g_mainThreadId);
    if (g_installedHooks) return true;
    InterlockedExchange(&g_installed, 0);
    return false;
}

void shutdown() {
    reportMessages();
    reportSlowLoads();
    // Safe to run before the block hook is unpatched: stop() clears the slab
    // pointer under its own lock and only releases the pages afterwards, and
    // both lookup and store re-check it inside that lock, so a call already in
    // flight finds an empty cache rather than freed memory. (Titan Quest never
    // reaches this at all, which is why the cache reports during the session.)
    tq::arccache::stop();
    // Reverse of the install order, and each restore checks the site still
    // holds what we wrote before it puts the original back. The forced loads
    // go back first because they went in last, and the two function pointers
    // are cleared only after the sites that reach them are restored.
    for (int i = (int)kForceLoadSiteCount - 1; i >= 0; --i)
        tq::detour::restoreCall(g_forceLoadPatches[i]);
    g_backgroundLoadLevel = nullptr;
    g_regionLoadLevel = nullptr;
    tq::detour::restoreCall(g_shadowMeshEnsurePatch);
    tq::detour::restoreCall(g_shadowInstanceBumpEnsurePatch);
    g_ensureAvailable = nullptr;
    tq::detour::restoreCall(g_shadowMaterialTexturePatch);
    tq::detour::restoreCall(g_shadowTextureParameterPatch);
    tq::detour::restoreCall(g_shadowMeshParameterPatch);
    g_graphicsTextureGetTexture = nullptr;
    g_graphicsMeshSetShaderParameters = nullptr;
    g_shadowMaterialTextureHooked = false;
    g_setTextureParameter = nullptr;
    g_shadowTextureParameterHooked = false;
    g_shadowMeshParameterHooked = false;
    g_shadowMeshContextPatchStatus =
        ShadowMeshContextPatchDependencyMissing;
    g_shadowTextureCallerSitesVerified = false;
    g_insideShadowMaterialTexture = false;
    g_shaderHasParameter = nullptr;
    g_shaderHasParameterVerified = false;
    g_nameHashLayoutVerified = false;
    g_shadowMaterialTexturePending = false;
    g_shadowMaterialTexturePendingUs = 0;
    g_shadowMaterialPendingNameHash = 0;
    g_shadowMaterialReports = 0;
    g_shadowTextureChainReports = 0;
    g_shadowMeshParameterContext = {};
    g_shadowMaterialPendingContext = {};
    g_shadowMaterialPendingTexture = nullptr;
    tq::detour::restoreCall(g_shadowRecordPatch);
    g_buildShadowRecord = nullptr;
    g_meshShadowStyle = nullptr;
    g_meshGetTexture = nullptr;
    g_resourceLoaderAccessor = nullptr;
    g_shadowEnqueue = nullptr;
    g_shadowDeferActive = false;
    tq::detour::restoreCall(g_shadowDirectionalPatch);
    g_renderDirectional = nullptr;
    g_lastShadowRegion = nullptr;
    g_cachedShadowSurface = nullptr;
    memset(g_cachedShadowMatrix, 0, sizeof(g_cachedShadowMatrix));
    g_cachedShadowResult = 0;
    g_cachedShadowValid = false;
    g_reusedLastShadow = false;
    g_shadowTracing = false;
    InterlockedExchange(&g_insideDirectional, 0);
    tq::detour::restoreCall(g_enginesleepPatch);
    g_engineSleep = nullptr;
    tq::detour::restoreCall(g_objWaitMultiplePatch);
    g_engineWaitMultiple = nullptr;
    tq::detour::restoreCall(g_objWaitPatch);
    g_engineWait = nullptr;
    tq::detour::restoreCall(g_csPatch);
    tq::detour::restoreCall(g_readFilePatch);
    g_readFile = nullptr;
    tq::detour::restoreCall(g_seekPatch);
    g_setFilePointerEx = nullptr;
    tq::detour::restoreCall(g_deleteArrayPatch);
    g_deleteArray = nullptr;
    tq::detour::restoreCall(g_newArrayPatch);
    g_newArray = nullptr;
    tq::detour::restoreCall(g_dispatchPatch);
    g_dispatchMessage = nullptr;
    tq::detour::restoreCall(g_peekPatch);
    g_peekMessage = nullptr;
    tq::detour::restoreCall(g_setTimerPatch);
    g_setTimer = nullptr;
    tq::detour::restoreCall(g_pumpPatch);
    g_pump = nullptr;
    tq::detour::restoreCall(g_questsPatch);
    g_quests = nullptr;
    tq::detour::restoreCall(g_soundPatch);
    g_sound = nullptr;
    tq::detour::restoreCall(g_jukeboxPatch);
    g_jukebox = nullptr;
    tq::detour::restoreCall(g_gfxOptionsPatch);
    g_gfxOptions = nullptr;
    tq::detour::restoreCall(g_platformPatch);
    g_platform = nullptr;
    tq::detour::restoreCall(g_collisionsPatch);
    g_collisions = nullptr;
    tq::detour::restoreCall(g_presentSurfacePatch);
    g_presentSurface = nullptr;
    tq::detour::restoreCall(g_loopWaitPatch);
    g_loopWait = nullptr;
    tq::detour::restoreCall(g_loopMessagePatch);
    g_loopGetMessage = nullptr;
    tq::detour::restoreCall(g_loopSleepPatch);
    g_loopSleep = nullptr;
    tq::detour::detach(g_gameUpdateDetour);
    g_gameUpdate = nullptr;
    tq::detour::detach(g_engineRenderDetour);
    g_engineRender = nullptr;
    tq::detour::detach(g_engineUpdateDetour);
    g_engineUpdate = nullptr;
    tq::detour::detach(g_waitForLoadingDetour);
    for (int i = (int)kSweepCount - 1; i >= 0; --i)
        tq::detour::restoreCall(g_sweepPatches[i]);
    g_sweep = nullptr;
    for (int i = (int)kLockSiteCount - 1; i >= 0; --i)
        tq::detour::restoreCall(g_lockPatches[i]);
    tq::detour::restoreCall(g_fencePatch);
    tq::detour::detach(g_archiveBlockDetour);
    g_archiveBlock = nullptr;
    tq::detour::detach(g_readFromFileDetour);
    g_readFromFile = nullptr;
    tq::detour::detach(g_enqueueDetour);
    g_enqueue = nullptr;
    tq::detour::detach(g_unloadLevelDetour);
    g_unloadLevel = nullptr;
    tq::detour::detach(g_loadResourceDetour);
    g_loadResource = nullptr;
    g_resourceStateVerified = false;
    g_resourceFileName = nullptr;
    g_resourceFileNameVerified = false;
    tq::detour::detach(g_guaranteedDetour);
    g_guaranteedGetLevel = nullptr;
    tq::detour::detach(g_loadLevelDetour);
    g_loadLevel = nullptr;
    g_mainThreadId = nullptr;
    g_engineBase = nullptr;
    g_chainModuleCount = 0;
    g_installedHooks = 0;
    g_tracing = false;
    g_pumpTracing = false;
    InterlockedExchange(&g_installed, 0);
}

#ifdef TQ_SELFTEST
unsigned pumpTimerFloorForTest() { return g_pumpTimerMinMs; }

unsigned installedForTest() { return g_installedHooks; }
void enterCriticalSectionForTest(LPCRITICAL_SECTION section) {
    hookEnterCriticalSection(section);
}
void setTraceMaskForTest(unsigned mask) { g_traceMask = mask; }
bool asyncLevelLoadForTest() { return g_asyncLevelLoad; }
bool shadowTransitionReuseForTest() { return g_shadowTransitionReuse; }
bool shadowDeferColdAlphaForTest() { return g_shadowDeferColdAlpha; }
bool shouldDeferShadowAlphaForTest(unsigned style, unsigned state) {
    return shouldDeferShadowAlpha(style, state);
}
void countDeferredShadowAlphaForTest(unsigned state, bool enqueued,
                                     bool failed) {
    const bool tracing = g_shadowTracing;
    g_shadowTracing = true;
    countDeferredShadowAlpha(state, enqueued, failed);
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
#endif

}  // namespace engineprobe
}  // namespace tq
