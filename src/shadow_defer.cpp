#include "engine_internal.h"

namespace tq { namespace engine { namespace detail {
MeshShadowStyleFn g_meshShadowStyle;
MeshGetTextureFn g_meshGetTexture;

ShadowMeshParameterContext g_shadowMeshParameterContext;

bool g_shaderHasParameterVerified;

bool g_resourceStateVerified;

LONG g_insideDirectional;

CallPatch g_shadowRecordPatch;

CallPatch g_shadowInstanceBumpEnsurePatch;

CallPatch g_shadowMeshParameterPatch;

CallPatch g_shadowMaterialTexturePatch;

CallPatch g_shadowActorUpdateMeshPatch;

CallPatch g_shadowDirectionalPatch;

Detour g_shadowMeshPassCountDetour;

RenderDirectionalFn g_renderDirectional;

EnqueueFn g_shadowEnqueue;

ResourceLoaderAccessorFn g_resourceLoaderAccessor;

BuildShadowRecordFn g_buildShadowRecord;

ShaderHasParameterFn g_shaderHasParameter;

GraphicsMeshSetShaderParametersFn g_graphicsMeshSetShaderParameters;

GraphicsTextureGetTextureFn g_graphicsTextureGetTexture;

ActorUpdateMeshInstanceFn g_actorUpdateMeshInstance;

ShadowMeshPassCountFn g_shadowMeshPassCount;

EnsureAvailableFn g_ensureAvailable;

void readShadowOptions(const wchar_t* iniPath) {
    // The accepted directional-shadow mitigation is enabled by default. It
    // covers cold root meshes for opaque and alpha-tested casters, cold alpha
    // base textures, and texture inputs absent from the active shadow shader.
    g_shadowDeferColdResources = !iniPath || !iniPath[0]
        || GetPrivateProfileIntW(L"performance",
                                 L"shadow_defer_cold_resources", 1,
                                 iniPath) != 0;
    // Moves the same cold-root decision earlier for the exact
    // Actor::AddToScene -> Actor::UpdateMeshInstance call proved by Run 68.
    // Enabling it implies the later complete shadow-defer patch set, because
    // the early skip is safe only when the cold renderable is then omitted.
    g_shadowDeferColdActorPose = !iniPath || !iniPath[0]
        || GetPrivateProfileIntW(L"performance",
                                 L"shadow_defer_cold_actor_pose", 1,
                                 iniPath) != 0;
}


// [performance] shadow_defer_cold_resources. Exact GraphicsMeshInstance casters
// whose root mesh is not resident are omitted before their pass count is read;
// alpha-tested casters whose base texture is not resident are omitted later at
// the record boundary. State-0 dependencies are explicitly handed to the
// engine's loader and return after reaching state 2. Colour rendering and
// resident casters are untouched.
bool g_shadowDeferColdResources;
bool g_shadowDeferActive;
// [performance] shadow_defer_cold_actor_pose. Run 68 resolves a still-earlier
// synchronous root-mesh dependency in the exact Actor::AddToScene class.
// When enabled, state-0 roots are queued and Actor::UpdateMeshInstance is
// deferred for this directional gather only. The later root-caster gate above
// then omits the not-yet-resident renderable. This option implies the complete
// accepted shadow-defer patch set and, like it, installs no trace group.
bool g_shadowDeferColdActorPose;
bool g_shadowActorPoseDeferActive;

extern "C" void* __cdecl shadowMaterialTextureFiltered(
    void* texture, const void* name, void* shader, const void* outerCaller) {
    if (!g_graphicsTextureGetTexture) return nullptr;
    const bool inShadow = onMainThread()
        && InterlockedCompareExchange(&g_insideDirectional, 0, 0) > 0
        && g_resourceStateVerified;
    const bool cold = g_shadowTracing && inShadow && texture
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
    if (!g_shadowTracing)
        return g_graphicsTextureGetTexture(texture, nullptr);
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
    const bool cold = g_shadowTracing && inShadow && texture && g_resourceStateVerified
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

bool shadowActorPoseQueueConfirmed(unsigned state, bool inQueue) {
    // A state-0 root is safe to omit only while it has an observable queue
    // owner. State 1 is the loader's in-progress state. State 2 must run the
    // pose update immediately; admitting a resident caster after skipping its
    // update would be worse than the synchronous fallback.
    return state == 1 || (state == 0 && inQueue);
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
            if (g_shadowTracing) countShadowActorPoseEnqueueFailure();
            g_actorUpdateMeshInstance(self, edx);
            return;
        }
    }
    if (g_shadowTracing) countDeferredShadowActorPose(state, enqueued, false);
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
    if (g_shadowTracing) countDeferredShadowMesh(state, enqueued, failed);
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
                    if (g_shadowTracing)
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

int __fastcall hookRenderDirectional(
    void* self, void* edx, void* canvas, const void* camera,
    const void* frustum, int algorithm, void* surface, void* matrix) {
    if (!g_renderDirectional) return 0;
    if (!g_tracing) {
        // Only the scope required by the two behavior features is active.
        InterlockedIncrement(&g_insideDirectional);
        const int result = g_renderDirectional(self, edx, canvas, camera,
                                               frustum, algorithm, surface, matrix);
        InterlockedDecrement(&g_insideDirectional);
        return result;
    }

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
    if (g_shadowTransitionReuse) rememberShadow(surface, matrix, result);
    return result;
}

// Resolve and verify every function the cold-resource fix calls before any entry
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
    if (ok && (g_shadowDeferColdResources || g_shadowDeferColdActorPose)) {
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
} } }
