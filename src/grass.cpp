#include "grass.h"

#include "detour.h"

#include "probe.h"

#include "hdr.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

namespace tq {
namespace grass {
namespace {

// Both RenderGrass implementations open with push ebp; mov ebp,esp; and esp,-8,
// the same six bytes the bloom detour already validates on this build.
const BYTE kRenderPrologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
const SIZE_T kRenderStolen = sizeof(kRenderPrologue);

// CreateGrassGeometry opens with mov eax, ds:[Terrain globals]; sub esp, 0xc.
// The first instruction carries an absolute operand, so it is relocated: the
// expected bytes have to be rebuilt from the loaded base rather than compared
// against a constant. Both instructions are position independent once copied,
// which is what lets the trampoline hold them.

const char kRenderGrassName[] =
    "?RenderGrass@TerrainRenderInterface@GAME@@EBEXABVName@2@AAVGraphicsCanvas@2@"
    "ABVGraphicsSceneRenderer@2@ABURenderablePass@2@@Z";
const char kRenderGrassRtName[] =
    "?RenderGrass@TerrainRenderInterfaceRT@GAME@@EBEXABVName@2@AAVGraphicsCanvas@2@"
    "ABVGraphicsSceneRenderer@2@ABURenderablePass@2@@Z";

// Titan Quest is __thiscall throughout. __fastcall with an ignored second
// argument matches it: this in ecx, the rest on the stack, callee cleaned.
typedef void (__fastcall* RenderGrassFn)(void* self, void* ignored, const void* name,
                                         void* canvas, const void* renderer,
                                         const void* pass);

}  // namespace


namespace {

// The detour machinery lives in src/detour.{h,cpp} now; this file was where it
// grew up, and the engine instrumentation needed the same primitives with a
// verify length separate from the stolen one.
using tq::detour::Detour;
using tq::detour::readable;

Detour g_render[2];
RenderGrassFn g_originalRender[2];

const BYTE* g_engineBegin;
const BYTE* g_engineEnd;
LONG g_installed;
LONG g_rendering;

// 0 wants a capture, 1 has copied and is waiting to map, 2 is finished. The
// copy has to happen with the buffers bound, inside the draw; the map has to
// happen later or it would wait on the GPU for the copy it just issued.

// One grass card: four vertices of position, normal and uv.
const unsigned kFloatsPerVertex = 8;
const unsigned kVerticesPerPlane = 4;
const unsigned kFloatsPerPlane = kFloatsPerVertex * kVerticesPerPlane;
const UINT kGrassStreamBytes = 44800;   // maxNumGrassPlanes * 4 * 32

// Dense scenes in the captured run draw over 330 streams per frame. Keep
// headroom without unbounded source/twin ownership (512 pairs = 43.75 MiB).
// At the cap, retain recent visible streams instead of cycling them every draw.
const unsigned kMaxGrassBuffers = 512;

// Buffers whose size and usage match a grass stream. That is only a hint --
// the game creates many of these -- so a candidate is promoted to a real grass
// stream by what it contains, never by being on this list.
const unsigned kMaxCandidates = 256;

struct GrassBuffer {
    ID3D11Buffer* buffer;    // owned reference: address cannot be recycled
    ID3D11Buffer* crossed;   // our rotated twin of it
    void* mapped;
    bool pending;            // no usable twin yet
    unsigned lastSeen;       // presented-frame ordinal of the last draw/fill
};

struct Candidate {
    ID3D11Buffer* buffer;    // owned until promotion, eviction or shutdown
    void* mapped;
    unsigned rejects;
};
Candidate g_candidates[kMaxCandidates];

// Our own device and context calls re-enter our own hooks: the game's context
// is the one whose vtable is patched, and a twin is created with byte-for-byte
// the same descriptor as a grass stream. Without this, creating a twin
// registers it as a candidate, unmapping it promotes it -- its contents are
// valid cards, because we just wrote them -- and promoting it creates another
// twin. Every internal operation is fenced instead.
LONG g_internal;

struct Internal {
    Internal() { InterlockedIncrement(&g_internal); }
    ~Internal() { InterlockedDecrement(&g_internal); }
};

bool internalBusy() { return InterlockedCompareExchange(&g_internal, 0, 0) != 0; }
unsigned g_candidateCursor;

// Only adoption scans the occupied prefix; draws and Map/Unmap use the
// bounded pointer indices below.
unsigned g_grassHigh;
unsigned g_candidateHigh;

// Buffer pointer to slot, so a map hook costs a hash and a probe rather than a
// walk that grows with how far the session has explored.
PointerIndex g_streamIndex;
PointerIndex g_candidateIndex;

// A full, protected cache rejects further admissions until the next frame.
// This keeps overflow from causing a descriptor query and scan on every draw.
// These two values belong to the render thread (Present and immediate-context
// draws/Map/Unmap). The loader's noteBufferCreated never accesses them.
unsigned g_frameOrdinal;
bool g_admissionBlocked;
GrassBuffer g_grassBuffers[kMaxGrassBuffers];
CRITICAL_SECTION g_grassLock;
bool g_grassLockReady;
LONG g_twinDrawLogged;
unsigned g_pendingTurned;

// Seeding a twin from a stream that is already filled. Adoption happens at the
// draw, which is the only place a grass stream is certain, but by then the
// game has long since written it: a twin that waits for the next Map/Unmap
// waits for the block to be recycled, which for most blocks never happens.
// A staging copy reads what is already there.
//
// Measured, and the reason this reads the way it does: the first version
// queued the copy during a draw and mapped it from onPresent, believing that
// to be the following frame. It is not -- onPresent runs *before* the game's
// Present -- so every seed blocked the render thread until the GPU had caught
// up with the copy it had just issued. One run of 8,176 frames spent 2,250 ms
// in 96 such stalls, 23.4 ms each, which is one whole frame every time.
//
// So the copy is now handed over in three steps: queued at a draw, released
// for reading only after a Present has actually happened, and then mapped with
// DO_NOT_WAIT so a copy the GPU has not reached yet costs a retry rather than
// a stall.
enum SeedStage { SeedIdle, SeedCopyQueued, SeedReadable };
ID3D11Buffer* g_seedStaging;
unsigned g_seedSlot = kMaxGrassBuffers;
LONG g_seedStage;

// One block's worth, reused. The rotate runs on this rather than on the
// game's upload memory, which is write-combined and painful to read twice.
BYTE* g_scratch;
// Published under g_grassLock and read without it by afterUnmap, which is on
// the path of every unmap in the game.
volatile LONG g_pendingSlot = (LONG)kMaxGrassBuffers;

void renderGrass(unsigned index, void* self, void* ignored, const void* name,
                 void* canvas, const void* renderer, const void* pass) {
    if (!g_originalRender[index]) return;
    // Draws issued while this is set are grass draws, and nothing else in the
    // frame is inside this function.
    InterlockedExchange(&g_rendering, 1);
    g_originalRender[index](self, ignored, name, canvas, renderer, pass);
    InterlockedExchange(&g_rendering, 0);
}

void __fastcall hookRenderGrass(void* self, void* ignored, const void* name,
                                void* canvas, const void* renderer,
                                const void* pass) {
    renderGrass(0, self, ignored, name, canvas, renderer, pass);
}

void __fastcall hookRenderGrassRt(void* self, void* ignored, const void* name,
                                  void* canvas, const void* renderer,
                                  const void* pass) {
    renderGrass(1, self, ignored, name, canvas, renderer, pass);
}

void iniPath(wchar_t path[MAX_PATH]) {
    path[0] = 0;
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) { path[0] = 0; return; }
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) { path[0] = 0; return; }
    lstrcpyW(slash + 1, L"tqflicker.ini");
}

// "enhanced" or "original", the same shape as the bloom setting. Read
// positively rather than as a switch: a value that is not understood leaves
// the game's grass untouched instead of quietly enabling a change to it.
bool readGrassMode() {
    wchar_t path[MAX_PATH];
    iniPath(path);
    // No ini at all is the shipped configuration, so it gets the shipped
    // default rather than the untouched game.
    if (!path[0]) return true;
    wchar_t value[32];
    GetPrivateProfileStringW(L"graphics", L"grass", L"enhanced", value, 32, path);
    // Read positively even though the default is on: a value that is not
    // understood falls back to the game's own grass rather than to a change
    // the setting did not ask for.
    return _wcsicmp(value, L"enhanced") == 0;
}

// Read once. The rewrite runs for every grass buffer the game fills, and a
// setting that changed mid-session would leave half the world bent.
struct Config {
    bool read;
    bool enhanced;
};
Config g_config;

const Config& config() {
    if (!g_config.read) {
        g_config.enhanced = readGrassMode();
        g_config.read = true;
    }
    return g_config;
}

bool tracking() { return config().enhanced; }

}  // namespace

