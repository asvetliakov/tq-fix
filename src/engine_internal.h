#pragma once

// Private contracts between audited Engine hooks, performance features and
// optional observers. Not an application-facing API. Site bytes remain
// verbatim and are checked by research/streaming/tools/verify-sites.py.
#include "engine.h"
#include "engine_probe.h"
#include "secondary_admission.h"
#include "arc_cache.h"
#include "detour.h"
#include "hdr.h"
#include "probe.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace tq { namespace engine { namespace detail {
const unsigned kRegionLevelOffset = 0x50;

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
const DWORD kShadowMaterialTextureWindowRva = 0x169ca8;
const unsigned kShadowMaterialTextureCallOffset = 3;
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
const DWORD kShadowMeshParameterCallRva = 0x17385e;
const unsigned kShadowMeshParameterCallOffset = 14;
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
// the cold-resource fix: priority 1, notify=true, immediate=false. This window is
// verified as behaviour, not patched.
const DWORD kPreloadResourceRva = 0x1200e0;
const char kPreloadResourceName[] =
    "?PreLoadResource@BaseResourceManager@GAME@@QAEXPBVResource@2@@Z";
const DWORD kPreloadEnqueueWindowRva = 0x120110;
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

// Offsets the four windows above establish, named once so the code that reads
// them says where each came from.
const unsigned kArchiveEntryTableOffset = 0x2c;
const unsigned kArchiveHandleOffset = 0xc;
const unsigned kArchiveBlockSizeOffset = 0x40;
const unsigned kArchiveEntryStride = 0x44;
const unsigned kArchiveEntryDescriptorsOffset = 0x20;
const unsigned kArchiveDescriptorStride = 0xc;

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
int __fastcall hookAddElementsLoadLevel(void* self, void* edx, int background);
int __fastcall hookPortalLoadLevel(void* self, void* edx, int background);
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
const unsigned kSweepCount = 7;

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
const DWORD kGameHekImageSize = 0x0059c000;
const DWORD kGameHekWrapperRva = 0x59a035;
const DWORD kGameHekTrampolineRva = 0x59a067;
// Exact supplied HekTo wrapper. Its post-update callback remains owned by
// that modification; the writable callback slot must never be overwritten.
const BYTE kGameHekUpdateBytes[] = {
    0xe9, 0x00, 0xfe, 0x3f, 0x00, 0x90,
    0x6a, 0xff, 0x68, 0, 0, 0, 0,
    0x64, 0xa1, 0x00, 0x00, 0x00, 0x00,
    0x50, 0x81, 0xec, 0x70, 0x04
};
const BYTE kGameHekWrapperBytes[] = {
    0x55, 0x8b, 0xec, 0xff, 0x75, 0x08,
    0xe8, 0x27, 0x00, 0x00, 0x00,
    0xe8, 0x00, 0x00, 0x00, 0x00, 0x58,
    0x8d, 0x80, 0xc7, 0x0f, 0x00, 0x00, 0x8b, 0x00,
    0x85, 0xc0, 0x74, 0x06, 0x0f, 0x1f, 0x40, 0x00,
    0xff, 0xd0, 0x5d, 0xc2, 0x04, 0x00
};
const BYTE kGameHekTrampolineBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,
    0xe9, 0xc4, 0x01, 0xc0, 0xff
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
extern unsigned g_traceMask;

extern LONG g_pumpLastFullTick;
    // GetTickCount of the last unfiltered peek
constexpr bool g_asyncLevelLoad = false;
constexpr bool g_shadowTransitionReuse = false;
extern bool g_shadowDeferColdResources;

extern bool g_shadowDeferActive;

extern bool g_shadowDeferColdActorPose;

extern bool g_shadowActorPoseDeferActive;

extern bool g_terrainPreloadLayers;

extern bool g_terrainPreloadLayersActive;

extern bool g_terrainTracing;

// The rejected reflection-omission experiments stay disabled. Their trace
// fields retain their positions so archived CSVs remain schema-compatible.
const unsigned kReflectionAdmissionBufferThreshold = 32;
const unsigned kSecondaryPassAdmissionBudgetMax = 64;
extern bool g_reflectionDeferAdmissionMeshActive;

extern bool g_reflectionDeferAdmissionAllActive;

extern unsigned g_secondaryPassAdmissionBudget;

extern bool g_secondaryPassAdmissionActive;

extern bool g_secondaryAdmissionArmed;

extern bool g_secondaryAdmissionDrawHooksReady;

