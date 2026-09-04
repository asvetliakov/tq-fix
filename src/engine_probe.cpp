#include "engine_probe.h"

#include "arc_cache.h"
#include "detour.h"
#include "hdr.h"
#include "probe.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace tq {
namespace engineprobe {
namespace detail {
volatile LONG gpuChunkDrawActive = 0;
volatile LONG secondaryAdmissionDrawSuppressDepth = 0;
}
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
// complete 24-byte function is verified. Trace-only boots retarget its E8;
// the behavior fix detours the entry while stealing only the first six
// complete, non-relative bytes. This is both the earliest exact cold-mesh
// boundary for a per-caster omission and the point option 2 would have to make
// resident earlier.
const DWORD kShadowMeshPassCountRva = 0x173440;
const char kShadowMeshPassCountName[] =
    "?GetNumShadowRenderPasses@GraphicsMeshInstance@GAME@@UBEHXZ";
const DWORD kEnsureAvailableRva = 0x2130f0;
const char kEnsureAvailableName[] =
    "?EnsureAvailable@Resource@GAME@@QBEXXZ";
const unsigned kGraphicsMeshResourceOffset = 4;
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

// Run 68 proved the remaining cold root meshes are forced earlier than the
// pass-count boundary above. GraphicsShadowMapDx11::RenderDirectional invokes
// Actor::AddToScene through its scene-gather virtual; that exact Actor method
// calls Actor::UpdateMeshInstance, which calls GraphicsMeshInstance::UpdatePose
// and synchronously ensures Actor+0x184 -> GraphicsMeshInstance+4. Retarget the
// existing E8 rather than detouring either shared-prologue function. The
// 23-byte caller window and independent 24-byte callee window prove the exact
// class, target, and Actor mesh-instance field before the wrapper reads it.
const DWORD kActorUpdateMeshInstanceRva = 0x112060;
const char kActorUpdateMeshInstanceName[] =
    "?UpdateMeshInstance@Actor@GAME@@QAEXXZ";
const unsigned kActorMeshInstanceOffset = 0x184;
const BYTE kActorUpdateMeshInstanceBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,        // shared prologue
    0x83, 0xec, 0x64,
    0x56,
    0x8b, 0xf1,
    0x8b, 0x86, 0x84, 0x01, 0x00, 0x00,        // [Actor+0x184]
    0xf3, 0x0f, 0x7e, 0x86, 0x74, 0x02
};
const DWORD kActorAddToSceneUpdateMeshWindowRva = 0x111fca;
const unsigned kActorAddToSceneUpdateMeshCallOffset = 11;
const BYTE kActorAddToSceneUpdateMeshWindowBytes[] = {
    0x80, 0xbe, 0x88, 0x01, 0x00, 0x00, 0x00,  // cmp byte [Actor+0x188],0
    0x74, 0x6e,
    0x8b, 0xce,                                // ecx = Actor
    0xe8, 0x86, 0x00, 0x00, 0x00,              // Actor::UpdateMeshInstance
    0x0f, 0xb6, 0x86, 0x94, 0x02, 0x00, 0x00
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

// --- TerrainType's own semantic preload and its two parameter binders.
// Run 62 resolved every cold terrain texture in the marked play burst to
// SetShaderParams or SetGrassShaderParams. PreLoad(true) walks precisely the
// base, bump and grass resources those functions consume. These entry hooks
// are diagnostic only: they preserve every argument and always call through.
const DWORD kTerrainPreloadRva = 0x23fe80;
const char kTerrainPreloadName[] = "?PreLoad@TerrainType@GAME@@QAEX_N@Z";
const BYTE kTerrainPreloadBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,
    0x6a, 0xff,
    0x68, 0, 0, 0, 0,
    0x64, 0xa1, 0x00, 0x00, 0x00, 0x00,
    0x50,
    0x83, 0xec, 0x28,
    0x53
};
const Relocation kTerrainPreloadRelocs[] = {{9, 0x2a4e08}};

const DWORD kTerrainLoadTexturesRva = 0x240160;
const char kTerrainLoadTexturesName[] =
    "?LoadTextures@TerrainType@GAME@@QAEXXZ";

const DWORD kTerrainSetShaderParamsRva = 0x23fb90;
const char kTerrainSetShaderParamsName[] =
    "?SetShaderParams@TerrainType@GAME@@QBEXPBVGraphicsShader2@2@H@Z";
const BYTE kTerrainSetShaderParamsBytes[] = {
    0xa1, 0, 0, 0, 0,
    0x83, 0xec, 0x08,
    0x53,
    0x8b, 0x5c, 0x24, 0x10,
    0x56, 0x57,
    0x8b, 0xf1,
    0xa8, 0x01,
    0x75, 0x49
};
const Relocation kTerrainSetShaderParamsRelocs[] = {{1, 0x41c280}};

const DWORD kTerrainSetGrassShaderParamsRva = 0x23fa40;
const char kTerrainSetGrassShaderParamsName[] =
    "?SetGrassShaderParams@TerrainType@GAME@@QBEXPBVGraphicsShader2@2@@Z";
const BYTE kTerrainSetGrassShaderParamsBytes[] = {
    0xa1, 0, 0, 0, 0,
    0x83, 0xec, 0x0c,
    0x53,
    0x8b, 0x5c, 0x24, 0x14,
    0x56, 0x57,
    0x8b, 0xf9,
    0xa8, 0x01,
    0x75, 0x49
};
const Relocation kTerrainSetGrassShaderParamsRelocs[] = {{1, 0x41c2a4}};

// The DX11 terrain ground class. Like the three other shared-prologue
// targets, identity is 24 bytes and the detour steals only the first six.
const DWORD kTerrainRenderGroundRva = 0x23a530;
const char kTerrainRenderGroundName[] =
    "?RenderGround@TerrainRenderInterfaceRT@GAME@@EBEXABVName@2@"
    "AAVGraphicsCanvas@2@ABVGraphicsSceneRenderer@2@"
    "ABURenderablePass@2@_N@Z";
const BYTE kTerrainRenderGroundBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,
    0x6a, 0xff,
    0x68, 0, 0, 0, 0,
    0x64, 0xa1, 0x00, 0x00, 0x00, 0x00,
    0x50,
    0x81, 0xec, 0x08, 0x01
};
const Relocation kTerrainRenderGroundRelocs[] = {{9, 0x2a4b7b}};

// The shipped game uses the unexported runtime TerrainRT implementation, not
// the exported editor Terrain above. Its constructor writes vtable
// Engine+0x2f8820; these are the exact runtime overrides selected by that
// table. Load attaches 12-byte layer records, LoadRenderData turns their
// TerrainType filenames into GraphicsTexture Resources, and PreLoad traverses
// nearby TerrainObjects but omits the layer TerrainTypes themselves. Run 64
// observes all three boundaries without changing that behaviour.
const DWORD kTerrainRtVtableRva = 0x2f8820;
const unsigned kTerrainRtLoadVtableOffset = 0x24;
const unsigned kTerrainRtLoadRenderDataVtableOffset = 0x28;
const unsigned kTerrainRtPreloadVtableOffset = 0x34;
const unsigned kTerrainRtNumLayersVtableOffset = 0x44;
const unsigned kTerrainRtLayerTypeVtableOffset = 0x48;

const DWORD kTerrainRtLoadRva = 0x23d8d0;
const BYTE kTerrainRtLoadBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,
    0x6a, 0xff,
    0x68, 0, 0, 0, 0,
    0x64, 0xa1, 0x00, 0x00, 0x00, 0x00,
    0x50,
    0x83, 0xec, 0x58
};
const Relocation kTerrainRtLoadRelocs[] = {{9, 0x2a4cd2}};

const DWORD kTerrainRtLoadRenderDataRva = 0x23d6d0;
const BYTE kTerrainRtLoadRenderDataBytes[] = {
    0x83, 0xec, 0x64,
    0xa1, 0, 0, 0, 0,
    0x33, 0xc4,
    0x89, 0x44, 0x24, 0x60,
    0xa1, 0, 0, 0, 0,
    0x55
};
const Relocation kTerrainRtLoadRenderDataRelocs[] = {
    {4, 0x36f000}, {15, 0x3743f0}
};

const DWORD kTerrainRtPreloadRva = 0x23d400;
const BYTE kTerrainRtPreloadBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,
    0x6a, 0xff,
    0x68, 0, 0, 0, 0,
    0x64, 0xa1, 0x00, 0x00, 0x00, 0x00,
    0x50,
    0x83, 0xec, 0x28
};
const Relocation kTerrainRtPreloadRelocs[] = {{9, 0x2a4caf}};

// This exact E8 is the earliest existing point at which each admitted runtime
// layer's texture Resource objects are complete. Prefer a call patch here to
// another entry detour; a later behaviour fix can reuse the same narrow site.
const DWORD kTerrainRtLoadTexturesWindowRva = 0x23d730;
const unsigned kTerrainRtLoadTexturesCallOffset = 18;
const BYTE kTerrainRtLoadTexturesWindowBytes[] = {
    0x8b, 0xb7, 0x84, 0x00, 0x00, 0x00,
    0x03, 0xf1,
    0x8b, 0x0e,
    0x85, 0xc9,
    0x0f, 0x84, 0x2b, 0x01, 0x00, 0x00,
    0xe8, 0x19, 0x2a, 0x00, 0x00
};

const DWORD kTerrainRtNumLayersRva = 0x23d060;
const BYTE kTerrainRtNumLayersBytes[] = {
    0x8b, 0x91, 0x88, 0x00, 0x00, 0x00,
    0x2b, 0x91, 0x84, 0x00, 0x00, 0x00,
    0xb8, 0xab, 0xaa, 0xaa, 0x2a,
    0xf7, 0xea,
    0xd1, 0xfa,
    0x8b, 0xc2
};

const DWORD kTerrainRtLayerTypeRva = 0x23d020;
const BYTE kTerrainRtLayerTypeBytes[] = {
    0x8b, 0x44, 0x24, 0x04,
    0x8d, 0x14, 0x40,
    0x8b, 0x81, 0x84, 0x00, 0x00, 0x00,
    0x8b, 0x04, 0x90,
    0xc2, 0x04, 0x00,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc
};
const unsigned kTerrainRtLayerLimit = 64;

// Run 63's retained stacks identify the two colour-terrain render functions
// that force ordinary layer textures. Their common entry is verified for the
// detour, and a separate unique 24-byte body window proves the class-specific
// TerrainType::SetShaderParams call before either entry is touched.
const DWORD kTerrainPlugRenderRva = 0x236240;
const BYTE kTerrainPlugRenderBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,
    0x81, 0xec, 0x9c, 0x00, 0x00, 0x00,
    0xa1, 0, 0, 0, 0,
    0x33, 0xc4
};
const Relocation kTerrainPlugRenderRelocs[] = {{13, 0x36f000}};
const DWORD kTerrainPlugShaderWindowRva = 0x2366c8;
const BYTE kTerrainPlugShaderWindowBytes[] = {
    0x6a, 0x00, 0x53, 0x8b, 0xce,
    0xe8, 0xbe, 0x94, 0x00, 0x00,
    0x8b, 0x74, 0x24, 0x1c,
    0x6a, 0x02,
    0x8b, 0x76, 0x1c,
    0x68, 0xbc, 0x09, 0x37, 0x10
};
const Relocation kTerrainPlugShaderWindowRelocs[] = {{20, 0x3709bc}};

const DWORD kTerrainBlockRenderRva = 0x23e1e0;
const BYTE kTerrainBlockRenderBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,
    0x81, 0xec, 0x9c, 0x00, 0x00, 0x00,
    0xa1, 0, 0, 0, 0,
    0x33, 0xc4
};
const Relocation kTerrainBlockRenderRelocs[] = {{13, 0x36f000}};
const DWORD kTerrainBlockShaderWindowRva = 0x23e738;
const BYTE kTerrainBlockShaderWindowBytes[] = {
    0x75, 0x1c,
    0x6a, 0x00,
    0x57,
    0x8b, 0xc8,
    0xe8, 0x4c, 0x14, 0x00, 0x00,
    0xe9, 0x84, 0xfe, 0xff, 0xff,
    0x80, 0xb9, 0x61, 0x09, 0x00, 0x00,
    0x00
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

// --- Reflection rendering inside one recursive DX11 portal/region branch.
// FUN_1017ead0 calls the exported manager before it admits the branch's
// regions to GraphicsDeferredRendererX. The manager then calls FUN_101861d0
// once per 0x48-byte water-reflection record. Both are unique E8 sites on this
// path, so patch the calls rather than either shared function entry.
const DWORD kReflectionManagerRva = 0x187270;
const char kReflectionManagerName[] =
    "?RenderReflections@GraphicsReflectionManager@GAME@@QAEHAAV"
    "GraphicsCanvas@2@ABVRenderSet@12@@Z";
const BYTE kReflectionManagerBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x83, 0xec,
    0x0c, 0x53, 0x56, 0x57, 0xff, 0x75, 0x0c, 0x8b,
    0xf1, 0x89, 0x74, 0x24, 0x14
};
const DWORD kReflectionManagerCallWindowRva = 0x17f2c6;
const unsigned kReflectionManagerCallOffset = 13;
const BYTE kReflectionManagerCallWindowBytes[] = {
    0x8d, 0x4c, 0x24, 0x58, 0x66, 0x0f, 0xd6, 0x84,
    0x24, 0x44, 0x01, 0x00, 0x00, 0xe8, 0x98, 0x7f,
    0x00, 0x00, 0x01, 0x86, 0xf8, 0x00, 0x00, 0x00
};
const DWORD kReflectionManagerTailRva = 0x1872c3;
const BYTE kReflectionManagerTailBytes[] = {
    0x4f, 0x75, 0xea, 0x8b, 0x44, 0x24, 0x14, 0x5f,
    0x5e, 0x5b, 0x8b, 0xe5, 0x5d, 0xc2, 0x08, 0x00
};