bool install(HMODULE engine, const Exports& exports) {
    if (!engine || InterlockedCompareExchange(&g_installed, 1, 0)) return false;

    {
        BYTE* text = nullptr;
        SIZE_T textSize = 0;
        if (tq::detour::moduleText(engine, &text, &textSize)) {
            g_engineBegin = (const BYTE*)engine;
            g_engineEnd = text + textSize;
        }
    }

    const tq::detour::Signature prologue = {kRenderPrologue, kRenderStolen,
                                            nullptr, 0};
    if (exports.renderGrass)
        tq::detour::attach(g_render[0], engine, exports.renderGrass, prologue,
                           kRenderStolen, (const void*)&hookRenderGrass,
                           (void**)&g_originalRender[0]);
    if (exports.renderGrassRT)
        tq::detour::attach(g_render[1], engine, exports.renderGrassRT, prologue,
                           kRenderStolen, (const void*)&hookRenderGrassRt,
                           (void**)&g_originalRender[1]);

    if (g_render[0].installed || g_render[1].installed)
        return true;
    InterlockedExchange(&g_installed, 0);
    return false;
}

void installFromModule(HMODULE engine) {
    // The crossing draw needs to know it is inside RenderGrass, and that is
    // what the detour reports. Nothing about a grass draw is distinguishable
    // at the device level without it that would not cost a COM call on every
    // draw in the frame.
    if (!enabled()) return;
    if (!engine) {
        tq::hdr::log("Grass hooks skipped: Engine.dll not loaded\r\n");
        return;
    }

    Exports exports = {};
    exports.renderGrass = (void*)GetProcAddress(engine, kRenderGrassName);
    exports.renderGrassRT = (void*)GetProcAddress(engine, kRenderGrassRtName);

    const bool ok = install(engine, exports);
    tq::hdr::log("Grass hooks installed: ok=%u render=%u renderRT=%u\r\n",
                 ok ? 1u : 0u, g_render[0].installed ? 1u : 0u,
                 g_render[1].installed ? 1u : 0u);
}

