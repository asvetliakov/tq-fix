#include "engine_internal.h"

namespace tq { namespace engine { namespace detail {

int __fastcall hookPortalLoadLevel(void* self, void* edx, int background);


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

BackgroundLoadLevelFn g_backgroundLoadLevel;

// Region::LoadLevel and Region::BackgroundLoadLevel as the module exports
// them, which is not the same thing as g_loadLevel above. g_loadLevel is the
// trace's trampoline and exists only when the loads group is installed;
// these two are resolved addresses and work with the probe off. Calling the
// export means that when the trace *is* installed the call still lands in
// hookLoadLevel and is counted, which is what we want.
LoadLevelFn g_regionLoadLevel;


// The three deferral thunks, declared here because the site table below names
// them and they are defined with the other hooks.
int __fastcall hookAddElementsLoadLevel(void* self, void* edx, int background);

LONG g_installed;
unsigned g_installedHooks;

const volatile DWORD* g_mainThreadId;

bool onMainThread() {
    return g_mainThreadId && *g_mainThreadId == GetCurrentThreadId();
}

const BYTE* g_engineBase;

// One thunk per site, so each gets its own pair of columns. They are three
// instructions each; what they exist for is to name which site deferred.
int __fastcall hookAddElementsLoadLevel(void* self, void* edx, int background) {
    return deferLoad(self, edx, background, tq::probe::CounterEngineAsyncLoad,
                     tq::probe::CounterEngineAsyncSync);
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

Signature signature(const BYTE* bytes, SIZE_T length,
                    const Relocation* relocations,
                    unsigned relocationCount) {
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
} } }

namespace tq { namespace engine {
using namespace tq::engine::detail;


void readOptions(const wchar_t* iniPath) {
    // No INI is the shipping configuration, and the shipping configuration has
    // the probe off, so the default here only decides what a trace run gets.
    g_traceMask = iniPath && iniPath[0]
        ? (unsigned)GetPrivateProfileIntW(L"debug", L"engine_trace", 1, iniPath)
        : 1u;
    g_pumpLastFullTick = (LONG)GetTickCount();
    readShadowOptions(iniPath);
    readTerrainOptions(iniPath);
    readSecondaryOptions(iniPath);
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
    // archive_cache_mb, shadow_defer_cold_resources,
    // shadow_defer_cold_actor_pose, terrain_preload_layers, and
    // secondary_pass_admission_budget add independent ways in
    // that install their own hooks and no instrumentation -- because they are
    // game-behaviour changes and have to work on a boot with the probe off.
    // wants() below refuses every trace group when g_tracing is false, so
    // none of them brings the rest of the instrument along. The marker also
    // installs only the existing PeekMessage import wrapper.
    const bool cache = tq::arccache::configured();
    const bool shadowActorPose = g_shadowDeferColdActorPose;
    // The earlier Actor boundary depends on the exact later root-caster gate;
    // requesting it therefore installs the same complete accepted patch set.
    const bool shadowDefer = g_shadowDeferColdResources || shadowActorPose;
    const bool terrainPreload = g_terrainPreloadLayers;
    const bool secondaryAdmission = g_secondaryPassAdmissionBudget != 0;
    const bool marker = tq::probe::stutterMarkerEnabled();
    decideTracing();
    const bool crossPass = wants(kGroupReflections)
                        && tq::probe::drawTimingEnabled();
    const bool gpuChunks = wants(kGroupReflections) && wants(kGroupTerrain)
                        && tq::probe::drawTimingEnabled();
    if (!g_tracing && !cache && !shadowDefer && !terrainPreload
        && !secondaryAdmission
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
    if (g_tracing) {
        g_chainModuleCount = 0;
        addChainModule(engine, kEngineImageSize, 'E', "Engine.dll");
        addChainModule(GetModuleHandleW(L"Game.dll"), kGameImageSize, 'G',
                       "Game.dll");
        addChainModule(GetModuleHandleW(nullptr), kExecutableImageSize, 'T',
                       "TQ.exe");
    }

    const volatile DWORD* mainThread =
        (const volatile DWORD*)((BYTE*)engine + kMainThreadIdRva);
    g_mainThreadId = tq::detour::readable((const void*)mainThread,
                                          sizeof(DWORD)) ? mainThread : nullptr;

    g_installedHooks = 0;

    g_terrainPreloadLayersActive = false;
    g_secondaryPassAdmissionActive = false;
    g_secondaryAdmissionArmed = false;
    g_insideReflectionRenderLight = false;
    g_secondaryAdmissionFrameSerial = 0;
    g_secondaryAdmissionBudgetFrame = UINT_MAX;
    g_secondaryAdmissionUsedThisFrame = 0;
    InterlockedExchange(&tq::secondaryadmission::detail::secondaryAdmissionDrawSuppressDepth, 0);
    memset(g_admissionRenderableIdentities, 0,
           sizeof(g_admissionRenderableIdentities));
    if (g_tracing) resetEngineTraceState();
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
    if (tracePump || marker) installPump(engine, tracePump);
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
    if (traceShadow || shadowDeferReady || crossPass || secondaryAdmission)
        shadowReady = installShadow(engine, traceShadow);
    if (wants(kGroupDeferredPasses) || crossPass)
        installDeferredPasses(engine);
    const bool traceReflections = wants(kGroupReflections);
    bool reflectionReady = !secondaryAdmission;
    if (traceReflections || secondaryAdmission)
        reflectionReady = installReflections(
            engine, traceReflections, false, false, secondaryAdmission);
    g_secondaryPassAdmissionActive = secondaryAdmission
        && g_secondaryAdmissionDrawHooksReady && terrainReady
        && shadowReady && reflectionReady;
    if (secondaryAdmission && !g_secondaryPassAdmissionActive)
        tq::hdr::log("Secondary-pass progressive admission unavailable --"
                     " leaving all draws stock\r\n");
    // Keep the withdrawn patch path build-checked and byte-verified for the
    // historical record. Its request is a compile-time false constant and no
    // INI key can make this call reachable.
    if (g_asyncLevelLoad) installAsyncLoad(engine);
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

    tq::hdr::log("Engine trace: %s, mask=0x%x, cache %s,"
                 " cold-resource shadow defer %s, cold actor-pose defer %s,"
                 " terrain layer preload %s,"
                 " secondary-pass admission budget %u,"
                 " hooks=%u, main thread id at %p\r\n",
                 g_tracing ? "on" : "off", g_traceMask,
                 cache ? "requested" : "off",
                 shadowDefer ? "requested" : "off",
                 shadowActorPose ? "requested" : "off",
                 terrainPreload ? "requested" : "off",
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
    InterlockedExchange(&tq::secondaryadmission::detail::secondaryAdmissionDrawSuppressDepth, 0);
    g_reflectionAdmissionBuildActive = false;
    g_reflectionAdmissionBuildBuffers = 0;
    g_reflectionAdmissionPending = false;
    g_reflectionAdmissionRenderActive = false;
    g_crossPassTracing = false;
    g_gpuChunkTracing = false;
    InterlockedExchange(&tq::engineprobe::detail::gpuChunkDrawActive, 0);
    g_activeGpuChunkRenderableCall = nullptr;
    InterlockedExchange(&g_deferredPass, DeferredPassNone);
    InterlockedExchange(&g_deferredGeometrySite, DeferredGeometrySiteNone);
    InterlockedExchange(&g_deferredOwnerInvocation, 0);
    InterlockedExchange(&g_reflectionManagerInvocation, 0);
    InterlockedExchange(&g_reflectionPlaneInvocation, 0);
    InterlockedExchange(&g_reflectionChild, 0);
    if (g_tracing) {
        reportMessages();
        reportSlowLoads();
    }
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
} }