extern bool g_insideReflectionRenderLight;

extern unsigned g_secondaryAdmissionFrameSerial;

extern unsigned g_secondaryAdmissionBudgetFrame;

extern unsigned g_secondaryAdmissionUsedThisFrame;

extern bool g_reflectionAdmissionBuildActive;

extern unsigned g_reflectionAdmissionBuildBuffers;

extern bool g_reflectionAdmissionPending;

extern bool g_reflectionAdmissionRenderActive;

extern bool g_tracing;

extern bool g_pumpTracing;

extern bool g_outsideDirResourceTracing;

extern bool g_shadowMeshResourceTracing;

extern unsigned g_installedHooks;

extern const volatile DWORD* g_mainThreadId;

bool onMainThread();


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
extern LoadLevelFn g_loadLevel;

extern LoadLevelFn g_regionLoadLevel;

extern BackgroundLoadLevelFn g_backgroundLoadLevel;

extern GuaranteedGetLevelFn g_guaranteedGetLevel;

extern LoadResourceFn g_loadResource;

extern EnsureAvailableFn g_ensureAvailable;

extern ShadowMeshPassCountFn g_shadowMeshPassCount;

extern ActorUpdateMeshInstanceFn g_actorUpdateMeshInstance;

extern ResourceFileNameFn g_resourceFileName;

extern GraphicsTextureGetTextureFn g_graphicsTextureGetTexture;

extern GraphicsMeshSetShaderParametersFn g_graphicsMeshSetShaderParameters;

extern SetTextureParameterFn g_setTextureParameter;

extern ShaderHasParameterFn g_shaderHasParameter;

extern BuildShadowRecordFn g_buildShadowRecord;

extern MeshShadowStyleFn g_meshShadowStyle;

extern MeshGetTextureFn g_meshGetTexture;

extern ResourceLoaderAccessorFn g_resourceLoaderAccessor;

extern EnqueueFn g_shadowEnqueue;

extern RenderDirectionalFn g_renderDirectional;

extern TerrainPreloadFn g_terrainPreload;

extern TerrainPreloadFn g_terrainPreloadEntry;

extern TerrainSetShaderParamsFn g_terrainSetShaderParams;

extern TerrainSetGrassShaderParamsFn g_terrainSetGrassShaderParams;

extern TerrainRenderGroundFn g_terrainRenderGround;

extern TerrainRtLoadFn g_terrainRtLoad;

extern TerrainRtLoadRenderDataFn g_terrainRtLoadRenderData;

extern TerrainRtPreloadFn g_terrainRtPreload;

extern TerrainRtNumLayersFn g_terrainRtNumLayers;

extern TerrainRtLayerTypeFn g_terrainRtLayerType;

extern TerrainTypeLoadTexturesFn g_terrainRtLoadTextures;

extern TerrainColourRenderFn g_terrainPlugRender;

extern TerrainColourRenderFn g_terrainBlockRender;

extern GraphicsMeshInstanceRenderPassFn g_graphicsMeshInstanceRenderPass;

extern DeferredRenderFn g_deferredRender;

extern DeferredFn2 g_deferredGeometrySetup;

extern DeferredFn5 g_deferredGeometryScene;

extern DeferredFn2 g_deferredShadows;

extern DeferredFn2 g_deferredLighting;

extern DeferredFn3 g_deferredResolve;

extern DeferredFn1 g_deferredAo;

extern DeferredFn2 g_deferredLateSceneA;

extern DeferredFn1 g_deferredLateSceneB;

extern DeferredFn5 g_deferredLateSceneList;

extern DeferredFn1 g_deferredPostHighlight;

extern DeferredFn2 g_deferredPostFog;

extern DeferredFn1 g_deferredPostMask;

extern DeferredFn5 g_deferredPostComposite;

extern DeferredFn1 g_deferredPostDebug;

extern ReflectionManagerFn g_reflectionManager;

extern ReflectionPlaneFn g_reflectionPlane;

extern ReflectionBuildSceneFn g_reflectionBuildScene;

extern ReflectionRenderLightFn g_reflectionRenderLight;

extern UnloadLevelFn g_unloadLevel;

extern EnqueueFn g_enqueue;

extern ReadFromFileFn g_readFromFile;

extern ArchiveBlockFn g_archiveBlock;

extern SweepFn g_sweep;

extern EngineUpdateFn g_engineUpdate;

extern EngineRenderFn g_engineRender;

extern GameUpdateFn g_gameUpdate;

extern NewArrayFn g_newArray;