bool rendering() {
    return InterlockedCompareExchange(&g_rendering, 0, 0) != 0;
}

static void completeSeed(ID3D11DeviceContext* context);

void onPresent(ID3D11DeviceContext* context) {
    completeSeed(context);
    if (g_grassLockReady) {
        // No shared table access here: do not wait behind a loader-thread
        // candidate notification just to advance render-thread bookkeeping.
        ++g_frameOrdinal;
        g_admissionBlocked = false;
    }
}

bool isGrassPlane(const float* plane) {
    if (!plane) return false;
    for (unsigned i = 0; i < kFloatsPerPlane; ++i)
        if (!_finite(plane[i])) return false;

    const float* v0 = plane;
    const float* v1 = plane + kFloatsPerVertex;
    const float* v2 = plane + 2 * kFloatsPerVertex;
    const float* v3 = plane + 3 * kFloatsPerVertex;

    // One unit normal, shared by the whole card bitwise. Measured, and the
    // sharpest part of the fingerprint: nothing else in the game's vertex data
    // looks like four identical normals across a four-vertex quad.
    const float* n = plane + 3;
    const double length = (double)n[0] * n[0] + (double)n[1] * n[1]
                        + (double)n[2] * n[2];
    if (length < 0.998 || length > 1.002) return false;
    if (memcmp(n, v1 + 3, 3 * sizeof(float))
        || memcmp(n, v2 + 3, 3 * sizeof(float))
        || memcmp(n, v3 + 3, 3 * sizeof(float)))
        return false;

    // Wound top-left, top-right, bottom-right, bottom-left: the top pair
    // shares a height, the bottom pair shares a lower one, and each side keeps
    // its ground position.
    if (v0[1] != v1[1] || v2[1] != v3[1] || !(v0[1] > v2[1])) return false;
    if (v0[0] != v3[0] || v0[2] != v3[2]) return false;
    if (v1[0] != v2[0] || v1[2] != v2[2]) return false;

    const double wx = (double)v1[0] - v0[0];
    const double wz = (double)v1[2] - v0[2];
    if (wx * wx + wz * wz < 1.0e-8) return false;

    // The atlas column: both left corners at one u, both right corners at
    // another, with v exactly 0 along the top and 1 along the bottom.
    if (v0[6] != v3[6] || v1[6] != v2[6] || v0[6] == v1[6]) return false;
    if (v0[7] != 0.0f || v1[7] != 0.0f || v2[7] != 1.0f || v3[7] != 1.0f)
        return false;
    return true;
}

bool rotatePlane(float* plane) {
    if (!isGrassPlane(plane)) return false;

    float* v0 = plane;                          // top-left
    float* v1 = plane + kFloatsPerVertex;       // top-right
    float* v2 = plane + 2 * kFloatsPerVertex;   // bottom-right
    float* v3 = plane + 3 * kFloatsPerVertex;   // bottom-left

    // A card stands upright: its two ground positions are the left and right
    // corners, and both heights are shared down each side. So the whole shape
    // is those two points, and turning the card is turning them.
    const double lx = v0[0], lz = v0[2];
    const double rx = v1[0], rz = v1[2];
    const double cx = (lx + rx) * 0.5;
    const double cz = (lz + rz) * 0.5;
    const double hx = (rx - lx) * 0.5;
    const double hz = (rz - lz) * 0.5;

    // A quarter turn about the card's own centre, which keeps the centre, the
    // width and the height exactly and moves nothing else.
    const double tx = -hz;
    const double tz = hx;
    const float leftX = (float)(cx - tx);
    const float leftZ = (float)(cz - tz);
    const float rightX = (float)(cx + tx);
    const float rightZ = (float)(cz + tz);
    if (!_finite(leftX) || !_finite(leftZ) || !_finite(rightX) || !_finite(rightZ))
        return false;

    v0[0] = leftX;  v0[2] = leftZ;
    v3[0] = leftX;  v3[2] = leftZ;
    v1[0] = rightX; v1[2] = rightZ;
    v2[0] = rightX; v2[2] = rightZ;
    return true;
}


// A tombstone marks a slot that held a key, so a probe must continue past it.
// A null slot ends the probe: nothing that hashed here was ever stored beyond.
static void* const kIndexTombstone = (void*)(uintptr_t)1;

