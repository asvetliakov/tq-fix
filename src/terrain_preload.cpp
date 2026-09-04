#include "engine_internal.h"

namespace tq { namespace engine { namespace detail {

void readTerrainOptions(const wchar_t* iniPath) {
    // Runtime TerrainRT creates layer texture Resources but omits the stock
    // TerrainType semantic preload that would queue them. This fix invokes
    // that stock non-blocking path at the exact post-LoadTextures boundary.
    g_terrainPreloadLayers = !iniPath || !iniPath[0]
        || GetPrivateProfileIntW(L"performance", L"terrain_preload_layers", 1,
                                 iniPath) != 0;
}


// [performance] terrain_preload_layers. After runtime LoadRenderData creates
// one TerrainType's texture Resources, call the engine's own semantic
// PreLoad(true) so those resources enter the ordinary background queue before
// first colour use. This is a fix, not an instrument.
bool g_terrainPreloadLayers;
bool g_terrainPreloadLayersActive;
// The exported entry, retained separately from g_terrainPreload's trace
// trampoline. Calling this makes behavior invocations visible to the trace
// when its entry detour is installed, and calls stock code directly otherwise.
TerrainPreloadFn g_terrainPreloadEntry;
TerrainTypeLoadTexturesFn g_terrainRtLoadTextures;
CallPatch g_terrainRtLoadTexturesPatch;

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
} } }