extern DeleteArrayFn g_deleteArray;

extern SetFilePointerExFn g_setFilePointerEx;

extern ReadFileFn g_readFile;

extern WaitFn g_engineWait;

extern WaitMultipleFn g_engineWaitMultiple;

extern CallPatch g_deferredCallPatches[kDeferredCallSiteCount];

extern CallPatch g_deferredOwnerPatch;

extern CallPatch g_reflectionManagerPatch;

extern CallPatch g_reflectionPlanePatch;

extern CallPatch g_reflectionBuildScenePatch;

extern CallPatch g_reflectionRenderLightPatch;

extern bool g_deferredPassTracing;

extern volatile LONG g_deferredPass;

extern volatile LONG g_deferredGeometrySite;

extern volatile LONG g_deferredOwnerInvocation;

extern unsigned g_deferredOwnerFrame;

extern unsigned g_deferredOwnerCallsThisFrame;

extern bool g_reflectionTracing;

extern bool g_gpuChunkTracing;

extern volatile LONG g_reflectionManagerInvocation;

extern volatile LONG g_reflectionPlaneInvocation;

extern volatile LONG g_reflectionChild;

extern unsigned g_reflectionManagerFrame;

extern unsigned g_reflectionManagerCallsThisFrame;

extern unsigned g_reflectionPlaneCallsThisManager;

extern SleepFn g_engineSleep;

extern Detour g_loadLevelDetour;

extern Detour g_guaranteedDetour;

extern Detour g_loadResourceDetour;

extern Detour g_shadowMeshPassCountDetour;

extern Detour g_unloadLevelDetour;

extern Detour g_enqueueDetour;

extern Detour g_readFromFileDetour;

extern Detour g_archiveBlockDetour;

extern Detour g_waitForLoadingDetour;

extern Detour g_engineUpdateDetour;

extern Detour g_engineRenderDetour;

extern Detour g_gameUpdateDetour;

extern Detour g_terrainPreloadDetour;

extern Detour g_terrainSetShaderParamsDetour;

extern Detour g_terrainSetGrassShaderParamsDetour;

extern Detour g_terrainRenderGroundDetour;

extern Detour g_terrainRtLoadDetour;

extern Detour g_terrainRtLoadRenderDataDetour;

extern Detour g_terrainRtPreloadDetour;

extern Detour g_terrainPlugRenderDetour;

extern Detour g_terrainBlockRenderDetour;

extern Detour g_graphicsMeshInstanceRenderPassDetour;

extern CallPatch g_terrainRtLoadTexturesPatch;

extern CallPatch g_newArrayPatch;

extern CallPatch g_deleteArrayPatch;

extern CallPatch g_seekPatch;

extern CallPatch g_readFilePatch;

extern CallPatch g_csPatch;

extern CallPatch g_objWaitPatch;

extern CallPatch g_objWaitMultiplePatch;

extern CallPatch g_enginesleepPatch;

extern CallPatch g_lockPatches[kLockSiteCount];

extern CallPatch g_forceLoadPatches[kForceLoadSiteCount];

extern CallPatch g_fencePatch;

extern CallPatch g_sweepPatches[kSweepCount];

extern CallPatch g_shadowDirectionalPatch;

extern CallPatch g_shadowActorUpdateMeshPatch;

extern CallPatch g_shadowMeshEnsurePatch;

extern CallPatch g_shadowMaterialTexturePatch;

extern CallPatch g_shadowTextureParameterPatch;

extern CallPatch g_shadowMeshParameterPatch;

extern CallPatch g_shadowInstanceBumpEnsurePatch;

extern CallPatch g_shadowRecordPatch;

extern LONG g_insideDirectional;

extern LONG g_insideEngineUpdate;

extern LONG g_insideEngineRender;

extern void* g_lastShadowRegion;

extern void* g_cachedShadowSurface;

extern DWORD g_cachedShadowMatrix[kShadowMatrixDwords];

extern int g_cachedShadowResult;

extern bool g_cachedShadowValid;

extern bool g_reusedLastShadow;

extern bool g_shadowTracing;

extern bool g_crossPassTracing;

extern bool g_reflectionChildTracing;

extern bool g_resourceStateVerified;

extern bool g_resourceFileNameVerified;

extern bool g_shaderHasParameterVerified;

extern bool g_nameHashLayoutVerified;

extern bool g_shadowMaterialTexturePending;

extern bool g_shadowMaterialTextureHooked;

extern bool g_shadowTextureParameterHooked;

extern bool g_shadowMeshParameterHooked;

