#include "frames.h"

#include <d3d11_1.h>
#include <d3d11shader.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "patch.h"
#ifndef TQFLICKER_REROUTE_DEFAULT
#define TQFLICKER_REROUTE_DEFAULT 17
#endif
#include "slots.h"   // generated: scripts/gen-slots.sh, no slot index is typed by hand

namespace tq {
namespace frames {

namespace {

// ------------------------------------------------------------------ counters
//
// Everything the render thread touches per call is an interlocked add on one of
// these, and `Present` swaps the whole block out. D3D11 permits draws from more
// than one thread; the game almost certainly uses one, but a counter that is
// wrong under two would be a wrong fact in docs/rev/, and interlocked adds cost
// nothing at 10fps.
struct Counts {
    volatile LONG drawIndexed, draw, drawIndexedInst, drawInst;
    volatile LONG other;        // DrawAuto + the two indirect draws + ExecuteCommandList
    volatile LONG empty;        // a Draw* whose index/vertex count (x instances) is zero
    volatile LONG verts;        // index or vertex count, times instance count, summed
    volatile LONG maps, mapsBusy;   // Map calls; Map returning WAS_STILL_DRAWING
    volatile LONG newBuffers;   // CreateBuffer calls landing inside the frame
    volatile LONG discardMaps;  // Map(WRITE_DISCARD) calls - the DXMT page-rotation path (O37)
    volatile LONG rerouted;     // Maps served from our shadow copy (TQFLICKER_REROUTE, H-F)
};
Counts g_now;

void add(volatile LONG* c, LONG n) { InterlockedExchangeAdd(c, n); }
LONG  take(volatile LONG* c)       { return InterlockedExchange(c, 0); }

volatile LONG g_frame;           // frames presented so far
LONG          g_totalDraws;      // render-thread only, read at report
LONG          g_lastDraws[2];    // the two frames before this one: dip detection
LONG          g_dips;            // frames whose draw count is below both neighbours
LONG          g_emptyFrames;     // frames containing at least one empty draw
LONG          g_busyFrames;      // frames in which a Map returned WAS_STILL_DRAWING
UINT          g_lastSync = 0xffffffff;
LARGE_INTEGER g_qpcFreq, g_qpcLast;

// A one-line summary into the main log every so many frames. 600 is a minute
// at the 10fps measuring cap and ten seconds of normal play: enough that the
// main log stays readable, and enough that a run which dies mid-way still
// leaves its numbers behind (O23: nothing held for exit survives).
const LONG kSummaryEvery = 600;
LONG g_sumMin, g_sumMax, g_sumDips, g_sumEmpty, g_sumBusy, g_sumBuffers;
LONGLONG g_sumDraws;

// ------------------------------------------------------------- frames table
//
// One line per frame, tab-separated, in its own file. The main log keeps the
// one-off facts; this file is the table, and it is a table with a row every
// 100ms when measuring and every ~10ms when not. It is truncated when the
// device is created, so it always describes the run that wrote it.

HANDLE  g_table = INVALID_HANDLE_VALUE;
wchar_t g_tablePath[MAX_PATH] = L"(none)";
DWORD   g_pid;

void tableOpen() {
    g_pid = GetCurrentProcessId();
    wchar_t path[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, path);
    if (n == 0 || n >= MAX_PATH - 32) return;
    lstrcatW(path, L"tqflicker-frames.log");
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    g_table = h;
    lstrcpynW(g_tablePath, path, MAX_PATH);
    // No FlushFileBuffers per row: WriteFile has already handed the bytes to
    // the kernel, which survives the process dying. Flushing to disk on every
    // frame would be a per-frame stall we added ourselves.
    const char* hdr = "time\tpid\tframe\tdt_ms\tsync\tdraws\tDrawIndexed\tDraw\tDrawIndexedInstanced"
                      "\tDrawInstanced\tother\tempty\tverts\tmaps\tmaps_busy\tnew_buffers"
                      "\tdiscard_maps\tmap_ptrs\tmin_reuse\trerouted\r\n";
    DWORD wrote;
    WriteFile(h, hdr, (DWORD)strlen(hdr), &wrote, NULL);
}

void tableRow(const char* s, int n) {
    if (g_table == INVALID_HANDLE_VALUE) return;
    DWORD wrote;
    WriteFile(g_table, s, (DWORD)n, &wrote, NULL);
}

// ------------------------------------------- Map pointer diagnostic (H-F)
//
// Every `Map(WRITE_DISCARD)` on a dynamic buffer hands back a pointer into one
// of DXMT's pages. Reuse of the same pointer is expected - a 2048-byte buffer
// has a handful of slots per page and the page FIFO turns over hundreds of
// times a frame - so this does not judge; it measures. Per frame: how many
// distinct pointers were handed out, and the shortest distance in frames since
// any of them was last handed out. Render thread only, so no locking.
CRITICAL_SECTION g_devLock;     // the cbuffer tables and g_rr; device calls may be off the render thread
struct PtrSlot { void* p; LONG frame; };
const unsigned kPtrSlots = 1u << 16;
PtrSlot g_ptrs[kPtrSlots];
LONG g_frDistinct, g_frMinReuse = -1;

void notePtr(void* p, LONG frame) {
    unsigned h = (unsigned)(((UINT_PTR)p >> 4) * 2654435761u) >> 16;
    for (unsigned i = 0; i < 8; i++) {
        PtrSlot& sl = g_ptrs[(h + i) & (kPtrSlots - 1)];
        if (sl.p == p) {
            LONG d = frame - sl.frame;
            if (d > 0) {
                g_frDistinct++;
                if (g_frMinReuse < 0 || d < g_frMinReuse) g_frMinReuse = d;
            }
            sl.frame = frame;
            return;
        }
        if (!sl.p) { sl.p = p; sl.frame = frame; g_frDistinct++; return; }
    }
    PtrSlot& sl = g_ptrs[h & (kPtrSlots - 1)];   // table full here: evict
    sl.p = p; sl.frame = frame; g_frDistinct++;
}

// ----------------------------------------------- reroute (H-F's experiment)
//
// TQFLICKER_REROUTE=1: the game's DYNAMIC constant buffers are created DEFAULT
// instead, and their Map/Unmap are served from a shadow copy in our heap that
// is pushed with UpdateSubresource at Unmap. =2 does the same for dynamic
// vertex and index buffers. This takes those buffers off DXMT's i386-only
// CpuPlaced page path entirely (docs/rev/observed.md O37). Off by default.
int g_reroute;
struct Rr { void* res; void* shadow; UINT size; bool alias; };
const unsigned kRrSlots = 4096;
Rr g_rr[kRrSlots];             // open addressing by resource pointer, under g_devLock
LONG g_rrCount, g_rrMaps, g_rrBadMaps;
const LONG kRrTraceEvents = 12;
// DXMT's UpdateSubresource on any non-output buffer calls Map/Unmap through
// the vtable - i.e. through these hooks (a recursion that hung the render
// thread on 2026-08-29). While this thread is inside our own UpdateSubresource,
// both hooks pass straight through.
volatile DWORD g_rrBypassThread;   // the first few served maps/unmaps, timestamped, to see where a hang is

unsigned rrHash(void* p) { return (unsigned)(((UINT_PTR)p >> 3) * 2654435761u) >> 20; }

Rr* rrFind(void* res) {
    unsigned h = rrHash(res);
    for (unsigned i = 0; i < kRrSlots; i++) {
        Rr& r = g_rr[(h + i) & (kRrSlots - 1)];
        if (r.res == res) return &r;
        if (!r.res) return nullptr;
    }
    return nullptr;
}

void rrForget(void* res) {   // the address is being reused by a new buffer
    Rr* r = rrFind(res);
    if (!r) return;
    if (!r->alias) { free(r->shadow); g_rrCount--; }
    r->shadow = nullptr; r->size = 0;
    r->res = (void*)1;       // tombstone: keeps probe chains intact
}

// `alias` entries share the owner's shadow: the same object seen through
// another interface pointer (DXMT's ID3D11Buffer, ID3D11Resource and IUnknown
// are not at one address, which cost a black screen on 2026-08-29).
bool rrAdd(void* res, UINT size, void* shadow = nullptr) {
    unsigned h = rrHash(res);
    for (unsigned i = 0; i < kRrSlots; i++) {
        Rr& r = g_rr[(h + i) & (kRrSlots - 1)];
        if (!r.res || r.res == (void*)1) {
            void* sh = shadow ? shadow : calloc(1, size ? size : 16);
            if (!sh) return false;
            r.res = res; r.shadow = sh; r.size = size; r.alias = shadow != nullptr;
            if (!shadow) g_rrCount++;
            return true;
        }
    }
    return false;
}

// The other identities of a freshly created buffer. Released immediately: we
// keep addresses, not references, and CreateBuffer's own reference keeps the
// object alive for as long as the game holds it.
void otherIdentities(ID3D11Buffer* b, void** resOut, void** unkOut) {
    *resOut = *unkOut = nullptr;
    ID3D11Resource* r = nullptr; IUnknown* u = nullptr;
    if (SUCCEEDED(b->QueryInterface(__uuidof(ID3D11Resource), (void**)&r)) && r) { *resOut = r; r->Release(); }
    if (SUCCEEDED(b->QueryInterface(__uuidof(IUnknown), (void**)&u)) && u) { *unkOut = u; u->Release(); }
}

// ------------------------------------------------ ring (mode 5, the fix)
//
// Modes 3/4 proved the fault is DXMT's rename path but cost a GPU blit per
// update, which splits the render pass and loses FX drawn into offscreen
// targets (O40). Mode 5 stays on ordinary DYNAMIC buffers and simply never
// asks DXMT to rename one soon: each game constant buffer becomes a ring of
// kRingK identical buffers; Map(DISCARD) advances the ring, maps the next
// member, and re-binds it wherever the game had the original bound.
const int kRingK = 4096;
const int kRings = 64;
const int kStages = 6;                // VS PS GS HS DS CS
const int kCbSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;   // 14
const int kVbSlots = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;          // 32
const UINT kRingBytes = 16u << 20;    // budget per ring; K = clamp(budget / size, 8, kRingK)
struct Ring {
    ID3D11Buffer* handle;             // what the game holds
    ID3D11Buffer** members;           // k buffers, members[0] == handle
    int cur, k;
    UINT size, bind;
};
Ring g_rings[kRings];
int  g_ringCount;
signed char g_bound[kStages][kCbSlots];   // ring index bound at (stage, slot), or -1
void* g_boundPtr[kStages][kCbSlots];      // the game's buffer pointer at (stage, slot)
signed char g_vbBound[kVbSlots];          // ring index bound at IA slot, or -1
UINT g_vbStride[kVbSlots], g_vbOffset[kVbSlots];
signed char g_ibBound = -1; DXGI_FORMAT g_ibFormat; UINT g_ibOffset;
LONG g_ringMaps, g_ringRebinds, g_dynSeen, g_barriers;

// Mode 12: cut the number of frames - and so the number of DXMT command
// chunks - in flight to one. O41 says the artefact grows with chunks in
// flight (mode 10) and vanishes with none (mode 8), which is what a
// per-chunk transient recycled too early looks like. This is a data-side
// DXGI call, no reroute, no wait.
void setFrameLatency(ID3D11Device* dev, UINT want) {
    IDXGIDevice1* d1 = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&d1)) || !d1) {
        tqlog("!! latency: no IDXGIDevice1 - cannot set the frame latency");
        return;
    }
    UINT was = 0;
    d1->GetMaximumFrameLatency(&was);
    HRESULT hr = d1->SetMaximumFrameLatency(want);
    UINT now = 0;
    d1->GetMaximumFrameLatency(&now);
    tqlog("latency:  maximum frame latency was %u, asked for %u, now %u (hr 0x%08lx)", was, want, now,
          (unsigned long)hr);
    d1->Release();
}