static unsigned indexSlot(const void* key) {
    uintptr_t v = (uintptr_t)key;
    // Buffer addresses are aligned and cluster, so the low bits alone collide
    // heavily. Folding the high bits in spreads them across the table.
    v ^= v >> 13;
    v ^= v >> 7;
    return (unsigned)((v >> 3) & (PointerIndex::kSize - 1));
}

void indexReset(PointerIndex& index) {
    memset(index.keys, 0, sizeof(index.keys));
    memset(index.values, 0, sizeof(index.values));
}

bool indexLookup(const PointerIndex& index, void* key, unsigned* value) {
    if (!key || key == kIndexTombstone) return false;
    unsigned slot = indexSlot(key);
    for (unsigned i = 0; i < PointerIndex::kProbe; ++i) {
        const unsigned at = (slot + i) & (PointerIndex::kSize - 1);
        void* held = index.keys[at];
        if (!held) return false;
        if (held == key) {
            if (value) *value = index.values[at];
            return true;
        }
    }
    return false;
}

bool indexInsert(PointerIndex& index, void* key, unsigned value) {
    if (!key || key == kIndexTombstone) return false;
    unsigned slot = indexSlot(key);
    unsigned spare = PointerIndex::kSize;
    for (unsigned i = 0; i < PointerIndex::kProbe; ++i) {
        const unsigned at = (slot + i) & (PointerIndex::kSize - 1);
        void* held = index.keys[at];
        if (held == key) {
            index.values[at] = value;
            return true;
        }
        if ((!held || held == kIndexTombstone) && spare == PointerIndex::kSize)
            spare = at;
        if (!held) break;
    }
    if (spare == PointerIndex::kSize) return false;
    index.keys[spare] = key;
    index.values[spare] = value;
    return true;
}

bool indexRemove(PointerIndex& index, void* key) {
    if (!key || key == kIndexTombstone) return false;
    unsigned slot = indexSlot(key);
    for (unsigned i = 0; i < PointerIndex::kProbe; ++i) {
        const unsigned at = (slot + i) & (PointerIndex::kSize - 1);
        void* held = index.keys[at];
        if (!held) return false;
        if (held == key) {
            index.keys[at] = kIndexTombstone;
            index.values[at] = 0;
            return true;
        }
    }
    return false;
}

// A twin is indistinguishable from a grass stream by descriptor and by
// contents, so the only reliable test is whether we made it.
static bool isOwnTwin(ID3D11Buffer* buffer) {
    if (!buffer) return false;
    for (unsigned i = 0; i < g_grassHigh; ++i)
        if (g_grassBuffers[i].crossed == buffer) return true;
    return false;
}

// The shape a grass stream is created with. One definition, used both when a
// buffer is created (candidacy) and when one is offered at a draw (adoption),
// so the two entry points cannot drift into disagreeing about what grass is.
static bool grassStreamShape(const D3D11_BUFFER_DESC& desc) {
    return desc.ByteWidth == kGrassStreamBytes
        && desc.Usage == D3D11_USAGE_DYNAMIC
        && (desc.BindFlags & D3D11_BIND_VERTEX_BUFFER)
        && (desc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE);
}

// All helpers below run under g_grassLock. Owning the source makes its
// immutable descriptor valid for the entire indexed lifetime, without a COM
// query on each Map/Unmap. Sources and candidates have separate fixed caps.
static void clearCandidate(unsigned slot) {
    Candidate& entry = g_candidates[slot];
    ID3D11Buffer* const buffer = entry.buffer;
    if (buffer) indexRemove(g_candidateIndex, buffer);
    memset(&entry, 0, sizeof(entry));
    if (buffer) buffer->Release();
}

static void cancelSlotWork(unsigned slot) {
    if (g_seedSlot == slot) {
        g_seedSlot = kMaxGrassBuffers;
        InterlockedExchange(&g_seedStage, SeedIdle);
    }
    if (g_pendingSlot == (LONG)slot) {
        g_pendingSlot = (LONG)kMaxGrassBuffers;
        g_pendingTurned = 0;
    }
}

static void clearStream(unsigned slot) {
    GrassBuffer& entry = g_grassBuffers[slot];
    cancelSlotWork(slot);
    ID3D11Buffer* const buffer = entry.buffer;
    ID3D11Buffer* const crossed = entry.crossed;
    if (buffer) indexRemove(g_streamIndex, buffer);
    memset(&entry, 0, sizeof(entry));
    if (crossed) crossed->Release();
    if (buffer) buffer->Release();
}

