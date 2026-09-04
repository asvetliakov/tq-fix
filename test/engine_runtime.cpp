#include "engine_internal.h"

namespace tq { namespace engine {
namespace detail {
void __fastcall hookTerrainRtLoadTextures(void*, void*);
void __fastcall hookGraphicsMeshInstanceRenderPass(
    void*, void*, const void*, const void*, void*, const void*);
void __fastcall hookReflectionRenderLight(
    void*, void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
int __fastcall hookRenderDirectional(
    void*, void*, void*, const void*, const void*, int, void*, void*);
int __fastcall hookShadowMeshPassCount(void*, void*);
void __fastcall hookShadowActorUpdateMeshInstance(void*, void*);
}
namespace {
using namespace detail;
unsigned loads, preloads, draws, suppressed, queued, meshCalls, actorCalls;
bool preloadOrdered;
int objects[3];

void __fastcall loadTextures(void*, void*) { ++loads; }
void __fastcall preload(void*, void*, int textures) {
    ++preloads;
    preloadOrdered = loads == 1 && textures == 1;
}
void draw() {
    if (tq::secondaryadmission::secondaryAdmissionDrawSuppressed()) {
        ++suppressed;
        tq::secondaryadmission::noteSecondaryAdmissionDrawSkipped();
    } else ++draws;
}
void __fastcall renderMesh(void*, void*, const void*, const void*, void*, const void*) { draw(); }
void __fastcall renderTerrain(void*, void*, const void*, const void*, const void*, const void*) { draw(); }
void renderPopulation() {
    hookGraphicsMeshInstanceRenderPass(&objects[0], nullptr, nullptr, nullptr, nullptr, nullptr);
    hookTerrainPlugRender(&objects[1], nullptr, nullptr, nullptr, nullptr, nullptr);
    hookTerrainBlockRender(&objects[2], nullptr, nullptr, nullptr, nullptr, nullptr);
}
void __fastcall reflect(void*, void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t) { renderPopulation(); }
int __fastcall directional(void*, void*, void*, const void*, const void*, int, void*, void*) {
    renderPopulation();
    return 1;
}
void* __fastcall loader(void* resource, void*) { return resource; }
void __fastcall enqueue(void*, void*, const void* resource, int priority, int notify, int immediate) {
    if (priority == 1 && notify == 1 && immediate == 0) ++queued;
    *(unsigned*)((BYTE*)resource + kResourceLoadedStateOffset) = 1;
}
int __fastcall meshPasses(void*, void*) { ++meshCalls; return 4; }
void __fastcall actorUpdate(void*, void*) { ++actorCalls; }
}

bool exerciseTraceOffHooksForTest() {
    using namespace detail;
    if (tq::probe::enabled()) return false;
    loads = preloads = draws = suppressed = queued = meshCalls = actorCalls = 0;
    preloadOrdered = false;
    const unsigned probeBefore = tq::probe::runtimeEntriesForTest();
    const unsigned engineBefore = tq::engineprobe::runtimeEntriesForTest();
    g_tracing = g_shadowTracing = g_terrainTracing = false;
    // Exercise the public disabled gates as well as the actual shared hooks.
    tq::probe::beginFrame(nullptr);
    tq::probe::endFrame(16);
    tq::probe::gpuBegin(nullptr, tq::probe::GpuShadowDirectional);
    tq::probe::gpuEnd(nullptr, tq::probe::GpuShadowDirectional);
    const bool disabled = tq::probe::now() == 0
        && tq::probe::microsecondsSince(1) == 0
        && tq::probe::currentFrameIndex() == 0
        && !tq::probe::currentGpuContext() && !tq::probe::isRenderThread();

    g_terrainRtLoadTextures = &loadTextures;
    g_terrainPreloadEntry = &preload;
    g_terrainPreloadLayersActive = true;
    hookTerrainRtLoadTextures(nullptr, nullptr);

    tq::engineprobe::resetSecondaryAdmissionForTest(2, false);
    g_graphicsMeshInstanceRenderPass = &renderMesh;
    g_terrainPlugRender = g_terrainBlockRender = &renderTerrain;
    g_reflectionRenderLight = &reflect;
    g_renderDirectional = &directional;
    hookReflectionRenderLight(nullptr, nullptr, 0, 0, 0, 0);
    hookRenderDirectional(nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr);
    tq::secondaryadmission::secondaryAdmissionFrameBoundary();
    hookRenderDirectional(nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr);
    const bool admission = draws == 7 && suppressed == 2
        && !tq::secondaryadmission::secondaryAdmissionDrawSuppressed();

    // The two cold-root boundaries must queue and omit without asking any
    // observer to classify/time the call, then return to stock when resident.
    DWORD resource[32] = {};
    void* instance[16] = {};
    instance[kGraphicsMeshResourceOffset / sizeof(void*)] = resource;
    void* actor[kActorMeshInstanceOffset / sizeof(void*) + 1] = {};
    actor[kActorMeshInstanceOffset / sizeof(void*)] = instance;
    g_resourceStateVerified = true;
    g_shadowDeferActive = g_shadowActorPoseDeferActive = true;
    g_resourceLoaderAccessor = &loader;
    g_shadowEnqueue = &enqueue;
    g_shadowMeshPassCount = &meshPasses;
    g_actorUpdateMeshInstance = &actorUpdate;
    g_insideDirectional = 1;
    const int cold = hookShadowMeshPassCount(instance, nullptr);
    resource[kResourceLoadedStateOffset / sizeof(DWORD)] = 2;
    const int resident = hookShadowMeshPassCount(instance, nullptr);
    resource[kResourceLoadedStateOffset / sizeof(DWORD)] = 0;
    hookShadowActorUpdateMeshInstance(actor, nullptr);
    resource[kResourceLoadedStateOffset / sizeof(DWORD)] = 2;
    hookShadowActorUpdateMeshInstance(actor, nullptr);
    g_insideDirectional = 0;
    const bool shadows = cold == 0 && resident == 4 && queued == 2
        && meshCalls == 1 && actorCalls == 1;
    const bool noProbe = probeBefore == tq::probe::runtimeEntriesForTest()
        && engineBefore == tq::engineprobe::runtimeEntriesForTest();
    // No game sites were patched. Clear mock pointers before subsequent tests.
    shutdown();
    return disabled && preloadOrdered && loads == 1 && preloads == 1
        && admission && shadows && noProbe;
}
} }
