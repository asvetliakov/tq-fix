#pragma once

#include <windows.h>
#include <d3d11.h>
#include <stdint.h>

namespace tq {
namespace probe {

// Frame-hitch attribution.
//
// The frame overlay answers "is this frame slow". This answers "why was *that*
// frame slow", which is a different question and needs a different instrument:
// a per-frame record of where the render thread spent its time, what it was
// asked to do, and what the GPU did, kept for the last few hundred frames and
// written out only for the frames that actually spiked -- together with how far
// each field sat from its own recent median, which is the part that names the
// cause.
//
// It is off unless [debug] performance_trace is set in tqflicker.ini: 1 writes
// one row per hitching frame and is cheap enough to leave on, `full` writes
// every frame for a measurement session. While it is off this module allocates
// nothing, measures nothing, writes nothing, and creates no device objects;
// the only cost that remains compiled in is one cached-bool branch at each
// instrumented call site.
//
// Not instrumented with timers by default: Map, Unmap, the draws and
// PSSetShaderResources run 1500-5000 times a frame, and a
// QueryPerformanceCounter pair on each is not free. Those get counters, and
// their real cost is priced by turning the feature off in the INI instead.
//
// `performance_trace=full` also arms a clock pair around the game's own
// Draw/DrawIndexed and Map calls -- the driver call itself, not the hook body.
// Findings §35 is why. Hitch-only tracing omits these high-frequency clocks;
// while they are off the hook costs one predictable branch.

// ---------------------------------------------------------------------------
// CPU phases. The order is the CSV column order and must not be reshuffled
// without regenerating any recorded run alongside it.
enum Phase {
    PhasePresent,        // our pre-Present callback, exclusive of the three
                         // phases below that it brackets
    PhaseGrassPresent,   // grass::onPresent -- completes a staged grass readback
    PhaseStreamStep,     // one progressive texture upload chunk
    PhaseOverlayRaster,  // overlay CPU rasterisation and its texture uploads
    PhaseOverlayDraw,    // overlay state save, draws, and restore
    PhaseSmaa,
    PhaseBloom,
    PhaseShaderCreate,   // CreateVertexShader/CreatePixelShader, incl. DXBC work
    PhaseTextureCreate,
    PhaseBufferCreate,
    PhaseGrassFill,      // grass unmap rewrite, accumulated over the frame
    PhaseGrassCross,     // the crossing draw, accumulated over the frame
    // The game's own Present call. When the GPU is the bottleneck this is
    // where the frame waits, so it is what separates "slow because of work on
    // this thread" from "slow because the GPU is behind".
    PhasePresentCall,
    // Appended after PhasePresentCall rather than filed beside the other
    // creation phases, because the phase order is the CSV column order: a run
    // recorded before these existed still reads correctly, with two columns
    // missing off the end rather than every column after them shifted.
    //
    // Both are the *game's* time, not the mod's, exactly like PhasePresentCall
    // -- tools/frames.py must not charge them to the mod's share.
    PhaseDrawSubmit,     // the game's Draw/DrawIndexed, driver call only
    PhaseMapResource,    // the game's Map, driver call only
    PhaseCount
};

// Counts of things done in the frame. Cheap enough for the hot hooks that
// timing them would be dishonest.
enum Counter {
    CounterDraw,
    CounterDrawIndexed,
    CounterGrassDraw,
    CounterGrassCross,
    CounterMap,
    CounterUnmap,
    CounterGrassFill,
    CounterGrassRotate,
    CounterGrassAdopt,
    CounterGrassSeedQueued,
    CounterGrassSeedDone,
    CounterGrassSeedFailed,
    CounterGrassTwinCreate,
    CounterGrassTwinFail,
    CounterUploadSteps,
    CounterUploadKiB,
    // Progressive jobs started and finished, so a session's total upload
    // volume can be read against the number of textures it actually covered
    // rather than assumed to be one pass over each.
    CounterUploadJobsStarted,
    CounterUploadJobsDone,
    CounterUploadRejected,
    CounterShaderCreate,
    CounterTextureCreate,
    CounterBufferCreate,
    CounterSetRenderTargets,
    CounterShadowBind,
    CounterShadowFitChange,
    CounterPsSetSrv,

    // ---------------------------------------------------------------------
    // The engine channel. Everything from here down is written from the game's
    // loader thread or its main thread, never from the render thread, and so
    // must go through engineCount() rather than count(): countInternal drops
    // every write that is not the render thread's, which is exactly why the
    // texture-load counters this block replaces were reading zero.
    //
    // Durations here are named `_us`, and that is load-bearing rather than
    // stylistic. tools/frames.py builds "the mod's share" from every column
    // ending in `_ms`, so an engine `_ms` column would be silently charged to
    // the mod; `_us` falls into its counter bucket instead.
    CounterEngineFirst,
    // The three outcomes CounterUploadRejected used to conflate, plus the two
    // Stage 2 adds. Their sum is still reported as upload_rejected, so a run
    // recorded before the split compares on that column unchanged.
    CounterUploadRejectPool = CounterEngineFirst,  // no free job slot
    CounterUploadRejectBudget,                     // over the retained-byte cap
    CounterUploadRejectAlloc,                      // the retention buffer failed
    CounterUploadRejectScan,                       // no owning File on the stack
    // Which File class served the texture. The whole progressive uploader is
    // predicated on an answer to this, and until now nothing reported it.
    CounterUploadSrcArc,
    CounterUploadSrcLoose,
    CounterUploadSrcNone,
    // CreateTexture2D calls that did not arrive on the render thread -- i.e.
    // the game's own texture loads -- and how long they took. This is the
    // number that prices Stage 2's copy against the upload it replaces.
    CounterEngineTexCreateOff,
    CounterEngineTexCreateOffUs,
    // The retire path, split three ways. Run 4 showed 9 of its 10 worst frames
    // retiring a job against a 5.4% base rate, on chunks of 128-512 KiB -- so
    // the cost is not the upload, it is what happens when a job finishes. That
    // is two COM releases, which under DXMT free the texture's device memory,
    // and one UnmapViewOfFile over a view of the whole source file. These say
    // which, rather than leaving it to be argued.
    // upload_unmap_us is the worker's time; the _inline_ pair is what still
    // had to be paid on the render thread because the queue was full. Run 5
    // measured 1.03 s of UnmapViewOfFile inside Present, 92-98% of every worst
    // stream_step frame, so the inline columns are the ones that must stay at
    // zero for the move to have worked.
    CounterUploadUnmap,
    CounterUploadUnmapUs,
    CounterUploadUnmapInline,
    CounterUploadUnmapInlineUs,
    CounterUploadReleaseUs,
    // The loose-file size gate. A texture pack can ship assets far larger than
    // anything the game itself contains -- this install has 984 over 4096 on a
    // side, up to 16384x16384 -- and those are what make a mapped view
    // expensive to tear down and a synchronous create expensive to do at all.
    // Files the loose source served at all, so a run can tell "the gate never
    // saw a loose file" from "it saw them and declined to inspect them" --
    // the ambiguity that hid a bug in the gate's own class check for a whole
    // session.
    CounterLooseOpen,
    CounterLooseProbe,
    CounterLooseProbeUs,
    CounterLooseRejectOversize,
    // Opens the archive source served. A redirect is only correct if the file
    // the loose source refused turns up here instead, so this is the other
    // half of the evidence rather than a curiosity.
    CounterArcOpen,
    // Address space held by live mapping leases, sampled once a frame so the
    // column reads as a gauge rather than a total. The pool's only real limit
    // is a count -- 128 leases -- and a count says nothing about bytes: at the
    // measured median texture that is ~340 MiB of views, and at the 4K cap's
    // maximum it would be 2.7 GiB, in a process that has about 3.
    CounterUploadLeasedMib,
    // The archive block cache, from src/arc_cache.cpp. Mod work, on the
    // engine's threads, which is why it counts through this channel and not
    // through count().
    //
    // `arc_cache_hit` against `engine_arc_blocks` is the whole result: blocks
    // the engine asked for, over blocks it had to read and inflate. The
    // denominator keeps its old meaning deliberately -- a hit still counts as
    // a block requested -- so the amplification figures from runs 10 and 17
    // stay comparable to a cached run's.
    //
    // `arc_cache_bad` must be zero. It counts blocks whose cached copy did not
    // match what the engine produced for the same key, which is the one
    // outcome the `verify` mode exists to rule out; a single one disables the
    // cache for the rest of the session and writes a line to the log.
    // `arc_cache_skip` counts requests refused before they reached the slab --
    // a descriptor claiming more than one block's worth -- and a non-zero
    // reading there means the structure offsets are being read wrong.
    CounterArcCacheHit,
    CounterArcCacheHitUs,
    CounterArcCacheStore,
    CounterArcCacheEvict,
    CounterArcCacheVerify,
    CounterArcCacheBad,
    CounterArcCacheSkip,