// Evict only streams absent from both this frame and the previous one. The
// previous-frame guard matters before the renderer has visited all this
// frame's draws. A staging read or mapped/uploading source stays pinned too.
static unsigned adoptStream(ID3D11Buffer* buffer) {
    if (!buffer) return kMaxGrassBuffers;
    EnterCriticalSection(&g_grassLock);
    unsigned chosen = kMaxGrassBuffers;
    if (indexLookup(g_streamIndex, buffer, &chosen)) {
        LeaveCriticalSection(&g_grassLock);
        return chosen;
    }
    if (g_admissionBlocked) {
        LeaveCriticalSection(&g_grassLock);
        return kMaxGrassBuffers;
    }
    // A draw in RenderGrass is not enough to establish the fixed copy size.
    D3D11_BUFFER_DESC desc = {};
    buffer->GetDesc(&desc);
    if (!grassStreamShape(desc)) {
        LeaveCriticalSection(&g_grassLock);
        return kMaxGrassBuffers;
    }
    unsigned free = kMaxGrassBuffers;
    unsigned coldest = kMaxGrassBuffers;
    unsigned coldestAge = 0;
    for (unsigned i = 0; i < g_grassHigh; ++i) {
        const GrassBuffer& entry = g_grassBuffers[i];
        if (!entry.buffer) {
            if (free == kMaxGrassBuffers) free = i;
            continue;
        }
        const unsigned age = g_frameOrdinal - entry.lastSeen;
        if (age > 1 && age > coldestAge && i != g_seedSlot
            && (LONG)i != g_pendingSlot && !entry.mapped) {
            coldestAge = age;
            coldest = i;
        }
    }
    if (free == kMaxGrassBuffers && g_grassHigh < kMaxGrassBuffers)
        free = g_grassHigh++;
    chosen = free < kMaxGrassBuffers ? free : coldest;
    if (chosen == kMaxGrassBuffers) {
        g_admissionBlocked = true;
        LeaveCriticalSection(&g_grassLock);
        return chosen;
    }
    clearStream(chosen);
    buffer->AddRef();
    g_grassBuffers[chosen].buffer = buffer;
    g_grassBuffers[chosen].pending = true;
    g_grassBuffers[chosen].lastSeen = g_frameOrdinal;
    // Failure remains lossy and safe; a collision never aliases two streams.
    if (!indexInsert(g_streamIndex, buffer, chosen)) {
        clearStream(chosen);
        chosen = kMaxGrassBuffers;
    } else {
        tq::probe::count(tq::probe::CounterGrassAdopt);
        // Promotion transfers the candidate's ownership and drops its mapping.
        unsigned candidate = 0;
        if (indexLookup(g_candidateIndex, buffer, &candidate))
            clearCandidate(candidate);
    }
    LeaveCriticalSection(&g_grassLock);
    return chosen;
}

// Recorded, not trusted. Matching a grass stream by its size and usage looked
// reasonable and was wrong: the game creates many buffers of that shape, and a
// table filled with them left the streams that were actually drawn nowhere to
// go. A candidate is promoted by what it contains, at its first fill.
void noteBufferCreated(ID3D11Buffer* buffer, const D3D11_BUFFER_DESC* desc) {
    if (internalBusy()) return;
    if (!buffer || !desc || !g_grassLockReady || !tracking()) return;
    if (!grassStreamShape(*desc)) return;
    EnterCriticalSection(&g_grassLock);
    unsigned existing = 0;
    if (indexLookup(g_streamIndex, buffer, &existing)
        || indexLookup(g_candidateIndex, buffer, &existing)) {
        LeaveCriticalSection(&g_grassLock);
        return;
    }
    unsigned slot = kMaxCandidates;
    for (unsigned i = 0; i < g_candidateHigh; ++i) {
        if (g_candidates[i].buffer == buffer) { slot = i; break; }
        if (!g_candidates[i].buffer && slot == kMaxCandidates) slot = i;
    }
    if (slot == kMaxCandidates && g_candidateHigh < kMaxCandidates)
        slot = g_candidateHigh++;
    if (slot == kMaxCandidates) slot = g_candidateCursor++ % kMaxCandidates;
    clearCandidate(slot);
    buffer->AddRef();
    g_candidates[slot].buffer = buffer;
    g_candidates[slot].mapped = nullptr;
    g_candidates[slot].rejects = 0;
    if (!indexInsert(g_candidateIndex, buffer, slot))
        clearCandidate(slot);
    LeaveCriticalSection(&g_grassLock);
}

void noteMap(ID3D11Resource* resource, UINT subresource,
             const D3D11_MAPPED_SUBRESOURCE* mapped) {
    if (internalBusy()) return;
    if (!resource || subresource || !mapped || !mapped->pData
        || !g_grassLockReady || !tracking())
        return;
    EnterCriticalSection(&g_grassLock);
    unsigned slot = 0;
    if (indexLookup(g_streamIndex, resource, &slot) && slot < kMaxGrassBuffers
        && g_grassBuffers[slot].buffer == (ID3D11Buffer*)resource) {
        g_grassBuffers[slot].mapped = mapped->pData;
        g_grassBuffers[slot].lastSeen = g_frameOrdinal;
    } else if (indexLookup(g_candidateIndex, resource, &slot) && slot < kMaxCandidates
        && g_candidates[slot].buffer == (ID3D11Buffer*)resource)
        g_candidates[slot].mapped = mapped->pData;
    LeaveCriticalSection(&g_grassLock);
}