// Mode 13: mode 8's wait, but only every kSyncEvery-th discard - a cheaper
// mitigation if the full wait turns out to be the only thing that works.
const LONG kSyncEvery = 4;

// Mode 14: DXMT suballocates any buffer whose size is <= DXMT_PAGE_SIZE (4096)
// into one shared page, and rotates a cursor through it on every discard
// (O37). A buffer LARGER than the page skips that entirely and gets a whole
// allocation of its own. So: pad every small DYNAMIC buffer past 4096 bytes.
// The game still maps it, still writes the same bytes at the same offsets, and
// nothing becomes a blit - so unlike modes 3/4 this cannot cost an FX.
const UINT kPadTo = 8192;
LONG g_padded;

// Mode 16 = mode 4 (one never-renamed allocation, blit updates - the fix that
// works) PLUS a forced re-dirty of every binding of the buffer after each
// update. DXMT dedups a redundant SetConstantBuffers (bind() with the same
// pointer dirties nothing), so the re-dirty is scratch-then-real, two calls.
// Modes 3/4 are the only fixes so far and they are also the only modes where
// the per-draw argument re-encode never runs; if THIS mode flickers, that
// re-encode is the faulty machinery. If it stays clean, it is not.
ID3D11Buffer* g_scratchCb;
LONG g_redirties;
void forceRedirty(ID3D11DeviceContext* c, void* res);

// ------------------------------------------ mode 17: 16KB-align the pages
//
// DXMT's i386 build backs every dynamic buffer with `_aligned_malloc`'d heap
// memory wrapped by `newBufferWithBytesNoCopy:` (O37). The pointers it hands
// out are 4KB-aligned but NOT host-page-aligned (O42, `mapptr:`), and Apple
// Silicon documents a 16KB page-alignment requirement for bytesNoCopy. This
// mode redirects d3d11.dll's import of `_aligned_malloc` - one IAT data
// write - to a wrapper that rounds the alignment up to 16KB and the size up
// to a 16KB multiple. If the flicker dies with nothing else changed, the
// fork's handling of unaligned bytesNoCopy memory is the fault.
typedef void* (__cdecl* AlignedMallocFn)(size_t, size_t);
AlignedMallocFn g_realAlignedMalloc;
LONG g_alignCalls, g_alignWidened;
const size_t kHostPage = 16384;

void* __cdecl hookAlignedMalloc(size_t size, size_t align) {
    if (!g_realAlignedMalloc) return nullptr;
    g_alignCalls++;
    size_t a = align, sz = size;
    if (a < kHostPage) a = kHostPage;
    if (sz % kHostPage) sz += kHostPage - (sz % kHostPage);
    if (a != align || sz != size) g_alignWidened++;
    return g_realAlignedMalloc(sz, a);
}

void installAlignHook() {
    HMODULE dxmt = GetModuleHandleW(L"d3d11.dll");
    if (!dxmt) { tqlog("!! align: d3d11.dll not in the process?"); return; }
    // The device exists, so d3d11.dll is long loaded and its imports snapped -
    // the wait device.cpp needs at process start does not apply here.
    void* orig = patch::iat(dxmt, "d3d11.dll", "api-ms-win-crt-heap-l1-1-0.dll", "_aligned_malloc",
                            (void*)&hookAlignedMalloc);
    if (!orig) { tqlog("!! align: d3d11.dll does not import _aligned_malloc where expected"); return; }
    g_realAlignedMalloc = (AlignedMallocFn)orig;
    tqlog("align:    d3d11.dll!_aligned_malloc -> ours; every allocation now %u-aligned, %u-multiple",
          (unsigned)kHostPage, (unsigned)kHostPage);
}


// Mode 7: no rerouting at all; at every Present, an event query + Flush + spin
// until the GPU reports it done. Kills pipelining on purpose: if the flicker
// survives a GPU that is provably idle at every frame boundary, the race is
// not GPU-side memory reuse.
ID3D11Device*  g_dev;
ID3D11Query*   g_syncQuery;
LONG g_syncWaits, g_syncSpinsMax;
void gpuSync(ID3D11DeviceContext* c) {
    if (!g_dev) return;
    if (!g_syncQuery) {
        D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
        if (FAILED(g_dev->CreateQuery(&qd, &g_syncQuery)) || !g_syncQuery) { g_dev = nullptr; tqlog("!! sync: CreateQuery failed"); return; }
    }
    c->End(g_syncQuery);
    c->Flush();
    LONG spins = 0;
    BOOL done = FALSE;
    while (c->GetData(g_syncQuery, &done, sizeof(done), 0) == S_FALSE) { if (++spins > 2000000) break; if (spins & 1023) continue; Sleep(0); }
    g_syncWaits++;
    if (spins > g_syncSpinsMax) g_syncSpinsMax = spins;
}

Ring* ringOf(void* res) {
    for (int i = 0; i < g_ringCount; i++)
        if ((void*)g_rings[i].handle == res) return &g_rings[i];
    return nullptr;
}

bool wantReroute(const D3D11_BUFFER_DESC* d) {
    if (!g_reroute || !d) return false;
    if (g_reroute >= 7 && g_reroute != 15 && g_reroute != 16) return false;   // sync/flush/pad modes never reroute
    if (d->Usage != D3D11_USAGE_DYNAMIC) return false;
    if (!(d->CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) || (d->CPUAccessFlags & D3D11_CPU_ACCESS_READ)) return false;
    if (d->MiscFlags) return false;
    UINT ok = D3D11_BIND_CONSTANT_BUFFER;
    if (g_reroute == 2 || g_reroute == 6 || g_reroute == 15) ok |= D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
    return (d->BindFlags & ~ok) == 0 && (d->BindFlags & ok) != 0;
}

// ----------------------------------------------------------------- Present

typedef HRESULT(WINAPI* PresentFn)(IDXGISwapChain*, UINT, UINT);
PresentFn g_realPresent;

/**
 * The frame boundary. Row `N` describes every call between Present `N-1` and
 * Present `N`, stamped with the wall-clock time at which the game finished
 * submitting it - which is the clock the screen recording is aligned against.
 */