    // ---------------------------------------------------------------------
    // Engine.dll's own work, from src/engine_probe.cpp. Everything below is
    // written from inside the game's code on whichever thread reached it, and
    // exists to name the residual tools/frames.py has always had to report as
    // "the game's frame": ~7-8.7 s of hitch time a session that no mod column
    // accounts for.
    //
    // The `_us` totals nest. A resource load inside a level load is counted in
    // both, so they are a breakdown of where a hitch went and not a partition
    // to be summed.
    //
    // `_main` is the subset that ran on the thread the engine recorded as its
    // own at Engine+0x41a5dc -- i.e. the loads the game forced synchronously
    // rather than handing to its loader thread. Those are the ones that are
    // in the frame rather than beside it.
    CounterEngineLevelLoad,
    CounterEngineLevelLoadUs,
    CounterEngineLevelLoadMain,
    CounterEngineLevelLoadMainUs,
    CounterEngineResLoad,
    CounterEngineResLoadUs,
    CounterEngineResLoadMain,
    CounterEngineResLoadMainUs,
    // Main-thread ResourceLoader calls outside the one global directional
    // shadow-map build. The phase and file-suffix dimensions each partition
    // this same population independently; their durations are the complete
    // game LoadResource calls and therefore remain `_us` engine counters.
    CounterEngineResOutsideDir,
    CounterEngineResOutsideDirUs,
    CounterEngineResOutsideDirRender,
    CounterEngineResOutsideDirRenderUs,
    CounterEngineResOutsideDirUpdate,
    CounterEngineResOutsideDirUpdateUs,
    CounterEngineResOutsideDirOther,
    CounterEngineResOutsideDirOtherUs,
    CounterEngineResOutsideDirMesh,
    CounterEngineResOutsideDirMeshUs,
    CounterEngineResOutsideDirShader,
    CounterEngineResOutsideDirShaderUs,
    CounterEngineResOutsideDirTexture,
    CounterEngineResOutsideDirTextureUs,
    CounterEngineResOutsideDirTypeOther,
    CounterEngineResOutsideDirTypeOtherUs,
    // Set on the F12 row if more recent outside-directional loads existed
    // than the fixed diagnostic ring could retain. Zero proves the emitted
    // caller/name list covers the marker's complete retained 120-frame window.
    CounterEngineResOutsideDirMarkerTruncated,
    // The one global directional-shadow build, timed at its direct call site.
    // The resource pair is the main-thread ResourceLoader work nested inside
    // that call, not a second population to add to engine_res_load_main_us.
    // A region change compares GraphicsShadowMapDx11+0x6c between successive
    // calls and gives a prospective scheduler a trigger rather than asking it
    // to infer one from an already-slow frame.
    CounterEngineShadowRender,
    CounterEngineShadowRenderUs,
    CounterEngineShadowRegionChange,
    CounterEngineShadowReuse,
    CounterEngineShadowResLoad,
    CounterEngineShadowResLoadUs,
    // State sampled immediately before each main-thread resource load nested
    // in the directional-shadow call. These subsets answer whether the
    // renderer waited for work already in flight (state 1) or discovered an
    // unloaded caster resource and loaded it itself (state 0). State 2 is
    // retained as a race/assumption check. `in_queue` overlaps the states and
    // is not a fourth partition. Every `_us` is the complete corresponding
    // LoadResource call duration.
    CounterEngineShadowResState0,
    CounterEngineShadowResState0Us,
    CounterEngineShadowResState1,
    CounterEngineShadowResState1Us,
    CounterEngineShadowResState2,
    CounterEngineShadowResState2Us,
    CounterEngineShadowResStateOther,
    CounterEngineShadowResStateOtherUs,
    CounterEngineShadowResInQueue,
    CounterEngineShadowResInQueueUs,
    // Resource file-name classes for the same nested LoadResource calls.
    // These partition by the engine's own .msh/.ssh/.tex suffixes. The
    // mesh-cold pair is narrower: the cold Resource reached at
    // GraphicsMeshInstance::GetNumShadowRenderPasses, before a caster enters
    // the directional-shadow draw list. Its duration overlaps the mesh load.
    CounterEngineShadowResMesh,
    CounterEngineShadowResMeshUs,
    CounterEngineShadowResShader,
    CounterEngineShadowResShaderUs,
    CounterEngineShadowResTexture,
    CounterEngineShadowResTextureUs,
    CounterEngineShadowResTypeOther,
    CounterEngineShadowResTypeOtherUs,
    // Direct GraphicsTexture::GetTexture caller partition for the texture
    // loads above. The material site is recognized by a narrow dynamic
    // bracket because run 51 already retargets that E8; the other nine direct
    // Engine.dll call sites are recognized from their verified return RVAs.
    // Unresolved includes indirect calls. Each pair overlaps, and exactly
    // partitions, engine_shadow_res_texture/count and duration.
    CounterEngineShadowTexFromMeshMaterial,
    CounterEngineShadowTexFromMeshMaterialUs,
    CounterEngineShadowTexFromBillboard,
    CounterEngineShadowTexFromBillboardUs,
    CounterEngineShadowTexFromForwardRenderer,
    CounterEngineShadowTexFromForwardRendererUs,
    CounterEngineShadowTexFromLineEffect,
    CounterEngineShadowTexFromLineEffectUs,
    CounterEngineShadowTexFromPieOmatic,
    CounterEngineShadowTexFromPieOmaticUs,
    CounterEngineShadowTexFromWater,
    CounterEngineShadowTexFromWaterUs,
    CounterEngineShadowTexFromStateParameter,
    CounterEngineShadowTexFromStateParameterUs,
    CounterEngineShadowTexFromFun1155b0,
    CounterEngineShadowTexFromFun1155b0Us,
    CounterEngineShadowTexFromFun12fa30,
    CounterEngineShadowTexFromFun12fa30Us,
    CounterEngineShadowTexFromFun23e1e0,
    CounterEngineShadowTexFromFun23e1e0Us,
    CounterEngineShadowTexFromUnresolved,
    CounterEngineShadowTexFromUnresolvedUs,
    CounterEngineShadowMeshCold,
    CounterEngineShadowMeshColdUs,
    // A cold root mesh rejected at GraphicsMeshInstance's exact pass-count
    // boundary. State 0 is enqueued; state 1 is already in flight.
    CounterEngineShadowMeshOmitted,
    CounterEngineShadowMeshOmittedState0,
    CounterEngineShadowMeshOmittedState1,
    CounterEngineShadowMeshOmittedEnqueued,
    CounterEngineShadowMeshOmittedEnqueueFailed,
    // The still-earlier exact Actor::AddToScene call skipped before
    // Actor::UpdateMeshInstance can enter GraphicsMeshInstance::UpdatePose.
    // These are counts only: no engine work is charged to the mod.
    CounterEngineShadowActorPoseDeferred,
    CounterEngineShadowActorPoseState0,
    CounterEngineShadowActorPoseState1,
    CounterEngineShadowActorPoseEnqueued,
    CounterEngineShadowActorPoseEnqueueFailed,
    // Cold material textures pulled by GraphicsMesh::SetShaderParameters
    // while rendering the directional map. `used` means the active shadow
    // shader reports a parameter with the material entry's Name; `unused`
    // means the game loaded the texture before its setter discovered that the
    // shadow shader has no such parameter. `unknown` preserves the partition
    // if the verified shader query is unavailable. These overlap the texture
    // resource-load class above.
    CounterEngineShadowMaterialTex,
    CounterEngineShadowMaterialTexUs,
    CounterEngineShadowMaterialTexUsed,
    CounterEngineShadowMaterialTexUsedUs,
    CounterEngineShadowMaterialTexUnused,
    CounterEngineShadowMaterialTexUnusedUs,
    CounterEngineShadowMaterialTexUnknown,
    CounterEngineShadowMaterialTexUnknownUs,
    // A second exact partition of the cold shader-used material population.
    // The style buckets are GraphicsMeshInstance's verified return values;
    // base_match compares the loaded Resource pointer with the one inspected
    // by the record gate; base_unknown is expected for opaque styles because
    // their base is not fetched merely to instrument them. pass0/pass_other
    // records the original pass supplied to GraphicsMeshInstance. Context
    // unknown preserves all three partitions when no accepted record matches.
    CounterEngineShadowMaterialUsedStyle0,
    CounterEngineShadowMaterialUsedStyle0Us,
    CounterEngineShadowMaterialUsedStyle1,
    CounterEngineShadowMaterialUsedStyle1Us,
    CounterEngineShadowMaterialUsedStyle2,
    CounterEngineShadowMaterialUsedStyle2Us,
    CounterEngineShadowMaterialUsedStyle3,
    CounterEngineShadowMaterialUsedStyle3Us,
    CounterEngineShadowMaterialUsedStyle4,
    CounterEngineShadowMaterialUsedStyle4Us,
    CounterEngineShadowMaterialUsedStyle5,
    CounterEngineShadowMaterialUsedStyle5Us,
    CounterEngineShadowMaterialUsedContextUnknown,
    CounterEngineShadowMaterialUsedContextUnknownUs,
    CounterEngineShadowMaterialUsedBaseMatch,
    CounterEngineShadowMaterialUsedBaseMatchUs,
    CounterEngineShadowMaterialUsedBaseOther,
    CounterEngineShadowMaterialUsedBaseOtherUs,
    CounterEngineShadowMaterialUsedBaseUnknown,
    CounterEngineShadowMaterialUsedBaseUnknownUs,
    CounterEngineShadowMaterialUsedPass0,
    CounterEngineShadowMaterialUsedPass0Us,
    CounterEngineShadowMaterialUsedPassOther,
    CounterEngineShadowMaterialUsedPassOtherUs,
    CounterEngineShadowMaterialUsedPassUnknown,
    CounterEngineShadowMaterialUsedPassUnknownUs,
    // Overlapping explanation of how the record-table lookup resolved. These
    // partition cold shader-used material textures independently of the
    // style/base/pass dimensions above.
    CounterEngineShadowMaterialLookupExact,
    CounterEngineShadowMaterialLookupExactUs,
    CounterEngineShadowMaterialLookupClassOther,
    CounterEngineShadowMaterialLookupClassOtherUs,
    CounterEngineShadowMaterialLookupPassMismatch,
    CounterEngineShadowMaterialLookupPassMismatchUs,
    CounterEngineShadowMaterialLookupInstanceMissing,
    CounterEngineShadowMaterialLookupInstanceMissingUs,
    CounterEngineShadowContextTableOverflow,
    // Exact caller of GraphicsMesh::SetShaderParameters for a cold used
    // material texture. The expected site is the one inside the verified
    // base GraphicsMeshInstance method; other includes indirect/other paths.
    CounterEngineShadowMaterialOuterInstanceSite,
    CounterEngineShadowMaterialOuterInstanceSiteUs,
    CounterEngineShadowMaterialOuterOtherSite,
    CounterEngineShadowMaterialOuterOtherSiteUs,
    // One of these is emitted for each traced directional build. Together
    // they expose why the optional instance/pass call patch is unavailable,
    // including the case where a later material hook forced it to roll back.
    CounterEngineShadowContextPatchActive,
    CounterEngineShadowContextPatchDependencyMissing,
    CounterEngineShadowContextPatchFrameMismatch,
    CounterEngineShadowContextPatchEntryMismatch,
    CounterEngineShadowContextPatchContextMismatch,
    CounterEngineShadowContextPatchCallFailed,
    CounterEngineShadowContextPatchReverted,
    // With shadow_defer_cold_resources enabled, a missing shader parameter is
    // checked before GetTexture. `skipped` is every avoided getter and
    // `skipped_cold` is the state-0 subset that would have loaded now.
    CounterEngineShadowMaterialTexSkipped,
    CounterEngineShadowMaterialTexSkippedCold,
    // The equivalent late-use bug in GraphicsMeshInstance's optional
    // bumpTexture override. Stock code ensures the Resource before its setter
    // discovers that the directional-shadow shader has no such parameter.
    CounterEngineShadowBumpTexSkipped,
    CounterEngineShadowBumpTexSkippedCold,
    // A GraphicsMeshInstance baseTexture override is bound after its generic
    // mesh material. A different generic baseTexture has no surviving value;
    // these count directional-only omissions and their cold subset.
    CounterEngineShadowBaseOverrideSkipped,
    CounterEngineShadowBaseOverrideSkippedCold,
    // [performance] shadow_defer_cold_resources. These describe omitted
    // alpha-tested caster/pass records by the base texture's pre-call state.
    // State 0 is explicitly queued unless it was already in the queue; state
    // 1 is already loading. No pointer survives the call, and state 2 is
    // rendered normally, so the counts also show how quickly casters return.
    CounterEngineShadowAlphaOmitted,
    CounterEngineShadowAlphaState0,
    CounterEngineShadowAlphaState1,
    CounterEngineShadowAlphaEnqueued,
    CounterEngineShadowAlphaEnqueueFailed,
    CounterEngineRegionUnload,
    CounterEngineRegionUnloadUs,
    // Archive::ReadFromFile calls and the bytes they asked for. Counted, not
    // timed: the worst frame measured carries ~1,300 archive opens, and a
    // QueryPerformanceCounter pair on each would price the instrument rather
    // than the read.
    CounterEngineArcRead,
    CounterEngineArcKib,
    // One 256 KiB block read and inflated. This one is timed, because a single
    // inflate costs on the order of a millisecond and the pair that measures
    // it does not.
    CounterEngineArcBlocks,
    CounterEngineArcInflateUs,
    // Work handed to the loader thread. Rising here before a hitch is the
    // backlog forming; rising in the _main columns is the backlog being paid.
    CounterEngineResEnqueued,
    // Engine::Update's rendezvous with the loader fence, once per update, so
    // the count doubles as the number of engine updates the frame contained.
    CounterEngineFenceWait,
    CounterEngineFenceWaitUs,
    // The region lock, taken on the render path by both AddElementsInBox
    // overloads and by Region::GetEntitiesInFrustum. Only contended
    // acquisitions are counted or timed -- an uncontended one costs one
    // interlocked op and a branch and records nothing.
    CounterEngineRegionLockHits,
    CounterEngineRegionLockUs,
    // The seven UnloadUnreferencedResources sweeps Engine::Update runs every
    // update. A candidate for the class of hitch that carries no archive
    // opens and names nothing, which is exactly why it is measured before
    // anything is done about it.
    CounterEngineSweeps,
    CounterEngineSweepUs,
    // Region::WaitForLoadingToFinish, a spin on a load flag that a full .text
    // scan finds no caller for. Non-zero here would be genuine news.
    CounterEngineWaitLoading,
    CounterEngineWaitLoadingUs,
    // The frame, split in two at the engine's own seam. Run 10 named only a
    // fifth to a third of the hitch time: its worst frame spent 511 ms in a
    // forced level load and 950 ms somewhere nothing could see, and a whole
    // class of 200-240 ms frames showed *every* other column at zero. These
    // two brackets say which half of the game's frame that time is in --
    // update or render -- or whether it is in neither, which would put it
    // outside Engine.dll altogether. Both run once a frame, so they cost
    // nothing worth counting.
    CounterEngineUpdate,
    CounterEngineUpdateUs,
    CounterEngineRender,
    CounterEngineRenderUs,
    // Game.dll's simulation tick, which is neither of the two above. Run 11
    // closed the session's accounting to 100% and found 38% of the hitch time
    // -- and the majority of hitching frames -- outside Engine.dll entirely,
    // on frames that render normally with the mod idle. This is the only
    // candidate large enough to hold it; if it stays small, what is left is
    // TQ.exe's own loop or the layer below the process.
    CounterGameUpdate,
    CounterGameUpdateUs,
    // TQ.exe's main loop, reached through its import table rather than by
    // patching anything. Run 12 left 11.6% of the session and 44.7% of the
    // time in frames over 100 ms outside every bracket, arriving as bursts of
    // 50-398 ms events rather than as a steady tax -- and the loop's whole
    // vocabulary for blocking is these three imports. It has no PeekMessage
    // at all, so its message pump is the blocking GetMessageA, and it asks
    // GameEngine::NeedsSleep and then sleeps.
    //
    // The requested/actual pair on the sleep is the point: a loop that asks
    // for one millisecond and is handed two hundred is an environment
    // problem, not a game one, and the two columns say which immediately.
    CounterLoopSleep,
    CounterLoopSleepRequestedUs,
    CounterLoopSleepUs,
    CounterLoopMessage,
    CounterLoopMessageUs,
    CounterLoopWait,
    CounterLoopWaitUs,
    // Free address space in the process, sampled once a frame as a gauge.
    // The mapping-lease explanation for the stalls is already dead, but the
    // 32-bit address space is the one resource whose exhaustion would look
    // exactly like this, and one call a frame settles it rather than leaving
    // it as a second run.
    CounterProcAvailVaMib,
    // Engine::PresentSurface, and the collision fixup beside it. Reading
    // TQ.exe's main loop settled where the residual has to be: the loop runs
    // GameEngine::Update, then *PresentSurface*, then Engine::Render -- and
    // the probe's own frame boundary is the D3D Present *inside*
    // PresentSurface. So the head of that function, where a wait on the
    // swapchain or the GPU would live, has been in every frame's window and
    // inside none of the brackets. Both are reached through TQ.exe's import
    // table, so neither costs a byte of patched code.
    CounterEnginePresentSurface,
    CounterEnginePresentSurfaceUs,
    CounterGameCollisions,
    CounterGameCollisionsUs,
    // The rest of TQ.exe's main loop, which run 14 proved is where the
    // residual lives -- PresentSurface took only 4.6% of the hitch time and
    // the loop is 2,326 bytes, not the 700 first read. These are the six
    // calls in it that do work rather than return a pointer, in the order the
    // loop makes them:
    //
    //   THQNO_Process              the online/platform layer. libcurl sits
    //                              behind it, and bursty network or IPC work
    //                              is the exact shape of the measured stalls.
    //   GraphicsEngine::UpdateFromOptions
    //   Jukebox::Update            music, streamed
    //   SoundManager::Update       audio, streamed
    //   QuestRepository::FireTriggers
    //   EWindow::ProcessMessages   the real message pump, and the reason
    //                              TQ.exe's own GetMessageA read zero calls:
    //                              the pump is Engine.dll's, not the exe's.
    //                              Its return value is what ends the loop.
    CounterLoopPlatform,
    CounterLoopPlatformUs,
    CounterLoopGfxOptions,
    CounterLoopGfxOptionsUs,
    CounterLoopJukebox,
    CounterLoopJukeboxUs,
    CounterLoopSound,
    CounterLoopSoundUs,
    CounterLoopQuests,
    CounterLoopQuestsUs,
    CounterLoopPump,
    CounterLoopPumpUs,
    // Inside the pump. Run 15 found it: EWindow::ProcessMessages is 20.3% of
    // the hitch time, and the worst frames are 119-214 ms of pump with
    // everything else at nothing. The function itself is nine instructions --
    // PeekMessageA(PM_REMOVE), TranslateMessage, DispatchMessageA, looping
    // until the queue drains -- so the function has two visible calls. But
    // PeekMessage can itself dispatch a third kind of work: a nonqueued sent
    // message's receiving window procedure, which the counters below split.
    //
    // Many cheap peeks means a message flood, and the count says so. One
    // expensive peek can be time below USER32 or a sent-message window
    // procedure run inline; its return value cannot distinguish them. Time in
    // explicit dispatch is another window-procedure path.
    //
    // These hook Engine.dll's import table rather than the executable's; the
    // pump belongs to Engine.dll, which is why TQ.exe's GetMessageA read zero.
    CounterPumpPeek,
    CounterPumpPeekUs,
    CounterPumpDispatch,
    CounterPumpDispatchUs,
    // The peek split by what it returned. An empty result is not conclusive:
    // PeekMessage dispatches pending nonqueued SendMessage traffic before it
    // checks the posted queue, so a window procedure may have run inline even
    // when no message is returned.
    CounterPumpPeekMiss,
    CounterPumpPeekMissUs,
    // Archived timer-range experiment counters. They retain their CSV slots
    // so old runs remain readable; current builds never filter the pump.
    CounterPumpTimerFull,
    CounterPumpTimerSplit,
    // Engine.dll's array allocator, reached through its import table. Run 23
    // broke the freeze frame down and found 795 ms of 1,310 -- 61% -- inside
    // Engine::Render and named by nothing: not the level load, not resource
    // loading, not the archive, not texture creation, not the mod, not the
    // pump.
    //
    // The candidate is heap churn, and it is verified in the disassembly
    // rather than guessed. FUN_1014d020, the archive `File` constructor,
    // allocates *two* buffers of up to 256 KiB through `operator new[]`
    // (Engine's IAT slot 0x2ac318) for every compressed entry opened, and
    // frees them through `operator delete[]` (0x2ac304) when it closes. That
    // frame opened 1,299 files: up to 649 MiB of allocate-and-free traffic in
    // one frame, about 2,600 pairs, in a 32-bit MSVC heap under Wine.
    //
    // `_big` is the subset at or above 64 KiB, which is what separates "the
    // engine makes a great many small allocations" from "the engine allocates
    // a quarter-megabyte buffer two thousand times a frame". Only the second
    // is fixable, by pooling the two scratch buffers.
    CounterEngineHeapAlloc,
    CounterEngineHeapAllocUs,
    CounterEngineHeapAllocKib,
    CounterEngineHeapBig,
    CounterEngineHeapBigUs,
    CounterEngineHeapFree,
    CounterEngineHeapFreeUs,
    // The two syscalls under the archive block routine, also through Engine's
    // import table. `engine_arc_inflate_us` brackets the whole routine -- the
    // seek, the read and the `uncompress` -- so its 580 microseconds a block
    // is all three together and nothing says which. These two split it: the
    // inflate is the subtraction.
    //
    // That is the number 4.2 and 4.3 are gated on. Mostly zlib points at
    // libdeflate; mostly syscall points at the bounded prefetch, which given
    // findings 14-17 -- where one host round trip cost 126-212 ms -- would be
    // in character for this install.
    //
    // Engine.dll's other callers of these two are all in the same archive
    // module (`FUN_1011bfd0` through `Archive::AddFileFromMemory`), so
    // comparing `engine_io_read` against `engine_arc_blocks` is what says the
    // attribution holds.
    CounterEngineIoSeek,
    CounterEngineIoSeekUs,
    CounterEngineIoRead,
    CounterEngineIoReadUs,
    CounterEngineIoReadKib,
    // Everything in Engine.dll that can block, reached through its import
    // table. Run 24 killed the heap hypothesis -- `operator new[]` and
    // `delete[]` cost 3.6 ms on a 1,534.8 ms freeze frame and 173 ms over a
    // whole session -- and left 996 ms of that frame, 65% of it, still named
    // by nothing.
    //
    // What has never been instrumented is the main thread *waiting*. The
    // region lock was measured at zero contention, but only at three call
    // sites; the fence was measured at one. Engine.dll's archive lock
    // (`archive+0x60`), held across every block read, and every other
    // critical section and wait in the module have been invisible. If the
    // render thread force-loads a level while the loader thread holds the
    // archive lock, that block is exactly the shape of the missing time.
    //
    // The critical-section hook times only *contended* acquisitions -- it
    // tries first and takes a timestamp only on failure -- so an uncontended
    // lock costs one interlocked operation and records nothing, which is what
    // makes it affordable on a path this hot. These columns are disjoint from
    // engine_region_lock_* and engine_fence_wait_*: those sites read a
    // mod-owned cell rather than the import slot, so nothing is counted twice.
    CounterEngineCsWait,
    CounterEngineCsWaitUs,
    CounterEngineObjWait,
    CounterEngineObjWaitUs,
    CounterEngineSleep,
    CounterEngineSleepUs,
    // The same three split by thread, which run 25 proved is the whole
    // question. Its `engine_obj_wait_us` read 178.7 seconds and its
    // `engine_sleep_us` 165.5 over a 96.3-second session -- both larger than
    // wall clock, because they sum across every thread in the process and
    // most of that is background threads sitting idle. Unattributed, those
    // two columns cannot answer anything; `_main` is what makes them mean
    // something, exactly as it does for the level and resource loads.
    //
    // The lead they carry is `Sleep`: 250 calls a frame in frames over 200 ms
    // against 5.8 in frames under 20, and the four worst frames of the run
    // carry 406, 653, 436 and 319 of them. That is a poll loop. If it is on
    // the main thread it is the 958 ms nothing has named -- and the
    // *requested* total beside the actual is what says whether the cost is
    // the poll or the host's sleep granularity, which is the difference
    // between a game bug and something `timeBeginPeriod` could reach.
    CounterEngineCsWaitMain,
    CounterEngineCsWaitMainUs,
    CounterEngineObjWaitMain,
    CounterEngineObjWaitMainUs,
    CounterEngineSleepMain,
    CounterEngineSleepMainUs,
    CounterEngineSleepMainReqUs,