void noteUnmap(ID3D11Resource* resource, UINT subresource) {
    if (internalBusy()) return;
    if (!resource || subresource || !g_grassLockReady || !tracking()) return;
    void* memory = nullptr;
    unsigned slot = kMaxGrassBuffers;
    EnterCriticalSection(&g_grassLock);
    unsigned found = 0;
    if (indexLookup(g_streamIndex, resource, &found) && found < kMaxGrassBuffers
        && g_grassBuffers[found].buffer == (ID3D11Buffer*)resource) {
        memory = g_grassBuffers[found].mapped;
        g_grassBuffers[found].mapped = nullptr;
        slot = found;
    }
    LeaveCriticalSection(&g_grassLock);

    if (!memory) {
        // Not a known stream. If it is a candidate, its first plane says
        // whether it is grass -- 128 bytes rather than the whole buffer -- and
        // a fill is the earliest a twin can exist, which is before the block is
        // ever drawn. That is what stops the crossing appearing a second late.
        void* candidate = nullptr;
        EnterCriticalSection(&g_grassLock);
        unsigned at = 0;
        if (indexLookup(g_candidateIndex, resource, &at) && at < kMaxCandidates
            && g_candidates[at].buffer == (ID3D11Buffer*)resource) {
            if (g_candidates[at].mapped && g_candidates[at].rejects < 4)
                candidate = g_candidates[at].mapped;
            g_candidates[at].mapped = nullptr;
        }
        LeaveCriticalSection(&g_grassLock);
        if (candidate) {
            EnterCriticalSection(&g_grassLock);
            const bool own = isOwnTwin((ID3D11Buffer*)resource);
            LeaveCriticalSection(&g_grassLock);
            if (own) return;
        }
        if (!candidate || !isGrassPlane((const float*)candidate)) {
            if (candidate) {
                EnterCriticalSection(&g_grassLock);
                unsigned reject = 0;
                if (indexLookup(g_candidateIndex, resource, &reject)
                    && reject < kMaxCandidates)
                    ++g_candidates[reject].rejects;
                LeaveCriticalSection(&g_grassLock);
            }
            return;
        }
        slot = adoptStream((ID3D11Buffer*)resource);
        if (slot >= kMaxGrassBuffers) return;
        memory = candidate;
    }
    if (slot >= kMaxGrassBuffers) return;

    // The game has just written this block's cards into its own upload memory
    // and is about to hand it to the driver. Every plane is checked before it
    // is read, so a buffer that reached here by size alone changes nothing.
    if (config().enhanced && g_scratch) {
        // Timed here, on the block that does the work, rather than around the
        // whole unmap hook: a clock pair on all 2,400 unmaps a frame costs
        // more than the lookups it would measure and charges driver time to
        // this column.
        tq::probe::Scope timing(tq::probe::PhaseGrassFill);
        memcpy(g_scratch, memory, kGrassStreamBytes);
        float* base = (float*)g_scratch;
        const unsigned planes =
            kGrassStreamBytes / (unsigned)(kFloatsPerPlane * sizeof(float));
        unsigned turned = 0;
        for (unsigned i = 0; i < planes; ++i)
            if (rotatePlane(base + (SIZE_T)i * kFloatsPerPlane)) ++turned;
        // How many of the 350 planes a fill actually turns: the block sampled
        // in research/grass.md drew 50 of them, so this is what says whether
        // the copy and the walk above are mostly wasted.
        tq::probe::count(tq::probe::CounterGrassRotate, turned);

        EnterCriticalSection(&g_grassLock);
        // A queued staging copy belongs to the previous fill. It must never
        // make an older version of this stream drawable after a refill.
        cancelSlotWork(slot);
        g_grassBuffers[slot].pending = true;
        g_pendingSlot = (LONG)slot;
        g_pendingTurned = turned;
        LeaveCriticalSection(&g_grassLock);
    }
}

void installBuffers() {
    if (g_grassLockReady || !tracking()) return;
    g_scratch = (BYTE*)HeapAlloc(GetProcessHeap(), 0, kGrassStreamBytes);
    if (!g_scratch) {
        tq::hdr::log("Grass crossed disabled: no scratch buffer\r\n");
        g_config.enhanced = false;
        return;
    }
    InitializeCriticalSection(&g_grassLock);
    memset(g_grassBuffers, 0, sizeof(g_grassBuffers));
    memset(g_candidates, 0, sizeof(g_candidates));
    g_pendingSlot = (LONG)kMaxGrassBuffers;
    indexReset(g_streamIndex);
    indexReset(g_candidateIndex);
    g_grassLockReady = true;
    tq::hdr::log("Grass tracked: mode=%s\r\n",
                 config().enhanced ? "enhanced" : "original");
}

bool enabled() { return config().enhanced; }

