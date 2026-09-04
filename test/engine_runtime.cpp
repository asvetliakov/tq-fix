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

namespace {
unsigned gameSequence, gameBodyOrder, gameCallbackOrder;
void* gameSelf;
int gameDelta;
void __fastcall mockGameBody(void* self, void*, int delta) {
    gameSelf = self;
    gameDelta = delta;
    gameBodyOrder = ++gameSequence;
}
void __cdecl mockGameCallback() { gameCallbackOrder = ++gameSequence; }
}

bool exerciseGameUpdateCompatibilityForTest() {
    using namespace detail;
    BYTE* image = (BYTE*)VirtualAlloc(nullptr, kGameHekImageSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!image) return false;
    HMODULE module = (HMODULE)image;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)image;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(image + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    nt->OptionalHeader.SizeOfImage = kGameHekImageSize;
    nt->FileHeader.NumberOfSections = 7;
    IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    memcpy(sections[0].Name, ".text", 5);
    sections[0].VirtualAddress = 0x1000;
    sections[0].Misc.VirtualSize = 0x31d77b;
    memcpy(sections[5].Name, ".rxHekTo", 8);
    sections[5].VirtualAddress = 0x59a000;
    sections[5].Misc.VirtualSize = 0x1000;
    sections[5].Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;
    memcpy(sections[6].Name, ".rwHekTo", 8);
    sections[6].VirtualAddress = 0x59b000;
    sections[6].Misc.VirtualSize = 0x1000;
    sections[6].Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
    BYTE* entry = image + kGameUpdateRva;
    memcpy(entry, kGameHekUpdateBytes, sizeof(kGameHekUpdateBytes));
    *(DWORD*)(entry + 9) = (DWORD)(image + kGameUpdateRelocs[0].rva);
    memcpy(image + kGameHekWrapperRva, kGameHekWrapperBytes,
           sizeof(kGameHekWrapperBytes));
    memcpy(image + kGameHekTrampolineRva, kGameHekTrampolineBytes,
           sizeof(kGameHekTrampolineBytes));
    *(void**)(image + 0x59b00c) = (void*)&mockGameCallback;

    // Complete the verified sub esp,0x470, then substitute a tiny original
    // body. Execute the real wrapper/trampoline at a relocated module base:
    // this catches an E9 copied without relocation, recursion, stack/this/
    // delta corruption, or a lost/reordered third-party callback.
    const BYTE tail[] = {0, 0, 0x8b, 0xe5, 0x5d, 0xb8, 0, 0, 0, 0, 0xff, 0xe0};
    memcpy(entry + sizeof(kGameHekUpdateBytes), tail, sizeof(tail));
    *(DWORD*)(entry + sizeof(kGameHekUpdateBytes) + 6) = (DWORD)&mockGameBody;
    BYTE before[sizeof(kGameHekUpdateBytes)];
    memcpy(before, entry, sizeof(before));
    bool ok = auditedGameImage(module);
    entry[1] ^= 1;
    ok &= !installGameUpdateAt(module, entry) && !g_gameUpdate;
    entry[1] ^= 1;
    image[kGameHekWrapperRva + 7] ^= 1;
    ok &= !installGameUpdateAt(module, entry) && !g_gameUpdate;
    image[kGameHekWrapperRva + 7] ^= 1;
    image[kGameHekTrampolineRva + 7] ^= 1;
    ok &= !installGameUpdateAt(module, entry) && !g_gameUpdate;
    image[kGameHekTrampolineRva + 7] ^= 1;
    entry[9] ^= 1;
    ok &= !auditedGameImage(module);
    entry[9] ^= 1;
    sections[5].Characteristics &= ~IMAGE_SCN_MEM_EXECUTE;
    ok &= !auditedGameImage(module);
    sections[5].Characteristics |= IMAGE_SCN_MEM_EXECUTE;
    nt->OptionalHeader.SizeOfImage += 0x1000;
    ok &= !auditedGameImage(module);
    nt->OptionalHeader.SizeOfImage -= 0x1000;
    ok &= !installGameUpdateAt(module, entry + 1);
    ok &= memcmp(before, entry, sizeof(before)) == 0;

    for (unsigned variant = 0; variant < 2; ++variant) {
        if (variant) {
            nt->OptionalHeader.SizeOfImage = kGameImageSize;
            nt->FileHeader.NumberOfSections = 5;
            memcpy(entry, kGameUpdateBytes, sizeof(kGameUpdateBytes));
            *(DWORD*)(entry + 9) = (DWORD)(image + kGameUpdateRelocs[0].rva);
            memcpy(before, entry, sizeof(before));
        }
        gameSequence = gameBodyOrder = gameCallbackOrder = 0;
        gameSelf = nullptr;
        gameDelta = 0;
        bool installed = installGameUpdateAt(module, entry);
        ok &= installed;
        if (installed) {
            ((GameUpdateFn)entry)(image + 0x500, nullptr, 37);
            ok &= gameSelf == image + 0x500 && gameDelta == 37
                && gameBodyOrder == 1
                && gameCallbackOrder == (variant ? 0u : 2u);
        }
        tq::detour::detach(g_gameUpdateDetour);
        g_gameUpdate = nullptr;
        ok &= memcmp(before, entry, sizeof(before)) == 0;
        ok &= memcmp(image + kGameHekWrapperRva, kGameHekWrapperBytes,
                     sizeof(kGameHekWrapperBytes)) == 0;
        ok &= *(void**)(image + 0x59b00c) == (void*)&mockGameCallback;
    }
    VirtualFree(image, 0, MEM_RELEASE);
    return ok;
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