HRESULT WINAPI hookPresent(IDXGISwapChain* self, UINT sync, UINT flags) {
    if (!g_realPresent) return E_FAIL;

    // DXGI_PRESENT_TEST asks "would this work?" and presents nothing. Not a frame.
    if (flags & DXGI_PRESENT_TEST) return g_realPresent(self, sync, flags);

    LONG frame = InterlockedIncrement(&g_frame);
    if (g_reroute == 7) {
        ID3D11DeviceContext* ctx = nullptr;
        if (g_dev) { g_dev->GetImmediateContext(&ctx); if (ctx) { gpuSync(ctx); ctx->Release(); } }
    }

    Counts c;
    c.drawIndexed     = take(&g_now.drawIndexed);
    c.draw            = take(&g_now.draw);
    c.drawIndexedInst = take(&g_now.drawIndexedInst);
    c.drawInst        = take(&g_now.drawInst);
    c.other           = take(&g_now.other);
    c.empty           = take(&g_now.empty);
    c.verts           = take(&g_now.verts);
    c.maps            = take(&g_now.maps);
    c.mapsBusy        = take(&g_now.mapsBusy);
    c.newBuffers      = take(&g_now.newBuffers);
    c.discardMaps     = take(&g_now.discardMaps);
    c.rerouted        = take(&g_now.rerouted);
    LONG ptrDistinct  = g_frDistinct;  g_frDistinct = 0;
    LONG ptrMinReuse  = g_frMinReuse;  g_frMinReuse = -1;
    LONG draws = c.drawIndexed + c.draw + c.drawIndexedInst + c.drawInst + c.other;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = g_qpcLast.QuadPart && g_qpcFreq.QuadPart
                    ? 1000.0 * (double)(now.QuadPart - g_qpcLast.QuadPart) / (double)g_qpcFreq.QuadPart
                    : 0.0;
    g_qpcLast = now;

    SYSTEMTIME t;
    GetLocalTime(&t);
    char row[320];
    int n = _snprintf(row, sizeof(row),
                      "%02d:%02d:%02d.%03d\t%lu\t%ld\t%.1f\t%u\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld"
                      "\t%ld\t%ld\t%ld\t%ld\r\n",
                      t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, (unsigned long)g_pid, frame, dt,
                      sync, draws, c.drawIndexed, c.draw, c.drawIndexedInst, c.drawInst, c.other,
                      c.empty, c.verts, c.maps, c.mapsBusy, c.newBuffers,
                      c.discardMaps, ptrDistinct, ptrMinReuse, c.rerouted);
    if (n > 0 && n < (int)sizeof(row)) tableRow(row, n);

    // Dip detection is on the *previous* frame, now that both its neighbours
    // are known: prev < this and prev < prev-prev.
    if (frame >= 3 && g_lastDraws[0] < g_lastDraws[1] && g_lastDraws[0] < draws) {
        g_dips++; g_sumDips++;
    }
    g_lastDraws[1] = g_lastDraws[0];
    g_lastDraws[0] = draws;

    g_totalDraws += draws;
    if (c.empty)    { g_emptyFrames++; g_sumEmpty++; }
    if (c.mapsBusy) { g_busyFrames++;  g_sumBusy++;  }
    g_sumBuffers += c.newBuffers;
    g_sumDraws   += draws;
    if (frame == 1 || draws < g_sumMin) g_sumMin = draws;
    if (draws > g_sumMax) g_sumMax = draws;

    if (frame == 1) {
        tqlog("frames:   first Present (sync interval %u, flags 0x%x) - the frame counter is running;"
              " the per-frame table is %S", sync, flags, g_tablePath);
    }
    if (sync != g_lastSync) {
        if (g_lastSync != 0xffffffff)
            tqlog("frames:   Present sync interval changed %u -> %u at frame %ld (vsync toggled; O10d)",
                  g_lastSync, sync, frame);
        g_lastSync = sync;
    }
    if (frame % kSummaryEvery == 0) {
        tqlog("frames:   %ld-%ld: draws/frame min %ld max %ld mean %.1f; dips %ld, frames with an"
              " empty draw %ld, with a busy Map %ld; buffers created %ld",
              frame - kSummaryEvery + 1, frame, g_sumMin, g_sumMax,
              (double)g_sumDraws / kSummaryEvery, g_sumDips, g_sumEmpty, g_sumBusy, g_sumBuffers);
        g_sumMin = g_sumMax = 0; g_sumDraws = 0;
        g_sumDips = g_sumEmpty = g_sumBusy = g_sumBuffers = 0;
    }

    return g_realPresent(self, sync, flags);
}

// ------------------------------------------------------------------- Draw*
//
// Each one: count, sum the vertices, note an empty draw, call through. The
// argument lists are D3D11's, verbatim; nothing is inspected beyond the counts.

typedef void(WINAPI* DrawIndexedFn)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void(WINAPI* DrawFn)(ID3D11DeviceContext*, UINT, UINT);
typedef void(WINAPI* DrawIndexedInstancedFn)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void(WINAPI* DrawInstancedFn)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef void(WINAPI* DrawAutoFn)(ID3D11DeviceContext*);
typedef void(WINAPI* DrawIndirectFn)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
typedef void(WINAPI* ExecuteCommandListFn)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
typedef HRESULT(WINAPI* MapFn)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                               D3D11_MAPPED_SUBRESOURCE*);

DrawIndexedFn          g_realDrawIndexed;
DrawFn                 g_realDraw;
DrawIndexedInstancedFn g_realDrawIndexedInstanced;
DrawInstancedFn        g_realDrawInstanced;
DrawAutoFn             g_realDrawAuto;
DrawIndirectFn         g_realDrawIndexedInstancedIndirect;
DrawIndirectFn         g_realDrawInstancedIndirect;
ExecuteCommandListFn   g_realExecuteCommandList;
MapFn                  g_realMap;
typedef void(WINAPI* UnmapFn)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
UnmapFn                g_realUnmap;
typedef void(WINAPI* SetCBFn)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
SetCBFn g_realSetCB[kStages];
const char* kStageName[kStages] = {"VS", "PS", "GS", "HS", "DS", "CS"};

void ringSetCB(ID3D11DeviceContext* c, int stage, UINT start, UINT n, ID3D11Buffer* const* bufs) {
    if (!g_realSetCB[stage]) return;
    if (bufs && n && start + n <= (UINT)kCbSlots)
        for (UINT i = 0; i < n; i++) g_boundPtr[stage][start + i] = (void*)bufs[i];
    if (!g_ringCount || !bufs || n == 0 || start + n > (UINT)kCbSlots) {
        if (bufs && n && start + n <= (UINT)kCbSlots)
            for (UINT i = 0; i < n; i++) g_bound[stage][start + i] = -1;
        g_realSetCB[stage](c, start, n, bufs);
        return;
    }
    ID3D11Buffer* sub[kCbSlots];
    for (UINT i = 0; i < n; i++) {
        sub[i] = bufs[i];
        g_bound[stage][start + i] = -1;
        for (int r = 0; r < g_ringCount; r++) {
            if (g_rings[r].handle == bufs[i]) {
                sub[i] = g_rings[r].members[g_rings[r].cur];
                g_bound[stage][start + i] = (signed char)r;
                break;
            }
        }
    }
    g_realSetCB[stage](c, start, n, sub);
}
void WINAPI hookVSSetCB(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) { ringSetCB(c, 0, s, n, b); }
void WINAPI hookPSSetCB(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) { ringSetCB(c, 1, s, n, b); }
void WINAPI hookGSSetCB(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) { ringSetCB(c, 2, s, n, b); }
void WINAPI hookHSSetCB(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) { ringSetCB(c, 3, s, n, b); }
void WINAPI hookDSSetCB(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) { ringSetCB(c, 4, s, n, b); }
void WINAPI hookCSSetCB(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) { ringSetCB(c, 5, s, n, b); }

typedef void(WINAPI* SetVBFn)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
typedef void(WINAPI* SetIBFn)(ID3D11DeviceContext*, ID3D11Buffer*, DXGI_FORMAT, UINT);
SetVBFn g_realSetVB; SetIBFn g_realSetIB;

// Mode 9: a Flush at every render-target change. DXMT reorders and coalesces
// encoders inside a command chunk; a Flush commits the chunk, so passes can
// no longer be reordered across this boundary. No blits, no memory.
typedef void(WINAPI* SetRTFn)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
SetRTFn g_realSetRT;
LONG g_rtFlushes;
void WINAPI hookSetRT(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (!g_realSetRT) return;
    if (g_reroute == 9) { c->Flush(); g_rtFlushes++; }
    g_realSetRT(c, n, rtvs, dsv);
}

void WINAPI hookSetVB(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* bufs, const UINT* strides,
                      const UINT* offsets) {
    if (!g_realSetVB) return;
    if (!bufs || !n || start + n > (UINT)kVbSlots || !g_ringCount) {
        if (n && start + n <= (UINT)kVbSlots) for (UINT i = 0; i < n; i++) g_vbBound[start + i] = -1;
        g_realSetVB(c, start, n, bufs, strides, offsets);
        return;
    }
    ID3D11Buffer* sub[kVbSlots];
    for (UINT i = 0; i < n; i++) {
        sub[i] = bufs[i];
        g_vbBound[start + i] = -1;
        g_vbStride[start + i] = strides ? strides[i] : 0;
        g_vbOffset[start + i] = offsets ? offsets[i] : 0;
        for (int r = 0; r < g_ringCount; r++)
            if (g_rings[r].handle == bufs[i]) { sub[i] = g_rings[r].members[g_rings[r].cur]; g_vbBound[start + i] = (signed char)r; break; }
    }
    g_realSetVB(c, start, n, sub, strides, offsets);
}