// Called with a buffer that is certainly a grass stream, because the caller is
// inside RenderGrass. Creation-time matching is a guess -- a size and a usage
// that something else in the game can share -- and a guess that misses leaves
// the feature silently doing nothing. This is the authoritative route; the
// cost is that a buffer crosses from its second fill onward.
ID3D11Buffer* crossedBuffer(ID3D11Buffer* source) {
    if (!source || !g_grassLockReady || !config().enhanced) return nullptr;
    ID3D11Buffer* result = nullptr;
    bool known = false;
    bool pending = false;
    bool canAdopt = false;
    EnterCriticalSection(&g_grassLock);
    unsigned at = 0;
    canAdopt = !g_admissionBlocked;
    if (indexLookup(g_streamIndex, source, &at) && at < kMaxGrassBuffers
        && g_grassBuffers[at].buffer == source) {
        known = true;
        pending = g_grassBuffers[at].pending;
        g_grassBuffers[at].lastSeen = g_frameOrdinal;
        // Only after a fill has been rotated into it; an unfilled twin would
        // draw whatever the buffer happened to contain.
        if (!pending) result = g_grassBuffers[at].crossed;
    }
    LeaveCriticalSection(&g_grassLock);
    if (!known && canAdopt) adoptStream(source);

    // One-shot and unconditional: the single line that says the feature is
    // actually drawing, as opposed to merely installed.
    if (result && !InterlockedCompareExchange(&g_twinDrawLogged, 1, 0))
        tq::hdr::log("Grass crossed: first crossing draw buffer=%p twin=%p\r\n",
                     source, result);
    return result;
}

void afterUnmap(ID3D11DeviceContext* context) {
    // Every unmap in the game arrives here -- about 2,400 a frame -- and almost
    // none of them left anything to do. An aligned volatile LONG load is
    // atomic on x86 and really is one load; a same-value compare-exchange, the
    // first version of this fast path, is a bus-locked write dressed as one.
    // A stale read costs nothing: the slot is only ever published by this same
    // thread's noteUnmap moments earlier, and the critical section below
    // re-reads it authoritatively.
    if (g_pendingSlot == (LONG)kMaxGrassBuffers) return;
    tq::probe::Scope timing(tq::probe::PhaseGrassFill);
    unsigned slot = kMaxGrassBuffers;
    unsigned turned = 0;
    EnterCriticalSection(&g_grassLock);
    slot = (unsigned)g_pendingSlot;
    turned = g_pendingTurned;
    g_pendingSlot = (LONG)kMaxGrassBuffers;
    g_pendingTurned = 0;
    LeaveCriticalSection(&g_grassLock);
    if (!context || slot >= kMaxGrassBuffers || !g_scratch) return;
    // Nothing recognisable in this fill: leaving the stream without a twin
    // draws it once, which is what it did before. Uploading an unturned copy
    // would draw the same cards twice in the same place instead.
    if (!turned) return;

    Internal fence;
    GrassBuffer& entry = g_grassBuffers[slot];
    if (!entry.crossed) {
        // Created here rather than inside the game's CreateBuffer or Unmap:
        // this runs after the driver call has returned, so it is an ordinary
        // device call from our own code.
        ID3D11Device* device = nullptr;
        context->GetDevice(&device);
        if (device) {
            D3D11_BUFFER_DESC desc = {};
            desc.ByteWidth = kGrassStreamBytes;
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateBuffer(&desc, nullptr, &entry.crossed)))
                entry.crossed = nullptr;
            device->Release();
        }
        if (!entry.crossed) {
            tq::probe::count(tq::probe::CounterGrassTwinFail);
            tq::hdr::log("Grass crossed: twin buffer creation failed\r\n");
            return;
        }
        tq::probe::count(tq::probe::CounterGrassTwinCreate);
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(entry.crossed, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))
        || !mapped.pData) {
        tq::probe::count(tq::probe::CounterGrassTwinFail);
        tq::hdr::log("Grass crossed: twin upload failed slot=%u\r\n", slot);
        return;
    }
    memcpy(mapped.pData, g_scratch, kGrassStreamBytes);
    context->Unmap(entry.crossed, 0);
    tq::probe::count(tq::probe::CounterGrassFill);

    EnterCriticalSection(&g_grassLock);
    entry.pending = false;
    LeaveCriticalSection(&g_grassLock);
}

void seedFromDraw(ID3D11DeviceContext* context, ID3D11Buffer* source) {
    if (!context || !source || !g_grassLockReady || !config().enhanced) return;
    if (InterlockedCompareExchange(&g_seedStage, 0, 0) != SeedIdle) return;

    unsigned slot = kMaxGrassBuffers;
    EnterCriticalSection(&g_grassLock);
    unsigned at = 0;
    if (indexLookup(g_streamIndex, source, &at) && at < kMaxGrassBuffers
        && g_grassBuffers[at].buffer == source
        && g_grassBuffers[at].pending && !g_grassBuffers[at].crossed)
        slot = at;
    LeaveCriticalSection(&g_grassLock);
    if (slot >= kMaxGrassBuffers) return;

    Internal fence;
    if (!g_seedStaging) {
        ID3D11Device* device = nullptr;
        context->GetDevice(&device);
        if (!device) return;
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = kGrassStreamBytes;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &g_seedStaging)))
            g_seedStaging = nullptr;
        device->Release();
        if (!g_seedStaging) return;
    }
    tq::probe::count(tq::probe::CounterGrassSeedQueued);
    context->CopyResource(g_seedStaging, source);
    g_seedSlot = slot;
    InterlockedExchange(&g_seedStage, SeedCopyQueued);
}

