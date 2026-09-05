#include "mesh_preload.h"
#include "engine_internal.h"

// Refresh renewed, resident preload interest before the native 800-frame idle
// eviction window. Permit renewed queue requests after automatic age eviction;
// keep native loading, eviction ages and memory budgets intact.
namespace tq { namespace meshpreload { namespace {
using namespace tq::engine::detail;
constexpr unsigned kRefreshAge = 400, kRefreshBudget = 8;
bool g_requested = true, g_active;
unsigned g_budgetFrame, g_used, g_refreshed, g_deferred;
const BYTE* g_engine;
tq::detour::CallPatch g_patch;
BYTE* g_idleCooldown[2];
typedef bool (__fastcall* EntityFn)(void*, void*, int, int);
EntityFn g_entity;

// Starts AFTER the seven bytes borrowed by the optional Actor trace detour.
// Thus one native call-site owner can coexist with the entry observer.
const BYTE kActorWindow[] = {
    0xff,0x74,0x24,0x0c,0xe8,0x40,0x31,0x03,0x00,0x84,0xc0,0x75,
    0x04,0x5e,0xc2,0x08,0x00,0xff,0x74,0x24,0x0c,0x8b,0x8e,0x84,0x01,0x00,0x00
};
const BYTE kEntityHead[] = {
    0x56,0x8b,0xf1,0x8b,0x86,0xe8,0x00,0x00,0x00,0x85,0xc0,0x7e,
    0x0a,0x2b,0x44,0x24,0x08,0x89,0x86,0xe8,0x00,0x00,0x00,0x83,
    0xbe,0xe8,0x00,0x00,0x00,0x00,0x7e,0x06
};
const BYTE kEntityTail[] = {
    0xc7,0x86,0xe8,0x00,0x00,0x00,0xf4,0x01,0x00,0x00,0xb0,0x01,0x5e,0xc2,0x08,0x00
};
const BYTE kTouched[] = {0x8b,0x41,0x3c,0xc3};
const BYTE kFrame[] = {0x8b,0x81,0xf0,0x03,0x00,0x00,0xc3};
// Engine::EvictOldResources: texture (+24) and mesh (+2c) managers only.
// Preserve the 800-frame touched age, 1600-frame used age, size filter and
// native callee. Change only push 200 to push 0 for the requeue cooldown.
const BYTE kTextureIdle[] = {
    0x8b,0x4e,0x24,0x68,0xc8,0x00,0x00,0x00,0x6a,0x00,0x68,0x40,
    0x06,0x00,0x00,0x68,0x20,0x03,0x00,0x00,0xe8,0x4c,0xdf,0xfd,0xff
};
const BYTE kMeshIdle[] = {
    0x8b,0x4e,0x2c,0x68,0xc8,0x00,0x00,0x00,0x6a,0x00,0x68,0x40,
    0x06,0x00,0x00,0x68,0x20,0x03,0x00,0x00,0xe8,0x33,0xdf,0xfd,0xff
};
void restoreIdleCooldowns() {
    const BYTE stock = 200, patched = 0;
    for (BYTE*& operand : g_idleCooldown) {
        if (operand) tq::detour::writeBytes(operand, &patched, &stock, 1);
        operand = nullptr;
    }
}
bool patchIdleCooldowns(BYTE* base) {
    if (!tq::detour::matches((HMODULE)base, base + 0x1418cb,
                            signature(kTextureIdle, sizeof(kTextureIdle)))
        || !tq::detour::matches((HMODULE)base, base + 0x1418e4,
                               signature(kMeshIdle, sizeof(kMeshIdle)))) return false;
    const unsigned offsets[] = {0x1418cf, 0x1418e8};
    const BYTE stock = 200, patched = 0;
    for (unsigned i = 0; i < 2; ++i) {
        if (!tq::detour::writeBytes(base + offsets[i], &stock, &patched, 1)) {
            restoreIdleCooldowns(); return false;
        }
        g_idleCooldown[i] = base + offsets[i];
    }
    return true;
}
unsigned word(const void* p, unsigned offset) {
    return *(const unsigned*)((const BYTE*)p + offset);
}
// Pure decision apart from the per-frame admission counter. No resource list,
// allocation, lock, timer query, or file I/O in the shipping path.
bool admit(unsigned now, unsigned touched, unsigned state, int countdown,
           int step, bool include, bool main, unsigned& frame, unsigned& used) {
    if (!main || !include || step <= 0 || countdown <= step || state != 2) return false;
    const unsigned age = now - touched;
    if (age < kRefreshAge || age >= 0x80000000u) return false;
    if (frame != now) { frame = now; used = 0; }
    if (used >= kRefreshBudget) return false;
    ++used;
    return true;
}
bool __fastcall entity(void* self, void* edx, int step, int include) {
    int adjusted = step;
    if (g_active && (include & 0xff) && step > 0 && onMainThread()) {
        const int countdown = (int)word(self, 0xe8);
        // Stock-due calls need no assistance or admission slot.
        if (countdown > step) {
            const void* instance = *(void**)((BYTE*)self + 0x184);
            const void* root = instance ? *(void* const*)((const BYTE*)instance + 4) : nullptr;
            const void* engine = *(void* const*)(g_engine + 0x3743f0);
            if (root && engine) {
                const unsigned now = word(engine, 0x3f0);
                const unsigned touched = word(root, 0x3c);
                const unsigned state = word(root, 0x30);
                if (admit(now, touched, state, countdown, step, true, true,
                          g_budgetFrame, g_used)) {
                    adjusted = countdown; // Original Entity code reaches its normal due branch.
                    if (g_tracing) ++g_refreshed;
                } else if (g_tracing && state == 2 && now - touched >= kRefreshAge
                           && now - touched < 0x80000000u && g_used >= kRefreshBudget) {
                    ++g_deferred;
                }
            }
        }
    }
    return g_entity(self, edx, adjusted, include);
}
} // namespace
void readOptions(const wchar_t* path) {
    g_requested = !path || !path[0]
        || GetPrivateProfileIntW(L"performance", L"mesh_preload_refresh", 1, path) != 0;
}
bool configured() { return g_requested; }
bool install(HMODULE engine) {
    if (!g_requested) return false;
    if (g_active) return true;
    BYTE* base = (BYTE*)engine;
    void* target = resolve(engine, "?PreLoad@Entity@GAME@@UAE_NH_N@Z", 0x148050);
    void* touched = resolve(engine, "?GetLastTouchedFrame@Resource@GAME@@QBEIXZ", 0x2130c0);
    void* frame = resolve(engine, "?GetFrameCount@Engine@GAME@@QBEIXZ", 0x146cd0);
    void* singleton = (void*)GetProcAddress(engine, "?gEngine@GAME@@3PAVEngine@1@A");
    const bool verified = target && touched && frame && singleton == base + 0x3743f0
        && resolve(engine, "?EvictOldResources@Engine@GAME@@QAEXXZ", 0x1418a0)
        && resolve(engine, "?EvictOldResources@BaseResourceManager@GAME@@QAEXIIII@Z", 0x11f830)
        && g_mainThreadId && verifyResourceStateLayout(engine)
        && tq::detour::matches(engine, target, signature(kEntityHead, sizeof(kEntityHead)))
        && tq::detour::matches(engine, base + 0x1480c3, signature(kEntityTail, sizeof(kEntityTail)))
        && tq::detour::matches(engine, touched, signature(kTouched, sizeof(kTouched)))
        && tq::detour::matches(engine, frame, signature(kFrame, sizeof(kFrame)));
    if (!verified) { tq::hdr::log("Mesh preload refresh unavailable: native layout mismatch\r\n"); return false; }
    g_entity = (EntityFn)target;
    g_engine = base;
    const bool actor = tq::detour::patchCall(g_patch, engine, base + 0x114f07,
        signature(kActorWindow, sizeof(kActorWindow)), 4, target, (void*)&entity);
    g_active = actor && patchIdleCooldowns(base);
    if (!g_active) { tq::detour::restoreCall(g_patch); restoreIdleCooldowns(); }
    tq::hdr::log("Mesh preload refresh %s: resident age=%u frames, accelerated visits <=%u/frame; idle requeue cooldown=%u\r\n",
                 g_active ? "installed" : "rejected", kRefreshAge, kRefreshBudget, g_active ? 0u : 200u);
    return g_active;
}
void shutdown() {
    g_active = false;
    tq::detour::restoreCall(g_patch);
    restoreIdleCooldowns();
    g_entity = nullptr; g_engine = nullptr;
    g_budgetFrame = g_used = g_refreshed = g_deferred = 0;
}
void report() {
    if (g_active) tq::hdr::log("Mesh preload refresh F12: accelerated=%u budgetDeferred=%u lastEngineFrame=%u used=%u/%u\r\n",
                              g_refreshed, g_deferred, g_budgetFrame, g_used, kRefreshBudget);
}
#ifdef TQ_SELFTEST
namespace {
#include "../test/fixtures/mesh-preload-native.inc"
EntityFn g_testActor;
unsigned g_testObserved;
void __fastcall captureEviction(void* self, void*, unsigned touched,
                               unsigned used, unsigned size, unsigned cooldown) {
    unsigned* args = (unsigned*)self;
    ++args[0]; args[1] = touched; args[2] = used; args[3] = size; args[4] = cooldown;
}
bool testIdleEviction(BYTE* image) {
    memcpy(image + 0x1418a0, kTestIdleEviction, sizeof(kTestIdleEviction));
    tq::detour::absoluteBranch(image + 0x11f830, (void*)&captureEviction);
    unsigned engine[128] = {}, holder[16] = {}, managers[4][5] = {};
    *(void**)((BYTE*)engine + 0x160) = holder;
    const unsigned offsets[] = {0x24, 0x2c, 0x28, 0x1c};
    for (unsigned i = 0; i < 4; ++i) *(void**)((BYTE*)holder + offsets[i]) = managers[i];
    using IdleFn = void (__fastcall*)(void*, void*);
    const auto invoke = (IdleFn)(image + 0x1418a0);
    bool ok = true;
    // Execute the native caller before/after the patch and after rollback.
    // The stand-in manager records its actual stack arguments and ret 16 ABI.
    for (unsigned mode = 0; mode < 3; ++mode) {
        if (mode == 1) ok &= patchIdleCooldowns(image);
        if (mode == 2) restoreIdleCooldowns();
        memset(managers, 0, sizeof(managers));
        invoke(engine, nullptr);
        for (unsigned i = 0; i < 4; ++i) {
            ok &= managers[i][0] == 1 && managers[i][1] == 800 && managers[i][3] == 0;
            ok &= managers[i][2] == (i < 2 ? 1600u : 0xffffffffu);
            ok &= managers[i][4] == (i < 2 && mode != 1 ? 200u : 0u);
        }
    }
    ok &= !memcmp(image + 0x1418a0, kTestIdleEviction, sizeof(kTestIdleEviction));
    // Reject any unsupported byte in either complete window before touching
    // the first site, including a mismatch found in the second site's callee.
    const unsigned sites[] = {0x1418cb, 0x1418e4};
    for (unsigned rva : sites) {
        for (unsigned n = 0; n < sizeof(kTextureIdle); ++n) {
            image[rva + n] ^= 1;
            ok &= !patchIdleCooldowns(image);
            image[rva + n] ^= 1;
            ok &= !g_idleCooldown[0] && !g_idleCooldown[1];
            ok &= !memcmp(image + 0x1418a0, kTestIdleEviction, sizeof(kTestIdleEviction));
        }
    }
    return ok;
}
bool __fastcall observeActor(void* self, void* edx, int step, int include) {
    ++g_testObserved;
    return g_testActor(self, edx, step, include);
}
bool testNative() {
    if (g_active || g_patch.installed) return false;
    BYTE* image = (BYTE*)VirtualAlloc(nullptr, kEngineImageSize,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!image) return false;
    auto* dos = (IMAGE_DOS_HEADER*)image;
    dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x100;
    auto* nt = (IMAGE_NT_HEADERS*)(image + 0x100);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    nt->OptionalHeader.SizeOfImage = kEngineImageSize;
    auto* section = IMAGE_FIRST_SECTION(nt);
    memcpy(section->Name, ".text", 5);
    section->VirtualAddress = 0x1000;
    section->Misc.VirtualSize = 0x220000;
    memcpy(image + 0x114f00, kTestActor, sizeof(kTestActor));
    memcpy(image + 0x148050, kTestEntity, sizeof(kTestEntity));
    // Stand-in only for the mesh preload callee: touch root, count visits, ret 8.
    const BYTE mesh[] = {0x8b,0x41,0x04,0xc7,0x40,0x3c,0xe8,0x03,0x00,0x00,
                        0xff,0x40,0x70,0xc2,0x08,0x00};
    memcpy(image + 0x174e50, mesh, sizeof(mesh));
    unsigned engine[256] = {}; engine[0x3f0 / 4] = 1000;
    *(void**)(image + 0x3743f0) = engine;
    DWORD mainThread = GetCurrentThreadId();
    const auto savedThread = g_mainThreadId;
    g_mainThreadId = &mainThread;
    g_engine = image; g_entity = (EntityFn)(image + 0x148050);
    bool ok = true;
    for (unsigned traced = 0; traced < 2; ++traced) {
        tq::detour::Detour observer = {};
        if (traced) ok &= tq::detour::attach(observer, (HMODULE)image, image + 0x114f00,
            signature(kTestActor, sizeof(kTestActor) - 1), 7, (void*)&observeActor, (void**)&g_testActor);
        const bool patched = tq::detour::patchCall(g_patch, (HMODULE)image, image + 0x114f07,
            signature(kActorWindow, sizeof(kActorWindow)), 4, image + 0x148050, (void*)&entity);
        ok &= patched;
        if (patched) {
            g_active = true; g_budgetFrame = g_used = 0; g_testObserved = 0;
            unsigned actor[128] = {}, instance[8] = {}, resource[32] = {};
            *(void**)((BYTE*)actor + 0x184) = instance;
            *(void**)((BYTE*)instance + 4) = resource;
            resource[0x30 / 4] = 2;
            auto call = (EntityFn)(image + 0x114f00);
            // Invoke the real Actor and Entity machine-code bodies, including
            // original countdown writes, boolean return and x86 stack cleanup.
            for (unsigned n = 0; n < 9; ++n) {
                actor[0xe8 / 4] = 500; resource[0x3c / 4] = 0; resource[0x70 / 4] = 0;
                const bool result = call(actor, nullptr, 30, 1);
                ok &= result == (n < 8);
                ok &= resource[0x70 / 4] == (n < 8 ? 1u : 0u);
                ok &= actor[0xe8 / 4] == (n < 8 ? 500u : 470u);
            }
            // A stock-due visit proceeds even when all acceleration slots are spent.
            actor[0xe8 / 4] = 20;
            ok &= call(actor, nullptr, 30, 1) && resource[0x70 / 4] == 1;
            // Calls to Entity outside this Actor site retain stock timing.
            actor[0xe8 / 4] = 500;
            ok &= !g_entity(actor, nullptr, 30, 1) && actor[0xe8 / 4] == 470;
            ok &= !traced || g_testObserved == 10;
        }
        g_active = false;
        tq::detour::restoreCall(g_patch);
        tq::detour::detach(observer);
        ok &= !memcmp(image + 0x114f00, kTestActor, sizeof(kTestActor));
    }
    g_mainThreadId = savedThread;
    ok &= testIdleEviction(image);
    shutdown();
    VirtualFree(image, 0, MEM_RELEASE);
    return ok;
}
} // namespace
bool test() {
    unsigned frame = 0, used = 0;
    bool ok = !admit(1000, 601, 2, 500, 30, true, true, frame, used);
    ok &= !admit(1000, 0, 0, 500, 30, true, true, frame, used);
    ok &= !admit(1000, 0, 1, 500, 30, true, true, frame, used);
    ok &= !admit(1000, 0, 2, 500, 30, false, true, frame, used);
    ok &= !admit(1000, 0, 2, 500, 30, true, false, frame, used);
    ok &= !admit(1000, 0, 2, 29, 30, true, true, frame, used);
    ok &= !admit(1000, 0, 2, 500, 0, true, true, frame, used);
    ok &= used == 0;
    for (unsigned n = 0; n < kRefreshBudget; ++n)
        ok &= admit(1000, 600, 2, 500, 30, true, true, frame, used);
    ok &= !admit(1000, 600, 2, 500, 30, true, true, frame, used);
    ok &= used == kRefreshBudget;
    ok &= admit(1001, 600, 2, 500, 30, true, true, frame, used) && used == 1;
    // A stock refreshed root is deduplicated by its native touch, without a cache.
    ok &= !admit(1001, 1001, 2, 500, 30, true, true, frame, used);
    ok &= !admit(1001, 1010, 2, 500, 30, true, true, frame, used);
    ok &= admit(200, 0xffffff00u, 2, 500, 30, true, true, frame, used);
    return ok && testNative();
}
#endif
} }