void WINAPI hookSetIB(ID3D11DeviceContext* c, ID3D11Buffer* buf, DXGI_FORMAT fmt, UINT off) {
    if (!g_realSetIB) return;
    g_ibBound = -1; g_ibFormat = fmt; g_ibOffset = off;
    for (int r = 0; r < g_ringCount; r++)
        if (buf && g_rings[r].handle == buf) { g_ibBound = (signed char)r; buf = g_rings[r].members[g_rings[r].cur]; break; }
    g_realSetIB(c, buf, fmt, off);
}

// After the ring advanced: every slot that held this ring gets the new member.
void ringRebind(ID3D11DeviceContext* c, int r) {
    ID3D11Buffer* m = g_rings[r].members[g_rings[r].cur];
    if (g_realSetVB)
        for (int sl = 0; sl < kVbSlots; sl++)
            if (g_vbBound[sl] == r) { g_realSetVB(c, (UINT)sl, 1, &m, &g_vbStride[sl], &g_vbOffset[sl]); g_ringRebinds++; }
    if (g_realSetIB && g_ibBound == r) { g_realSetIB(c, m, g_ibFormat, g_ibOffset); g_ringRebinds++; }
    for (int st = 0; st < kStages; st++) {
        if (!g_realSetCB[st]) continue;
        for (int sl = 0; sl < kCbSlots; sl++)
            if (g_bound[st][sl] == r) { g_realSetCB[st](c, (UINT)sl, 1, &m); g_ringRebinds++; }
    }
}

void WINAPI hookDrawIndexed(ID3D11DeviceContext* c, UINT count, UINT start, INT base) {
    add(&g_now.drawIndexed, 1);
    add(&g_now.verts, (LONG)count);
    if (!count) add(&g_now.empty, 1);
    if (g_realDrawIndexed) g_realDrawIndexed(c, count, start, base);
}

void WINAPI hookDraw(ID3D11DeviceContext* c, UINT count, UINT start) {
    add(&g_now.draw, 1);
    add(&g_now.verts, (LONG)count);
    if (!count) add(&g_now.empty, 1);
    if (g_realDraw) g_realDraw(c, count, start);
}

void WINAPI hookDrawIndexedInstanced(ID3D11DeviceContext* c, UINT count, UINT instances,
                                     UINT start, INT base, UINT startInstance) {
    add(&g_now.drawIndexedInst, 1);
    add(&g_now.verts, (LONG)(count * instances));
    if (!count || !instances) add(&g_now.empty, 1);
    if (g_realDrawIndexedInstanced)
        g_realDrawIndexedInstanced(c, count, instances, start, base, startInstance);
}

void WINAPI hookDrawInstanced(ID3D11DeviceContext* c, UINT count, UINT instances, UINT start,
                              UINT startInstance) {
    add(&g_now.drawInst, 1);
    add(&g_now.verts, (LONG)(count * instances));
    if (!count || !instances) add(&g_now.empty, 1);
    if (g_realDrawInstanced) g_realDrawInstanced(c, count, instances, start, startInstance);
}

void WINAPI hookDrawAuto(ID3D11DeviceContext* c) {
    add(&g_now.other, 1);
    if (g_realDrawAuto) g_realDrawAuto(c);
}

void WINAPI hookDrawIndexedInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT off) {
    add(&g_now.other, 1);
    if (g_realDrawIndexedInstancedIndirect) g_realDrawIndexedInstancedIndirect(c, args, off);
}

void WINAPI hookDrawInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT off) {
    add(&g_now.other, 1);
    if (g_realDrawInstancedIndirect) g_realDrawInstancedIndirect(c, args, off);
}

// A command list is draws recorded on a deferred context, whose vtable we have
// not patched. Counting the execute at least says whether the game uses one:
// if `other` is ever non-zero and the three *Indirect/Auto lines never fire,
// this is where the draws we cannot see went.
void WINAPI hookExecuteCommandList(ID3D11DeviceContext* c, ID3D11CommandList* list, BOOL restore) {
    add(&g_now.other, 1);
    if (g_realExecuteCommandList) g_realExecuteCommandList(c, list, restore);
}

// H-E was refuted by the config knob (O13). This counts the thing directly:
// how often does `Map` actually come back WAS_STILL_DRAWING? Zero on the bad
// frame closes it from the other side; non-zero would reopen it.
HRESULT WINAPI hookMap(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub, D3D11_MAP type,
                       UINT flags, D3D11_MAPPED_SUBRESOURCE* out) {
    if (!g_realMap) return E_FAIL;
    add(&g_now.maps, 1);
    if (type == D3D11_MAP_WRITE_DISCARD) add(&g_now.discardMaps, 1);
    if (g_reroute == 8 && type == D3D11_MAP_WRITE_DISCARD) gpuSync(c);   // GPU provably idle at every discard
    if (g_reroute == 13 && type == D3D11_MAP_WRITE_DISCARD) {
        static LONG k;
        if ((++k % kSyncEvery) == 0) gpuSync(c);
    }
    if (g_reroute == 10 && type == D3D11_MAP_WRITE_DISCARD) { c->Flush(); g_rtFlushes++; }   // chunk boundary, no wait

    if ((g_reroute == 5 || g_reroute == 6 || g_reroute == 15) && sub == 0 && out) {
        if (Ring* rg = ringOf((void*)res)) {
            if (type == D3D11_MAP_WRITE_DISCARD) {
                rg->cur = (rg->cur + 1) % rg->k;
                ringRebind(c, (int)(rg - g_rings));
            } else if (type != D3D11_MAP_WRITE_NO_OVERWRITE) {
                return g_realMap(c, res, sub, type, flags, out);
            }
            // Mode 15: NO_OVERWRITE is served from the member's existing
            // allocation - DXMT does not rename, does not suballocate, does
            // not allocate. The ring is what makes that safe: a member is
            // rewritten only after every other member has been used, which is
            // thousands of draws and several frames later. No blit, so unlike
            // modes 3/4 this cannot cost an FX (O40).
            D3D11_MAP mt = (g_reroute == 15 && type == D3D11_MAP_WRITE_DISCARD) ? D3D11_MAP_WRITE_NO_OVERWRITE : type;
            HRESULT hr = g_realMap(c, rg->members[rg->cur], sub, mt, flags, out);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) add(&g_now.mapsBusy, 1);
            if (SUCCEEDED(hr)) { add(&g_now.rerouted, 1); g_ringMaps++; }
            return hr;
        }
    }

    if (g_reroute && (g_reroute < 5 || g_reroute == 16) && sub == 0 && out && g_rrBypassThread != GetCurrentThreadId()) {
        EnterCriticalSection(&g_devLock);
        Rr* r = rrFind(res);
        void* shadow = r ? r->shadow : nullptr;
        UINT  size   = r ? r->size : 0;
        LeaveCriticalSection(&g_devLock);
        if (shadow) {
            if (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE) {
                out->pData = shadow;
                out->RowPitch = out->DepthPitch = size;
                add(&g_now.rerouted, 1);
                if (++g_rrMaps <= kRrTraceEvents)
                    tqlog("reroute:  Map #%ld %p type %d -> shadow %p (%u B), frame %ld", g_rrMaps, (void*)res,
                          (int)type, shadow, (unsigned)size, (long)g_frame);
                return S_OK;
            }
            // A map type the shadow cannot honour. The buffer is DEFAULT now, so
            // the real Map would fail too; count it and say so once.
            if (!g_rrBadMaps++) tqlog("!! reroute: Map type %d on a rerouted buffer %p", (int)type, (void*)res);
            return E_INVALIDARG;
        }
    }

    HRESULT hr = g_realMap(c, res, sub, type, flags, out);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) add(&g_now.mapsBusy, 1);
    if (SUCCEEDED(hr) && out && type == D3D11_MAP_WRITE_DISCARD) {
        notePtr(out->pData, g_frame);
        // The shipped DXMT is a fork whose page size we cannot read from
        // source. The pointers it hands back say it empirically: their
        // alignment gives the page size, their low bits the suballocation
        // stride, and (ptr & 0x3FFF) whether the pages are 16KB-host-page
        // aligned, which newBufferWithBytesNoCopy on Apple Silicon nominally
        // requires (O37/O41).
        static LONG ptrLines;
        if (++ptrLines <= 24)
            tqlog("mapptr:   #%ld res %p -> %p (low bits 0x%03x, mod16k 0x%04x)", ptrLines, (void*)res,
                  out->pData, (unsigned)((UINT_PTR)out->pData & 0xFFF), (unsigned)((UINT_PTR)out->pData & 0x3FFF));
    }
    return hr;
}