// Marks a slot as no longer worth seeding. Leaving it pending with no twin is
// what turned a single failure into a copy and a map on every subsequent
// frame; clearing it stops that, and a later ordinary fill still promotes the
// block through the Map/Unmap path.
static void abandonSeed(unsigned slot) {
    tq::probe::count(tq::probe::CounterGrassSeedFailed);
    if (slot >= kMaxGrassBuffers) return;
    EnterCriticalSection(&g_grassLock);
    g_grassBuffers[slot].pending = false;
    LeaveCriticalSection(&g_grassLock);
}

static void completeSeed(ID3D11DeviceContext* context) {
    if (!context) return;
    // One Present has to have happened between the copy and the read. This
    // callback runs before the game's Present, so the frame in which the copy
    // was queued is not yet on the GPU when it returns.
    if (InterlockedCompareExchange(&g_seedStage, SeedReadable, SeedCopyQueued)
        == SeedCopyQueued)
        return;
    if (InterlockedCompareExchange(&g_seedStage, 0, 0) != SeedReadable) return;
    const unsigned slot = g_seedSlot;
    if (slot >= kMaxGrassBuffers || !g_seedStaging || !g_scratch) {
        g_seedSlot = kMaxGrassBuffers;
        InterlockedExchange(&g_seedStage, SeedIdle);
        return;
    }
    Internal fence;
    GrassBuffer& entry = g_grassBuffers[slot];
    if (!entry.buffer || entry.crossed) {
        g_seedSlot = kMaxGrassBuffers;
        InterlockedExchange(&g_seedStage, SeedIdle);
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = context->Map(g_seedStaging, 0, D3D11_MAP_READ,
                              D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    // The copy has not retired yet. Keep the stage where it is and come back
    // next frame; the whole point of this path is that it never waits.
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
    g_seedSlot = kMaxGrassBuffers;
    InterlockedExchange(&g_seedStage, SeedIdle);
    if (FAILED(hr) || !mapped.pData) {
        abandonSeed(slot);
        return;
    }
    memcpy(g_scratch, mapped.pData, kGrassStreamBytes);
    context->Unmap(g_seedStaging, 0);

    float* base = (float*)g_scratch;
    const unsigned planes =
        kGrassStreamBytes / (unsigned)(kFloatsPerPlane * sizeof(float));
    unsigned turned = 0;
    for (unsigned i = 0; i < planes; ++i)
        if (rotatePlane(base + (SIZE_T)i * kFloatsPerPlane)) ++turned;
    // Nothing recognisable: leave the block uncrossed, and stop offering it.
    if (!turned) { abandonSeed(slot); return; }

    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (device) {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = kGrassStreamBytes;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &entry.crossed)))
            entry.crossed = nullptr;
        device->Release();
    }
    if (!entry.crossed) { abandonSeed(slot); return; }

    D3D11_MAPPED_SUBRESOURCE twin = {};
    if (FAILED(context->Map(entry.crossed, 0, D3D11_MAP_WRITE_DISCARD, 0, &twin))
        || !twin.pData) {
        // An empty twin must never become bindable, so it goes away with the
        // slot rather than being left for the draw hook to find.
        entry.crossed->Release();
        entry.crossed = nullptr;
        abandonSeed(slot);
        return;
    }
    memcpy(twin.pData, g_scratch, kGrassStreamBytes);
    context->Unmap(entry.crossed, 0);

    tq::probe::count(tq::probe::CounterGrassSeedDone);
    EnterCriticalSection(&g_grassLock);
    entry.pending = false;
    LeaveCriticalSection(&g_grassLock);
}

void shutdown() {
    g_originalRender[0] = nullptr;
    g_originalRender[1] = nullptr;
    tq::detour::detach(g_render[0]);
    tq::detour::detach(g_render[1]);
    g_engineBegin = nullptr;
    g_engineEnd = nullptr;
    if (g_grassLockReady) {
        EnterCriticalSection(&g_grassLock);
        for (unsigned i = 0; i < kMaxGrassBuffers; ++i) clearStream(i);
        for (unsigned i = 0; i < kMaxCandidates; ++i) clearCandidate(i);
        LeaveCriticalSection(&g_grassLock);
        g_grassLockReady = false;
        DeleteCriticalSection(&g_grassLock);
    }
    memset(g_grassBuffers, 0, sizeof(g_grassBuffers));
    if (g_seedStaging) g_seedStaging->Release();
    g_seedStaging = nullptr;
    g_seedSlot = kMaxGrassBuffers;
    InterlockedExchange(&g_seedStage, 0);
    memset(g_candidates, 0, sizeof(g_candidates));
    g_candidateCursor = 0;
    g_grassHigh = 0;
    g_candidateHigh = 0;
    indexReset(g_streamIndex);
    indexReset(g_candidateIndex);
    g_frameOrdinal = 0;
    g_admissionBlocked = false;
    if (g_scratch) HeapFree(GetProcessHeap(), 0, g_scratch);
    g_scratch = nullptr;
    g_pendingSlot = (LONG)kMaxGrassBuffers;
    InterlockedExchange(&g_twinDrawLogged, 0);
    InterlockedExchange(&g_rendering, 0);
    InterlockedExchange(&g_installed, 0);
}

bool installed() {
    return InterlockedCompareExchange(&g_installed, 1, 1) != 0;
}

}  // namespace grass
}  // namespace tq