    // Archived async-level experiment counters. They retain their CSV slots
    // so old runs remain readable; current builds never retarget those calls.
    CounterEngineAsyncLoad,
    CounterEngineAsyncSync,
    CounterEnginePortalAsyncLoad,
    CounterEnginePortalAsyncSync,
    // TerrainType's own semantic preload and draw entry points. These are
    // diagnostic engine work, so durations remain `_us`; the GPU ground
    // span below is the matching device-side interval.
    CounterEngineTerrainPreload,
    CounterEngineTerrainPreloadUs,
    CounterEngineTerrainPreloadTrue,
    CounterEngineTerrainPreloadFalse,
    CounterEngineTerrainPreloadTableOverflow,
    CounterEngineTerrainShaderParams,
    CounterEngineTerrainGrassParams,
    CounterEngineTerrainGround,
    CounterEngineTerrainGroundUs,
    CounterEngineTerrainRtLoad,
    CounterEngineTerrainRtLoadUs,
    CounterEngineTerrainRtLoadRender,
    CounterEngineTerrainRtLoadRenderUs,
    CounterEngineTerrainRtLoadRenderMain,
    CounterEngineTerrainRtLoadRenderMainUs,
    CounterEngineTerrainRtLoadRenderOther,
    CounterEngineTerrainRtLoadRenderOtherUs,
    CounterEngineTerrainRtLoadTextures,
    CounterEngineTerrainRtLoadTexturesUs,
    CounterEngineTerrainRtPreload,
    CounterEngineTerrainRtPreloadUs,
    CounterEngineTerrainRtPreloadLayers,
    CounterEngineTerrainRtLayerOverflow,
    CounterEngineTerrainPlug,
    CounterEngineTerrainPlugUs,
    CounterEngineTerrainBlock,
    CounterEngineTerrainBlockUs,
    // The direct children of GraphicsDeferredRendererX::Render, grouped into
    // six ordered top-level spans.  These are engine work, not mod work, so
    // CPU and Draw/DrawIndexed durations use the integer `_us` channel.  The
    // matching GPU spans below are timestamped without waiting for results.
    CounterEngineDeferredGeometry,
    CounterEngineDeferredGeometryUs,
    CounterEngineDeferredGeometryDrawUs,
    CounterEngineDeferredShadows,
    CounterEngineDeferredShadowsUs,
    CounterEngineDeferredShadowsDrawUs,
    CounterEngineDeferredLighting,
    CounterEngineDeferredLightingUs,
    CounterEngineDeferredLightingDrawUs,
    CounterEngineDeferredResolve,
    CounterEngineDeferredResolveUs,
    CounterEngineDeferredResolveDrawUs,
    CounterEngineDeferredLateScene,
    CounterEngineDeferredLateSceneUs,
    CounterEngineDeferredLateSceneDrawUs,
    CounterEngineDeferredPost,
    CounterEngineDeferredPostUs,
    CounterEngineDeferredPostDrawUs,
    // Run 70 proved the owner is invoked twice per frame.  These split the
    // two geometry children by exact call site and owner invocation.  The
    // six location blocks below then say where synchronous Resource and D3D
    // creation work happened: in either exact geometry child, or elsewhere
    // inside that invocation.  They are game/engine time and therefore stay
    // on the integer `_us` channel.
    CounterEngineDeferredOwner,
    CounterEngineDeferredOwnerOverflow,
    CounterEngineDeferredI1GeometrySetup,
    CounterEngineDeferredI1GeometrySetupUs,
    CounterEngineDeferredI1GeometrySetupDrawUs,
    CounterEngineDeferredI1GeometryScene,
    CounterEngineDeferredI1GeometrySceneUs,
    CounterEngineDeferredI1GeometrySceneDrawUs,
    CounterEngineDeferredI2GeometrySetup,
    CounterEngineDeferredI2GeometrySetupUs,
    CounterEngineDeferredI2GeometrySetupDrawUs,
    CounterEngineDeferredI2GeometryScene,
    CounterEngineDeferredI2GeometrySceneUs,
    CounterEngineDeferredI2GeometrySceneDrawUs,