void WINAPI hookUnmap(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub) {
    if (!g_realUnmap) return;
    // Mode 11: the game has just finished writing into DXMT's mapped page. If
    // those stores are still in flight when the GPU reads the page - which is
    // what "only a full GPU wait fixes it" (mode 8) and "more flushes make it
    // worse" (mode 10) both point at - a barrier here is the whole fix, and it
    // costs one instruction. FEX has to honour an x86 fence.
    if (g_reroute == 11) { __sync_synchronize(); g_barriers++; }
    if ((g_reroute == 5 || g_reroute == 6 || g_reroute == 15) && sub == 0) {
        if (Ring* rg = ringOf((void*)res)) { g_realUnmap(c, rg->members[rg->cur], sub); return; }
    }
    if (g_reroute && (g_reroute < 5 || g_reroute == 16) && sub == 0 && g_rrBypassThread != GetCurrentThreadId()) {
        EnterCriticalSection(&g_devLock);
        Rr* r = rrFind(res);
        void* shadow = r ? r->shadow : nullptr;
        LeaveCriticalSection(&g_devLock);
        if (shadow) {
            g_rrBypassThread = GetCurrentThreadId();
            // Not hooked, so this is DXMT's own UpdateSubresource: a DEFAULT-buffer
            // write, which on i386 takes the updateContents path, not the page path.
            static LONG n;
            LONG k = ++n;
            if (k <= kRrTraceEvents) tqlog("reroute:  Unmap #%ld %p -> UpdateSubresource...", k, (void*)res);
            c->UpdateSubresource(res, 0, nullptr, shadow, 0, 0);
            if (g_reroute == 16) forceRedirty(c, (void*)res);
            g_rrBypassThread = 0;
            if (k <= kRrTraceEvents) tqlog("reroute:  Unmap #%ld %p <- returned", k, (void*)res);
            return;
        }
    }
    g_realUnmap(c, res, sub);
}

// ------------------------------------------------------- device: resources
//
// The device vtable is open for the same reason, so the H-B1 errand and the
// sampler errand from the plan are done here. All of it is logging, bounded.

typedef HRESULT(WINAPI* CreateBufferFn)(ID3D11Device*, const D3D11_BUFFER_DESC*,
                                        const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
typedef HRESULT(WINAPI* CreateVSFn)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                    ID3D11VertexShader**);
typedef HRESULT(WINAPI* CreatePSFn)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                    ID3D11PixelShader**);
typedef HRESULT(WINAPI* CreateSamplerFn)(ID3D11Device*, const D3D11_SAMPLER_DESC*,
                                         ID3D11SamplerState**);

CreateBufferFn  g_realCreateBuffer;
CreateVSFn      g_realCreateVS;
CreatePSFn      g_realCreatePS;
CreateSamplerFn g_realCreateSampler;

void forceRedirty(ID3D11DeviceContext* c, void* res) {
    if (!g_dev) return;
    if (!g_scratchCb) {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = 2048; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (!g_realCreateBuffer || FAILED(g_realCreateBuffer(g_dev, &d, nullptr, &g_scratchCb)) || !g_scratchCb) {
            tqlog("!! redirty: no scratch buffer"); g_dev = nullptr; return;
        }
    }
    for (int st = 0; st < kStages; st++) {
        if (!g_realSetCB[st]) continue;
        for (int sl = 0; sl < kCbSlots; sl++) {
            if (g_boundPtr[st][sl] == res) {
                ID3D11Buffer* b = (ID3D11Buffer*)res;
                g_realSetCB[st](c, (UINT)sl, 1, &g_scratchCb);
                g_realSetCB[st](c, (UINT)sl, 1, &b);
                g_redirties++;
            }
        }
    }
}

// Constant buffers: every width the game has ever asked for, with a count.
// Widths are 16-byte multiples and a game has a handful of them, so a small
// fixed table is enough; anything past it is counted as "other".
struct Width { UINT bytes; LONG count; };
Width g_cbWidths[32];
int   g_cbWidthCount;
LONG  g_cbTotal, g_cbOther, g_buffersTotal;
UINT  g_cbLargest;
const LONG kMaxCbLines = 96;    // per-creation lines before we stop detailing

// g_devLock is declared with the pointer tables above.

const char* usageName(D3D11_USAGE u) {
    switch (u) {
        case D3D11_USAGE_DEFAULT:   return "DEFAULT";
        case D3D11_USAGE_IMMUTABLE: return "IMMUTABLE";
        case D3D11_USAGE_DYNAMIC:   return "DYNAMIC";
        case D3D11_USAGE_STAGING:   return "STAGING";
        default:                    return "?";
    }
}

void noteCbWidth(UINT bytes) {
    for (int i = 0; i < g_cbWidthCount; i++)
        if (g_cbWidths[i].bytes == bytes) { g_cbWidths[i].count++; return; }
    if (g_cbWidthCount < (int)(sizeof(g_cbWidths) / sizeof(g_cbWidths[0]))) {
        g_cbWidths[g_cbWidthCount].bytes = bytes;
        g_cbWidths[g_cbWidthCount].count = 1;
        g_cbWidthCount++;
    } else {
        g_cbOther++;
    }
}