const DWORD kReflectionPlaneRva = 0x1861d0;
const BYTE kReflectionPlaneBytes[] = {
    0x6a, 0xff, 0x68, 0x00, 0x00, 0x00, 0x00, 0x64,
    0xa1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x81, 0xec,
    0x48, 0x0f, 0x00, 0x00
};
const Relocation kReflectionPlaneRelocs[] = {{3, 0x299dcb}};
const DWORD kReflectionPlaneCallWindowRva = 0x1872b0;
const unsigned kReflectionPlaneCallOffset = 11;
const BYTE kReflectionPlaneCallWindowBytes[] = {
    0xff, 0x75, 0x0c, 0x8b, 0x0b, 0xff, 0x75, 0x08,
    0x03, 0xce, 0x51, 0xe8, 0x10, 0xef, 0xff, 0xff,
    0x83, 0xc6, 0x48, 0x4f
};
const DWORD kReflectionPlaneTailRva = 0x1869e4;
const BYTE kReflectionPlaneTailBytes[] = {
    0x8b, 0x8c, 0x24, 0x40, 0x0f, 0x00, 0x00, 0x33,
    0xcc, 0xe8, 0x7e, 0x47, 0xf7, 0xff, 0x81, 0xc4,
    0x54, 0x0f, 0x00, 0x00, 0xc2, 0x0c, 0x00, 0xcc
};

// Direct children of one reflection-plane helper. These two unique E8 sites
// split first-use scene admission from forward colour submission without
// detouring either shared GraphicsForwardRenderer entry.
const DWORD kReflectionBuildSceneRva = 0x17d9d0;
const char kReflectionBuildSceneName[] =
    "?BuildScene@GraphicsForwardRenderer@GAME@@QAEX_N@Z";
const BYTE kReflectionBuildSceneBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x6a, 0xff,
    0x68, 0, 0, 0, 0, 0x64, 0xa1, 0, 0, 0, 0,
    0x50, 0x83, 0xec, 0x18, 0x53
};
const Relocation kReflectionBuildSceneRelocs[] = {{9, 0x299ca0}};
const DWORD kReflectionBuildSceneCallWindowRva = 0x1864f0;
const unsigned kReflectionBuildSceneCallOffset = 17;
const BYTE kReflectionBuildSceneCallWindowBytes[] = {
    0x66, 0xc7, 0x84, 0x24, 0x20, 0x0e, 0x00, 0x00,
    0x00, 0x00, 0x89, 0x84, 0x24, 0xe0, 0x0d, 0x00,
    0x00, 0xe8, 0xca, 0x74, 0xff, 0xff
};
const DWORD kReflectionBuildSceneTailRva = 0x17dac3;
const BYTE kReflectionBuildSceneTailBytes[] = {
    0x64, 0x89, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x59,
    0x5f, 0x5e, 0x5b, 0x8b, 0xe5, 0x5d, 0xc2, 0x04, 0x00
};

const DWORD kReflectionRenderLightRva = 0x179a40;
const char kReflectionRenderLightName[] =
    "?RenderLightStyle@GraphicsForwardRenderer@GAME@@QAEXAAV"
    "GraphicsCanvas@2@ABVGraphicsLight@2@ABVName@2@I@Z";
const BYTE kReflectionRenderLightBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x81, 0xec,
    0xa0, 0x00, 0x00, 0x00, 0xa1, 0, 0, 0, 0, 0x33, 0xc4
};
const Relocation kReflectionRenderLightRelocs[] = {{13, 0x36f000}};
const DWORD kReflectionRenderLightCallWindowRva = 0x18693b;
const unsigned kReflectionRenderLightCallOffset = 18;
const BYTE kReflectionRenderLightCallWindowBytes[] = {
    0x6a, 0x00, 0x68, 0, 0, 0, 0, 0xff,
    0x70, 0x18, 0x8d, 0x8c, 0x24, 0xd0, 0x04, 0x00,
    0x00, 0x56, 0xe8, 0xee, 0x30, 0xff, 0xff
};
const Relocation kReflectionRenderLightCallRelocs[] = {{3, 0x41b3c8}};
const DWORD kReflectionRenderLightTailRva = 0x179bc4;
const BYTE kReflectionRenderLightTailBytes[] = {
    0x8b, 0x8c, 0x24, 0xa4, 0x00, 0x00, 0x00, 0x5f,
    0x5e, 0x33, 0xcc, 0xe8, 0x9c, 0x15, 0xf8, 0xff,
    0x8b, 0xe5, 0x5d, 0xc2, 0x10, 0x00
};

// The RenderLightStyle scene-list executor dispatches RenderPass through
// GraphicsRenderable's virtual slot +0x28, so there is no direct E8 that can
// be narrowed with patchCall. This exact exported GraphicsMeshInstance
// override supplies the missing major renderable class for Run 79. Its
// opening is one of four shared `55 8b ec 83 e4 f8` targets: verify all 24
// bytes, including the relocated security-cookie address, and steal only the
// first six complete non-relative bytes. The independent tail proves four
// explicit stack arguments (`ret 0x10`).
const DWORD kGraphicsMeshInstanceRenderPassRva = 0x172dd0;
const char kGraphicsMeshInstanceRenderPassName[] =
    "?RenderPass@GraphicsMeshInstance@GAME@@UBEX"
    "ABURenderablePass@2@ABVName@2@AAVGraphicsCanvas@2@"
    "ABVGraphicsSceneRenderer@2@@Z";
const BYTE kGraphicsMeshInstanceRenderPassBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,
    0x81, 0xec, 0xfc, 0x00, 0x00, 0x00,
    0xa1, 0x00, 0x00, 0x00, 0x00,
    0x33, 0xc4, 0x89, 0x84, 0x24, 0xf8, 0x00
};
const Relocation kGraphicsMeshInstanceRenderPassRelocs[] = {
    {13, 0x36f000}
};
const DWORD kGraphicsMeshInstanceRenderPassTailRva = 0x173127;
const BYTE kGraphicsMeshInstanceRenderPassTailBytes[] = {
    0x8b, 0x8c, 0x24, 0x04, 0x01, 0x00, 0x00,
    0x5f, 0x5e, 0x5b, 0x33, 0xcc,
    0xe8, 0x38, 0x80, 0xf8, 0xff,
    0x8b, 0xe5, 0x5d, 0xc2, 0x10, 0x00
};

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