extern bool g_shadowTextureCallerSitesVerified;

extern bool g_insideShadowMaterialTexture;

extern uint32_t g_shadowMaterialTexturePendingUs;

extern uint32_t g_shadowMaterialPendingNameHash;

extern LONG g_shadowMaterialReports;

extern LONG g_shadowTextureChainReports;


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

struct ReflectionLocation {
    unsigned manager;
    unsigned plane;
    ReflectionCell cell;
};
ReflectionLocation currentReflectionLocation();


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
const unsigned kGpuChunkCount = 16;
const unsigned kGpuChunkEventSlots = 32;
const unsigned kGpuChunkRenderableCallSlots = 256;
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
extern GpuChunkEvent g_gpuChunkEvents[kGpuChunkEventSlots];

extern unsigned g_gpuChunkEventSequence;

extern ActiveGpuChunkEvent g_activeGpuChunks[GpuChunkClassCount];

extern unsigned g_gpuChunkLastFrame[GpuChunkClassCount];

extern bool g_reflectionGpuChunkPending;

extern unsigned g_reflectionGpuChunkTriggerUs;

extern GpuChunkRenderableCall* g_activeGpuChunkRenderableCall;

void armGpuChunks(const ReflectionLocation& reflection, unsigned triggerUs);

void closeGpuChunks();


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

const unsigned kAdmissionRenderableIdentitySlots = 8192;
const unsigned kAdmissionRenderableIdentityProbe = 16;
const unsigned kAdmissionRenderableIdentityHashSalt = 0x9e3779b1;

struct AdmissionRenderableIdentity {
    const void* object;
    unsigned kind;
    unsigned consumerMask;
    unsigned secondaryState;
};
extern AdmissionRenderableIdentity
    g_admissionRenderableIdentities[kAdmissionRenderableIdentitySlots];

bool admissionRenderableFirst(const void* object,
                              GpuChunkRenderableKind kind,
                              AdmissionConsumer consumer);


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
void countSecondaryAdmission(SecondaryAdmissionContext context,
                             bool admitted);

bool shouldDeferSecondaryAdmission(GpuChunkRenderableKind kind,
                                   const void* object);



void countAdmissionRenderable(GpuChunkRenderableKind kind,
                              const void* object);


enum DeferredCreationKind {
    DeferredCreationTexture,
    DeferredCreationBuffer
};

const unsigned kCrossPassBufferSlots = 4096;
const unsigned kCrossPassIndexSlots = 8192;
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
extern CrossPassBufferRecord g_crossPassBuffers[kCrossPassBufferSlots];

extern CrossPassIndexEntry g_crossPassIndex[kCrossPassIndexSlots];

extern unsigned g_crossPassBufferSequence;

extern unsigned g_crossPassIndexOverflows;

extern unsigned g_crossPassRecentEvictions;


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
extern DeferredCreationRecord g_deferredCreations[kDeferredCreationSlots];

extern unsigned g_deferredCreationSequence;


// Run 79's reacted-to play burst contained 51 off-main CreateTexture2D calls,
// but the owner-scoped creation ring deliberately rejected them and therefore
// retained no descriptor, thread, or cross-frame extent. This lock-free ring
// publishes each completed record last. F12 takes a sequence snapshot and
// accepts only slots whose publication sequence still matches, so a loader
// thread can never leave a partially written record looking valid.
const unsigned kOffMainTextureSlots = 512;
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
extern OffMainTextureRecord g_offMainTextures[kOffMainTextureSlots];

extern volatile LONG g_offMainTextureSequence;


const unsigned kDeferredSlowFrameSlots = 128;
const unsigned kDeferredTopDrawsPerFrame = 12;

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
extern DeferredSlowFrame g_deferredSlowFrames[kDeferredSlowFrameSlots];


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
    void* self, void* edx, uintptr_t canvas, uintptr_t renderSet);

uintptr_t __stdcall hookReflectionPlane(
    uintptr_t record, uintptr_t canvas, uintptr_t renderSet);

void __fastcall hookReflectionBuildScene(
    void* self, void* edx, int includeHidden);


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
extern ShadowMeshParameterContext g_shadowMeshParameterContext;

extern ShadowMeshParameterContext g_shadowMaterialPendingContext;

extern const void* g_shadowMaterialPendingTexture;

extern ShadowMeshContextPatchStatus g_shadowMeshContextPatchStatus;

void resetShadowRecordContexts();