HRESULT WINAPI hookCreateBuffer(ID3D11Device* dev, const D3D11_BUFFER_DESC* desc,
                                const D3D11_SUBRESOURCE_DATA* init, ID3D11Buffer** out) {
    if (!g_realCreateBuffer) return E_FAIL;
    bool reroute = wantReroute(desc);
    if (g_reroute == 14 && desc && desc->Usage == D3D11_USAGE_DYNAMIC && desc->ByteWidth <= 4096 &&
        !desc->MiscFlags) {
        D3D11_BUFFER_DESC pad = *desc;
        pad.ByteWidth = kPadTo;
        HRESULT hr = g_realCreateBuffer(dev, &pad, nullptr, out);
        if (SUCCEEDED(hr)) {
            // Initial data, if any, goes in afterwards: the padded buffer is
            // bigger than the game's, so CreateBuffer would have rejected it.
            if (init && init->pSysMem && out && *out) {
                D3D11_MAPPED_SUBRESOURCE m;
                ID3D11DeviceContext* ctx = nullptr;
                dev->GetImmediateContext(&ctx);
                if (ctx) {
                    if (SUCCEEDED(g_realMap ? g_realMap(ctx, *out, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)
                                            : ctx->Map(*out, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                        memcpy(m.pData, init->pSysMem, desc->ByteWidth);
                        if (g_realUnmap) g_realUnmap(ctx, *out, 0); else ctx->Unmap(*out, 0);
                    }
                    ctx->Release();
                }
            }
            if (++g_padded <= 16)
                tqlog("pad:      #%ld %p  %u -> %u bytes, bind 0x%x, frame %ld", g_padded, out ? (void*)*out : nullptr,
                      (unsigned)desc->ByteWidth, kPadTo, (unsigned)desc->BindFlags, (long)g_frame);
            add(&g_now.newBuffers, 1);
            EnterCriticalSection(&g_devLock);
            g_buffersTotal++;
            if (desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER) { g_cbTotal++; noteCbWidth(desc->ByteWidth); }
            LeaveCriticalSection(&g_devLock);
            return hr;
        }
        tqlog("!! pad: CreateBuffer refused %u bytes (hr 0x%08lx) - leaving this one as the game asked",
              (unsigned)kPadTo, (unsigned long)hr);
    }
    static LONG dynLines;
    if (desc && desc->Usage == D3D11_USAGE_DYNAMIC) g_dynSeen++;
    if (desc && desc->Usage == D3D11_USAGE_DYNAMIC && ++dynLines <= 80)
        tqlog("dynamic:  #%ld %u bytes, bind 0x%x, cpu 0x%x, misc 0x%x, frame %ld%s", dynLines, (unsigned)desc->ByteWidth,
              (unsigned)desc->BindFlags, (unsigned)desc->CPUAccessFlags, (unsigned)desc->MiscFlags, (long)g_frame,
              reroute ? "  -> ring" : "");
    if (reroute && (g_reroute == 5 || g_reroute == 6 || g_reroute == 15)) {
        HRESULT hr = g_realCreateBuffer(dev, desc, init, out);
        if (SUCCEEDED(hr) && out && *out) {
            // Big buffers get a smaller budget: 64 rings x 16 MB would not fit a 32-bit heap.
            UINT budget = desc->ByteWidth > 4096 ? (8u << 20) : kRingBytes;
            int k = (int)(budget / (desc->ByteWidth ? desc->ByteWidth : 1));
            if (k > kRingK) k = kRingK;
            if (k < 8) k = 8;
            EnterCriticalSection(&g_devLock);
            if (g_ringCount < kRings) {
                Ring& rg = g_rings[g_ringCount];
                rg.members = (ID3D11Buffer**)calloc(k, sizeof(ID3D11Buffer*));
                int made = 0;
                if (rg.members) {
                    rg.members[0] = *out;
                    made = 1;
                    for (; made < k; made++) {
                        if (FAILED(g_realCreateBuffer(dev, desc, init, &rg.members[made])) || !rg.members[made]) break;
                    }
                }
                if (made == k) {
                    rg.handle = *out; rg.cur = 0; rg.k = k; rg.size = desc->ByteWidth; rg.bind = desc->BindFlags;
                    g_ringCount++;
                    tqlog("ring:     #%d %p  %u bytes, bind 0x%x: ring of %d dynamic buffers, frame %ld", g_ringCount,
                          (void*)*out, (unsigned)desc->ByteWidth, (unsigned)desc->BindFlags, k, (long)g_frame);
                } else {
                    tqlog("!! ring: only %d of %d members created for %p - buffer left as the game made it", made,
                          k, (void*)*out);
                    for (int i = 1; i < made; i++) if (rg.members[i]) rg.members[i]->Release();
                    free(rg.members); rg.members = nullptr;
                }
            } else {
                tqlog("!! ring: table full - %p left as the game made it", (void*)*out);
            }
            LeaveCriticalSection(&g_devLock);
        }
        reroute = false;              // fall through to the accounting below with the real result
        add(&g_now.newBuffers, 1);
        if (SUCCEEDED(hr) && desc && (desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER)) {
            EnterCriticalSection(&g_devLock);
            g_cbTotal++; g_buffersTotal++;
            noteCbWidth(desc->ByteWidth);
            if (desc->ByteWidth > g_cbLargest) g_cbLargest = desc->ByteWidth;
            LeaveCriticalSection(&g_devLock);
        } else if (SUCCEEDED(hr)) {
            EnterCriticalSection(&g_devLock); g_buffersTotal++; LeaveCriticalSection(&g_devLock);
        }
        return hr;
    }
    D3D11_BUFFER_DESC alt;
    if (reroute) {
        alt = *desc;
        alt.Usage = D3D11_USAGE_DEFAULT;
        alt.CPUAccessFlags = 0;
        // Mode 3: an output bind flag keeps DXMT from giving the buffer a
        // DynamicBuffer at all, so UpdateSubresource becomes a GPU blit into
        // one allocation that is never renamed (O37/O38).
        if (g_reroute == 3) alt.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        if (g_reroute == 4 || g_reroute == 16) alt.BindFlags |= D3D11_BIND_STREAM_OUTPUT;   // same no-rename effect, no UAV semantics
    }
    HRESULT hr = g_realCreateBuffer(dev, reroute ? &alt : desc, init, out);
    if (reroute && FAILED(hr)) {
        tqlog("!! reroute: CreateBuffer refused the altered desc (hr 0x%08lx, bind 0x%x) - creating it as the game asked",
              (unsigned long)hr, (unsigned)alt.BindFlags);
        reroute = false;
        hr = g_realCreateBuffer(dev, desc, init, out);
    }
    add(&g_now.newBuffers, 1);

    if (SUCCEEDED(hr) && out && *out) {
        void *asRes, *asUnk;
        otherIdentities(*out, &asRes, &asUnk);
        EnterCriticalSection(&g_devLock);
        rrForget((void*)*out);     // an address coming back around is a new object
        if (asRes && asRes != (void*)*out) rrForget(asRes);
        if (asUnk && asUnk != (void*)*out && asUnk != asRes) rrForget(asUnk);
        bool added = reroute && rrAdd((void*)*out, desc->ByteWidth);
        if (added) {
            Rr* own = rrFind((void*)*out);
            if (asRes && asRes != (void*)*out) rrAdd(asRes, own->size, own->shadow);
            if (asUnk && asUnk != (void*)*out && asUnk != asRes) rrAdd(asUnk, own->size, own->shadow);
        }
        LONG nr = g_rrCount;
        LeaveCriticalSection(&g_devLock);
        if (added && nr <= 24)
            tqlog("reroute:  identities buffer %p resource %p unknown %p", (void*)*out, asRes, asUnk);
        if (reroute && !added) {
            tqlog("!! reroute: table full or out of memory for %p - buffer left DEFAULT and UNSERVED", (void*)*out);
        } else if (added && nr <= 24) {
            tqlog("reroute:  #%ld %p  %u bytes, bind 0x%x: DYNAMIC -> DEFAULT + shadow, frame %ld", nr,
                  (void*)*out, (unsigned)desc->ByteWidth, (unsigned)desc->BindFlags, (long)g_frame);
        }
    }

    if (SUCCEEDED(hr) && desc && (desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER)) {
        EnterCriticalSection(&g_devLock);
        LONG n = ++g_cbTotal;
        g_buffersTotal++;
        noteCbWidth(desc->ByteWidth);
        if (desc->ByteWidth > g_cbLargest) g_cbLargest = desc->ByteWidth;
        LeaveCriticalSection(&g_devLock);

        if (n <= kMaxCbLines) {
            tqlog("cbuffer:  #%ld %p  %u bytes, %s, cpu 0x%x, misc 0x%x%s, frame %ld", n,
                  out ? (void*)*out : nullptr, (unsigned)desc->ByteWidth, usageName(desc->Usage),
                  (unsigned)desc->CPUAccessFlags, (unsigned)desc->MiscFlags,
                  init ? ", with initial data" : "", (long)g_frame);
        } else if (n == kMaxCbLines + 1) {
            tqlog("cbuffer:  more constant buffers - counted by width from here on, see the"
                  " summary lines");
        }
    } else {
        EnterCriticalSection(&g_devLock);
        g_buffersTotal++;
        LeaveCriticalSection(&g_devLock);
    }
    return hr;
}

// ------------------------------------------------------- device: shaders
//
// `D3DReflect` from the D3DCOMPILER_43 already in the process (substrate.md).
// The IID is compiler-version specific; this is 43's. The interface's leading
// methods - GetDesc, GetConstantBufferByIndex, GetResourceBindingDesc - are
// the same in 43 and 47, and nothing later is called.

typedef HRESULT(WINAPI* ReflectFn)(const void*, SIZE_T, REFIID, void**);
ReflectFn g_reflect;
bool      g_reflectLookedUp;
const GUID kIID_Reflection43 = {0x0a233719, 0x3960, 0x4578, {0x9d, 0x7c, 0x20, 0x3b, 0x8b, 0x1d, 0x9c, 0xc1}};

LONG g_shaders, g_shaderLines, g_declaredOver;
UINT g_declaredLargest;
const LONG kMaxShaderLines = 160;

void lookupReflect() {
    if (g_reflectLookedUp) return;
    g_reflectLookedUp = true;
    // GetModuleHandle, never LoadLibrary: Direct3D11.dll imports this module
    // statically so it is here, and loading anything from inside a hook is a
    // way to take the loader lock on the game's thread.
    HMODULE m = GetModuleHandleW(L"D3DCOMPILER_43.dll");
    if (!m) {
        tqlog("reflect:  D3DCOMPILER_43.dll is not loaded - shader constant-buffer sizes will not"
              " be reflected");
        return;
    }
    g_reflect = (ReflectFn)(void*)GetProcAddress(m, "D3DReflect");
    tqlog("reflect:  D3DReflect %s in D3DCOMPILER_43.dll", g_reflect ? "found" : "NOT FOUND");
}

/** Declared constant-buffer sizes of one shader, logged, and compared with the
 *  widths the game has created so far. */
void reflectShader(const char* kind, const void* code, SIZE_T len, void* result) {
    lookupReflect();
    if (!g_reflect || !code || !len) return;

    ID3D11ShaderReflection* r = nullptr;
    HRESULT hr = g_reflect(code, len, kIID_Reflection43, (void**)&r);
    LONG n = InterlockedIncrement(&g_shaders);
    if (FAILED(hr) || !r) {
        if (n <= 8) tqlog("reflect:  %s %p: D3DReflect failed, hr 0x%08lx", kind, result, (unsigned long)hr);
        return;
    }

    D3D11_SHADER_DESC sd;
    if (SUCCEEDED(r->GetDesc(&sd))) {
        char line[512];
        int at = _snprintf(line, sizeof(line), "reflect:  %s %p: %u cbuffer(s)", kind, result,
                           (unsigned)sd.ConstantBuffers);
        if (at < 0) at = 0;
        UINT over = 0;
        for (UINT i = 0; i < sd.ConstantBuffers && at < (int)sizeof(line) - 48; i++) {
            ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByIndex(i);
            D3D11_SHADER_BUFFER_DESC bd;
            if (!cb || FAILED(cb->GetDesc(&bd))) continue;
            if (bd.Type != D3D_CT_CBUFFER) continue;

            // Which slot? The binding table says, by name.
            int slot = -1;
            for (UINT j = 0; j < sd.BoundResources; j++) {
                D3D11_SHADER_INPUT_BIND_DESC ib;
                if (FAILED(r->GetResourceBindingDesc(j, &ib))) continue;
                if (ib.Type == D3D_SIT_CBUFFER && bd.Name && ib.Name && strcmp(bd.Name, ib.Name) == 0) {
                    slot = (int)ib.BindPoint;
                    break;
                }
            }
            int w = _snprintf(line + at, sizeof(line) - at, "  b%d %s %uB", slot,
                              bd.Name ? bd.Name : "?", (unsigned)bd.Size);
            if (w < 0) break;
            at += w;

            if (bd.Size > g_declaredLargest) g_declaredLargest = bd.Size;
            if (bd.Size > g_cbLargest) over = bd.Size;
        }
        line[sizeof(line) - 1] = 0;

        LONG shown = InterlockedIncrement(&g_shaderLines);
        if (shown <= kMaxShaderLines) tqlog("%s", line);
        else if (shown == kMaxShaderLines + 1) tqlog("reflect:  more shaders - counted, not detailed");
        if (over) {
            InterlockedIncrement(&g_declaredOver);
            // Always logged, never capped: this is the one line H-B1 is waiting for.
            // It is only suggestive - the buffer may be created later - so the
            // summary at exit restates it against the final largest width.
            tqlog("!! reflect: %s %p declares a %uB cbuffer; the largest constant buffer the game"
                  " has created so far is %uB (H-B1 candidate, see docs/plans/stage-4)",
                  kind, result, (unsigned)over, (unsigned)g_cbLargest);
        }
    }
    r->Release();
}

HRESULT WINAPI hookCreateVS(ID3D11Device* dev, const void* code, SIZE_T len,
                            ID3D11ClassLinkage* link, ID3D11VertexShader** out) {
    if (!g_realCreateVS) return E_FAIL;
    HRESULT hr = g_realCreateVS(dev, code, len, link, out);
    if (SUCCEEDED(hr)) reflectShader("VS", code, len, out ? (void*)*out : nullptr);
    return hr;
}

HRESULT WINAPI hookCreatePS(ID3D11Device* dev, const void* code, SIZE_T len,
                            ID3D11ClassLinkage* link, ID3D11PixelShader** out) {
    if (!g_realCreatePS) return E_FAIL;
    HRESULT hr = g_realCreatePS(dev, code, len, link, out);
    if (SUCCEEDED(hr)) reflectShader("PS", code, len, out ? (void*)*out : nullptr);
    return hr;
}

// ------------------------------------------------------- device: samplers
//
// The errand for the Stage 6 bug report: the complete description of every
// sampler, so the one DXMT warns about (O2) can be named by pass and by filter.

LONG g_samplers, g_samplersBorder;
const LONG kMaxSamplerLines = 64;

const char* addressName(D3D11_TEXTURE_ADDRESS_MODE m) {
    switch (m) {
        case D3D11_TEXTURE_ADDRESS_WRAP:        return "WRAP";
        case D3D11_TEXTURE_ADDRESS_MIRROR:      return "MIRROR";
        case D3D11_TEXTURE_ADDRESS_CLAMP:       return "CLAMP";
        case D3D11_TEXTURE_ADDRESS_BORDER:      return "BORDER";
        case D3D11_TEXTURE_ADDRESS_MIRROR_ONCE: return "MIRROR_ONCE";
        default:                                return "?";
    }
}

HRESULT WINAPI hookCreateSampler(ID3D11Device* dev, const D3D11_SAMPLER_DESC* d,
                                 ID3D11SamplerState** out) {
    if (!g_realCreateSampler) return E_FAIL;
    HRESULT hr = g_realCreateSampler(dev, d, out);
    if (FAILED(hr) || !d) return hr;

    LONG n = InterlockedIncrement(&g_samplers);
    bool border = d->AddressU == D3D11_TEXTURE_ADDRESS_BORDER ||
                  d->AddressV == D3D11_TEXTURE_ADDRESS_BORDER ||
                  d->AddressW == D3D11_TEXTURE_ADDRESS_BORDER;
    if (border) InterlockedIncrement(&g_samplersBorder);
    bool comparison = (d->Filter & 0x80) != 0;    // D3D11_FILTER_COMPARISON_* all set bit 7

    if (n <= kMaxSamplerLines || border) {
        tqlog("sampler:  #%ld %p  filter 0x%x%s  addr %s/%s/%s  border (%g, %g, %g, %g)  cmp %d"
              "  lod %g..%g bias %g  aniso %u  frame %ld",
              n, out ? (void*)*out : nullptr, (unsigned)d->Filter, comparison ? " (comparison)" : "",
              addressName(d->AddressU), addressName(d->AddressV), addressName(d->AddressW),
              d->BorderColor[0], d->BorderColor[1], d->BorderColor[2], d->BorderColor[3],
              (int)d->ComparisonFunc, d->MinLOD, d->MaxLOD, d->MipLODBias, (unsigned)d->MaxAnisotropy,
              (long)g_frame);
    } else if (n == kMaxSamplerLines + 1) {
        tqlog("sampler:  more samplers - counted from here on; BORDER ones are still logged");
    }
    return hr;
}

// --------------------------------------------------------------- installing

bool g_installed;

template <typename T>
bool slot(void** vt, int idx, const char* what, void* hook, T* realOut) {
    void* orig = patch::vtableSlot(vt, idx, what, hook);
    if (!orig) return false;
    *realOut = (T)orig;
    return true;
}

void** vtableOf(void* obj) {
    if (!patch::readable(obj, sizeof(void*))) return nullptr;
    return *(void***)obj;
}

/** Risk 3, for the context this time. Returns the Context1 vtable if it is a
 *  different one, else null. */
void** probeContext1(ID3D11DeviceContext* ctx) {
    ID3D11DeviceContext1* c1 = nullptr;
    HRESULT hr = ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1);
    if (FAILED(hr) || !c1) {
        tqlog("  ID3D11DeviceContext1: not supported (hr 0x%08lx) - one interface, one vtable",
              (unsigned long)hr);
        return nullptr;
    }
    void** vt1 = vtableOf(c1);
    bool same = ((void*)c1 == (void*)ctx) && vt1 == vtableOf(ctx);
    tqlog("  ID3D11DeviceContext1 %p (vtable %p) - %s object as ID3D11DeviceContext%s",
          (void*)c1, (void*)vt1, same ? "the SAME" : "a DIFFERENT",
          same ? "" : "  <-- Risk 3 for the context is real; patching BOTH vtables");
    c1->Release();
    return same ? nullptr : vt1;
}

void patchContext(void** vt, const char* name) {
    if (!vt) return;
    slot(vt, TQ_SLOT_ID3D11DeviceContext_DrawIndexed, name, (void*)&hookDrawIndexed, &g_realDrawIndexed);
    slot(vt, TQ_SLOT_ID3D11DeviceContext_Draw, name, (void*)&hookDraw, &g_realDraw);
    slot(vt, TQ_SLOT_ID3D11DeviceContext_DrawIndexedInstanced, name, (void*)&hookDrawIndexedInstanced,
         &g_realDrawIndexedInstanced);
    slot(vt, TQ_SLOT_ID3D11DeviceContext_DrawInstanced, name, (void*)&hookDrawInstanced, &g_realDrawInstanced);
    slot(vt, TQ_SLOT_ID3D11DeviceContext_DrawAuto, name, (void*)&hookDrawAuto, &g_realDrawAuto);
    slot(vt, TQ_SLOT_ID3D11DeviceContext_DrawIndexedInstancedIndirect, name,
         (void*)&hookDrawIndexedInstancedIndirect, &g_realDrawIndexedInstancedIndirect);
    slot(vt, TQ_SLOT_ID3D11DeviceContext_DrawInstancedIndirect, name, (void*)&hookDrawInstancedIndirect,
         &g_realDrawInstancedIndirect);
    slot(vt, TQ_SLOT_ID3D11DeviceContext_ExecuteCommandList, name, (void*)&hookExecuteCommandList,
         &g_realExecuteCommandList);
    slot(vt, TQ_SLOT_ID3D11DeviceContext_Map, name, (void*)&hookMap, &g_realMap);
    slot(vt, TQ_SLOT_ID3D11DeviceContext_Unmap, name, (void*)&hookUnmap, &g_realUnmap);
    slot(vt, TQ_SLOT_ID3D11DeviceContext_VSSetConstantBuffers, name, (void*)&hookVSSetCB, &g_realSetCB[0]);
    slot(vt, TQ_SLOT_ID3D11DeviceContext_PSSetConstantBuffers, name, (void*)&hookPSSetCB, &g_realSetCB[1]);
    slot(vt, TQ_SLOT_ID3D11DeviceContext_GSSetConstantBuffers, name, (void*)&hookGSSetCB, &g_realSetCB[2]);
    slot(vt, TQ_SLOT_ID3D11DeviceContext_HSSetConstantBuffers, name, (void*)&hookHSSetCB, &g_realSetCB[3]);
    slot(vt, TQ_SLOT_ID3D11DeviceContext_DSSetConstantBuffers, name, (void*)&hookDSSetCB, &g_realSetCB[4]);
    slot(vt, TQ_SLOT_ID3D11DeviceContext_CSSetConstantBuffers, name, (void*)&hookCSSetCB, &g_realSetCB[5]);
    slot(vt, TQ_SLOT_ID3D11DeviceContext_IASetVertexBuffers, name, (void*)&hookSetVB, &g_realSetVB);
    slot(vt, TQ_SLOT_ID3D11DeviceContext_IASetIndexBuffer, name, (void*)&hookSetIB, &g_realSetIB);
    slot(vt, TQ_SLOT_ID3D11DeviceContext_OMSetRenderTargets, name, (void*)&hookSetRT, &g_realSetRT);
}

}  // namespace