const BYTE kDeferredGeometrySetupCallBytes[] = {
    0xe8, 0xf3, 0xef, 0xff, 0xff, 0x83, 0x3d, 0, 0, 0, 0,
    0x00, 0x74, 0x0a, 0x53, 0x8b
};
const Relocation kDeferredGeometrySetupCallRelocs[] = {{7, 0x374418}};
const BYTE kDeferredGeometrySceneCallBytes[] = {
    0xe8, 0xd9, 0x1f, 0x02, 0x00, 0xc7, 0x44, 0x24,
    0x14, 0x00, 0x00, 0x00, 0x00, 0xc7, 0x44, 0x24
};
const BYTE kDeferredShadowsCallBytes[] = {
    0xe8, 0xf7, 0xdb, 0xff, 0xff, 0x8d, 0x44, 0x24,
    0x14, 0x50, 0x53, 0x8b, 0xcf, 0xe8, 0xda, 0xe1
};
const BYTE kDeferredLightingCallBytes[] = {
    0xe8, 0xda, 0xe1, 0xff, 0xff, 0x8b, 0xf0, 0x80,
    0x7c, 0x24, 0x13, 0x00, 0x8b, 0x4c, 0x24, 0x24
};
const BYTE kDeferredResolveCallBytes[] = {
    0xe8, 0x7e, 0x03, 0x00, 0x00, 0x8b, 0x87, 0x24,
    0x09, 0x00, 0x00, 0x85, 0xc0, 0x74, 0x12, 0x50
};
const BYTE kDeferredAoCallBytes[] = {
    0xe8, 0x4c, 0x64, 0xff, 0xff, 0xc7, 0x87, 0x24,
    0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8d
};
const BYTE kDeferredLateSceneACallBytes[] = {
    0xe8, 0xd5, 0xb7, 0xff, 0xff, 0x53, 0x8b, 0xcf,
    0xe8, 0x4d, 0xb5, 0xff, 0xff, 0xa1, 0x04, 0xa4
};
const BYTE kDeferredLateSceneBCallBytes[] = {
    0xe8, 0x4d, 0xb5, 0xff, 0xff, 0xa1, 0, 0, 0, 0,
    0x6a, 0x00, 0x6a, 0x00, 0x68, 0x20
};
const Relocation kDeferredLateSceneBCallRelocs[] = {{6, 0x41a404}};
const BYTE kDeferredLateSceneListCallBytes[] = {
    0xe8, 0xe9, 0x1e, 0x02, 0x00, 0x53, 0x8b, 0xcf,
    0xe8, 0x61, 0xb5, 0xff, 0xff, 0xff, 0x75, 0x20
};
const BYTE kDeferredPostHighlightCallBytes[] = {
    0xe8, 0x61, 0xb5, 0xff, 0xff, 0xff, 0x75, 0x20,
    0x8b, 0xcf, 0x53, 0xe8, 0x86, 0xf5, 0xff, 0xff
};
const BYTE kDeferredPostFogCallBytes[] = {
    0xe8, 0x86, 0xf5, 0xff, 0xff, 0x33, 0xf6, 0x80,
    0x7d, 0x14, 0x00, 0x74, 0x0a, 0x53, 0x8b, 0xcf
};
const BYTE kDeferredPostMaskCallBytes[] = {
    0xe8, 0xd6, 0xbc, 0xff, 0xff, 0x8b, 0xf0, 0x80,
    0xbf, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x74, 0x35
};
const BYTE kDeferredPostCompositeCallBytes[] = {
    0xe8, 0x23, 0xf2, 0xff, 0xff, 0xa1, 0, 0, 0, 0,
    0x8b, 0x80, 0x60, 0x01, 0x00, 0x00
};
const Relocation kDeferredPostCompositeCallRelocs[] = {{6, 0x3743f0}};
const BYTE kDeferredPostDebugCallBytes[] = {
    0xe8, 0x77, 0xb1, 0xff, 0xff, 0x8b, 0x44, 0x24,
    0x14, 0x85, 0xc0, 0x74, 0x0a, 0x50, 0xff, 0x15
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

enum DeferredPass {
    DeferredPassNone,
    DeferredPassGeometry,
    DeferredPassShadows,
    DeferredPassLighting,
    DeferredPassResolve,
    DeferredPassLateScene,
    DeferredPassPost,
    DeferredPassCount
};

enum DeferredGeometrySite {
    DeferredGeometrySiteNone,
    DeferredGeometrySiteSetup,
    DeferredGeometrySiteScene,
    DeferredGeometrySiteCount
};

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

struct DeferredCallSite {
    DWORD callRva;
    DWORD targetRva;
    const BYTE* bytes;
    const Relocation* relocations;
    unsigned relocationCount;
    DeferredPass pass;
    unsigned arguments;
};

const DeferredCallSite kDeferredCallSites[] = {
    {0x1663a8, 0x1653a0, kDeferredGeometrySetupCallBytes,
     kDeferredGeometrySetupCallRelocs, 1, DeferredPassGeometry, 2},
    {0x166412, 0x1883f0, kDeferredGeometrySceneCallBytes,
     nullptr, 0, DeferredPassGeometry, 5},
    {0x166454, 0x164050, kDeferredShadowsCallBytes,
     nullptr, 0, DeferredPassShadows, 2},
    {0x166461, 0x164640, kDeferredLightingCallBytes,
     nullptr, 0, DeferredPassLighting, 2},
    {0x16647d, 0x166800, kDeferredResolveCallBytes,
     nullptr, 0, DeferredPassResolve, 3},
    {0x16648f, 0x15c8e0, kDeferredAoCallBytes,
     nullptr, 0, DeferredPassResolve, 1},
    {0x1664a6, 0x161c80, kDeferredLateSceneACallBytes,
     nullptr, 0, DeferredPassLateScene, 2},
    {0x1664ae, 0x161a00, kDeferredLateSceneBCallBytes,
     kDeferredLateSceneBCallRelocs, 1, DeferredPassLateScene, 1},
    {0x166502, 0x1883f0, kDeferredLateSceneListCallBytes,
     nullptr, 0, DeferredPassLateScene, 5},
    {0x16650a, 0x161a70, kDeferredPostHighlightCallBytes,
     nullptr, 0, DeferredPassPost, 1},
    {0x166515, 0x165aa0, kDeferredPostFogCallBytes,
     nullptr, 0, DeferredPassPost, 2},
    {0x166525, 0x162200, kDeferredPostMaskCallBytes,
     nullptr, 0, DeferredPassPost, 1},
    {0x166588, 0x1657b0, kDeferredPostCompositeCallBytes,
     kDeferredPostCompositeCallRelocs, 1, DeferredPassPost, 5},
    {0x1665a4, 0x161720, kDeferredPostDebugCallBytes,
     nullptr, 0, DeferredPassPost, 1},
};
const unsigned kDeferredCallSiteCount =
    sizeof(kDeferredCallSites) / sizeof(kDeferredCallSites[0]);

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
const unsigned kGroupTerrain = 0x8000;
const unsigned kGroupDeferredPasses = 0x10000;
const unsigned kGroupReflections = 0x20000;

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
// [performance] shadow_defer_cold_alpha. Exact GraphicsMeshInstance casters
// whose root mesh is not resident are omitted before their pass count is read;
// alpha-tested casters whose base texture is not resident are omitted later at
// the record boundary. State-0 dependencies are explicitly handed to the
// engine's loader and return after reaching state 2. Colour rendering and
// resident casters are untouched.
bool g_shadowDeferColdAlpha;
bool g_shadowDeferActive;
// [performance] shadow_defer_cold_actor_pose. Run 68 resolves a still-earlier
// synchronous root-mesh dependency in the exact Actor::AddToScene class.
// When enabled, state-0 roots are queued and Actor::UpdateMeshInstance is
// deferred for this directional gather only. The later root-caster gate above
// then omits the not-yet-resident renderable. This option implies the complete
// accepted shadow-defer patch set and, like it, installs no trace group.
bool g_shadowDeferColdActorPose;
bool g_shadowActorPoseDeferActive;
// [performance] terrain_preload_layers. After runtime LoadRenderData creates
// one TerrainType's texture Resources, call the engine's own semantic
// PreLoad(true) so those resources enter the ordinary background queue before
// first colour use. This is a fix, not an instrument.
bool g_terrainPreloadLayers;
bool g_terrainPreloadLayersActive;
bool g_terrainTracing;
// [performance] reflection_defer_admission_mesh. The exact reflection
// BuildScene call is the only writer, and CreateBuffer observes it on the same
// render thread. A fixed count is preferable to a machine-dependent elapsed
// time: the marked transition population was 63--172 buffers in seven
// independent runs, while the largest neighbouring population was 30.
const unsigned kReflectionAdmissionBufferThreshold = 32;
const unsigned kSecondaryPassAdmissionBudgetMax = 64;
bool g_reflectionDeferAdmissionMesh;
bool g_reflectionDeferAdmissionMeshActive;
bool g_reflectionDeferAdmissionAll;
bool g_reflectionDeferAdmissionAllActive;
// [performance] secondary_pass_admission_budget. Unlike the rejected
// one-consumer omissions, this keeps resource/material preparation in place
// and budgets first GPU participation across reflection and directional
// shadow as one population.
unsigned g_secondaryPassAdmissionBudget;
bool g_secondaryPassAdmissionActive;
bool g_secondaryAdmissionArmed;
bool g_secondaryAdmissionDrawHooksReady;
bool g_insideReflectionRenderLight;
unsigned g_secondaryAdmissionFrameSerial;
unsigned g_secondaryAdmissionBudgetFrame;
unsigned g_secondaryAdmissionUsedThisFrame;
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

LONG g_installed;
unsigned g_installedHooks;

const volatile DWORD* g_mainThreadId;

bool onMainThread() {
    return g_mainThreadId && *g_mainThreadId == GetCurrentThreadId();
}

bool reflectionAdmissionThresholdReached(unsigned buffers) {
    return buffers >= kReflectionAdmissionBufferThreshold;
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
typedef int (__fastcall* ShadowMeshPassCountFn)(void* self, void* edx);
typedef void (__fastcall* ActorUpdateMeshInstanceFn)(void* self, void* edx);
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
typedef void (__fastcall* TerrainPreloadFn)(void* self, void* edx,
                                            int includeTextures);
typedef void (__fastcall* TerrainSetShaderParamsFn)(
    const void* self, void* edx, const void* shader, int materialIndex);
typedef void (__fastcall* TerrainSetGrassShaderParamsFn)(
    const void* self, void* edx, const void* shader);
typedef void (__fastcall* TerrainRenderGroundFn)(
    const void* self, void* edx, const void* name, void* canvas,
    const void* sceneRenderer, const void* pass, int flag);
typedef int (__fastcall* TerrainRtLoadFn)(void* self, void* edx,
                                         void* reader, int version);
typedef int (__fastcall* TerrainRtLoadRenderDataFn)(void* self, void* edx);
typedef void (__fastcall* TerrainRtPreloadFn)(
    void* self, void* edx, int priority, const void* frustum,
    unsigned flags);
typedef unsigned (__fastcall* TerrainRtNumLayersFn)(const void* self,
                                                     void* edx);
typedef void* (__fastcall* TerrainRtLayerTypeFn)(const void* self, void* edx,
                                                 unsigned layer);
typedef void (__fastcall* TerrainTypeLoadTexturesFn)(void* self, void* edx);
// The decompiler's five parameters include the implicit this pointer. Both
// concrete functions end in `ret 0x10`: exactly four explicit stack args.
typedef void (__fastcall* TerrainColourRenderFn)(
    void* self, void* edx, const void* a, const void* b, const void* c,
    const void* d);
typedef void (__fastcall* GraphicsMeshInstanceRenderPassFn)(
    void* self, void* edx, const void* pass, const void* name, void* canvas,
    const void* sceneRenderer);
typedef uintptr_t (__fastcall* DeferredFn1)(
    void* self, void* edx, uintptr_t a);
typedef uintptr_t (__fastcall* DeferredFn2)(
    void* self, void* edx, uintptr_t a, uintptr_t b);
typedef uintptr_t (__fastcall* DeferredFn3)(
    void* self, void* edx, uintptr_t a, uintptr_t b, uintptr_t c);
typedef uintptr_t (__fastcall* DeferredFn5)(
    void* self, void* edx, uintptr_t a, uintptr_t b, uintptr_t c,
    uintptr_t d, uintptr_t e);
typedef uintptr_t (__fastcall* DeferredRenderFn)(
    void* self, void* edx, uintptr_t a, uintptr_t b, uintptr_t c,
    uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g);
typedef int (__fastcall* ReflectionManagerFn)(
    void* self, void* edx, uintptr_t canvas, uintptr_t renderSet);
typedef uintptr_t (__stdcall* ReflectionPlaneFn)(
    uintptr_t record, uintptr_t canvas, uintptr_t renderSet);
typedef void (__fastcall* ReflectionBuildSceneFn)(
    void* self, void* edx, int includeHidden);
typedef void (__fastcall* ReflectionRenderLightFn)(
    void* self, void* edx, uintptr_t canvas, uintptr_t light,
    uintptr_t styleName, uintptr_t flags);
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
ShadowMeshPassCountFn g_shadowMeshPassCount;
ActorUpdateMeshInstanceFn g_actorUpdateMeshInstance;
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
TerrainPreloadFn g_terrainPreload;
// The exported entry, retained separately from g_terrainPreload's trace
// trampoline. Calling this makes behavior invocations visible to the trace
// when its entry detour is installed, and calls stock code directly otherwise.
TerrainPreloadFn g_terrainPreloadEntry;
TerrainSetShaderParamsFn g_terrainSetShaderParams;
TerrainSetGrassShaderParamsFn g_terrainSetGrassShaderParams;
TerrainRenderGroundFn g_terrainRenderGround;
TerrainRtLoadFn g_terrainRtLoad;
TerrainRtLoadRenderDataFn g_terrainRtLoadRenderData;
TerrainRtPreloadFn g_terrainRtPreload;
TerrainRtNumLayersFn g_terrainRtNumLayers;
TerrainRtLayerTypeFn g_terrainRtLayerType;
TerrainTypeLoadTexturesFn g_terrainRtLoadTextures;
TerrainColourRenderFn g_terrainPlugRender;
TerrainColourRenderFn g_terrainBlockRender;
GraphicsMeshInstanceRenderPassFn g_graphicsMeshInstanceRenderPass;
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
ReflectionRenderLightFn g_reflectionRenderLight;
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

CallPatch g_deferredCallPatches[kDeferredCallSiteCount];
CallPatch g_deferredOwnerPatch;
CallPatch g_reflectionManagerPatch;
CallPatch g_reflectionPlanePatch;
CallPatch g_reflectionBuildScenePatch;
CallPatch g_reflectionRenderLightPatch;
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
Detour g_shadowMeshPassCountDetour;
Detour g_unloadLevelDetour;
Detour g_enqueueDetour;
Detour g_readFromFileDetour;
Detour g_archiveBlockDetour;
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
Detour g_terrainPlugRenderDetour;
Detour g_terrainBlockRenderDetour;
Detour g_graphicsMeshInstanceRenderPassDetour;
CallPatch g_terrainRtLoadTexturesPatch;
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
CallPatch g_shadowActorUpdateMeshPatch;
CallPatch g_shadowMeshEnsurePatch;
CallPatch g_shadowMaterialTexturePatch;
CallPatch g_shadowTextureParameterPatch;
CallPatch g_shadowMeshParameterPatch;
CallPatch g_shadowInstanceBumpEnsurePatch;
CallPatch g_shadowRecordPatch;
LONG g_insideDirectional;
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

enum ReflectionCell {
    ReflectionCellNone,
    ReflectionCellI1P1,
    ReflectionCellI1P2,
    ReflectionCellI2P1,
    ReflectionCellI2P2,
    ReflectionCellCount
};

struct ReflectionCellCounters {
    tq::probe::Counter count;
    tq::probe::Counter durationUs;
    tq::probe::Counter drawUs;
    tq::probe::Counter resourceCount;
    tq::probe::Counter resourceUs;
    tq::probe::Counter textureCount;
    tq::probe::Counter textureUs;
    tq::probe::Counter bufferCount;
    tq::probe::Counter bufferUs;
    tq::probe::Counter buildSceneCount;
    tq::probe::Counter buildSceneUs;
    tq::probe::Counter renderLightCount;
    tq::probe::Counter renderLightUs;
};

#define TQ_REFLECTION_CELL_ROW(prefix) \
    {tq::probe::CounterEngineReflection##prefix, \
     tq::probe::CounterEngineReflection##prefix##Us, \
     tq::probe::CounterEngineReflection##prefix##DrawUs, \
     tq::probe::CounterEngineReflection##prefix##ResLoad, \
     tq::probe::CounterEngineReflection##prefix##ResLoadUs, \
     tq::probe::CounterEngineReflection##prefix##TexCreate, \
     tq::probe::CounterEngineReflection##prefix##TexCreateUs, \
     tq::probe::CounterEngineReflection##prefix##BufCreate, \
     tq::probe::CounterEngineReflection##prefix##BufCreateUs, \
     tq::probe::CounterEngineReflection##prefix##BuildScene, \
     tq::probe::CounterEngineReflection##prefix##BuildSceneUs, \
     tq::probe::CounterEngineReflection##prefix##RenderLight, \
     tq::probe::CounterEngineReflection##prefix##RenderLightUs}
const ReflectionCellCounters kReflectionCellCounters[] = {
    {tq::probe::CounterCount, tq::probe::CounterCount,
     tq::probe::CounterCount, tq::probe::CounterCount,
     tq::probe::CounterCount, tq::probe::CounterCount,
     tq::probe::CounterCount, tq::probe::CounterCount,
     tq::probe::CounterCount, tq::probe::CounterCount,
     tq::probe::CounterCount, tq::probe::CounterCount,
     tq::probe::CounterCount},
    TQ_REFLECTION_CELL_ROW(I1P1),
    TQ_REFLECTION_CELL_ROW(I1P2),
    TQ_REFLECTION_CELL_ROW(I2P1),
    TQ_REFLECTION_CELL_ROW(I2P2),
};
#undef TQ_REFLECTION_CELL_ROW

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
enum ReflectionChild {
    ReflectionChildBuildScene,
    ReflectionChildRenderLight,
    ReflectionChildCount
};
const tq::probe::GpuPhase kReflectionChildGpuPhases[][ReflectionChildCount] = {
    {tq::probe::GpuPhaseCount, tq::probe::GpuPhaseCount},
    {tq::probe::GpuReflectionI1P1BuildScene,
     tq::probe::GpuReflectionI1P1RenderLight},
    {tq::probe::GpuReflectionI1P2BuildScene,
     tq::probe::GpuReflectionI1P2RenderLight},
    {tq::probe::GpuReflectionI2P1BuildScene,
     tq::probe::GpuReflectionI2P1RenderLight},
    {tq::probe::GpuReflectionI2P2BuildScene,
     tq::probe::GpuReflectionI2P2RenderLight},
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

struct ReflectionLocation {
    unsigned manager;
    unsigned plane;
    ReflectionCell cell;
};

ReflectionLocation currentReflectionLocation() {
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

// Runs 77 and 78 proved that a fixed moving ordinal window can miss the marked
// reflection producer: Run 78's selected child ended at exactly draw 192.
// Run 79 keeps the same sixteen distinct query pairs but covers draws 1--320
// continuously in 20-draw bins. Ordinary frames still execute only the
// inactive inline branch in the D3D hooks.
enum GpuChunkClass {
    GpuChunkNone,
    GpuChunkReflection,
    GpuChunkClassCount
};
const unsigned kGpuChunkDraws = 20;
const unsigned kGpuChunkStartDraw = 1;
const unsigned kGpuChunkCount = 16;
const unsigned kGpuChunkEventSlots = 32;
const unsigned kGpuChunkMarkerFrames = 120;
const unsigned kGpuChunkRenderableCallSlots = 256;
const unsigned kGpuChunkRenderableHotCpuUs = 250;
const unsigned kGpuChunkReflectionBuildSceneTriggerUs = 2000;

struct GpuChunkBin {
    unsigned firstDraw;
    unsigned draws;
    unsigned indexedDraws;
    unsigned long long elements;
    unsigned pixelShaderNullDraws;
    unsigned resource0NullDraws;
    unsigned bindingChanges;
    const void* firstVertexShader;
    const void* lastVertexShader;
    const void* firstPixelShader;
    const void* lastPixelShader;
    const void* firstResource0;
    const void* lastResource0;
    const void* firstVertexBuffer0;
    const void* lastVertexBuffer0;
    const void* firstIndexBuffer;
    const void* lastIndexBuffer;
};

enum GpuChunkRenderableKind {
    GpuChunkRenderableNone,
    GpuChunkTerrainPlug,
    GpuChunkTerrainBlock,
    GpuChunkMeshInstance
};

struct GpuChunkRenderableCall {
    GpuChunkRenderableKind kind;
    const void* object;
    const void* terrainType;
    int materialIndex;
    unsigned firstDraw;
    unsigned lastDraw;
    unsigned cpuUs;
    unsigned resourceCount;
    unsigned resourceUs;
    unsigned textureCount;
    unsigned textureUs;
    unsigned bufferCount;
    unsigned bufferUs;
};

struct GpuChunkEvent {
    unsigned framePlusOne;
    GpuChunkClass kind;
    unsigned manager;
    unsigned plane;
    unsigned startDraw;
    unsigned triggerUs;
    unsigned chunks;
    bool overflow;
    unsigned renderableCalls;
    bool renderableCallOverflow;
    GpuChunkBin bins[kGpuChunkCount];
    GpuChunkRenderableCall renderables[kGpuChunkRenderableCallSlots];
};

struct ActiveGpuChunkEvent {
    GpuChunkEvent* event;
    ID3D11DeviceContext* context;
    unsigned chunk;
    unsigned drawsSeen;
    bool opened;
    bool recording;
};

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
    InterlockedExchange(&detail::gpuChunkDrawActive, active ? 1 : 0);
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

struct GpuChunkRenderableCallScope {
    GpuChunkRenderableCall* call;
    GpuChunkRenderableCall* prior;

    GpuChunkRenderableCallScope(GpuChunkRenderableKind kind,
                                const void* object)
        : call(nullptr), prior(g_activeGpuChunkRenderableCall) {
        if (!g_gpuChunkTracing || !onMainThread()) return;
        ActiveGpuChunkEvent& active =
            g_activeGpuChunks[GpuChunkReflection];
        if (!active.event || !active.recording) return;
        GpuChunkEvent& event = *active.event;
        // Retain every major renderable call that can overlap the continuous
        // draw window. Calls beyond draw 320 cannot explain a covered bin.
        if (active.drawsSeen + 1 < event.startDraw) return;
        if (event.renderableCalls >= kGpuChunkRenderableCallSlots) {
            event.renderableCallOverflow = true;
            return;
        }
        call = &event.renderables[event.renderableCalls++];
        memset(call, 0, sizeof(*call));
        call->kind = kind;
        call->object = object;
        call->materialIndex = -1;
        call->firstDraw = active.drawsSeen + 1;
        g_activeGpuChunkRenderableCall = call;
    }

    void finish(unsigned elapsedUs) {
        if (!call) return;
        call->cpuUs = elapsedUs;
        call->lastDraw =
            g_activeGpuChunks[GpuChunkReflection].drawsSeen;
    }

    ~GpuChunkRenderableCallScope() {
        g_activeGpuChunkRenderableCall = prior;
    }
};

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

// Run 82 removed the complete first reflection child, but the same marked
// transition paid first-use GPU work in the next exact colour and directional
// consumers.  Keep one bounded identity table across the session so a frame
// can distinguish a large newly seen renderable population from ordinary
// repeated draws.  This is trace-only and adds no Engine patch or clock.
enum AdmissionConsumer {
    AdmissionConsumerNone,
    AdmissionConsumerReflectionI2P1,
    AdmissionConsumerDeferredI2Setup,
    AdmissionConsumerDeferredI2Scene,
    AdmissionConsumerShadowDirectional,
    AdmissionConsumerCount
};

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

const unsigned kAdmissionRenderableIdentitySlots = 8192;
const unsigned kAdmissionRenderableIdentityProbe = 16;
const unsigned kAdmissionRenderableIdentityHashSalt = 0x9e3779b1;

struct AdmissionRenderableIdentity {
    const void* object;
    unsigned kind;
    unsigned consumerMask;
    unsigned secondaryState;
};

AdmissionRenderableIdentity
    g_admissionRenderableIdentities[kAdmissionRenderableIdentitySlots];

unsigned admissionRenderableIdentityStart(const void* object,
                                           GpuChunkRenderableKind kind) {
    uintptr_t value = (uintptr_t)object;
    value ^= value >> 7;
    value ^= value >> 15;
    value ^= (uintptr_t)kind * kAdmissionRenderableIdentityHashSalt;
    return (unsigned)value & (kAdmissionRenderableIdentitySlots - 1);
}

AdmissionRenderableIdentity* findAdmissionRenderableIdentity(
    const void* object, GpuChunkRenderableKind kind, bool create) {
    if (!object || kind <= GpuChunkRenderableNone
        || kind > GpuChunkMeshInstance) return nullptr;
    const unsigned start = admissionRenderableIdentityStart(object, kind);
    for (unsigned i = 0; i < kAdmissionRenderableIdentityProbe; ++i) {
        AdmissionRenderableIdentity& entry =
            g_admissionRenderableIdentities[
                (start + i) & (kAdmissionRenderableIdentitySlots - 1)];
        if (!entry.object) {
            if (!create) return nullptr;
            entry.object = object;
            entry.kind = (unsigned)kind;
            entry.consumerMask = 0;
            entry.secondaryState = 0;
            return &entry;
        }
        if (entry.object != object || entry.kind != (unsigned)kind) continue;
        return &entry;
    }
    tq::probe::engineCount(tq::probe::CounterEngineAdmissionIdentityOverflow);
    return nullptr;
}

bool admissionRenderableFirst(const void* object,
                              GpuChunkRenderableKind kind,
                              AdmissionConsumer consumer) {
    if (consumer <= AdmissionConsumerNone
        || consumer >= AdmissionConsumerCount) return false;
    AdmissionRenderableIdentity* const entry =
        findAdmissionRenderableIdentity(object, kind, true);
    if (!entry) return false;
    const unsigned mask = 1u << (unsigned)consumer;
    if (entry->consumerMask & mask) return false;
    entry->consumerMask |= mask;
    return true;
}

enum SecondaryAdmissionState {
    SecondaryAdmissionUnseen,
    SecondaryAdmissionAdmitted,
    SecondaryAdmissionPending
};

enum SecondaryAdmissionContext {
    SecondaryAdmissionContextNone,
    SecondaryAdmissionContextReflection,
    SecondaryAdmissionContextShadow
};

SecondaryAdmissionContext currentSecondaryAdmissionContext() {
    if (!g_secondaryPassAdmissionActive || !onMainThread())
        return SecondaryAdmissionContextNone;
    if (g_insideReflectionRenderLight)
        return SecondaryAdmissionContextReflection;
    if (InterlockedCompareExchange(&g_insideDirectional, 0, 0) > 0)
        return SecondaryAdmissionContextShadow;
    return SecondaryAdmissionContextNone;
}

void countSecondaryAdmission(SecondaryAdmissionContext context,
                             bool admitted) {
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

void armSecondaryAdmission() {
    if (!g_secondaryPassAdmissionActive || g_secondaryAdmissionArmed) return;
    g_secondaryAdmissionArmed = true;
    tq::probe::engineCount(tq::probe::CounterEngineSecondaryAdmissionTrigger);
}

bool shouldDeferSecondaryAdmission(GpuChunkRenderableKind kind,
                                   const void* object) {
    const SecondaryAdmissionContext context =
        currentSecondaryAdmissionContext();
    if (context == SecondaryAdmissionContextNone) return false;
    AdmissionRenderableIdentity* const entry =
        findAdmissionRenderableIdentity(object, kind, true);
    if (!entry) return false;  // Untracked objects keep the safe stock path.
    if (entry->secondaryState == SecondaryAdmissionAdmitted) return false;
    if (g_secondaryAdmissionBudgetFrame != g_secondaryAdmissionFrameSerial) {
        g_secondaryAdmissionBudgetFrame = g_secondaryAdmissionFrameSerial;
        g_secondaryAdmissionUsedThisFrame = 0;
    }
    if (g_secondaryAdmissionUsedThisFrame < g_secondaryPassAdmissionBudget) {
        ++g_secondaryAdmissionUsedThisFrame;
        entry->secondaryState = SecondaryAdmissionAdmitted;
        countSecondaryAdmission(context, true);
        return false;
    }
    // The first identity beyond the frame budget is the transition signal.
    // This observes the exact population being controlled and needs neither
    // a reflection nor a change between two non-null shadow-region pointers.
    armSecondaryAdmission();
    entry->secondaryState = SecondaryAdmissionPending;
    countSecondaryAdmission(context, false);
    return true;
}

struct SecondaryAdmissionDrawScope {
    bool active;
    SecondaryAdmissionDrawScope(GpuChunkRenderableKind kind,
                                const void* object)
        : active(shouldDeferSecondaryAdmission(kind, object)) {
        if (active)
            InterlockedIncrement(
                &detail::secondaryAdmissionDrawSuppressDepth);
    }
    ~SecondaryAdmissionDrawScope() {
        if (active)
            InterlockedDecrement(
                &detail::secondaryAdmissionDrawSuppressDepth);
    }
};

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

enum DeferredCreationKind {
    DeferredCreationTexture,
    DeferredCreationBuffer
};

enum CrossPassFamily {
    CrossPassNone = 0,
    CrossPassReflection = 1,
    CrossPassShadow = 2,
    CrossPassDeferred = 4
};

const unsigned kCrossPassBufferSlots = 4096;
const unsigned kCrossPassIndexSlots = 8192;
const unsigned kCrossPassIndexProbe = 16;
const unsigned kCrossPassFreshFrames = 120;
const unsigned kCrossPassMarkerReportLimit = 128;
struct CrossPassBufferRecord {
    const void* object;
    unsigned sequence;
    unsigned createdFrame;
    unsigned byteWidth;
    unsigned bindFlags;
    unsigned createdReflectionManager;
    unsigned createdReflectionPlane;
    unsigned createdDeferredInvocation;
    DeferredPass createdDeferredPass;
    DeferredGeometrySite createdDeferredSite;
    unsigned useMask;
    unsigned reflectionFirstFrame;
    unsigned reflectionManager;
    unsigned reflectionPlane;
    unsigned reflectionDraws;
    unsigned shadowFirstFrame;
    unsigned shadowDraws;
    unsigned deferredFirstFrame;
    unsigned deferredInvocation;
    DeferredPass deferredPass;
    DeferredGeometrySite deferredSite;
    unsigned deferredDraws;
};
struct CrossPassIndexEntry {
    const void* object;
    unsigned sequence;
};
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

const unsigned kDeferredCreationSlots = 4096;
struct DeferredCreationRecord {
    const void* object;
    unsigned frame;
    unsigned invocation;
    DeferredPass pass;
    DeferredGeometrySite site;
    DeferredCreationKind kind;
    unsigned elapsedUs;
    unsigned a;
    unsigned b;
    unsigned c;
    unsigned d;
    unsigned e;
    unsigned f;
};
DeferredCreationRecord g_deferredCreations[kDeferredCreationSlots];
unsigned g_deferredCreationSequence;

// Run 79's reacted-to play burst contained 51 off-main CreateTexture2D calls,
// but the owner-scoped creation ring deliberately rejected them and therefore
// retained no descriptor, thread, or cross-frame extent. This lock-free ring
// publishes each completed record last. F12 takes a sequence snapshot and
// accepts only slots whose publication sequence still matches, so a loader
// thread can never leave a partially written record looking valid.
const unsigned kOffMainTextureSlots = 512;
const unsigned kOffMainTextureMarkerFrames = 120;
const unsigned kOffMainTextureReportLimit = 192;
struct OffMainTextureRecord {
    volatile LONG publishedSequence;
    unsigned startFrame;
    unsigned finishFrame;
    unsigned elapsedUs;
    unsigned threadId;
    unsigned width;
    unsigned height;
    unsigned mipLevels;
    unsigned format;
    unsigned bindFlags;
    unsigned miscFlags;
    bool hasInitialData;
};
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

const unsigned kDeferredSlowFrameSlots = 128;
const unsigned kDeferredTopDrawsPerFrame = 12;
const unsigned kDeferredSlowMarkerFrames = 120;
const unsigned kDeferredSlowReportFrames = 8;
const unsigned kDeferredSlowFrameMinUs = 15000;

struct DeferredSlowDrawRecord {
    unsigned elapsedUs;
    unsigned ordinal;
    bool indexed;
    unsigned count;
    unsigned start;
    int base;
    unsigned invocation;
    DeferredGeometrySite site;
    tq::engineprobe::DeferredDrawBindings bindings;
};

struct DeferredSlowFrame {
    unsigned framePlusOne;
    unsigned drawUs;
    unsigned drawCount;
    unsigned recordCount;
    DeferredSlowDrawRecord records[kDeferredTopDrawsPerFrame];
};
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

struct ReflectionChildScope {
    ReflectionCell cell;
    ReflectionChild child;
    LONG priorChild;
    int64_t started;
    ID3D11DeviceContext* context;
    bool active;

    explicit ReflectionChildScope(ReflectionChild which)
        : cell(ReflectionCellNone), child(which), priorChild(0),
          started(0), context(nullptr), active(false) {
        if (!g_reflectionChildTracing || which >= ReflectionChildCount)
            return;
        const ReflectionLocation location = currentReflectionLocation();
        if (location.cell <= ReflectionCellNone
            || location.cell >= ReflectionCellCount) return;
        cell = location.cell;
        active = true;
        priorChild = InterlockedExchange(
            &g_reflectionChild, (LONG)which + 1);
        started = tq::probe::now();
        context = tq::probe::currentGpuContext();
        tq::probe::gpuBegin(context, kReflectionChildGpuPhases[cell][child]);
    }

    ~ReflectionChildScope() {
        if (!active) return;
        if (child == ReflectionChildRenderLight)
            closeGpuChunks();
        tq::probe::gpuEnd(context, kReflectionChildGpuPhases[cell][child]);
        const unsigned elapsed = tq::probe::microsecondsSince(started);
        const ReflectionCellCounters& counters = kReflectionCellCounters[cell];
        tq::probe::engineCount(
            child == ReflectionChildBuildScene
                ? counters.buildSceneCount : counters.renderLightCount);
        tq::probe::engineCount(
            child == ReflectionChildBuildScene
                ? counters.buildSceneUs : counters.renderLightUs,
            elapsed);
        if (child == ReflectionChildBuildScene
            && cell == ReflectionCellI2P1) {
            g_reflectionGpuChunkPending =
                elapsed >= kGpuChunkReflectionBuildSceneTriggerUs;
            g_reflectionGpuChunkTriggerUs = elapsed;
        }
        InterlockedExchange(&g_reflectionChild, priorChild);
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

void __fastcall hookReflectionRenderLight(
    void* self, void* edx, uintptr_t canvas, uintptr_t light,
    uintptr_t styleName, uintptr_t flags) {
    const ReflectionLocation location = currentReflectionLocation();
    const bool admission = g_reflectionAdmissionPending;
    const bool deferAll = g_reflectionDeferAdmissionAllActive && admission;
    if (!deferAll && g_gpuChunkTracing && g_reflectionGpuChunkPending
        && location.cell == ReflectionCellI2P1)
        armGpuChunks(location, g_reflectionGpuChunkTriggerUs);
    g_reflectionGpuChunkPending = false;
    g_reflectionGpuChunkTriggerUs = 0;
    const bool priorAdmissionRender = g_reflectionAdmissionRenderActive;
    g_reflectionAdmissionRenderActive =
        g_reflectionDeferAdmissionMeshActive && admission;
    g_reflectionAdmissionPending = false;
    ReflectionChildScope scope(ReflectionChildRenderLight);
    const bool priorInsideReflection = g_insideReflectionRenderLight;
    g_insideReflectionRenderLight = true;
    if (deferAll) {
        tq::probe::engineCount(
            tq::probe::CounterEngineReflectionAdmissionAllDeferred);
    } else if (g_reflectionRenderLight) {
        g_reflectionRenderLight(self, edx, canvas, light, styleName, flags);
    }
    g_insideReflectionRenderLight = priorInsideReflection;
    g_reflectionAdmissionRenderActive = priorAdmissionRender;
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

// Run 60's marked full-scene, collision-active play frame has 30 main-thread
// ResourceLoader calls / 102.518 ms, but only 17 / 23.383 ms are inside the
// directional-shadow bracket. This independently partitions the remainder by
// Engine phase and engine-native filename suffix. It is diagnostic only: the
// exact LoadResource trampoline is still called once with unchanged arguments.
enum OutsideDirResourcePhase {
    OutsideDirResourceRender,
    OutsideDirResourceUpdate,
    OutsideDirResourceOther,
    OutsideDirResourcePhaseCount
};

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

enum TerrainParameterPath {
    TerrainParameterNone,
    TerrainParameterMaterial,
    TerrainParameterGrass
};

// PreLoad can be called off the render thread, while its consumers are on the
// main thread. Keep a fixed, allocation-free identity table and copy a
// snapshot into each retained load record. That preserves the state as it was
// at the load rather than accidentally crediting a later preload before F12.
const unsigned kTerrainPreloadStateSlots = 2048;
static_assert((kTerrainPreloadStateSlots
               & (kTerrainPreloadStateSlots - 1)) == 0,
              "terrain preload table must be a power of two");

struct TerrainPreloadState {
    void* volatile terrain;
    LONG trueCount;
    LONG falseCount;
    LONG lastTrueFramePlusOne;
    LONG lastFalseFramePlusOne;
    LONG rtLoadAttachCount;
    LONG rtLoadAttachFirstFramePlusOne;
    LONG rtLoadAttachLastFramePlusOne;
    LONG rtLoadTexturesCount;
    LONG rtLoadTexturesFirstFramePlusOne;
    LONG rtLoadTexturesLastFramePlusOne;
    LONG rtOwnerPreloadCount;
    LONG rtOwnerPreloadFirstFramePlusOne;
    LONG rtOwnerPreloadLastFramePlusOne;
};

struct TerrainPreloadSnapshot {
    unsigned trueCount;
    unsigned falseCount;
    unsigned lastTrueFramePlusOne;
    unsigned lastFalseFramePlusOne;
    unsigned rtLoadAttachCount;
    unsigned rtLoadAttachFirstFramePlusOne;
    unsigned rtLoadAttachLastFramePlusOne;
    unsigned rtLoadTexturesCount;
    unsigned rtLoadTexturesFirstFramePlusOne;
    unsigned rtLoadTexturesLastFramePlusOne;
    unsigned rtOwnerPreloadCount;
    unsigned rtOwnerPreloadFirstFramePlusOne;
    unsigned rtOwnerPreloadLastFramePlusOne;
};

enum TerrainRtEvent {
    TerrainRtLoadAttach,
    TerrainRtLoadTextures,
    TerrainRtOwnerPreload
};

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

const unsigned kOutsideDirResourceReportSlots = 128;
const unsigned kOutsideDirResourceMarkerFrames = 120;
const unsigned kOutsideDirResourceNameChars = 128;
const unsigned kOutsideDirResourceCallerDepth = 24;

struct OutsideDirResourceReport {
    LONG ready;
    unsigned frame;
    uint32_t us;
    unsigned state;
    OutsideDirResourcePhase phase;
    ShadowResourceType type;
    bool stateKnown;
    bool callerVerified;
    char callerTag;
    DWORD callerRva;
    unsigned callerDepth;
    ChainFrame callerFrames[kOutsideDirResourceCallerDepth];
    const void* terrainType;
    TerrainParameterPath terrainPath;
    int terrainMaterialIndex;
    TerrainPreloadSnapshot terrainPreload;
    unsigned deferredInvocation;
    DeferredPass deferredPass;
    DeferredGeometrySite deferredSite;
    unsigned reflectionManager;
    unsigned reflectionPlane;
    char resource[kOutsideDirResourceNameChars + 1];
};

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

// The remaining Run 67 play transition has 26 state-0 mesh loads inside the
// directional build after the exact base GraphicsMeshInstance root gate has
// already omitted 12 other casters. Preserve the same bounded, delayed caller
// evidence used for outside-directional loads, but only for that narrow class.
const unsigned kShadowMeshResourceReportSlots = 128;
const unsigned kShadowMeshResourceMarkerFrames = 120;

struct ShadowMeshResourceReport {
    LONG ready;
    unsigned frame;
    uint32_t us;
    bool inQueue;
    bool callerVerified;
    char callerTag;
    DWORD callerRva;
    unsigned callerDepth;
    ChainFrame callerFrames[kOutsideDirResourceCallerDepth];
    char resource[kOutsideDirResourceNameChars + 1];
};

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

bool shouldDeferShadowMesh(unsigned state) {
    // Resource states 0 and 1 are respectively cold and loading. The stock
    // method synchronously ensures both before it can read the pass count.
    return state <= 1;
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

void countDeferredShadowMesh(unsigned state, bool enqueued, bool failed) {
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

bool shadowActorPoseQueueConfirmed(unsigned state, bool inQueue) {
    // A state-0 root is safe to omit only while it has an observable queue
    // owner. State 1 is the loader's in-progress state. State 2 must run the
    // pose update immediately; admitting a resident caster after skipping its
    // update would be worse than the synchronous fallback.
    return state == 1 || (state == 0 && inQueue);
}

void countShadowActorPoseEnqueueFailure() {
    if (g_shadowTracing)
        tq::probe::engineCount(
            tq::probe::CounterEngineShadowActorPoseEnqueueFailed);
}

void __fastcall hookShadowActorUpdateMeshInstance(void* self, void* edx) {
    if (!g_actorUpdateMeshInstance) return;
    // This wrapper replaces only Actor::AddToScene's direct call. The dynamic
    // bracket narrows it further to the main-thread DX11 directional gather;
    // every colour, point-shadow, worker, resident, and other Actor update is
    // forwarded unchanged.
    if (!g_shadowActorPoseDeferActive || !onMainThread()
        || InterlockedCompareExchange(&g_insideDirectional, 0, 0) <= 0
        || !g_resourceStateVerified || !self || !g_resourceLoaderAccessor
        || !g_shadowEnqueue) {
        g_actorUpdateMeshInstance(self, edx);
        return;
    }

    void* const instance = *(void**)((BYTE*)self + kActorMeshInstanceOffset);
    void* const mesh = instance
        ? *(void**)((BYTE*)instance + kGraphicsMeshResourceOffset) : nullptr;
    if (!mesh) {
        g_actorUpdateMeshInstance(self, edx);
        return;
    }
    const unsigned state = *(const unsigned*)((const BYTE*)mesh
                                              + kResourceLoadedStateOffset);
    if (!shouldDeferShadowMesh(state)) {
        g_actorUpdateMeshInstance(self, edx);
        return;
    }

    bool enqueued = false;
    bool failed = false;
    if (state == 0
        && !*(void* const*)((const BYTE*)mesh + kResourceInQueueOffset)) {
        void* const loader = g_resourceLoaderAccessor(mesh, nullptr);
        if (loader) {
            // The same stock preload tuple used by the later root-caster gate:
            // priority 1, notify=true, immediate=false.
            g_shadowEnqueue(loader, nullptr, mesh, 1, 1, 0);
            const unsigned after = *(const unsigned*)((const BYTE*)mesh
                                                       + kResourceLoadedStateOffset);
            const bool inQueue = *(void* const*)((const BYTE*)mesh
                                                 + kResourceInQueueOffset)
                != nullptr;
            enqueued = shadowActorPoseQueueConfirmed(after, inQueue);
            if (after >= 2) {
                g_actorUpdateMeshInstance(self, edx);
                return;
            }
        }
        failed = !enqueued;
        if (failed) {
            countShadowActorPoseEnqueueFailure();
            g_actorUpdateMeshInstance(self, edx);
            return;
        }
    }
    countDeferredShadowActorPose(state, enqueued, false);
    // Do not enter UpdatePose for this directional gather. Actor::AddToScene
    // still submits the renderable; the already-installed exact-class
    // GetNumShadowRenderPasses gate returns zero while this root is cold, and
    // both paths restore themselves automatically after residency reaches 2.
}

int __fastcall hookShadowMeshPassCount(void* self, void* edx) {
    if (!g_shadowMeshPassCount) return 0;
    // This exported method is global, but its behavior changes only for the
    // main-thread directional build. Every colour, point-shadow, worker, and
    // resident call reaches the exact original function through its
    // trampoline.
    if (!g_shadowDeferActive || !onMainThread()
        || InterlockedCompareExchange(&g_insideDirectional, 0, 0) <= 0
        || !g_resourceStateVerified || !self || !g_resourceLoaderAccessor
        || !g_shadowEnqueue)
        return g_shadowMeshPassCount(self, edx);

    void* const mesh = *(void**)((BYTE*)self + kGraphicsMeshResourceOffset);
    if (!mesh) return g_shadowMeshPassCount(self, edx);
    const unsigned state = *(const unsigned*)((const BYTE*)mesh
                                              + kResourceLoadedStateOffset);
    if (!shouldDeferShadowMesh(state))
        return g_shadowMeshPassCount(self, edx);

    bool enqueued = false;
    bool failed = false;
    if (state == 0
        && !*(void* const*)((const BYTE*)mesh + kResourceInQueueOffset)) {
        void* const loader = g_resourceLoaderAccessor(mesh, nullptr);
        if (loader) {
            // Same verified stock preload tuple used by the alpha-base gate:
            // priority 1, notify=true, immediate=false.
            g_shadowEnqueue(loader, nullptr, mesh, 1, 1, 0);
            const unsigned after = *(const unsigned*)((const BYTE*)mesh
                                                       + kResourceLoadedStateOffset);
            enqueued = after != 0
                || *(void* const*)((const BYTE*)mesh
                                    + kResourceInQueueOffset);
        }
        failed = !enqueued;
    }
    countDeferredShadowMesh(state, enqueued, failed);
    // The verified stock null-mesh arm returns the same value. At this point
    // no caster/pass record, material dependency, or draw has been built.
    return 0;
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

void __fastcall hookTerrainRtLoadTextures(void* self, void* edx) {
    if (!g_terrainRtLoadTextures) return;
    const int64_t started = g_terrainTracing ? tq::probe::now() : 0;
    g_terrainRtLoadTextures(self, edx);
    if (g_terrainTracing) {
        tq::probe::engineCount(tq::probe::CounterEngineTerrainRtLoadTextures);
        tq::probe::engineCount(
            tq::probe::CounterEngineTerrainRtLoadTexturesUs,
            tq::probe::microsecondsSince(started));
        // Record completion: only after this call do TerrainType's base, bump
        // and grass Resource pointers exist for semantic PreLoad(true).
        rememberTerrainRtEvent(self, TerrainRtLoadTextures);
    }
    if (g_terrainPreloadLayersActive && g_terrainPreloadEntry)
        g_terrainPreloadEntry(self, nullptr, 1);
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

void __fastcall hookTerrainPlugRender(
    void* self, void* edx, const void* a, const void* b, const void* c,
    const void* d) {
    if (!g_terrainPlugRender) return;
    countAdmissionRenderable(GpuChunkTerrainPlug, self);
    SecondaryAdmissionDrawScope secondaryAdmission(
        GpuChunkTerrainPlug, self);
    GpuChunkRenderableCallScope terrainCall(GpuChunkTerrainPlug, self);
    const int64_t started = tq::probe::now();
    g_terrainPlugRender(self, edx, a, b, c, d);
    const unsigned elapsed = tq::probe::microsecondsSince(started);
    terrainCall.finish(elapsed);
    tq::probe::engineCount(tq::probe::CounterEngineTerrainPlug);
    tq::probe::engineCount(tq::probe::CounterEngineTerrainPlugUs,
                           elapsed);
}

void __fastcall hookTerrainBlockRender(
    void* self, void* edx, const void* a, const void* b, const void* c,
    const void* d) {
    if (!g_terrainBlockRender) return;
    countAdmissionRenderable(GpuChunkTerrainBlock, self);
    SecondaryAdmissionDrawScope secondaryAdmission(
        GpuChunkTerrainBlock, self);
    GpuChunkRenderableCallScope terrainCall(GpuChunkTerrainBlock, self);
    const int64_t started = tq::probe::now();
    g_terrainBlockRender(self, edx, a, b, c, d);
    const unsigned elapsed = tq::probe::microsecondsSince(started);
    terrainCall.finish(elapsed);
    tq::probe::engineCount(tq::probe::CounterEngineTerrainBlock);
    tq::probe::engineCount(tq::probe::CounterEngineTerrainBlockUs,
                           elapsed);
}

void __fastcall hookGraphicsMeshInstanceRenderPass(
    void* self, void* edx, const void* pass, const void* name, void* canvas,
    const void* sceneRenderer) {
    if (!g_graphicsMeshInstanceRenderPass) return;
    countAdmissionRenderable(GpuChunkMeshInstance, self);
    if (g_reflectionAdmissionRenderActive && onMainThread()) {
        tq::probe::engineCount(
            tq::probe::CounterEngineReflectionAdmissionMeshDeferred);
        return;
    }
    SecondaryAdmissionDrawScope secondaryAdmission(
        GpuChunkMeshInstance, self);
    GpuChunkRenderableCallScope renderableCall(GpuChunkMeshInstance, self);
    if (!renderableCall.call) {
        g_graphicsMeshInstanceRenderPass(
            self, edx, pass, name, canvas, sceneRenderer);
        return;
    }
    const int64_t started = tq::probe::now();
    g_graphicsMeshInstanceRenderPass(
        self, edx, pass, name, canvas, sceneRenderer);
    renderableCall.finish(tq::probe::microsecondsSince(started));
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
    const bool bracketDirectional = g_shadowTracing || g_shadowDeferActive
                                 || g_crossPassTracing
                                 || g_secondaryPassAdmissionActive;
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
    if (g_outsideDirResourceTracing)
        InterlockedIncrement(&g_insideEngineRender);
    g_engineRender(self, edx);
    if (g_outsideDirResourceTracing)
        InterlockedDecrement(&g_insideEngineRender);
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
    void* const passCount = resolve(engine, kShadowMeshPassCountName,
                                    kShadowMeshPassCountRva);
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
        && enqueue && preload && ensure && passCount && materialOwner
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
               engine, passCount,
               signature(kShadowMeshPassCountBytes,
                         sizeof(kShadowMeshPassCountBytes)))
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

bool installReflections(HMODULE engine, bool trace, bool deferAdmissionMesh,
                        bool deferAdmissionAll,
                        bool secondaryPassAdmission) {
    BYTE* const base = (BYTE*)engine;
    void* const manager = resolve(
        engine, kReflectionManagerName, kReflectionManagerRva);
    void* const buildScene = resolve(
        engine, kReflectionBuildSceneName, kReflectionBuildSceneRva);
    void* const renderLight = resolve(
        engine, kReflectionRenderLightName, kReflectionRenderLightRva);
    void* const meshRenderPass = resolve(
        engine, kGraphicsMeshInstanceRenderPassName,
        kGraphicsMeshInstanceRenderPassRva);
    const bool needMesh = trace || deferAdmissionMesh
                       || secondaryPassAdmission;
    const bool verified = manager && buildScene && renderLight
        && (!needMesh || meshRenderPass)
        && tq::detour::matches(
            engine, manager,
            signature(kReflectionManagerBytes,
                      sizeof(kReflectionManagerBytes)))
        && tq::detour::matches(
            engine, base + kReflectionManagerCallWindowRva,
            signature(kReflectionManagerCallWindowBytes,
                      sizeof(kReflectionManagerCallWindowBytes)))
        && tq::detour::matches(
            engine, base + kReflectionManagerTailRva,
            signature(kReflectionManagerTailBytes,
                      sizeof(kReflectionManagerTailBytes)))
        && tq::detour::matches(
            engine, base + kReflectionPlaneRva,
            signature(kReflectionPlaneBytes, sizeof(kReflectionPlaneBytes),
                      kReflectionPlaneRelocs, 1))
        && tq::detour::matches(
            engine, base + kReflectionPlaneCallWindowRva,
            signature(kReflectionPlaneCallWindowBytes,
                      sizeof(kReflectionPlaneCallWindowBytes)))
        && tq::detour::matches(
            engine, base + kReflectionPlaneTailRva,
            signature(kReflectionPlaneTailBytes,
                      sizeof(kReflectionPlaneTailBytes)))
        && tq::detour::matches(
            engine, buildScene,
            signature(kReflectionBuildSceneBytes,
                      sizeof(kReflectionBuildSceneBytes),
                      kReflectionBuildSceneRelocs, 1))
        && tq::detour::matches(
            engine, base + kReflectionBuildSceneCallWindowRva,
            signature(kReflectionBuildSceneCallWindowBytes,
                      sizeof(kReflectionBuildSceneCallWindowBytes)))
        && tq::detour::matches(
            engine, base + kReflectionBuildSceneTailRva,
            signature(kReflectionBuildSceneTailBytes,
                      sizeof(kReflectionBuildSceneTailBytes)))
        && tq::detour::matches(
            engine, renderLight,
            signature(kReflectionRenderLightBytes,
                      sizeof(kReflectionRenderLightBytes),
                      kReflectionRenderLightRelocs, 1))
        && tq::detour::matches(
            engine, base + kReflectionRenderLightCallWindowRva,
            signature(kReflectionRenderLightCallWindowBytes,
                      sizeof(kReflectionRenderLightCallWindowBytes),
                      kReflectionRenderLightCallRelocs, 1))
        && tq::detour::matches(
            engine, base + kReflectionRenderLightTailRva,
            signature(kReflectionRenderLightTailBytes,
                      sizeof(kReflectionRenderLightTailBytes)))
        && (!needMesh || tq::detour::matches(
            engine, meshRenderPass,
            signature(kGraphicsMeshInstanceRenderPassBytes,
                      sizeof(kGraphicsMeshInstanceRenderPassBytes),
                      kGraphicsMeshInstanceRenderPassRelocs, 1)))
        && (!needMesh || tq::detour::matches(
            engine, base + kGraphicsMeshInstanceRenderPassTailRva,
            signature(kGraphicsMeshInstanceRenderPassTailBytes,
                      sizeof(kGraphicsMeshInstanceRenderPassTailBytes))))
        && kReflectionManagerCallWindowBytes[kReflectionManagerCallOffset]
            == 0xe8
        && kReflectionPlaneCallWindowBytes[kReflectionPlaneCallOffset]
            == 0xe8
        && kReflectionBuildSceneCallWindowBytes[
               kReflectionBuildSceneCallOffset] == 0xe8
        && kReflectionRenderLightCallWindowBytes[
               kReflectionRenderLightCallOffset] == 0xe8
        && kReflectionManagerTailBytes[13] == 0xc2
        && kReflectionManagerTailBytes[14] == 2 * sizeof(uintptr_t)
        && kReflectionManagerTailBytes[15] == 0
        && kReflectionPlaneTailBytes[20] == 0xc2
        && kReflectionPlaneTailBytes[21] == 3 * sizeof(uintptr_t)
        && kReflectionPlaneTailBytes[22] == 0
        && kReflectionBuildSceneTailBytes[14] == 0xc2
        && kReflectionBuildSceneTailBytes[15] == sizeof(uintptr_t)
        && kReflectionBuildSceneTailBytes[16] == 0
        && kReflectionRenderLightTailBytes[19] == 0xc2
        && kReflectionRenderLightTailBytes[20] == 4 * sizeof(uintptr_t)
        && kReflectionRenderLightTailBytes[21] == 0
        && kGraphicsMeshInstanceRenderPassTailBytes[20] == 0xc2
        && kGraphicsMeshInstanceRenderPassTailBytes[21]
            == 4 * sizeof(uintptr_t)
        && kGraphicsMeshInstanceRenderPassTailBytes[22] == 0;
    if (!verified) {
        note("DX11 branch reflection windows", false);
        return false;
    }

    g_reflectionManager = (ReflectionManagerFn)manager;
    g_reflectionPlane = (ReflectionPlaneFn)(base + kReflectionPlaneRva);
    g_reflectionBuildScene = (ReflectionBuildSceneFn)buildScene;
    g_reflectionRenderLight = (ReflectionRenderLightFn)renderLight;
    const bool managerOk = !trace || tq::detour::patchCall(
        g_reflectionManagerPatch, engine,
        base + kReflectionManagerCallWindowRva,
        signature(kReflectionManagerCallWindowBytes,
                  sizeof(kReflectionManagerCallWindowBytes)),
        kReflectionManagerCallOffset, manager,
        (const void*)&hookReflectionManager);
    const bool planeOk = managerOk && (!trace || tq::detour::patchCall(
        g_reflectionPlanePatch, engine,
        base + kReflectionPlaneCallWindowRva,
        signature(kReflectionPlaneCallWindowBytes,
                  sizeof(kReflectionPlaneCallWindowBytes)),
        kReflectionPlaneCallOffset, base + kReflectionPlaneRva,
        (const void*)&hookReflectionPlane));
    const bool buildOk = planeOk && tq::detour::patchCall(
        g_reflectionBuildScenePatch, engine,
        base + kReflectionBuildSceneCallWindowRva,
        signature(kReflectionBuildSceneCallWindowBytes,
                  sizeof(kReflectionBuildSceneCallWindowBytes)),
        kReflectionBuildSceneCallOffset, buildScene,
        (const void*)&hookReflectionBuildScene);
    const bool lightOk = buildOk && tq::detour::patchCall(
        g_reflectionRenderLightPatch, engine,
        base + kReflectionRenderLightCallWindowRva,
        signature(kReflectionRenderLightCallWindowBytes,
                  sizeof(kReflectionRenderLightCallWindowBytes),
                  kReflectionRenderLightCallRelocs, 1),
        kReflectionRenderLightCallOffset, renderLight,
        (const void*)&hookReflectionRenderLight);
    const bool meshOk = lightOk && (!needMesh || tq::detour::attach(
        g_graphicsMeshInstanceRenderPassDetour, engine, meshRenderPass,
        signature(kGraphicsMeshInstanceRenderPassBytes,
                  sizeof(kGraphicsMeshInstanceRenderPassBytes),
                  kGraphicsMeshInstanceRenderPassRelocs, 1),
        6, (const void*)&hookGraphicsMeshInstanceRenderPass,
        (void**)&g_graphicsMeshInstanceRenderPass));
    if (!meshOk) {
        tq::detour::detach(g_graphicsMeshInstanceRenderPassDetour);
        tq::detour::restoreCall(g_reflectionRenderLightPatch);
        tq::detour::restoreCall(g_reflectionBuildScenePatch);
        tq::detour::restoreCall(g_reflectionPlanePatch);
        tq::detour::restoreCall(g_reflectionManagerPatch);
        g_reflectionManager = nullptr;
        g_reflectionPlane = nullptr;
        g_reflectionBuildScene = nullptr;
        g_reflectionRenderLight = nullptr;
        g_graphicsMeshInstanceRenderPass = nullptr;
        note("DX11 branch reflection windows", false);
        return false;
    }

    InterlockedExchange(&g_reflectionManagerInvocation, 0);
    InterlockedExchange(&g_reflectionPlaneInvocation, 0);
    g_reflectionManagerFrame = UINT_MAX;
    g_reflectionManagerCallsThisFrame = 0;
    g_reflectionPlaneCallsThisManager = 0;
    g_reflectionChildTracing = trace;
    g_reflectionTracing = trace;
    g_reflectionDeferAdmissionMeshActive = deferAdmissionMesh;
    g_reflectionDeferAdmissionAllActive = deferAdmissionAll;
    g_secondaryPassAdmissionActive = secondaryPassAdmission;
    ++g_installedHooks;
    if (trace) {
        note("DX11 branch reflection windows", true);
        note("reflection BuildScene/RenderLightStyle child calls", true);
        note("GraphicsMeshInstance reflection RenderPass", true);
    }
    tq::hdr::log(
        "Reflection admission mesh defer: %s (buffer threshold %u)\r\n",
        deferAdmissionMesh ? "active" : "off",
        kReflectionAdmissionBufferThreshold);
    tq::hdr::log(
        "Reflection admission whole-pass defer: %s (buffer threshold %u)\r\n",
        deferAdmissionAll ? "active" : "off",
        kReflectionAdmissionBufferThreshold);
    tq::hdr::log(
        "Secondary-pass progressive admission: %s (budget %u objects/frame)\r\n",
        secondaryPassAdmission ? "active" : "off",
        g_secondaryPassAdmissionBudget);
    return true;
}

bool installTerrain(HMODULE engine, bool traceTerrain, bool preloadLayers,
                    bool secondaryPassAdmission) {
    BYTE* const base = (BYTE*)engine;
    const BYTE* const vtable = base + kTerrainRtVtableRva;
    const bool vtableReadable = tq::detour::readable(
        vtable, kTerrainRtLayerTypeVtableOffset + sizeof(DWORD));
    const bool runtimeIdentity = vtableReadable
        && *(void* const*)(vtable + kTerrainRtLoadVtableOffset)
            == base + kTerrainRtLoadRva
        && *(void* const*)(vtable + kTerrainRtLoadRenderDataVtableOffset)
            == base + kTerrainRtLoadRenderDataRva
        && *(void* const*)(vtable + kTerrainRtPreloadVtableOffset)
            == base + kTerrainRtPreloadRva
        && *(void* const*)(vtable + kTerrainRtNumLayersVtableOffset)
            == base + kTerrainRtNumLayersRva
        && *(void* const*)(vtable + kTerrainRtLayerTypeVtableOffset)
            == base + kTerrainRtLayerTypeRva;
    const bool runtimeBytes = runtimeIdentity
        && tq::detour::matches(
            engine, base + kTerrainRtNumLayersRva,
            signature(kTerrainRtNumLayersBytes,
                      sizeof(kTerrainRtNumLayersBytes)))
        && tq::detour::matches(
            engine, base + kTerrainRtLayerTypeRva,
            signature(kTerrainRtLayerTypeBytes,
                      sizeof(kTerrainRtLayerTypeBytes)))
        && tq::detour::matches(
            engine, base + kTerrainPlugShaderWindowRva,
            signature(kTerrainPlugShaderWindowBytes,
                      sizeof(kTerrainPlugShaderWindowBytes),
                      kTerrainPlugShaderWindowRelocs, 1))
        && tq::detour::matches(
            engine, base + kTerrainBlockShaderWindowRva,
            signature(kTerrainBlockShaderWindowBytes,
                      sizeof(kTerrainBlockShaderWindowBytes)));
    if (traceTerrain && runtimeBytes) {
        g_terrainRtNumLayers = (TerrainRtNumLayersFn)(
            base + kTerrainRtNumLayersRva);
        g_terrainRtLayerType = (TerrainRtLayerTypeFn)(
            base + kTerrainRtLayerTypeRva);
    }

    if (traceTerrain && runtimeBytes)
        tq::detour::attach(
            g_terrainRtLoadDetour, engine, base + kTerrainRtLoadRva,
            signature(kTerrainRtLoadBytes, sizeof(kTerrainRtLoadBytes),
                      kTerrainRtLoadRelocs, 1),
            6, (const void*)&hookTerrainRtLoad,
            (void**)&g_terrainRtLoad);

    if (traceTerrain && runtimeBytes)
        tq::detour::attach(
            g_terrainRtLoadRenderDataDetour, engine,
            base + kTerrainRtLoadRenderDataRva,
            signature(kTerrainRtLoadRenderDataBytes,
                      sizeof(kTerrainRtLoadRenderDataBytes),
                      kTerrainRtLoadRenderDataRelocs, 2),
            8, (const void*)&hookTerrainRtLoadRenderData,
            (void**)&g_terrainRtLoadRenderData);

    if (traceTerrain && runtimeBytes)
        tq::detour::attach(
            g_terrainRtPreloadDetour, engine,
            base + kTerrainRtPreloadRva,
            signature(kTerrainRtPreloadBytes,
                      sizeof(kTerrainRtPreloadBytes),
                      kTerrainRtPreloadRelocs, 1),
            6, (const void*)&hookTerrainRtPreload,
            (void**)&g_terrainRtPreload);

    void* const loadTextures = resolve(
        engine, kTerrainLoadTexturesName, kTerrainLoadTexturesRva);
    void* const preloadTarget = resolve(
        engine, kTerrainPreloadName, kTerrainPreloadRva);
    const bool preloadVerified = preloadTarget
        && tq::detour::matches(
            engine, preloadTarget,
            signature(kTerrainPreloadBytes, sizeof(kTerrainPreloadBytes),
                      kTerrainPreloadRelocs, 1));
    const bool needLoadTextures = traceTerrain || preloadLayers;
    g_terrainPreloadEntry = needLoadTextures && preloadVerified
        ? (TerrainPreloadFn)preloadTarget : nullptr;
    g_terrainRtLoadTextures = needLoadTextures
        ? (TerrainTypeLoadTexturesFn)loadTextures : nullptr;
    const bool loadTexturesPatched = !needLoadTextures
        || (loadTextures && preloadVerified
        && (!traceTerrain || runtimeBytes)
        && tq::detour::patchCall(
            g_terrainRtLoadTexturesPatch, engine,
            base + kTerrainRtLoadTexturesWindowRva,
            signature(kTerrainRtLoadTexturesWindowBytes,
                      sizeof(kTerrainRtLoadTexturesWindowBytes)),
            kTerrainRtLoadTexturesCallOffset, loadTextures,
            (const void*)&hookTerrainRtLoadTextures));

    if ((traceTerrain || secondaryPassAdmission) && runtimeBytes)
        tq::detour::attach(
            g_terrainPlugRenderDetour, engine,
            base + kTerrainPlugRenderRva,
            signature(kTerrainPlugRenderBytes,
                      sizeof(kTerrainPlugRenderBytes),
                      kTerrainPlugRenderRelocs, 1),
            6, (const void*)&hookTerrainPlugRender,
            (void**)&g_terrainPlugRender);

    if ((traceTerrain || secondaryPassAdmission) && runtimeBytes)
        tq::detour::attach(
            g_terrainBlockRenderDetour, engine,
            base + kTerrainBlockRenderRva,
            signature(kTerrainBlockRenderBytes,
                      sizeof(kTerrainBlockRenderBytes),
                      kTerrainBlockRenderRelocs, 1),
            6, (const void*)&hookTerrainBlockRender,
            (void**)&g_terrainBlockRender);

    void* target = preloadTarget;
    if (traceTerrain && target)
        tq::detour::attach(
            g_terrainPreloadDetour, engine, target,
            signature(kTerrainPreloadBytes, sizeof(kTerrainPreloadBytes),
                      kTerrainPreloadRelocs, 1),
            6, (const void*)&hookTerrainPreload, (void**)&g_terrainPreload);

    target = traceTerrain
        ? resolve(engine, kTerrainSetShaderParamsName,
                  kTerrainSetShaderParamsRva)
        : nullptr;
    if (traceTerrain && target)
        tq::detour::attach(
            g_terrainSetShaderParamsDetour, engine, target,
            signature(kTerrainSetShaderParamsBytes,
                      sizeof(kTerrainSetShaderParamsBytes),
                      kTerrainSetShaderParamsRelocs, 1),
            8, (const void*)&hookTerrainSetShaderParams,
            (void**)&g_terrainSetShaderParams);

    target = traceTerrain
        ? resolve(engine, kTerrainSetGrassShaderParamsName,
                  kTerrainSetGrassShaderParamsRva)
        : nullptr;
    if (traceTerrain && target)
        tq::detour::attach(
            g_terrainSetGrassShaderParamsDetour, engine, target,
            signature(kTerrainSetGrassShaderParamsBytes,
                      sizeof(kTerrainSetGrassShaderParamsBytes),
                      kTerrainSetGrassShaderParamsRelocs, 1),
            8, (const void*)&hookTerrainSetGrassShaderParams,
            (void**)&g_terrainSetGrassShaderParams);

    target = traceTerrain
        ? resolve(engine, kTerrainRenderGroundName,
                  kTerrainRenderGroundRva)
        : nullptr;
    if (traceTerrain && target)
        tq::detour::attach(
            g_terrainRenderGroundDetour, engine, target,
            signature(kTerrainRenderGroundBytes,
                      sizeof(kTerrainRenderGroundBytes),
                      kTerrainRenderGroundRelocs, 1),
            6, (const void*)&hookTerrainRenderGround,
            (void**)&g_terrainRenderGround);

    const bool traceOk = !traceTerrain || (runtimeBytes && g_terrainRtLoad
        && g_terrainRtLoadRenderData && g_terrainRtPreload
        && loadTexturesPatched && g_terrainPlugRender && g_terrainBlockRender
        && g_terrainPreload && g_terrainSetShaderParams
        && g_terrainSetGrassShaderParams && g_terrainRenderGround);
    const bool preloadOk = !preloadLayers
        || (loadTexturesPatched && preloadVerified);
    const bool secondaryOk = !secondaryPassAdmission
        || (runtimeBytes && g_terrainPlugRender && g_terrainBlockRender);
    const bool ok = traceOk && preloadOk && secondaryOk;
    if (!ok) {
        tq::detour::detach(g_terrainRenderGroundDetour);
        tq::detour::detach(g_terrainSetGrassShaderParamsDetour);
        tq::detour::detach(g_terrainSetShaderParamsDetour);
        tq::detour::detach(g_terrainPreloadDetour);
        tq::detour::detach(g_terrainBlockRenderDetour);
        tq::detour::detach(g_terrainPlugRenderDetour);
        tq::detour::restoreCall(g_terrainRtLoadTexturesPatch);
        tq::detour::detach(g_terrainRtPreloadDetour);
        tq::detour::detach(g_terrainRtLoadRenderDataDetour);
        tq::detour::detach(g_terrainRtLoadDetour);
        g_terrainRenderGround = nullptr;
        g_terrainSetGrassShaderParams = nullptr;
        g_terrainSetShaderParams = nullptr;
        g_terrainPreload = nullptr;
        g_terrainPreloadEntry = nullptr;
        g_terrainBlockRender = nullptr;
        g_terrainPlugRender = nullptr;
        g_terrainRtLoadTextures = nullptr;
        g_terrainRtPreload = nullptr;
        g_terrainRtLoadRenderData = nullptr;
        g_terrainRtLoad = nullptr;
        g_terrainRtLayerType = nullptr;
        g_terrainRtNumLayers = nullptr;
    }
    g_terrainTracing = ok && traceTerrain;
    g_terrainPreloadLayersActive = ok && preloadLayers;
    if (traceTerrain) {
        note("TerrainRT::Load", ok);
        note("TerrainRT::LoadRenderData", ok);
        note("TerrainRT::LoadRenderData -> TerrainType::LoadTextures", ok);
        note("TerrainRT::PreLoad", ok);
        note("TerrainPlug colour render", ok);
        note("TerrainBlock colour render", ok);
        note("TerrainType::PreLoad", ok);
        note("TerrainType::SetShaderParams", ok);
        note("TerrainType::SetGrassShaderParams", ok);
        note("TerrainRenderInterfaceRT::RenderGround", ok);
    } else {
        note("Terrain layer semantic preload", ok);
        if (secondaryPassAdmission)
            note("Terrain secondary-pass progressive admission", ok);
    }
    return ok;
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
    bool ok = tq::detour::patchCall(
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
    if (ok && (g_shadowDeferColdAlpha || g_shadowDeferColdActorPose)) {
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
        void* const passCount = resolve(engine, kShadowMeshPassCountName,
                                        kShadowMeshPassCountRva);
        const bool meshOk = bumpOk && passCount
            && tq::detour::attach(
                g_shadowMeshPassCountDetour, engine, passCount,
                signature(kShadowMeshPassCountBytes,
                          sizeof(kShadowMeshPassCountBytes)),
                6, (const void*)&hookShadowMeshPassCount,
                (void**)&g_shadowMeshPassCount);
        const bool deferOk = recordOk && contextOk && filterOk && bumpOk
            && meshOk;
        if (!deferOk) {
            tq::detour::detach(g_shadowMeshPassCountDetour);
            g_shadowMeshPassCount = nullptr;
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
        note("cold root-mesh caster deferral", deferOk && meshOk);
    }
    if (ok && g_shadowDeferColdActorPose) {
        void* const updateMesh = resolve(engine, kActorUpdateMeshInstanceName,
                                         kActorUpdateMeshInstanceRva);
        const bool updateMeshVerified = updateMesh
            && tq::detour::matches(
                engine, updateMesh,
                signature(kActorUpdateMeshInstanceBytes,
                          sizeof(kActorUpdateMeshInstanceBytes)));
        g_actorUpdateMeshInstance = updateMeshVerified
            ? (ActorUpdateMeshInstanceFn)updateMesh : nullptr;
        const bool actorPoseOk = g_shadowDeferActive && updateMeshVerified
            && tq::detour::patchCall(
                g_shadowActorUpdateMeshPatch, engine,
                (BYTE*)engine + kActorAddToSceneUpdateMeshWindowRva,
                signature(kActorAddToSceneUpdateMeshWindowBytes,
                          sizeof(kActorAddToSceneUpdateMeshWindowBytes)),
                kActorAddToSceneUpdateMeshCallOffset, updateMesh,
                (const void*)&hookShadowActorUpdateMeshInstance);
        if (!actorPoseOk) g_actorUpdateMeshInstance = nullptr;
        g_shadowActorPoseDeferActive = actorPoseOk;
        note("Actor::AddToScene cold directional pose deferral", actorPoseOk);
    }
    if (ok && trace && g_resourceStateVerified) {
        void* const ensure = resolve(engine, kEnsureAvailableName,
                                     kEnsureAvailableRva);
        g_ensureAvailable = (EnsureAvailableFn)ensure;
        bool meshOk = g_shadowDeferActive
            && g_shadowMeshPassCountDetour.installed;
        if (!meshOk) {
            void* const owner = resolve(engine, kShadowMeshPassCountName,
                                        kShadowMeshPassCountRva);
            meshOk = owner && ensure && tq::detour::patchCall(
                g_shadowMeshEnsurePatch, engine, owner,
                signature(kShadowMeshPassCountBytes,
                          sizeof(kShadowMeshPassCountBytes)),
                kShadowMeshEnsureCallOffset, ensure,
                (const void*)&hookShadowMeshEnsure);
        }
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

void noteSecondaryAdmissionDrawSkipped() {
    tq::probe::engineCount(
        tq::probe::CounterEngineSecondaryAdmissionDrawSkipped);
}

void secondaryAdmissionFrameBoundary() {
    if (!g_secondaryPassAdmissionActive) return;
    ++g_secondaryAdmissionFrameSerial;
}

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
    return g_reflectionDeferAdmissionMesh || g_reflectionDeferAdmissionAll;
}

bool secondaryPassAdmissionRequested() {
    return g_secondaryPassAdmissionBudget != 0;
}

void setSecondaryAdmissionDrawHooksReady(bool ready) {
    g_secondaryAdmissionDrawHooksReady = ready;
}

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
    // Moves the same cold-root decision earlier for the exact
    // Actor::AddToScene -> Actor::UpdateMeshInstance call proved by Run 68.
    // Enabling it implies the later complete shadow-defer patch set, because
    // the early skip is safe only when the cold renderable is then omitted.
    g_shadowDeferColdActorPose = iniPath && iniPath[0]
        && GetPrivateProfileIntW(L"performance",
                                 L"shadow_defer_cold_actor_pose", 0,
                                 iniPath) != 0;
    // Runtime TerrainRT creates layer texture Resources but omits the stock
    // TerrainType semantic preload that would queue them. This fix invokes
    // that stock non-blocking path at the exact post-LoadTextures boundary.
    g_terrainPreloadLayers = iniPath && iniPath[0]
        && GetPrivateProfileIntW(L"performance", L"terrain_preload_layers", 0,
                                 iniPath) != 0;
    // A transition-sized reflection scene is identified by an exact count of
    // successful D3D buffer creations during BuildScene, not by elapsed time.
    // The immediately following mesh-instance reflection calls are omitted;
    // normal color later in the same branch and the next reflection are stock.
    g_reflectionDeferAdmissionMesh = iniPath && iniPath[0]
        && GetPrivateProfileIntW(L"performance",
                                 L"reflection_defer_admission_mesh", 0,
                                 iniPath) != 0;
    // Run 81 proved that omitting all 87 GraphicsMeshInstance calls left the
    // marked burst and its terrain-only reflection GPU interval intact.  This
    // stronger A/B skips that one whole RenderLightStyle call, so terrain and
    // mesh reflection both return together on the following frame.
    g_reflectionDeferAdmissionAll = iniPath && iniPath[0]
        && GetPrivateProfileIntW(L"performance",
                                 L"reflection_defer_admission_all", 0,
                                 iniPath) != 0;
    // Zero is stock. A positive value is an object-identity budget, not a
    // millisecond target, so the same recorded population is treated the same
    // on native Windows and Wine. Values above the audited bound are refused
    // rather than silently turning this into effectively unbounded admission.
    const int secondaryBudget = iniPath && iniPath[0]
        ? GetPrivateProfileIntW(L"performance",
                                L"secondary_pass_admission_budget", 0,
                                iniPath)
        : 0;
    g_secondaryPassAdmissionBudget = secondaryBudget > 0
        && secondaryBudget <= (int)kSecondaryPassAdmissionBudgetMax
        ? (unsigned)secondaryBudget : 0u;
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
    // archive_cache_mb, async_level_load, shadow_transition_reuse,
    // shadow_defer_cold_alpha, shadow_defer_cold_actor_pose,
    // terrain_preload_layers, reflection_defer_admission_mesh,
    // reflection_defer_admission_all, and secondary_pass_admission_budget add
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
    const bool shadowActorPose = g_shadowDeferColdActorPose;
    // The earlier Actor boundary depends on the exact later root-caster gate;
    // requesting it therefore installs the same complete accepted patch set.
    const bool shadowDefer = g_shadowDeferColdAlpha || shadowActorPose;
    const bool terrainPreload = g_terrainPreloadLayers;
    const bool secondaryAdmission = g_secondaryPassAdmissionBudget != 0;
    const bool reflectionDefer = g_reflectionDeferAdmissionMesh
                              || g_reflectionDeferAdmissionAll
                              || secondaryAdmission;
    const bool marker = tq::probe::stutterMarkerEnabled();
    decideTracing();
    const bool crossPass = wants(kGroupReflections)
                        && tq::probe::drawTimingEnabled();
    const bool gpuChunks = wants(kGroupReflections) && wants(kGroupTerrain)
                        && tq::probe::drawTimingEnabled();
    if (!g_tracing && !cache && !async && !pumpFilter && !shadowReuse
        && !shadowDefer && !terrainPreload && !reflectionDefer
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
    g_terrainPreloadLayersActive = false;
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
    g_secondaryPassAdmissionActive = false;
    g_secondaryAdmissionArmed = false;
    g_insideReflectionRenderLight = false;
    g_secondaryAdmissionFrameSerial = 0;
    g_secondaryAdmissionBudgetFrame = UINT_MAX;
    g_secondaryAdmissionUsedThisFrame = 0;
    InterlockedExchange(&detail::secondaryAdmissionDrawSuppressDepth, 0);
    g_reflectionAdmissionBuildActive = false;
    g_reflectionAdmissionBuildBuffers = 0;
    g_reflectionAdmissionPending = false;
    g_reflectionAdmissionRenderActive = false;
    memset(g_admissionRenderableIdentities, 0,
           sizeof(g_admissionRenderableIdentities));
    g_crossPassTracing = false;
    g_gpuChunkTracing = false;
    InterlockedExchange(&detail::gpuChunkDrawActive, 0);
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
    g_shadowActorPoseDeferActive = false;
    g_actorUpdateMeshInstance = nullptr;
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
    const bool traceTerrain = wants(kGroupTerrain);
    bool terrainReady = !secondaryAdmission;
    if (traceTerrain || terrainPreload || secondaryAdmission)
        terrainReady = installTerrain(engine, traceTerrain, terrainPreload,
                                      secondaryAdmission);
    const bool traceShadow = wants(kGroupShadow);
    bool shadowReady = !secondaryAdmission;
    if (traceShadow || shadowReuse || shadowDeferReady || crossPass
        || secondaryAdmission)
        shadowReady = installShadow(engine, traceShadow);
    if (wants(kGroupDeferredPasses) || crossPass)
        installDeferredPasses(engine);
    const bool traceReflections = wants(kGroupReflections);
    bool reflectionReady = !secondaryAdmission;
    if (traceReflections || reflectionDefer)
        reflectionReady = installReflections(
            engine, traceReflections, g_reflectionDeferAdmissionMesh,
            g_reflectionDeferAdmissionAll, secondaryAdmission);
    g_secondaryPassAdmissionActive = secondaryAdmission
        && g_secondaryAdmissionDrawHooksReady && terrainReady
        && shadowReady && reflectionReady;
    if (secondaryAdmission && !g_secondaryPassAdmissionActive)
        tq::hdr::log("Secondary-pass progressive admission unavailable --"
                     " leaving all draws stock\r\n");
    if (async) installAsyncLoad(engine);
    g_crossPassTracing = crossPass && g_renderDirectional
        && g_deferredPassTracing && g_reflectionTracing
        && g_reflectionChildTracing;
    tq::hdr::log("Engine trace: cross-pass first-use identity %s\r\n",
                 g_crossPassTracing ? "active" : "unavailable");
    g_gpuChunkTracing = gpuChunks && g_reflectionTracing
        && g_reflectionChildTracing && g_terrainPlugRender
        && g_terrainBlockRender && g_graphicsMeshInstanceRenderPass;
    tq::hdr::log("Engine trace: complete reflection GPU chunks %s\r\n",
                 g_gpuChunkTracing ? "active" : "unavailable");

    // These columns and marker records mean exactly "main-thread
    // LoadResource outside RenderDirectional, partitioned by Engine phase."
    // Refuse the diagnostic unless every bracket and both Resource accessors
    // are live; a missing hook must produce zeros rather than a false class.
    g_outsideDirResourceTracing = g_loadResource && g_engineUpdate
        && g_engineRender && g_shadowTracing && g_resourceStateVerified
        && g_resourceFileNameVerified;
    tq::hdr::log("Engine trace: outside-directional Resource attribution %s"
                 "\r\n",
                 g_outsideDirResourceTracing ? "active" : "unavailable");
    g_shadowMeshResourceTracing = g_loadResource && g_shadowTracing
        && g_resourceStateVerified && g_resourceFileNameVerified;
    tq::hdr::log("Engine trace: directional cold-mesh retention %s\r\n",
                 g_shadowMeshResourceTracing ? "active" : "unavailable");

    tq::hdr::log("Engine trace: %s, mask=0x%x, cache %s, async load %s,"
                 " pump timer floor %u ms, shadow transition reuse %s,"
                 " cold alpha-shadow defer %s, cold actor-pose defer %s,"
                 " terrain layer preload %s, reflection admission mesh defer %s,"
                 " reflection admission whole-pass defer %s,"
                 " secondary-pass admission budget %u,"
                 " hooks=%u, main thread id at %p\r\n",
                 g_tracing ? "on" : "off", g_traceMask,
                 cache ? "requested" : "off", async ? "requested" : "off",
                 g_pumpTimerMinMs, shadowReuse ? "requested" : "off",
                 shadowDefer ? "requested" : "off",
                 shadowActorPose ? "requested" : "off",
                 terrainPreload ? "requested" : "off",
                 g_reflectionDeferAdmissionMesh ? "requested" : "off",
                 g_reflectionDeferAdmissionAll ? "requested" : "off",
                 g_secondaryPassAdmissionBudget,
                 g_installedHooks,
                 (const void*)g_mainThreadId);
    if (g_installedHooks) return true;
    InterlockedExchange(&g_installed, 0);
    return false;
}

void shutdown() {
    // Stop classification before removing any one of the three brackets it
    // depends on. The game does not normally unload us, but explicit teardown
    // must never turn a missing hook into an "outside directional" sample.
    g_outsideDirResourceTracing = false;
    g_shadowMeshResourceTracing = false;
    g_deferredPassTracing = false;
    g_reflectionTracing = false;
    g_reflectionChildTracing = false;
    g_reflectionDeferAdmissionMeshActive = false;
    g_reflectionDeferAdmissionAllActive = false;
    g_secondaryPassAdmissionActive = false;
    g_secondaryAdmissionArmed = false;
    g_secondaryAdmissionDrawHooksReady = false;
    g_insideReflectionRenderLight = false;
    InterlockedExchange(&detail::secondaryAdmissionDrawSuppressDepth, 0);
    g_reflectionAdmissionBuildActive = false;
    g_reflectionAdmissionBuildBuffers = 0;
    g_reflectionAdmissionPending = false;
    g_reflectionAdmissionRenderActive = false;
    g_crossPassTracing = false;
    g_gpuChunkTracing = false;
    InterlockedExchange(&detail::gpuChunkDrawActive, 0);
    g_activeGpuChunkRenderableCall = nullptr;
    InterlockedExchange(&g_deferredPass, DeferredPassNone);
    InterlockedExchange(&g_deferredGeometrySite, DeferredGeometrySiteNone);
    InterlockedExchange(&g_deferredOwnerInvocation, 0);
    InterlockedExchange(&g_reflectionManagerInvocation, 0);
    InterlockedExchange(&g_reflectionPlaneInvocation, 0);
    InterlockedExchange(&g_reflectionChild, 0);
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
    tq::detour::detach(g_graphicsMeshInstanceRenderPassDetour);
    tq::detour::restoreCall(g_reflectionRenderLightPatch);
    tq::detour::restoreCall(g_reflectionBuildScenePatch);
    tq::detour::restoreCall(g_reflectionPlanePatch);
    tq::detour::restoreCall(g_reflectionManagerPatch);
    g_reflectionPlane = nullptr;
    g_reflectionManager = nullptr;
    g_reflectionBuildScene = nullptr;
    g_reflectionRenderLight = nullptr;
    g_graphicsMeshInstanceRenderPass = nullptr;
    g_reflectionManagerFrame = UINT_MAX;
    g_reflectionManagerCallsThisFrame = 0;
    g_reflectionPlaneCallsThisManager = 0;
    for (int i = (int)kDeferredCallSiteCount - 1; i >= 0; --i)
        tq::detour::restoreCall(g_deferredCallPatches[i]);
    tq::detour::restoreCall(g_deferredOwnerPatch);
    g_deferredRender = nullptr;
    g_deferredGeometrySetup = nullptr;
    g_deferredGeometryScene = nullptr;
    g_deferredShadows = nullptr;
    g_deferredLighting = nullptr;
    g_deferredResolve = nullptr;
    g_deferredAo = nullptr;
    g_deferredLateSceneA = nullptr;
    g_deferredLateSceneB = nullptr;
    g_deferredLateSceneList = nullptr;
    g_deferredPostHighlight = nullptr;
    g_deferredPostFog = nullptr;
    g_deferredPostMask = nullptr;
    g_deferredPostComposite = nullptr;
    g_deferredPostDebug = nullptr;
    g_deferredOwnerFrame = UINT_MAX;
    g_deferredOwnerCallsThisFrame = 0;
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
    g_backgroundLoadLevel = nullptr;
    g_regionLoadLevel = nullptr;
    tq::detour::restoreCall(g_shadowActorUpdateMeshPatch);
    g_actorUpdateMeshInstance = nullptr;
    g_shadowActorPoseDeferActive = false;
    tq::detour::restoreCall(g_shadowMeshEnsurePatch);
    tq::detour::detach(g_shadowMeshPassCountDetour);
    g_shadowMeshPassCount = nullptr;
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
    InterlockedExchange(&g_insideEngineUpdate, 0);
    InterlockedExchange(&g_insideEngineRender, 0);
    tq::detour::detach(g_terrainRenderGroundDetour);
    tq::detour::detach(g_terrainSetGrassShaderParamsDetour);
    tq::detour::detach(g_terrainSetShaderParamsDetour);
    tq::detour::detach(g_terrainPreloadDetour);
    tq::detour::detach(g_terrainBlockRenderDetour);
    tq::detour::detach(g_terrainPlugRenderDetour);
    tq::detour::restoreCall(g_terrainRtLoadTexturesPatch);
    tq::detour::detach(g_terrainRtPreloadDetour);
    tq::detour::detach(g_terrainRtLoadRenderDataDetour);
    tq::detour::detach(g_terrainRtLoadDetour);
    g_terrainRenderGround = nullptr;
    g_terrainSetGrassShaderParams = nullptr;
    g_terrainSetShaderParams = nullptr;
    g_terrainPreload = nullptr;
    g_terrainPreloadEntry = nullptr;
    g_terrainBlockRender = nullptr;
    g_terrainPlugRender = nullptr;
    g_terrainRtLoadTextures = nullptr;
    g_terrainRtPreload = nullptr;
    g_terrainRtLoadRenderData = nullptr;
    g_terrainRtLoad = nullptr;
    g_terrainRtLayerType = nullptr;
    g_terrainRtNumLayers = nullptr;
    g_terrainTracing = false;
    g_terrainPreloadLayersActive = false;
    g_activeTerrainType = nullptr;
    g_activeTerrainThread = 0;
    g_activeTerrainPath = TerrainParameterNone;
    g_activeTerrainMaterialIndex = -1;
    memset(g_terrainPreloadStates, 0, sizeof(g_terrainPreloadStates));
    memset(g_outsideDirResourceReports, 0,
           sizeof(g_outsideDirResourceReports));
    InterlockedExchange(&g_outsideDirResourceSequence, 0);
    InterlockedExchange(&g_outsideDirResourceReportedThrough, 0);
    memset(g_shadowMeshResourceReports, 0,
           sizeof(g_shadowMeshResourceReports));
    InterlockedExchange(&g_shadowMeshResourceSequence, 0);
    InterlockedExchange(&g_shadowMeshResourceReportedThrough, 0);
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
    return (g_reflectionDeferAdmissionMesh || g_reflectionDeferAdmissionAll)
        && reflectionAdmissionThresholdReached(buffers);
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
    InterlockedExchange(&detail::secondaryAdmissionDrawSuppressDepth, 0);
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
    InterlockedExchange(&detail::gpuChunkDrawActive, 0);
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
#endif

}  // namespace engineprobe
}  // namespace tq