    CounterEngineDeferredI1OtherResLoad,
    CounterEngineDeferredI1OtherResLoadUs,
    CounterEngineDeferredI1OtherTexCreate,
    CounterEngineDeferredI1OtherTexCreateUs,
    CounterEngineDeferredI1OtherBufCreate,
    CounterEngineDeferredI1OtherBufCreateUs,
    CounterEngineDeferredI1GeometrySetupResLoad,
    CounterEngineDeferredI1GeometrySetupResLoadUs,
    CounterEngineDeferredI1GeometrySetupTexCreate,
    CounterEngineDeferredI1GeometrySetupTexCreateUs,
    CounterEngineDeferredI1GeometrySetupBufCreate,
    CounterEngineDeferredI1GeometrySetupBufCreateUs,
    CounterEngineDeferredI1GeometrySceneResLoad,
    CounterEngineDeferredI1GeometrySceneResLoadUs,
    CounterEngineDeferredI1GeometrySceneTexCreate,
    CounterEngineDeferredI1GeometrySceneTexCreateUs,
    CounterEngineDeferredI1GeometrySceneBufCreate,
    CounterEngineDeferredI1GeometrySceneBufCreateUs,
    CounterEngineDeferredI2OtherResLoad,
    CounterEngineDeferredI2OtherResLoadUs,
    CounterEngineDeferredI2OtherTexCreate,
    CounterEngineDeferredI2OtherTexCreateUs,
    CounterEngineDeferredI2OtherBufCreate,
    CounterEngineDeferredI2OtherBufCreateUs,
    CounterEngineDeferredI2GeometrySetupResLoad,
    CounterEngineDeferredI2GeometrySetupResLoadUs,
    CounterEngineDeferredI2GeometrySetupTexCreate,
    CounterEngineDeferredI2GeometrySetupTexCreateUs,
    CounterEngineDeferredI2GeometrySetupBufCreate,
    CounterEngineDeferredI2GeometrySetupBufCreateUs,
    CounterEngineDeferredI2GeometrySceneResLoad,
    CounterEngineDeferredI2GeometrySceneResLoadUs,
    CounterEngineDeferredI2GeometrySceneTexCreate,
    CounterEngineDeferredI2GeometrySceneTexCreateUs,
    CounterEngineDeferredI2GeometrySceneBufCreate,
    CounterEngineDeferredI2GeometrySceneBufCreateUs,
    // Reflection rendering precedes each recursive deferred portal/region
    // owner. The manager is split by its first two branch invocations and
    // each manager's first two water planes. Resource and D3D creation clocks
    // are game work, so every duration stays on the integer `_us` channel.
    CounterEngineReflectionManager,
    CounterEngineReflectionManagerUs,
    CounterEngineReflectionManagerDrawUs,
    CounterEngineReflectionManagerResLoad,
    CounterEngineReflectionManagerResLoadUs,
    CounterEngineReflectionManagerTexCreate,
    CounterEngineReflectionManagerTexCreateUs,
    CounterEngineReflectionManagerBufCreate,
    CounterEngineReflectionManagerBufCreateUs,
    CounterEngineReflectionManagerOverflow,
    CounterEngineReflectionI1,
    CounterEngineReflectionI1Us,
    CounterEngineReflectionI1DrawUs,
    CounterEngineReflectionI2,
    CounterEngineReflectionI2Us,
    CounterEngineReflectionI2DrawUs,
    CounterEngineReflectionPlaneOverflow,
#define TQ_REFLECTION_CELL_COUNTERS(prefix) \
    CounterEngineReflection##prefix, \
    CounterEngineReflection##prefix##Us, \
    CounterEngineReflection##prefix##DrawUs, \
    CounterEngineReflection##prefix##ResLoad, \
    CounterEngineReflection##prefix##ResLoadUs, \
    CounterEngineReflection##prefix##TexCreate, \
    CounterEngineReflection##prefix##TexCreateUs, \
    CounterEngineReflection##prefix##BufCreate, \
    CounterEngineReflection##prefix##BufCreateUs, \
    CounterEngineReflection##prefix##BuildScene, \
    CounterEngineReflection##prefix##BuildSceneUs, \
    CounterEngineReflection##prefix##RenderLight, \
    CounterEngineReflection##prefix##RenderLightUs
    TQ_REFLECTION_CELL_COUNTERS(I1P1),
    TQ_REFLECTION_CELL_COUNTERS(I1P2),
    TQ_REFLECTION_CELL_COUNTERS(I2P1),
    TQ_REFLECTION_CELL_COUNTERS(I2P2),
#undef TQ_REFLECTION_CELL_COUNTERS
    // A behavior A/B, not a duration: a reflection BuildScene that creates
    // the transition-sized buffer population can defer resident mesh-instance
    // draws until the normal color pass has consumed that scene once.
    CounterEngineReflectionAdmissionDeferred,
    CounterEngineReflectionAdmissionMeshDeferred,
    CounterEngineReflectionAdmissionAllDeferred,
    // A coordinated behavior A/B: the first N previously unseen identities in
    // a frame issue their first reflection/directional-shadow draws, and N+1
    // self-arms deferral. Resource/material preparation still runs; these
    // counters contain no durations.
    CounterEngineSecondaryAdmissionTrigger,
    CounterEngineSecondaryAdmissionReflectionAdmitted,
    CounterEngineSecondaryAdmissionReflectionDeferred,
    CounterEngineSecondaryAdmissionShadowAdmitted,
    CounterEngineSecondaryAdmissionShadowDeferred,
    CounterEngineSecondaryAdmissionDrawSkipped,
    // Passive first-consumer identity. Each exact renderer records its draw
    // population plus the three major renderable classes; "First" means this
    // object identity has not previously reached that exact consumer.
#define TQ_ADMISSION_RENDER_COUNTERS(prefix) \
    CounterEngineAdmission##prefix##Draw, \
    CounterEngineAdmission##prefix##TerrainPlug, \
    CounterEngineAdmission##prefix##TerrainPlugFirst, \
    CounterEngineAdmission##prefix##TerrainBlock, \
    CounterEngineAdmission##prefix##TerrainBlockFirst, \
    CounterEngineAdmission##prefix##Mesh, \
    CounterEngineAdmission##prefix##MeshFirst
    TQ_ADMISSION_RENDER_COUNTERS(ReflectionI2P1),
    TQ_ADMISSION_RENDER_COUNTERS(DeferredI2Setup),
    TQ_ADMISSION_RENDER_COUNTERS(DeferredI2Scene),
    TQ_ADMISSION_RENDER_COUNTERS(ShadowDirectional),
#undef TQ_ADMISSION_RENDER_COUNTERS
    CounterEngineAdmissionIdentityOverflow,
    // Run 74 correlates newly created main-thread D3D buffers across the
    // exact reflection-plane, directional-shadow, and deferred-owner classes.
    // These are counts/bytes only; all draw clocks remain the existing sample.
    CounterEngineCrossPassBufferCreated,
    CounterEngineCrossPassBufferCreatedBytes,
    CounterEngineCrossPassReflectionDraw,
    CounterEngineCrossPassShadowDraw,
    CounterEngineCrossPassDeferredDraw,
    CounterEngineCrossPassFreshReflectionBuffer,
    CounterEngineCrossPassFreshShadowBuffer,
    CounterEngineCrossPassFreshDeferredBuffer,
    CounterEngineCrossPassJoinReflectionShadow,
    CounterEngineCrossPassJoinReflectionDeferred,
    CounterEngineCrossPassJoinShadowDeferred,
    CounterEngineCrossPassJoinAllThree,
    CounterEngineCrossPassIndexOverflow,
    CounterEngineCrossPassRecentEviction,
    CounterEngineShadowDirectionalDraw,
    // Sparse reflection draw-record subdivision. Run 78 counts past the
    // Run-77-excluded first 192 draws, then times draws 193--320 in sixteen
    // eight-draw bins. These are counts/ordinals, never CPU durations.
    CounterEngineGpuChunkReflectionArm,
    CounterEngineGpuChunkReflectionStartDraw,
    CounterEngineGpuChunkReflectionDraw,
    CounterEngineGpuChunkReflectionOverflow,
    CounterEngineGpuChunkReflectionCollision,
    // A human observation, not an inferred hitch class. With
    // [debug] stutter_marker=1, an F12 key-down returned by the game's message
    // pump marks the Present interval in which it was retrieved.
    CounterStutterMarker,
    CounterCount
};

// GPU regions, timed with timestamp queries. Non-blocking: results are read
// with D3D11_ASYNC_GETDATA_DONOTFLUSH several frames later and attributed to
// the frame they were opened in, so a frame's GPU columns may be blank.
//
// A region entered and left more than once in a frame -- the eight point-shadow
// passes -- measures from its first entry to its last exit, gaps included.
enum GpuPhase {
    GpuFrame,
    GpuShadowDirectional,
    GpuShadowPoint,
    GpuGrass,
    GpuSmaa,
    GpuBloom,
    GpuTerrainGround,
    GpuTerrainRtLoadRender,
    // One query pair for each geometry site in each of the two observed
    // owner invocations. Unlike Run 70's group pairs, none can span from the
    // first GraphicsDeferredRendererX::Render call into the second.
    GpuDeferredI1GeometrySetup,
    GpuDeferredI1GeometryScene,
    GpuDeferredI2GeometrySetup,
    GpuDeferredI2GeometryScene,
    GpuReflectionI1,
    GpuReflectionI2,
    GpuReflectionI1P1,
    GpuReflectionI1P2,
    GpuReflectionI2P1,
    GpuReflectionI2P2,
    GpuReflectionI1P1BuildScene,
    GpuReflectionI1P1RenderLight,
    GpuReflectionI1P2BuildScene,
    GpuReflectionI1P2RenderLight,
    GpuReflectionI2P1BuildScene,
    GpuReflectionI2P1RenderLight,
    GpuReflectionI2P2BuildScene,
    GpuReflectionI2P2RenderLight,
    GpuChunkReflection00,
    GpuChunkReflection01,
    GpuChunkReflection02,
    GpuChunkReflection03,
    GpuChunkReflection04,
    GpuChunkReflection05,
    GpuChunkReflection06,
    GpuChunkReflection07,
    GpuChunkReflection08,
    GpuChunkReflection09,
    GpuChunkReflection10,
    GpuChunkReflection11,
    GpuChunkReflection12,
    GpuChunkReflection13,
    GpuChunkReflection14,
    GpuChunkReflection15,
    GpuPhaseCount
};

// ---------------------------------------------------------------------------
namespace detail {
// Read once at install and never written again from the render thread, so the
// hot-path test is a plain load. Exposed only so enabled() can be inlined.
extern bool active;
// Folded with full-trace mode at readOptions so the hot path is one load rather
// than two. Same discipline as `active`: written once at install, read from
// the render thread thereafter.
extern bool drawTiming;
}

inline bool enabled() { return detail::active; }

// The marker is consumed from the game's existing WM_KEYDOWN path. Keeping
// this separate from enabled() lets the pump hook do no marker work unless the
// run asked for it, and avoids adding a second Win32 input poll every frame.
bool stutterMarkerEnabled();
void markStutter();

// True when the draw and map hooks should take a clock pair. Implies enabled().
inline bool drawTimingEnabled() { return detail::drawTiming; }

// Reads [debug] performance_trace from the INI beside the executable. Called
// once from the visual install, before anything else here. Taking the path
// rather than deriving it is what lets the off-game test drive this module.
void readOptions(const wchar_t* iniPath);

// Where the CSV is written. Defaults to tqflicker-frames.csv beside the
// executable; the off-game test redirects it. Must be called before the first
// completed frame, and only has an effect while enabled.
void setOutputPath(const wchar_t* csvPath);

// True when every frame is written, rather than only the frames that hitched.
bool logsEveryFrame();

// ---------------------------------------------------------------------------
// Timing.

// QueryPerformanceCounter, or 0 while disabled.
int64_t nowInternal();
inline int64_t now() { return detail::active ? nowInternal() : 0; }

// Adds the interval from `startTicks` to now into this frame's `phase` total.
// Phases and counters record the render thread only: the frame record is one
// unsynchronized struct, and the question the columns answer is where *this
// thread's* frame went -- a texture created concurrently on the game's loader
// thread is neither, and writing it here would tear the record.
void addPhaseInternal(Phase phase, int64_t startTicks);
inline void addPhase(Phase phase, int64_t startTicks) {
    if (detail::active) addPhaseInternal(phase, startTicks);
}

// The Draw hooks need the same completed interval both in draw_submit_ms and
// in the active engine-render-pass `_draw_us` bucket.  This variant takes the
// ending clock sample once, records the phase with full tick precision, and
// returns its duration for the integer engine channel.  It therefore adds no
// second QueryPerformanceCounter call to a path used thousands of times.
uint32_t finishPhaseInternal(Phase phase, int64_t startTicks);
inline uint32_t finishPhase(Phase phase, int64_t startTicks) {
    return detail::active ? finishPhaseInternal(phase, startTicks) : 0;
}

// Scoped form. Costs one predictable branch while the probe is off.
struct Scope {
    int64_t start;
    Phase phase;
    explicit Scope(Phase p) : start(enabled() ? now() : 0), phase(p) {}
    ~Scope() { if (start) addPhase(phase, start); }
};

// Counted on hooks that run thousands of times a frame, so the disabled path
// has to be a load and a branch rather than a call into another object file.
void countInternal(Counter counter, uint32_t amount);
inline void count(Counter counter, uint32_t amount = 1) {
    if (detail::active) countInternal(counter, amount);
}

// Whether the caller is the thread whose frames are being recorded. While
// enabled, true before a render thread has been learned; false when disabled.
bool isRenderThreadInternal();
inline bool isRenderThread() {
    return detail::active && isRenderThreadInternal();
}

// Index of the frame currently accumulating. The stutter-marker resource
// diagnostic uses it to retain pre-reaction events without logging on the
// slow frame itself. Meaningful only while the probe is enabled.
unsigned currentFrameIndexInternal();
inline unsigned currentFrameIndex() {
    return detail::active ? currentFrameIndexInternal() : 0;
}

// The counting channel for the game's own threads. Accumulates into an
// interlocked side array and folds into the frame record at endFrame, so the
// column, its median and the `unusual` attribution all work unchanged -- but
// unlike count() it cannot tear the record, because it never touches it.
//
// Legal for any counter, including the ones above CounterEngineFirst; a
// render-thread write lands in the frame that is closing, exactly as count()
// would. What is not legal is the reverse: nothing below CounterEngineFirst
// may be written with count(), or it silently reads zero.
void engineCountInternal(Counter counter, uint32_t amount);
inline void engineCount(Counter counter, uint32_t amount = 1) {
    if (detail::active) engineCountInternal(counter, amount);
}

// Elapsed microseconds since a now() reading, saturating at UINT32_MAX and 0
// while the probe is disabled. For the `_us` counters, whose whole point is to
// carry a duration through the integer channel.
uint32_t microsecondsSinceInternal(int64_t startTicks);
inline uint32_t microsecondsSince(int64_t startTicks) {
    return detail::active && startTicks
        ? microsecondsSinceInternal(startTicks) : 0;
}

// ---------------------------------------------------------------------------
// GPU regions. Created on the shader-build worker rather than the render
// thread, for the same reason the overlay's resources are: DXMT does not want
// device objects created re-entrantly from inside a hooked call.
bool createResources(ID3D11Device* device);
void releaseResources();
void gpuBeginInternal(ID3D11DeviceContext* context, GpuPhase phase);
void gpuEndInternal(ID3D11DeviceContext* context, GpuPhase phase);
inline void gpuBegin(ID3D11DeviceContext* context, GpuPhase phase) {
    if (detail::active) gpuBeginInternal(context, phase);
}
inline void gpuEnd(ID3D11DeviceContext* context, GpuPhase phase) {
    if (detail::active) gpuEndInternal(context, phase);
}
// Borrowed for the duration of a render-thread scope. Null off the thread that
// opened the frame, before beginFrame, or when timestamp resources are
// unavailable; callers must not retain it.
ID3D11DeviceContext* currentGpuContextInternal();
inline ID3D11DeviceContext* currentGpuContext() {
    return detail::active ? currentGpuContextInternal() : nullptr;
}

// Scoped form, so a function with several returns cannot leave a region open
// -- an unclosed region is never resolved and its column silently reads blank.
struct GpuScope {
    ID3D11DeviceContext* context;
    GpuPhase phase;
    GpuScope(ID3D11DeviceContext* c, GpuPhase p) : context(c), phase(p) {
        gpuBegin(context, phase);
    }
    ~GpuScope() { gpuEnd(context, phase); }
};

// ---------------------------------------------------------------------------
// Frame boundary. beginFrame collects whatever GPU results have landed and
// opens the whole-frame region; endFrame closes the record with the frame time
// the overlay measured, decides whether it hitched, and queues a row.
void beginFrameInternal(ID3D11DeviceContext* context);
inline void beginFrame(ID3D11DeviceContext* context) {
    if (detail::active) beginFrameInternal(context);
}
// Closes the whole-frame GPU region on the context beginFrame was given, so it
// needs no context of its own.
void endFrameInternal(float cpuFrameMilliseconds);
inline void endFrame(float cpuFrameMilliseconds) {
    if (detail::active) endFrameInternal(cpuFrameMilliseconds);
}

// The three phases with the largest recent mean, and the phase that dominated
// the most recent hitch, formatted for the overlay panel. Writes an empty
// string when there is nothing to report.
void summarize(char* out, size_t size);

// Flushes and stops the writer. Safe to call when disabled or never started.
void shutdown();

// Writes whatever the ring still holds, without touching the writer thread or
// any lock. For the process-termination path only, where every other thread is
// already gone and shutdown() is never reached: Titan Quest exits without an
// explicit unload, so this is what saves the tail of a run.
void flushOnExit();

#ifdef TQ_SELFTEST
// Counts entries into the out-of-line recorder, including clock/thread/GPU
// helpers. Disabled inline gates must leave this unchanged.
unsigned runtimeEntriesForTest();
// The frame the ring is about to write, so a test can drive frames and then
// assert on what was recorded without waiting for the writer.
unsigned frameCountForTest();
float phaseMillisecondsForTest(unsigned framesBack, Phase phase);
// Whether that frame's timestamp queries came back, so a test can assert the
// GPU side actually reports rather than merely not crashing.
bool gpuResolvedForTest(unsigned framesBack);
uint32_t counterForTest(unsigned framesBack, Counter counter);
float phaseForTest(unsigned framesBack, Phase phase);
// The async sink must batch rows through one persistent file handle. This
// exposes that invariant without making the production path observable.
unsigned logFileOpensForTest();
void resetForTest();
#endif

}  // namespace probe
}  // namespace tq
