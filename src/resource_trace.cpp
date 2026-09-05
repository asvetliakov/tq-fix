#include "resource_trace.h"
#include "engine_internal.h"
#include <stdio.h>

// Diagnostic only. Native loading/preloading calls never run under the recorder lock.
// The verified, non-loading filename accessor is used only when creating a record.
namespace tq { namespace resourcetrace { namespace {
using namespace tq::engine::detail;
constexpr unsigned kEntries = 8192, kProbes = 32, kDemands = 1024;
struct Entry {
    const void* object;
    const void* parentObject;
    unsigned id, firstFrame, parent;
    char name[160];
    unsigned queueCalls, accepted, dependencies, resident, notQueued;
    unsigned queueFrame, queueTick, queueBefore, queueAfter, queueFlags;
    unsigned queueCaller, firstQueueFrame, firstQueueTick, acceptedFrame, acceptedTick;
    unsigned queueEngineFrame, queueUntil;
    unsigned mainLoads, workerLoads, loadFrame, loadTick, loadUs, loadFinishedTick;
    unsigned unloads, unloadFrame, unloadTick, unloadCaller, unloadState, until;
    unsigned actorCalls, actorTrue, actorSkipped, actorFrame;
    int actorCountdown, actorStep;
    unsigned meshCalls, meshTrue, meshFrame, dependencyCalls, dependencyFrame;
};
struct Demand {
    unsigned ticket, frame, engineFrame, tick, us, state, queued, until;
    Entry resource, parent;
};
Entry* g_entries;
Demand* g_demands;
SRWLOCK g_lock = SRWLOCK_INIT;
unsigned g_serial, g_sequence, g_replacements, g_demandOverwrites, g_reported;
bool g_requested, g_active;
HMODULE g_engine;
__thread const void* g_dependencyOwner;
__thread const void* g_renderOwner;
typedef bool (__fastcall* ActorFn)(void*, void*, int, int);
typedef void (__fastcall* MeshFn)(void*, void*, int, int);
typedef void (__fastcall* UnaryFn)(void*, void*);
typedef void (__fastcall* ActualLoadFn)(void*, void*, void*, unsigned);
typedef void (__fastcall* UnloadFn)(void*, void*, unsigned);
typedef void* (__fastcall* ConstructFn)(void*, void*, void*, const void*, int);
ActorFn g_actor;
MeshFn g_mesh;
UnaryFn g_meshDependencies, g_shaderDependencies;
ActualLoadFn g_actualLoad;
UnloadFn g_unload;
ConstructFn g_construct;
tq::detour::Detour g_hooks[7];

unsigned word(const void* p, unsigned offset) {
    return *(const unsigned*)((const BYTE*)p + offset);
}
unsigned frame() { return tq::probe::currentFrameIndex(); }
unsigned engineFrame() {
    const void* engine = *(void**)((BYTE*)g_engine + 0x3743f0);
    return engine ? word(engine, 0x3f0) : 0;
}
unsigned rva(const void* p) {
    const uintptr_t delta = (uintptr_t)p - (uintptr_t)g_engine;
    return delta < kEngineImageSize ? (unsigned)delta : 0;
}
Entry* find(const void* object, bool create) {
    if (!object || !g_entries) return nullptr;
    uintptr_t hash = (uintptr_t)object;
    hash ^= hash >> 7; hash ^= hash >> 15;
    const unsigned start = (unsigned)hash & (kEntries - 1);
    Entry* oldest = nullptr;
    Entry* empty = nullptr;
    for (unsigned n = 0; n < kProbes; ++n) {
        Entry& e = g_entries[(start + n) & (kEntries - 1)];
        if (e.object == object) return &e;
        if (!oldest || e.id < oldest->id) oldest = &e;
        if (!e.object) {
            if (!empty) empty = &e;
        }
    }
    if (!create) return nullptr;
    if (empty) oldest = empty;
    if (oldest->object) ++g_replacements;
    *oldest = {};
    oldest->object = object; oldest->id = ++g_serial; oldest->firstFrame = frame();
    const char* name = g_resourceFileName(const_cast<void*>(object), nullptr);
    if (name) {
        unsigned n = 0;
        while (n + 1 < sizeof(oldest->name) && name[n]) {
            oldest->name[n] = name[n]; ++n;
        }
        oldest->name[n] = 0;
        if (name[n] && n >= 3) memcpy(oldest->name + n - 3, "...", 3);
    }
    return oldest;
}
void snapshotParent(Entry& out, const Entry& child) {
    Entry* parent = find(child.parentObject, false);
    if (parent && parent->id == child.parent) out = *parent;
}
struct Lock {
    Lock() { AcquireSRWLockExclusive(&g_lock); }
    ~Lock() { ReleaseSRWLockExclusive(&g_lock); }
};

bool __fastcall actor(void* self, void* edx, int step, int include) {
    if (!g_active) return g_actor(self, edx, step, include);
    void* instance = *(void**)((BYTE*)self + 0x184);
    const void* resource = instance ? *(void**)((BYTE*)instance + 4) : nullptr;
    const int countdown = (int)word(self, 0xe8);
    const bool result = g_actor(self, edx, step, include);
    if (resource) {
        Lock lock;
        Entry* e = find(resource, true);
        ++e->actorCalls; e->actorTrue += (include & 0xff) != 0; e->actorSkipped += !result;
        e->actorFrame = frame(); e->actorCountdown = countdown; e->actorStep = step;
    }
    return result;
}
void __fastcall mesh(void* self, void* edx, int step, int include) {
    if (g_active) {
        const void* root = *(void**)((BYTE*)self + 4);
        Lock lock;
        Entry* e = find(root, true);
        if (e) {
            ++e->meshCalls; e->meshTrue += (include & 0xff) != 0; e->meshFrame = frame();
            const unsigned parentId = e->id;
            const unsigned offsets[] = {0x14, 0x18};
            for (unsigned offset : offsets) {
                Entry* child = find(*(void**)((BYTE*)self + offset), true);
                if (child) { child->parent = parentId; child->parentObject = root; }
            }
        }
    }
    g_mesh(self, edx, step, include);
}
void dependencyCall(UnaryFn original, void* self, void* edx) {
    if (!g_active) { original(self, edx); return; }
    { Lock lock; Entry* e = find(self, true); ++e->dependencyCalls; e->dependencyFrame = frame(); }
    const void* prior = g_dependencyOwner;
    g_dependencyOwner = self;
    original(self, edx);
    g_dependencyOwner = prior;
}
void __fastcall meshDependencies(void* self, void* edx) {
    dependencyCall(g_meshDependencies, self, edx);
}
void __fastcall shaderDependencies(void* self, void* edx) {
    dependencyCall(g_shaderDependencies, self, edx);
}
void __fastcall actualLoad(void* self, void* edx, void* resource, unsigned queuedAt) {
    if (!g_active) { g_actualLoad(self, edx, resource, queuedAt); return; }
    const bool main = onMainThread();
    unsigned identity;
    { Lock lock; Entry* e = find(resource, true); identity = e->id;
      e->loadUs = e->loadFinishedTick = 0;
      if (main) ++e->mainLoads; else ++e->workerLoads;
      e->loadFrame = frame(); e->loadTick = GetTickCount(); }
    const int64_t started = tq::probe::now();
    g_actualLoad(self, edx, resource, queuedAt);
    const unsigned us = tq::probe::microsecondsSince(started);
    { Lock lock; Entry* e = find(resource, false);
      if (e && e->id == identity) { e->loadUs = us; e->loadFinishedTick = GetTickCount(); } }
}
void __fastcall unload(void* self, void* edx, unsigned until) {
    if (!g_active) { g_unload(self, edx, until); return; }
    const unsigned before = word(self, 0x30);
    const void* caller = __builtin_return_address(0);
    g_unload(self, edx, until);
    Lock lock;
    Entry* e = find(self, true);
    if (before == 2 || before == 1) ++e->unloads;
    e->unloadFrame = frame(); e->unloadTick = GetTickCount();
    e->unloadCaller = rva(caller); e->unloadState = before; e->until = until;
}
void* __fastcall construct(void* self, void* edx, void* manager, const void* name, int type) {
    void* result = g_construct(self, edx, manager, name, type);
    if (g_active) { Lock lock; Entry* e = find(self, false); if (e) *e = {}; }
    return result;
}

void printEntry(const char* role, const Entry& e) {
    if (!e.id) return;
    tq::hdr::log("Lifecycle %s id=%u ptr=%p first=%u parent=%u name=%s\r\n",
        role, e.id, e.object, e.firstFrame, e.parent, e.name);
    tq::hdr::log("Lifecycle history id=%u queue=%u accepted=%u deps=%u resident=%u notQueued=%u "
        "firstQueue=%u firstQueueTick=%u lastQueue=%u queueTick=%u queueEngineFrame=%u queueUntil=%u "
        "acceptedFrame=%u acceptedTick=%u state=%u->%u flags=%u queueCaller=E+%#x "
        "loadMain=%u loadWorker=%u lastLoad=%u loadTick=%u loadUs=%u loadFinishedTick=%u "
        "unloads=%u lastUnload=%u unloadTick=%u unloadCaller=E+%#x unloadState=%u unloadUntil=%u\r\n",
        e.id, e.queueCalls, e.accepted, e.dependencies, e.resident, e.notQueued,
        e.firstQueueFrame, e.firstQueueTick, e.queueFrame, e.queueTick, e.queueEngineFrame, e.queueUntil,
        e.acceptedFrame, e.acceptedTick, e.queueBefore, e.queueAfter, e.queueFlags, e.queueCaller,
        e.mainLoads, e.workerLoads, e.loadFrame, e.loadTick, e.loadUs, e.loadFinishedTick,
        e.unloads, e.unloadFrame, e.unloadTick, e.unloadCaller, e.unloadState, e.until);
    tq::hdr::log("Lifecycle preload id=%u actor=%u actorIncludes=%u actorSkipped=%u actorFrame=%u countdown=%d step=%d "
        "mesh=%u meshIncludes=%u meshFrame=%u dependencyCalls=%u dependencyFrame=%u\r\n", e.id,
        e.actorCalls, e.actorTrue, e.actorSkipped, e.actorFrame, e.actorCountdown, e.actorStep,
        e.meshCalls, e.meshTrue, e.meshFrame, e.dependencyCalls, e.dependencyFrame);
}
} // namespace

void readOptions(const wchar_t* path) {
    g_requested = path && GetPrivateProfileIntW(L"debug", L"resource_lifecycle", 0, path) != 0;
}
bool enabled() { return g_active; }
const void* setRenderOwner(const void* owner) {
    const void* prior = g_renderOwner;
    g_renderOwner = owner;
    return prior;
}
unsigned beginDemand(const void* resource) {
    if (!g_active || !resource || !onMainThread() || word(resource, 0x30) == 2) return 0;
    Lock lock;
    const unsigned ticket = ++g_sequence;
    Demand& d = g_demands[(ticket - 1) % kDemands];
    if (d.ticket > g_reported) ++g_demandOverwrites;
    d = {}; d.ticket = ticket; d.frame = frame(); d.tick = GetTickCount();
    d.engineFrame = engineFrame();
    d.state = word(resource, 0x30); d.queued = word(resource, 0x60) != 0;
    d.until = word(resource, 0x34);
    Entry* entry = find(resource, true);
    if (g_renderOwner && g_renderOwner != resource) {
        const unsigned parentId = find(g_renderOwner, true)->id;
        entry = find(resource, true);
        entry->parent = parentId; entry->parentObject = g_renderOwner;
    }
    d.resource = *entry;
    snapshotParent(d.parent, d.resource);
    return ticket;
}
void finishDemand(unsigned ticket, unsigned elapsedUs) {
    if (!ticket || !g_active) return;
    Lock lock;
    Demand& d = g_demands[(ticket - 1) % kDemands];
    if (d.ticket == ticket) d.us = elapsedUs;
}
void enqueue(const void* resource, int priority, int dependencies, int touch,
             unsigned before, bool queuedBefore, unsigned untilBefore, const void* caller) {
    if (!g_active || !resource) return;
    dependencies &= 0xff; touch &= 0xff; // Native bool stack arguments.
    Lock lock;
    Entry* e = find(resource, true);
    if (!e->queueCalls) { e->firstQueueFrame = frame(); e->firstQueueTick = GetTickCount(); }
    ++e->queueCalls; e->dependencies += dependencies != 0;
    const unsigned after = word(resource, 0x30);
    const bool queued = word(resource, 0x60) != 0;
    const bool accepted = before == 0 && !queuedBefore && (queued || after != 0);
    e->accepted += accepted;
    if (accepted) { e->acceptedFrame = frame(); e->acceptedTick = GetTickCount(); }
    e->resident += before == 2;
    e->notQueued += before == 0 && !queuedBefore && !queued && after == 0;
    e->queueFrame = frame(); e->queueTick = GetTickCount();
    e->queueBefore = before; e->queueAfter = after;
    e->queueUntil = untilBefore; e->queueEngineFrame = engineFrame();
    e->queueFlags = (priority & 3) | (dependencies ? 4 : 0) | (touch ? 8 : 0)
        | (queuedBefore ? 16 : 0) | (queued ? 32 : 0);
    e->queueCaller = rva(caller);
    if (g_dependencyOwner) {
        const unsigned parentId = find(g_dependencyOwner, true)->id;
        e = find(resource, true);
        e->parent = parentId; e->parentObject = g_dependencyOwner;
    }
}
void report() {
    if (!g_active) return;
    unsigned last, reported, replaced, overwritten;
    { Lock lock; last = g_sequence; reported = g_reported;
      replaced = g_replacements; overwritten = g_demandOverwrites; }
    const unsigned marker = frame();
    unsigned emitted = 0;
    unsigned first = last > kDemands ? last - kDemands : 0;
    if (reported > first) first = reported;
    for (unsigned n = first; n < last; ++n) {
        Demand d;
        { Lock lock; d = g_demands[n % kDemands]; }
        if (d.ticket != n + 1 || d.frame > marker || marker - d.frame > 600) continue;
        tq::hdr::log("Lifecycle demand ticket=%u frame=%u engineFrame=%u tick=%u us=%u state=%u queued=%u until=%u id=%u\r\n",
            d.ticket, d.frame, d.engineFrame, d.tick, d.us, d.state, d.queued, d.until, d.resource.id);
        printEntry("resource", d.resource); printEntry("parent", d.parent); ++emitted;
    }
    { Lock lock; g_reported = last; }
    tq::hdr::log("Lifecycle F12 frame=%u demands=%u emitted=%u tableReplacements=%u "
        "unreportedDemandOverwrites=%u history=before-demand capacity=%u/%u\r\n",
        marker, last, emitted, replaced, overwritten, kEntries, kDemands);
}

// All bytes, relocation operands, stolen instruction boundaries and return
// cleanup are verified against the native binary before any hook is reachable.
#include "resource_trace_sites.inc"

bool install(HMODULE engine) {
    if (!g_requested || !g_tracing || !tq::probe::enabled()) return false;
    if (g_active) return true;
    if (!g_resourceStateVerified || !g_resourceFileNameVerified || !g_resourceFileName
        || !g_loadResource || !g_enqueue || !g_graphicsMeshInstanceRenderPass) {
        tq::hdr::log("Resource lifecycle unavailable: required load/name/render observers missing\r\n");
        return false;
    }
    g_engine = engine;
    for (const Site& s : kSites)
        if (!tq::detour::matches(engine, (BYTE*)engine + s.rva, s.signature)
            || !tq::detour::matches(engine, (BYTE*)engine + s.tailRva, s.tail)) {
            tq::hdr::log("Resource lifecycle rejected: unsupported site E+%#lx\r\n", (unsigned long)s.rva);
            return false;
        }
    g_entries = (Entry*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Entry) * kEntries);
    g_demands = (Demand*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Demand) * kDemands);
    bool ok = g_entries && g_demands;
    for (unsigned i = 0; ok && i < sizeof(kSites) / sizeof(kSites[0]); ++i) {
        const Site& s = kSites[i];
        ok = tq::detour::attach(g_hooks[i], engine, (BYTE*)engine + s.rva,
            s.signature, s.stolen, s.hook, s.original);
    }
    if (!ok) { shutdown(); tq::hdr::log("Resource lifecycle rolled back after installation failure\r\n"); return false; }
    g_active = true;
    tq::hdr::log("Resource lifecycle installed: 7/7 sites; actor/mesh/dependencies, "
        "all enqueue requests, main+worker loads, all unloads and resource lifetimes\r\n");
    return true;
}
#ifdef TQ_SELFTEST
namespace {
const char* __fastcall testName(void*, void*) { return "fixture.msh"; }
void* __fastcall testConstruct(void* self, void*, void*, const void*, int) { return self; }
void __fastcall testUnload(void* self, void*, unsigned until) {
    ((unsigned*)self)[0x30 / 4] = 0;
    ((unsigned*)self)[0x34 / 4] = until;
}
void __fastcall testLoad(void*, void*, void* resource, unsigned) {
    ((unsigned*)resource)[0x30 / 4] = 2;
}
}
bool test() {
    if (g_active || g_entries || g_demands) return false;
    BYTE* image = (BYTE*)VirtualAlloc(nullptr, kEngineImageSize,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!image) return false;
    bool ok = true;
    // The production matcher must accept rebased operands and reject a change
    // at every verified byte (including the callee's stack cleanup).
    for (const Site& site : kSites) {
        const DWORD offsets[] = {site.rva, site.tailRva};
        const tq::detour::Signature signatures[] = {site.signature, site.tail};
        for (unsigned n = 0; n < 2; ++n) {
            const auto& sig = signatures[n];
            BYTE* at = image + offsets[n];
            memcpy(at, sig.bytes, sig.length);
            for (unsigned r = 0; r < sig.relocationCount; ++r) {
                const auto& rel = sig.relocations[r];
                const uintptr_t address = (uintptr_t)image + rel.rva;
                memcpy(at + rel.offset, &address, sizeof(address));
            }
            ok &= tq::detour::matches((HMODULE)image, at, sig);
            for (unsigned b = 0; b < sig.length; ++b) {
                at[b] ^= 1;
                ok &= !tq::detour::matches((HMODULE)image, at, sig);
                at[b] ^= 1;
            }
        }
    }
    const auto savedName = g_resourceFileName;
    const auto savedThread = g_mainThreadId;
    const auto savedConstruct = g_construct;
    const auto savedUnload = g_unload;
    const auto savedLoad = g_actualLoad;
    const HMODULE savedEngine = g_engine;
    DWORD mainThread = GetCurrentThreadId();
    g_resourceFileName = testName; g_mainThreadId = &mainThread;
    g_construct = testConstruct; g_unload = testUnload; g_actualLoad = testLoad;
    g_engine = (HMODULE)image;
    g_entries = (Entry*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Entry) * kEntries);
    g_demands = (Demand*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Demand) * kDemands);
    if (g_entries && g_demands) {
        g_active = true;
        unsigned root[32] = {}, child[32] = {};
        Entry* e = find(root, true);
        const unsigned initialId = e->id;
        root[0x60 / 4] = 1;
        enqueue(root, 1, 1, 0, 0, false, 0, image + 0x174e74);
        ok &= e->accepted == 1 && e->dependencies == 1 && e->queueCaller == 0x174e74;
        unsigned ticket = beginDemand(root);
        actualLoad(nullptr, nullptr, root, 123);
        finishDemand(ticket, 900);
        const Demand& before = g_demands[(ticket - 1) % kDemands];
        ok &= before.queued == 1 && before.state == 0 && before.us == 900;
        ok &= before.resource.mainLoads == 0 && e->mainLoads == 1;
        // Worker dispatch is counted independently, using the same native path.
        mainThread = 0;
        actualLoad(nullptr, nullptr, root, 124);
        ok &= e->workerLoads == 1 && beginDemand(root) == 0;
        mainThread = GetCurrentThreadId();
        unload(root, nullptr, 42);
        ticket = beginDemand(root);
        ok &= g_demands[(ticket - 1) % kDemands].resource.unloads == 1;
        ok &= g_demands[(ticket - 1) % kDemands].until == 42;
        setRenderOwner(root);
        ticket = beginDemand(child);
        ok &= g_demands[(ticket - 1) % kDemands].parent.id == initialId;
        ok &= g_demands[(ticket - 1) % kDemands].parent.workerLoads == 1;
        setRenderOwner(nullptr);
        construct(root, nullptr, nullptr, nullptr, 0);
        ok &= find(root, false) == nullptr;
        ok &= find(root, true)->id != initialId;
        ticket = beginDemand(child);
        ok &= g_demands[(ticket - 1) % kDemands].parent.id == 0;
        // A reused address must not contaminate already retained demand history.
        ok &= before.resource.id == initialId;
        for (unsigned n = 0; n <= kDemands; ++n) beginDemand(root);
        ok &= g_demandOverwrites > 0;
        const unsigned staleTicket = 1;
        const unsigned retained = g_demands[0].ticket;
        finishDemand(staleTicket, 999999);
        ok &= g_demands[0].ticket == retained && g_demands[0].us != 999999;
    } else ok = false;
    shutdown();
    g_engine = savedEngine; g_resourceFileName = savedName; g_mainThreadId = savedThread;
    g_construct = savedConstruct; g_unload = savedUnload; g_actualLoad = savedLoad;
    VirtualFree(image, 0, MEM_RELEASE);
    return ok;
}
#endif

void shutdown() {
    g_active = false;
    for (int i = 6; i >= 0; --i) tq::detour::detach(g_hooks[i]);
    if (g_entries) HeapFree(GetProcessHeap(), 0, g_entries);
    if (g_demands) HeapFree(GetProcessHeap(), 0, g_demands);
    g_entries = nullptr; g_demands = nullptr;
    g_dependencyOwner = g_renderOwner = nullptr;
    g_serial = g_sequence = g_replacements = g_demandOverwrites = g_reported = 0;
}
} }