bool install(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGISwapChain* sc) {
    if (g_installed) {
        tqlog("frames:   a second device - not patching it; the first one is the renderer's");
        return false;
    }
    if (!dev || !ctx || !sc) {
        tqlog("!! frames: missing an object (device %p, context %p, swapchain %p) - not patching",
              (void*)dev, (void*)ctx, (void*)sc);
        return false;
    }
    g_installed = true;
    InitializeCriticalSection(&g_devLock);
    memset(g_bound, -1, sizeof(g_bound));
    g_dev = dev;
    memset(g_vbBound, -1, sizeof(g_vbBound));
    {
        wchar_t v[8] = L"";
        DWORD n = GetEnvironmentVariableW(L"TQFLICKER_REROUTE", v, 8);
        // Compiled-in default (the env var did not reach the game on 2026-08-29,
        // reason unknown); the variable can still override it either way.
        g_reroute = TQFLICKER_REROUTE_DEFAULT;
        if (n && n < 8) {
            int m = 0; bool ok = v[0] != 0;
            for (DWORD i = 0; i < n && v[i]; i++) {
                if (v[i] < L'0' || v[i] > L'9') { ok = false; break; }
                m = m * 10 + (int)(v[i] - L'0');
            }
            if (ok) g_reroute = m;
        }
        tqlog("frames:   TQFLICKER_REROUTE=%d - %s", g_reroute,
              g_reroute == 0 ? "observing only (dynamic buffers untouched)"
              : g_reroute == 1 ? "dynamic CONSTANT buffers -> DEFAULT + shadow + UpdateSubresource (H-F)"
              : g_reroute == 2 ? "dynamic constant, VERTEX and INDEX buffers -> DEFAULT + shadow (H-F)"
              : g_reroute == 3 ? "dynamic CONSTANT buffers -> DEFAULT|UAV: no DXMT rename at all, GPU blit (H-F)"
              : g_reroute == 4 ? "dynamic CONSTANT buffers -> DEFAULT|STREAM_OUTPUT: no rename, GPU blit (fountain test)"
              : g_reroute == 5 ? "dynamic CONSTANT buffers -> a RING of dynamic buffers, rebound on every discard"
              : g_reroute == 6 ? "dynamic constant, VERTEX and INDEX buffers -> RINGS of dynamic buffers, rebound on every discard"
              : g_reroute == 7 ? "NO rerouting; full GPU sync (event query + Flush + wait) at every Present"
              : g_reroute == 8 ? "NO rerouting; full GPU sync before EVERY Map(WRITE_DISCARD) - a slideshow, on purpose"
              : g_reroute == 9 ? "NO rerouting; Flush at every OMSetRenderTargets (pins pass order across DXMT's encoder reordering)"
              : g_reroute == 10 ? "NO rerouting; Flush (no wait) before EVERY Map(WRITE_DISCARD)"
              : g_reroute == 11 ? "NO rerouting; a full memory BARRIER at every Unmap (store visibility, one instruction)"
              : g_reroute == 12 ? "NO rerouting; SetMaximumFrameLatency(1) - one frame, and so few chunks, in flight"
              : g_reroute == 13 ? "NO rerouting; a full GPU sync every 4th discard (cheap-mitigation probe)"
              : g_reroute == 14 ? "small DYNAMIC buffers PADDED past DXMT's 4096-byte page: no suballocation, no blit"
              : g_reroute == 15 ? "RINGS of dynamic buffers mapped NO_OVERWRITE: DXMT never renames or allocates, and no blit"
              : g_reroute == 16 ? "mode 4 PLUS a forced per-draw re-dirty of the binding (does the argument re-encode flicker?)"
                                : "NO rerouting; d3d11.dll's _aligned_malloc IAT-hooked to 16KB alignment and size (H: bytesNoCopy)");
    }
    QueryPerformanceFrequency(&g_qpcFreq);
    tableOpen();

    if (g_reroute == 12) setFrameLatency(dev, 1);   // after the mode is known, not before (cost one launch)
    if (g_reroute == 17) installAlignHook();

    void** vtSc  = vtableOf(sc);
    void** vtCtx = vtableOf(ctx);
    void** vtDev = vtableOf(dev);
    int before = patch::installed();

    tqlog("frames:   patching vtables (slots from the MinGW headers via scripts/gen-slots.sh:"
          " swapchain has %d methods, context %d, device %d)",
          TQ_SLOTS_IDXGISwapChain, TQ_SLOTS_ID3D11DeviceContext, TQ_SLOTS_ID3D11Device);

    slot(vtSc, TQ_SLOT_IDXGISwapChain_Present, "IDXGISwapChain::Present", (void*)&hookPresent, &g_realPresent);

    // Risk 3, asked of the context before it is patched (RUNBOOK). If Context1
    // turned out to be a different vtable it would NOT be patched here: the
    // game was handed an ID3D11DeviceContext and calls through that, and
    // patching a second table would overwrite the originals stored for the
    // first. The log line says which case this run is in, and a draw count that
    // is implausibly low would be the sign the game switched interfaces.
    void** vtCtx1 = probeContext1(ctx);
    if (vtCtx1) tqlog("!! frames: ID3D11DeviceContext1 has its own vtable at %p and is NOT patched -"
                      " only the context the game was handed is", (void*)vtCtx1);
    patchContext(vtCtx, "ID3D11DeviceContext");

    slot(vtDev, TQ_SLOT_ID3D11Device_CreateBuffer, "ID3D11Device", (void*)&hookCreateBuffer, &g_realCreateBuffer);
    slot(vtDev, TQ_SLOT_ID3D11Device_CreateVertexShader, "ID3D11Device", (void*)&hookCreateVS, &g_realCreateVS);
    slot(vtDev, TQ_SLOT_ID3D11Device_CreatePixelShader, "ID3D11Device", (void*)&hookCreatePS, &g_realCreatePS);
    slot(vtDev, TQ_SLOT_ID3D11Device_CreateSamplerState, "ID3D11Device", (void*)&hookCreateSampler,
         &g_realCreateSampler);

    int made = patch::installed() - before;
    tqlog("frames:   %d vtable slot(s) patched%s. Present=%s Draw*=%s Map=%s CreateBuffer=%s shaders=%s/%s"
          " sampler=%s",
          made, g_realPresent && g_realDrawIndexed ? "" : "  <-- INCOMPLETE",
          g_realPresent ? "ok" : "NO", g_realDrawIndexed && g_realDraw ? "ok" : "NO", g_realMap && g_realUnmap ? "ok" : "NO",
          g_realCreateBuffer ? "ok" : "NO", g_realCreateVS ? "ok" : "NO", g_realCreatePS ? "ok" : "NO",
          g_realCreateSampler ? "ok" : "NO");
    return g_realPresent && g_realDrawIndexed;
}

