#include "resource_trace.h"
#include "engine_internal.h"

namespace tq { namespace secondaryadmission { namespace detail {
volatile LONG secondaryAdmissionDrawSuppressDepth = 0;
} } }

namespace tq { namespace engine { namespace detail {
struct SecondaryAdmissionDrawScope {
    bool active;
    SecondaryAdmissionDrawScope(GpuChunkRenderableKind kind,
                                const void* object)
        : active(shouldDeferSecondaryAdmission(kind, object)) {
        if (active)
            InterlockedIncrement(
                &tq::secondaryadmission::detail::secondaryAdmissionDrawSuppressDepth);
    }
    ~SecondaryAdmissionDrawScope() {
        if (active)
            InterlockedDecrement(
                &tq::secondaryadmission::detail::secondaryAdmissionDrawSuppressDepth);
    }
};

Detour g_graphicsMeshInstanceRenderPassDetour;

Detour g_terrainBlockRenderDetour;

Detour g_terrainPlugRenderDetour;

CallPatch g_reflectionRenderLightPatch;

ReflectionRenderLightFn g_reflectionRenderLight;

GraphicsMeshInstanceRenderPassFn g_graphicsMeshInstanceRenderPass;

TerrainColourRenderFn g_terrainBlockRender;

TerrainColourRenderFn g_terrainPlugRender;

void readSecondaryOptions(const wchar_t* iniPath) {
    // A positive value is an object-identity budget, not a
    // millisecond target, so the same recorded population is treated the same
    // on native Windows and Wine. Values above the audited bound are refused
    // rather than silently turning this into effectively unbounded admission.
    const int secondaryBudget = iniPath && iniPath[0]
        ? GetPrivateProfileIntW(L"performance",
                                L"secondary_pass_admission_budget", 8,
                                iniPath)
        : 8;
    g_secondaryPassAdmissionBudget = secondaryBudget > 0
        && secondaryBudget <= (int)kSecondaryPassAdmissionBudgetMax
        ? (unsigned)secondaryBudget : 0u;
}


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

SecondaryAdmissionContext currentSecondaryAdmissionContext() {
    if (!g_secondaryPassAdmissionActive || !onMainThread())
        return SecondaryAdmissionContextNone;
    if (g_insideReflectionRenderLight)
        return SecondaryAdmissionContextReflection;
    if (InterlockedCompareExchange(&g_insideDirectional, 0, 0) > 0)
        return SecondaryAdmissionContextShadow;
    return SecondaryAdmissionContextNone;
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
        if (g_tracing) countSecondaryAdmission(context, true);
        return false;
    }
    // The first identity beyond the frame budget is the transition signal.
    // This observes the exact population being controlled and needs neither
    // a reflection nor a change between two non-null shadow-region pointers.
    armSecondaryAdmission();
    entry->secondaryState = SecondaryAdmissionPending;
    if (g_tracing) countSecondaryAdmission(context, false);
    return true;
}

void __fastcall hookReflectionRenderLight(
    void* self, void* edx, uintptr_t canvas, uintptr_t light,
    uintptr_t styleName, uintptr_t flags) {
    if (!g_tracing) {
        const bool prior = g_insideReflectionRenderLight;
        g_insideReflectionRenderLight = true;
        if (g_reflectionRenderLight)
            g_reflectionRenderLight(self, edx, canvas, light, styleName, flags);
        g_insideReflectionRenderLight = prior;
        return;
    }
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

void __fastcall hookTerrainPlugRender(
    void* self, void* edx, const void* a, const void* b, const void* c,
    const void* d) {
    if (!g_terrainPlugRender) return;
    if (!g_tracing) {
        SecondaryAdmissionDrawScope secondaryAdmission(GpuChunkTerrainPlug, self);
        g_terrainPlugRender(self, edx, a, b, c, d);
        return;
    }
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
    if (!g_tracing) {
        SecondaryAdmissionDrawScope secondaryAdmission(GpuChunkTerrainBlock, self);
        g_terrainBlockRender(self, edx, a, b, c, d);
        return;
    }
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
    if (!g_tracing) {
        SecondaryAdmissionDrawScope secondaryAdmission(GpuChunkMeshInstance, self);
        g_graphicsMeshInstanceRenderPass(
            self, edx, pass, name, canvas, sceneRenderer);
        return;
    }
    tq::resourcetrace::RenderScope lifecycle(self);
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
    const bool buildOk = planeOk && (!trace || tq::detour::patchCall(
        g_reflectionBuildScenePatch, engine,
        base + kReflectionBuildSceneCallWindowRva,
        signature(kReflectionBuildSceneCallWindowBytes,
                  sizeof(kReflectionBuildSceneCallWindowBytes)),
        kReflectionBuildSceneCallOffset, buildScene,
        (const void*)&hookReflectionBuildScene));
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
} } }


namespace tq { namespace secondaryadmission {
using namespace tq::engine::detail;


void secondaryAdmissionFrameBoundary() {
    if (!g_secondaryPassAdmissionActive) return;
    ++g_secondaryAdmissionFrameSerial;
}


bool secondaryPassAdmissionRequested() {
    return g_secondaryPassAdmissionBudget != 0;
}


void setSecondaryAdmissionDrawHooksReady(bool ready) {
    g_secondaryAdmissionDrawHooksReady = ready;
}
} }