bool rememberShadowRecordContext(void* instance, int pass, unsigned style,
                                 bool styleKnown, bool baseKnown,
                                 const void* baseTexture);

bool findShadowRecordContext(void* instance, int pass,
                             ShadowMeshParameterContext* out);

void explainShadowRecordMiss(ShadowMeshParameterContext* context);

extern const BYTE* g_engineBase;

extern unsigned g_chainModuleCount;


struct ChainFrame {
    DWORD rva;
    char tag;
};
void reportSlowLoads();

int deferLoad(void* self, void* edx, int background,
              tq::probe::Counter deferred, tq::probe::Counter fellThrough);

int __fastcall hookAddElementsLoadLevel(void* self, void* edx, int background);

int __fastcall hookPortalLoadLevel(void* self, void* edx, int background);


enum ShadowResourceType {
    ShadowResourceMesh,
    ShadowResourceShader,
    ShadowResourceTexture,
    ShadowResourceOther,
};

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
extern TerrainPreloadState g_terrainPreloadStates[kTerrainPreloadStateSlots];

extern const void* g_activeTerrainType;

extern DWORD g_activeTerrainThread;

extern TerrainParameterPath g_activeTerrainPath;

extern int g_activeTerrainMaterialIndex;

void rememberTerrainRtEvent(const void* terrain, TerrainRtEvent event);


const unsigned kOutsideDirResourceReportSlots = 128;
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
extern OutsideDirResourceReport
    g_outsideDirResourceReports[kOutsideDirResourceReportSlots];

extern LONG g_outsideDirResourceSequence;

extern LONG g_outsideDirResourceReportedThrough;


// The remaining Run 67 play transition has 26 state-0 mesh loads inside the
// directional build after the exact base GraphicsMeshInstance root gate has
// already omitted 12 other casters. Preserve the same bounded, delayed caller
// evidence used for outside-directional loads, but only for that narrow class.
const unsigned kShadowMeshResourceReportSlots = 128;

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
extern ShadowMeshResourceReport
    g_shadowMeshResourceReports[kShadowMeshResourceReportSlots];

extern LONG g_shadowMeshResourceSequence;

extern LONG g_shadowMeshResourceReportedThrough;

void countShadowMeshContextPatchStatus();

void flushPendingShadowMaterialTexture(bool known, bool used);

int __fastcall hookShadowTextureParameter(
    void* shader, void* edx, const void* name, unsigned index,
    void* reserved, void* textureValue);

bool shouldDeferShadowAlpha(unsigned style, unsigned state);

bool shouldDeferShadowMesh(unsigned state);

void countDeferredShadowAlpha(unsigned state, bool enqueued, bool failed);

void countDeferredShadowMesh(unsigned state, bool enqueued, bool failed);

void countDeferredShadowActorPose(unsigned state, bool enqueued, bool failed);

bool shadowActorPoseQueueConfirmed(unsigned state, bool inQueue);

void countShadowActorPoseEnqueueFailure();

void __fastcall hookShadowMeshEnsure(void* resource, void* edx);

int __fastcall hookTerrainRtLoad(void* self, void* edx, void* reader,
                                 int version);

int __fastcall hookTerrainRtLoadRenderData(void* self, void* edx);

void __fastcall hookTerrainRtPreload(void* self, void* edx, int priority,
                                     const void* frustum, unsigned flags);

void __fastcall hookTerrainPlugRender(
    void* self, void* edx, const void* a, const void* b, const void* c,
    const void* d);

void __fastcall hookTerrainBlockRender(
    void* self, void* edx, const void* a, const void* b, const void* c,
    const void* d);

void __fastcall hookTerrainPreload(void* self, void* edx,
                                   int includeTextures);

void __fastcall hookTerrainSetShaderParams(
    const void* self, void* edx, const void* shader, int materialIndex);

void __fastcall hookTerrainSetGrassShaderParams(
    const void* self, void* edx, const void* shader);

void __fastcall hookTerrainRenderGround(
    const void* self, void* edx, const void* name, void* canvas,
    const void* sceneRenderer, const void* pass, int flag);

bool reusePreviousShadow(bool regionChanged, void* surface, void* matrix);

void rememberShadow(void* surface, const void* matrix, int result);

int __fastcall hookReadFromFile(void* self, void* edx, int entry, BYTE* dest,
                                unsigned offset, unsigned size,
                                void* blockBuffer);
   // peeks over 5 ms returning it

typedef UINT_PTR (WINAPI* SetTimerFn)(HWND, UINT_PTR, UINT, TIMERPROC);
extern SetTimerFn g_setTimer;