void report(const char* when) {
    if (!g_installed) {
        tqlog("frames (%s): nothing patched in this process.", when);
        return;
    }
    LONG frames = g_frame;
    if (!frames) {
        tqlog("!! frames (%s): patched, but Present never fired. Either the game never presented, or"
              " it calls through a vtable we did not patch.", when);
    } else {
        tqlog("frames (%s): %ld frame(s), %ld draw(s), %.1f per frame; %ld dip frame(s), %ld with an"
              " empty draw, %ld with a busy Map. Table: %S",
              when, frames, g_totalDraws, (double)g_totalDraws / frames, g_dips, g_emptyFrames,
              g_busyFrames, g_tablePath);
    }
    char widths[512] = "";
    int at = 0;
    for (int i = 0; i < g_cbWidthCount && at < (int)sizeof(widths) - 24; i++) {
        int w = _snprintf(widths + at, sizeof(widths) - at, "%s%u x%ld", i ? ", " : "",
                          (unsigned)g_cbWidths[i].bytes, g_cbWidths[i].count);
        if (w < 0) break;
        at += w;
    }
    widths[sizeof(widths) - 1] = 0;
    tqlog("cbuffer (%s): %ld constant buffer(s) of %ld buffer(s); widths: %s%s; largest %uB",
          when, g_cbTotal, g_buffersTotal, widths, g_cbOther ? " (+more)" : "", (unsigned)g_cbLargest);
    tqlog("reflect (%s): %ld shader(s); largest declared cbuffer %uB against largest created %uB -"
          " %ld shader(s) declared more than had been created at the time%s",
          when, g_shaders, (unsigned)g_declaredLargest, (unsigned)g_cbLargest, g_declaredOver,
          g_declaredLargest > g_cbLargest ? "  <-- STILL larger at exit: H-B1 has an instance" : "");
    tqlog("sampler (%s): %ld sampler(s), %ld with ADDRESS_BORDER", when, g_samplers, g_samplersBorder);
    if (g_reroute == 17)
        tqlog("align (%s): %ld _aligned_malloc call(s) seen, %ld widened to 16KB", when, g_alignCalls, g_alignWidened);
    if (g_reroute == 16)
        tqlog("redirty (%s): %ld forced re-dirt(ies) after updates", when, g_redirties);
    if (g_reroute == 14)
        tqlog("pad (%s): %ld dynamic buffer(s) padded to %u bytes", when, g_padded, (unsigned)kPadTo);
    if (g_reroute == 13)
        tqlog("sync16 (%s): %ld GPU wait(s), longest %ld spin(s)", when, g_syncWaits, g_syncSpinsMax);
    if (g_reroute == 11)
        tqlog("barrier (%s): %ld barrier(s) at Unmap", when, g_barriers);
    if (g_reroute == 9 || g_reroute == 10)
        tqlog("flush (%s): %ld Flush(es), %.1f per frame", when, g_rtFlushes,
              g_frame ? (double)g_rtFlushes / g_frame : 0.0);
    if (g_reroute == 7 || g_reroute == 8)
        tqlog("sync (%s): %ld GPU wait(s) at Present, longest %ld spin(s)", when, g_syncWaits, g_syncSpinsMax);
    if (g_reroute == 5 || g_reroute == 6 || g_reroute == 15)
        tqlog("ring (%s): %d ring(s) of %d, %ld map(s) through the rings, %ld rebind(s); %ld dynamic buffer(s) seen",
              when, g_ringCount, kRings, g_ringMaps, g_ringRebinds, g_dynSeen);
    else if (g_reroute)
        tqlog("reroute (%s): mode %d, %ld buffer(s) rerouted, %ld map(s) served from shadow, %ld unservable",
              when, g_reroute, g_rrCount, g_rrMaps, g_rrBadMaps);
}

}  // namespace frames
}  // namespace tq
