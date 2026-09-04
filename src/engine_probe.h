#pragma once

#include <windows.h>

struct ID3D11DeviceContext;

namespace tq {
namespace engineprobe {

// Audited instrumentation and performance fixes written into Engine.dll.
//
// Every instrument before this one was a vtable slot or mod-side code. This
// one patches the game's own instruction stream on paths that run thousands of
// times a second, so installation is split strictly by purpose:
//
// - trace groups install only when the performance probe is on *and* [debug]
//   engine_trace is not 0;
// - accepted [performance] fixes install independently with both trace switches
//   at 0, and bring none of those trace groups with them;
// - every target is resolved by decorated export name and then asserted
//   against the RVA the audit recorded, so a different Engine.dll resolves to
//   a different address and installs nothing;
// - every site verifies 16-24 bytes before it writes 4-7, because the four
//   busiest targets share the same six-byte prologue;
// - each hook installs independently and one failure leaves the others alone;
// - shutdown() restores every site in reverse, and only where the bytes are
//   still the ones we wrote.
//
// What it answers is in probe.h, below CounterEngineLevelLoad.

// Reads [debug] engine_trace. 0 disables the module outright; 1 (the default)
// installs everything; any larger value is a mask, so a run that misbehaves
// can be bisected from the INI rather than from a rebuild:
//   2  region and resource loads      4  archive reads and block inflates
//   8  the loader fence wait         16  the region lock on the render path
//  32  the resource-manager sweeps   64  Region::WaitForLoadingToFinish
// 128  Engine::Update and Engine::Render, bracketed whole
// 256  GameEngine::Update -- the one hook that is in Game.dll
// 512  TQ.exe's main loop, through its import table, patching nothing: its
//      sleep and waits, the platform pump, graphics options, music, sound,
//      Engine::PresentSurface, the collision fixup, quest triggers, and
//      EWindow::ProcessMessages -- eleven imports covering every call in the
//      loop that does work rather than return a pointer
//1024  inside the pump: PeekMessageA and DispatchMessageA, which are
//      Engine.dll's imports rather than the executable's
//      The F12 stutter marker also reuses the PeekMessageA import, even with
//      engine_trace=0, so it never adds a second Win32 input query per frame.
//2048  Engine.dll's operator new[] and operator delete[]. Run 23 broke the
//      freeze frame down and found 61% of it named by nothing; the archive
//      File constructor allocates two buffers of up to 256 KiB per compressed
//      entry opened, and that frame opened 1,299 of them.
//4096  the seek and the read under the archive block routine, so the inflate
//      can be recovered from engine_arc_inflate_us by subtraction -- the
//      number 4.2 and 4.3 are gated on
//8192  everything in Engine.dll that can block: every critical section
//      (contended acquisitions only), both waits, and Sleep. Run 24 killed
//      the heap candidate and left 996 ms of a 1,534.8 ms frame unnamed; the
//      main thread waiting on the loader thread has never been measured
//      outside three lock sites and one fence. Installs after those groups,
//      because they check these same slots still hold kernel32's exports.
//16384 the deferred renderer's one GraphicsShadowMapDx11::RenderDirectional
//      call: whole-call CPU time, region changes, and main-thread resource
//      loads nested inside it. With group 2, each nested load is also split by
//      the resource's raw pre-call loaded state (0/1/2/other), overlapping
//      in-queue flag, and engine filename class (mesh/shader/texture/other).
//      A call-site wrapper also measures cold meshes at
//      GraphicsMeshInstance::GetNumShadowRenderPasses, before the caster's
//      draw record is built. Two further call-site wrappers classify cold
//      material textures pulled during RenderPass by whether the active shadow
//      shader actually declares that material parameter. A third relates the
//      mesh-instance style/pass captured at the earlier record gate to used
//      material textures; alpha styles carry base identity and opaque styles
//      report base_unknown. Every accepted renderable is retained so a miss
//      splits into class override, pass mismatch, or missing record without
//      another engine call. The direct GraphicsTexture::GetTexture callers
//      independently partition all shadow-nested texture loads. Select group 2 as well when
//      using a mask so ResourceLoader::LoadResource and the verified Resource
//      layout are present to populate all splits.
//32768 TerrainType::PreLoad and the exact DX11 terrain consumers found by run
//      62: SetShaderParams, SetGrassShaderParams, and
//      TerrainRenderInterfaceRT::RenderGround. The first three associate each
//      retained terrain load with that exact TerrainType's preload history;
//      the last adds whole-call CPU and GPU spans.
//65536 the verified direct children of GraphicsDeferredRendererX::Render,
//      grouped into geometry, shadow-map construction, light accumulation,
//      deferred resolve/AO, later scene lists, and post/fog/composite. Run 71
//      also patches the owner's sole direct caller, splitting its two dynamic
//      invocations and the two geometry call sites. CPU and Draw timing are
//      exact per-site sums; four non-blocking GPU pairs cover only one site in
//      one invocation. Slow Draw records retain the already tracked D3D state,
//      never per-draw queries. Resource and D3D creation work is split between
//      those four cells and the non-geometry remainder of each invocation.
//131072 the unique reflection-manager call in each recursive DX11 portal /
//      region branch, plus that manager's per-water-plane forward renderer.
//      The first two managers and first two planes in each manager receive
//      exact CPU, game-draw, Resource, D3D-creation, and non-blocking GPU
//      fields; explicit overflow counters expose any wider frame. The hooks
//      patch only the two verified E8 sites and do not change render choices.
// With groups 2, 128, and 16384 all installed, the same verified load hook
// also measures the main-thread complement outside RenderDirectional. Phase
// (Engine::Render/Update/other) and engine filename type independently
// partition it. F12 emits the preceding 120 frames' retained load identities,
// call-shaped immediate return sites, and bounded call-shaped upstream stack
// candidates; no log formatting runs on the candidate frame.
void readOptions(const wchar_t* iniPath);

// [performance] shadow_defer_cold_resources, a fix and defaulting to 1.
//
// Before any caster record exists, GraphicsMeshInstance ensures its root mesh
// merely to read the number of shadow passes. At 1, a root mesh in state 0 or
// 1 makes that exact caster return zero passes; state 0 is explicitly enqueued
// with the engine's normal preload arguments, and the caster returns when the
// mesh reaches state 2. This applies to opaque and alpha-tested mesh-instance
// casters only inside the directional map. Resident casters and the colour
// pass are unchanged.
//
// GraphicsMeshInstance's alpha-tested shadow styles need a base texture only
// to cut holes in the caster. A caster/pass whose verified base texture
// Resource is in state 0 or 1 is likewise omitted until resident. Opaque
// resident casters still render normally, but a material texture whose Name
// is absent from their active shadow shader is not loaded. This avoids
// rendering foliage/fences as solid while removing needless synchronous
// shadow-side texture load. GraphicsMeshInstance's optional
// bumpTexture override has the same stock ordering bug--EnsureAvailable runs
// before the setter checks the shader--so it is likewise skipped only when
// the active directional-shadow shader proves it has no bumpTexture input.
// The base mesh material can also carry a baseTexture that is immediately
// replaced by GraphicsMeshInstance+0x14. Inside the directional pass only,
// that earlier getter is skipped when the live override is non-null, distinct,
// and the material Name exactly matches baseTexture; the verified stock code
// ensures and binds the override before any draw.
//
// It reaches install() with the performance probe off and brings no trace
// group. Group 16384 reports omitted states, enqueue outcomes, and skipped
// material/bump dependencies when enabled.

// [performance] shadow_defer_cold_actor_pose, a fix and defaulting to 1.
//
// Run 68 proved another exact root-mesh EnsureAvailable occurs earlier, while
// GraphicsShadowMapDx11::RenderDirectional gathers actors: Actor::AddToScene
// calls Actor::UpdateMeshInstance, which enters GraphicsMeshInstance::UpdatePose.
// At 1, this switch queues a state-0 Actor root and defers that one pose update
// while the root is state 0/1. It implies the complete later shadow-defer gate,
// which then omits the still-cold caster until state 2. Other Actor update
// callers, colour rendering, point shadows, and resident actors are unchanged.
//
// It reaches install() with the performance probe off and brings no trace
// group. Group 16384 reports the exact state/enqueue outcome when enabled.

// [performance] terrain_preload_layers, a fix and defaulting to 1.
//
// Runtime TerrainRT::LoadRenderData creates each layer TerrainType's base,
// bump, and grass texture Resources by calling TerrainType::LoadTextures, but
// TerrainRT::PreLoad never calls the semantic TerrainType::PreLoad method that
// queues those Resources. At 1, the already verified LoadTextures call site is
// retargeted to a wrapper that calls the original and then the stock
// TerrainType::PreLoad(true) on that exact object. It queues through the game's
// existing ResourceLoaders and does not wait, omit colour, or invent a loader.
//
// It reaches install() with the performance probe off and brings no trace
// group. Group 32768 observes the same stock calls when enabled.

// [performance] secondary_pass_admission_budget, a fix and defaulting to 8.
//
// Run 83 found that the felt play transition introduces 134 previously unseen
// reflection renderables and 15 directional-shadow renderables at once, while
// Runs 81--82 showed that omitting one consumer merely moves GPU first use to
// the next consumer/frame. The first N identities render normally; identity
// N+1 proves a real pending population and self-arms admission. A deferred
// RenderPass still executes resource/material setup,
// but its D3D Draw/DrawIndexed calls are suppressed until that identity wins a
// later frame's budget. Normal colour rendering and already admitted objects
// are unchanged.
//
// It reaches install() with the performance probe off and brings no trace
// group. Count-only columns report triggers, admitted/deferred identities, and
// omitted secondary-pass draws when tracing is independently enabled.

// Installs whatever the mask selects and the build supports. Returns true if
// at least one hook went in. Safe to call when the probe is disabled, when
// `engine` is null, or twice.
bool install(HMODULE engine);

enum {
    DeferredTraceVertexBufferSlots = 4,
    DeferredTracePixelResourceSlots = 8
};

// Updated only by the already-needed D3D state-setting hooks. The slow-draw
// recorder copies it only when a draw enters that frame's bounded top set; no
// Get-state call is made from Draw/DrawIndexed.
struct DeferredDrawBindings {
    const void* vertexBuffers[DeferredTraceVertexBufferSlots];
    unsigned vertexStrides[DeferredTraceVertexBufferSlots];
    unsigned vertexOffsets[DeferredTraceVertexBufferSlots];
    const void* indexBuffer;
    unsigned indexFormat;
    unsigned indexOffset;
    const void* vertexShader;
    const void* pixelShader;
    const void* pixelResources[DeferredTracePixelResourceSlots];
};

// True after options have been read when group 65536 or reflection group
// 131072 and full performance tracing request the D3D binding hooks. It does not mean the
// verified Engine sites installed. Group 131072 uses the same setter snapshot
// to correlate fresh buffer identities across reflection/shadow/color passes.
bool deferredDrawTraceRequested();

// Sparse GPU subdivision inside an armed exact engine class. The inline gate
// is the ordinary-draw path: when neither class is recording, visual.cpp does
// not call either helper at all.
namespace detail {
extern volatile LONG gpuChunkDrawActive;
extern volatile LONG secondaryAdmissionDrawSuppressDepth;
}
inline bool gpuChunkDrawActive() {
    return detail::gpuChunkDrawActive != 0;
}
inline bool secondaryAdmissionDrawSuppressed() {
    return detail::secondaryAdmissionDrawSuppressDepth != 0;
}
// Visual owns the D3D vtable hook, so it reports an actually suppressed call
// here rather than making the Engine-side RenderPass wrapper guess its count.
void noteSecondaryAdmissionDrawSkipped();
// An independent serial is required because probe::currentFrameIndex() is
// deliberately zero when performance_trace=0, while this behavior must work
// in the normal shipping configuration.
void secondaryAdmissionFrameBoundary();
// The begin call precedes the game's draw; the finish call follows any
// enhanced-grass companion draw.
void beginGpuChunkDraw(ID3D11DeviceContext* context);
void finishGpuChunkDraw(bool indexed, unsigned count,
                        const DeferredDrawBindings* bindings);

// Called by the D3D11 Draw/DrawIndexed hooks after their one timing sample.
// A non-zero pass exists only while one verified direct child is active.
void countDeferredDraw(unsigned elapsedUs, bool indexed, unsigned count,
                       unsigned start, int base,
                       const DeferredDrawBindings* bindings);

// Successful game-owned D3D creations. These partition their existing timing
// sample by active owner invocation/site and retain only fixed-size identity
// metadata for correlation with the slow draws emitted at F12.
void noteDeferredTextureCreated(const void* texture, unsigned elapsedUs,
                                unsigned width, unsigned height,
                                unsigned mipLevels, unsigned format,
                                unsigned bindFlags, unsigned miscFlags);
// Passive Run-80 detail for successful asset texture creations that execute
// on a loader thread. Kept separate from the owner-scoped creation ring: an
// off-main call cannot truthfully inherit the render thread's active owner.
void noteOffMainTextureCreated(unsigned startFrame, unsigned finishFrame,
                               unsigned elapsedUs, unsigned threadId,
                               unsigned width, unsigned height,
                               unsigned mipLevels, unsigned format,
                               unsigned bindFlags, unsigned miscFlags,
                               bool hasInitialData);
void noteDeferredBufferCreated(const void* buffer, unsigned elapsedUs,
                               unsigned byteWidth, unsigned bindFlags,
                               unsigned usage, unsigned cpuAccessFlags,
                               unsigned miscFlags);

// The two rejected reflection-omission experiments need the existing device
// CreateBuffer slot even when draw timing and every trace group are off.
bool reflectionAdmissionBufferTrackingRequested();

// Progressive secondary admission needs both Draw slots independently of
// every visual enhancement and trace group. Visual publishes their atomic
// readiness before Engine-side behavior is allowed to activate.
bool secondaryPassAdmissionRequested();
void setSecondaryAdmissionDrawHooksReady(bool ready);

// Restores every patched site. Safe when nothing was installed.
void shutdown();

#ifdef TQ_SELFTEST
// How many hooks the last install() put in, so a test can assert that an
// incompatible image remains untouched regardless of requested fixes.
unsigned installedForTest();
bool asyncLevelLoadForTest();
bool shadowTransitionReuseForTest();
void setTraceMaskForTest(unsigned mask);
// Whether install() would install one trace group, decided the way install()
// decides it. archive_cache_mb can reach install() with the performance probe
// off, and this is what says the trace does not come with it.
bool wantsForTest(unsigned group);
bool shadowDeferColdResourcesForTest();
bool shadowDeferColdActorPoseForTest();
bool terrainPreloadLayersForTest();
bool reflectionDeferAdmissionMeshForTest();
bool reflectionDeferAdmissionAllForTest();
unsigned secondaryPassAdmissionBudgetForTest();
bool reflectionAdmissionTriggeredForTest(unsigned buffers);
void resetAdmissionRenderableIdentitiesForTest();
bool admissionRenderableFirstForTest(const void* object, unsigned kind,
                                     unsigned consumer);
void resetSecondaryAdmissionForTest(unsigned budget, bool armed);
bool secondaryAdmissionArmedForTest();
bool secondaryAdmissionRenderableDeferredForTest(const void* object,
                                                  unsigned kind,
                                                  bool reflection,
                                                  bool directional);
void setDeferredPassForTest(unsigned pass);
void setDeferredOwnerContextForTest(unsigned invocation, unsigned site);
void setReflectionContextForTest(unsigned manager, unsigned plane);
void setCrossPassTracingForTest(bool enabled);
void setGpuChunkTracingForTest(bool enabled);
void armGpuChunksForTest();
void closeGpuChunksForTest();
unsigned gpuChunkBinDrawsForTest(unsigned bin);
void recordGpuChunkTerrainCallForTest(bool block, const void* object,
                                      unsigned cpuUs, unsigned resourceUs,
                                      unsigned textureUs);
void recordGpuChunkMeshCallForTest(const void* object, unsigned cpuUs);
unsigned gpuChunkRenderableKindForTest(unsigned index);
bool gpuChunkTerrainCallForTest(unsigned index, bool* block,
                               unsigned* firstDraw, unsigned* lastDraw,
                               unsigned* cpuUs, unsigned* resourceCount,
                               unsigned* resourceUs,
                               unsigned* textureCount,
                               unsigned* textureUs);
void setDirectionalContextForTest(bool enabled);
void noteCrossPassBufferForTest(const void* buffer, unsigned bytes = 64);
void countCrossPassDrawForTest(const void* buffer);
void countDeferredDrawForTest(unsigned elapsedUs, bool indexed = true,
                              unsigned count = 3);
void noteDeferredCreationForTest(bool texture, unsigned elapsedUs);
void resetOffMainTexturesForTest();
bool latestOffMainTextureForTest(unsigned* startFrame, unsigned* finishFrame,
                                 unsigned* elapsedUs, unsigned* threadId,
                                 unsigned* width, unsigned* height,
                                 unsigned* mipLevels, bool* hasInitialData);
bool shouldDeferShadowAlphaForTest(unsigned style, unsigned state);
bool shouldDeferShadowMeshForTest(unsigned state);
bool shadowActorPoseQueueConfirmedForTest(unsigned state, bool inQueue);
void countDeferredShadowAlphaForTest(unsigned state, bool enqueued,
                                     bool failed);
void countDeferredShadowMeshForTest(unsigned state, bool enqueued,
                                    bool failed);
void countDeferredShadowActorPoseForTest(unsigned state, bool enqueued,
                                         bool failed);
// Drives the whole-map/matrix cache without patching an off-game synthetic
// module. The production wrapper uses these same two helpers.
void primeShadowReuseForTest(void* region, void* surface, const void* matrix);
bool reuseShadowForTest(void* region, void* surface, void* matrix);
void countShadowResourceStateForTest(unsigned state, bool inQueue,
                                     unsigned elapsedUs);
void countShadowResourceTypeForTest(const char* name, unsigned elapsedUs);
void countOutsideDirResourceForTest(unsigned type, unsigned phase,
                                    unsigned elapsedUs);
void outsideDirResourceResetForTest();
void outsideDirResourceRememberForTest(unsigned frame);
unsigned outsideDirResourceWindowForTest(unsigned markerFrame,
                                         bool* truncated);
void shadowMeshResourceResetForTest();
void shadowMeshResourceRememberForTest(unsigned frame);
unsigned shadowMeshResourceWindowForTest(unsigned markerFrame,
                                         bool* truncated);
void terrainPreloadResetForTest();
void terrainPreloadRememberForTest(const void* terrain, bool includeTextures,
                                   unsigned frame);
void terrainPreloadSnapshotForTest(const void* terrain, unsigned* trueCount,
                                   unsigned* falseCount,
                                   unsigned* lastTrueFramePlusOne,
                                   unsigned* lastFalseFramePlusOne);
void terrainRtEventRememberForTest(const void* terrain, unsigned event,
                                   unsigned frame);
void terrainRtEventSnapshotForTest(
    const void* terrain, unsigned* attachCount, unsigned* attachFirst,
    unsigned* attachLast, unsigned* texturesCount, unsigned* texturesFirst,
    unsigned* texturesLast, unsigned* preloadCount, unsigned* preloadFirst,
    unsigned* preloadLast);
void countShadowMaterialTextureForTest(bool known, bool used,
                                       unsigned elapsedUs);
void countShadowMaterialUsedContextForTest(bool callKnown, bool context,
                                           bool styleKnown,
                                           unsigned match, unsigned style,
                                           bool baseKnown, bool baseMatch, int pass,
                                           bool outerInstanceSite,
                                           unsigned elapsedUs);
void resetShadowRecordContextsForTest();
void rememberShadowRecordContextForTest(void* instance, int pass,
                                        unsigned style, bool styleKnown,
                                        bool baseKnown,
                                        const void* baseTexture);
bool findShadowRecordContextForTest(void* instance, int pass,
                                    unsigned* style, bool* styleKnown,
                                    bool* baseKnown,
                                    const void** baseTexture);
unsigned explainShadowRecordMissForTest(void* instance, int pass);
void countShadowMeshContextPatchStatusForTest(unsigned status);
void countShadowTextureCallerForTest(unsigned caller, unsigned elapsedUs);
unsigned shadowTextureCallerFromWordsForTest(const void* const* words,
                                             unsigned count,
                                             const void* engineBase);
// The slow-LoadLevel caller table, which decides where Stage 5.1 should point.
// Driven directly rather than through a real detour: what is worth testing is
// the aggregation and the module bound, not __builtin_return_address.
void slowLoadResetForTest(const void* base);
void slowLoadRecordForTest(const void* caller, unsigned us, bool main);
bool slowLoadSlotForTest(unsigned slot, unsigned long* rva, long* calls,
                         long* main, long* us, long* worst);
long slowLoadLostForTest();
// The stack scan that replaces hooking one function per boot. The filter is
// the part worth testing off-game: it is what makes a raw scan trustworthy
// without frame pointers, and a false negative on a virtual call would lose
// exactly the frames the render path is made of.
void chainTextForTest(BYTE* begin, SIZE_T size, char tag);
bool precededByCallForTest(const BYTE* ret);
unsigned captureChainForTest(const void* from, unsigned* depth, char* tags);
// The region-lock thunk, so the off-game test can drive it both contended and
// not. It enters the section exactly as EnterCriticalSection would.
void enterCriticalSectionForTest(LPCRITICAL_SECTION section);
#endif

}  // namespace engineprobe
}  // namespace tq