extern CallPatch g_setTimerPatch;

void reportMessages();


typedef BOOL (WINAPI* GetMessageFn)(LPMSG, HWND, UINT, UINT);
extern SleepFn g_loopSleep;

extern GetMessageFn g_loopGetMessage;

extern WaitFn g_loopWait;

extern CallPatch g_loopSleepPatch;

extern CallPatch g_loopMessagePatch;

extern CallPatch g_loopWaitPatch;


// Both __thiscall, both reached through TQ.exe's import table, both once a
// frame. PresentSurface takes no arguments; the collision fixup takes one
// reference, so it pops four.
typedef void (__fastcall* PresentSurfaceFn)(void* self, void* edx);
typedef void (__fastcall* CollisionsFn)(void* self, void* edx, const void* camera);
extern PresentSurfaceFn g_presentSurface;

extern CollisionsFn g_collisions;

extern CallPatch g_presentSurfacePatch;

extern CallPatch g_collisionsPatch;


// The remaining six, all __thiscall except the platform pump, which is a C
// entry point taking no arguments -- at zero arguments __stdcall and __cdecl
// emit the same epilogue, so the declaration cannot get the cleanup wrong.
// The call site confirms it: no pushes before, no stack adjustment after.
typedef void (__stdcall* PlatformProcessFn)(void);
typedef void (__fastcall* ThisVoidFn)(void* self, void* edx);
typedef void (__fastcall* SoundUpdateFn)(void* self, void* edx,
                                         const void* frustum);
typedef int (__fastcall* PumpFn)(void* self, void* edx);
extern PlatformProcessFn g_platform;

extern ThisVoidFn g_gfxOptions;

extern ThisVoidFn g_jukebox;

extern SoundUpdateFn g_sound;

extern ThisVoidFn g_quests;

extern PumpFn g_pump;

extern CallPatch g_platformPatch, g_gfxOptionsPatch, g_jukeboxPatch;

extern CallPatch g_soundPatch, g_questsPatch, g_pumpPatch;


// The pump, split. Both are USER32 imports of Engine.dll -- not of the
// executable -- because EWindow::ProcessMessages is Engine.dll's code.
typedef BOOL (WINAPI* PeekMessageFn)(LPMSG, HWND, UINT, UINT, UINT);
typedef LRESULT (WINAPI* DispatchMessageFn)(const MSG*);
extern PeekMessageFn g_peekMessage;

extern DispatchMessageFn g_dispatchMessage;

extern CallPatch g_peekPatch, g_dispatchPatch;

void decideTracing();

bool wants(unsigned group);

Signature signature(const BYTE* bytes, SIZE_T length,
                    const Relocation* relocations = nullptr,
                    unsigned relocationCount = 0);

void* resolve(HMODULE engine, const char* name, DWORD rva);

void note(const char* what, bool ok);

bool verifyResourceStateLayout(HMODULE engine);

bool prepareShadowAlphaDefer(HMODULE engine);

bool verifyShadowTextureDirectCallers(HMODULE engine, const void* getter);

bool installLoads(HMODULE engine);

bool installArchive(HMODULE engine, bool trace, bool cache);

bool installFence(HMODULE engine);

bool installRegionLock(HMODULE engine);

bool installSweeps(HMODULE engine);

bool auditedImage(HMODULE module, DWORD expectedSize, const char* what);

void addChainModule(HMODULE module, DWORD expectedSize, char tag,
                    const char* what);

bool auditedGameImage(HMODULE game);
bool installGameUpdateAt(HMODULE game, void* target);
bool installGame();

bool installLoop();

bool installPump(HMODULE engine, bool tracePump);

bool installHeap(HMODULE engine);

bool installArchiveIo(HMODULE engine);

bool installBlocking(HMODULE engine);

bool installFrame(HMODULE engine);

bool installDeferredPasses(HMODULE engine);

bool installReflections(HMODULE engine, bool trace, bool deferAdmissionMesh,
                        bool deferAdmissionAll,
                        bool secondaryPassAdmission);

bool installTerrain(HMODULE engine, bool traceTerrain, bool preloadLayers,
                    bool secondaryPassAdmission);

bool installShadow(HMODULE engine, bool trace);

bool installWait(HMODULE engine);


void readShadowOptions(const wchar_t* iniPath);


void readTerrainOptions(const wchar_t* iniPath);


void readSecondaryOptions(const wchar_t* iniPath);

void resetEngineTraceState();

} } } // namespace tq::engine::detail
