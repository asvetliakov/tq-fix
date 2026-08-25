#include "frames.h"

#include <d3d11_1.h>
#include <d3d11shader.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "patch.h"
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
                      "\tDrawInstanced\tother\tempty\tverts\tmaps\tmaps_busy\tnew_buffers\r\n";
    DWORD wrote;
    WriteFile(h, hdr, (DWORD)strlen(hdr), &wrote, NULL);
}

void tableRow(const char* s, int n) {
    if (g_table == INVALID_HANDLE_VALUE) return;
    DWORD wrote;
    WriteFile(g_table, s, (DWORD)n, &wrote, NULL);
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
    LONG draws = c.drawIndexed + c.draw + c.drawIndexedInst + c.drawInst + c.other;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = g_qpcLast.QuadPart && g_qpcFreq.QuadPart
                    ? 1000.0 * (double)(now.QuadPart - g_qpcLast.QuadPart) / (double)g_qpcFreq.QuadPart
                    : 0.0;
    g_qpcLast = now;

    SYSTEMTIME t;
    GetLocalTime(&t);
    char row[256];
    int n = _snprintf(row, sizeof(row),
                      "%02d:%02d:%02d.%03d\t%lu\t%ld\t%.1f\t%u\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\r\n",
                      t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, (unsigned long)g_pid, frame, dt,
                      sync, draws, c.drawIndexed, c.draw, c.drawIndexedInst, c.drawInst, c.other,
                      c.empty, c.verts, c.maps, c.mapsBusy, c.newBuffers);
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
    HRESULT hr = g_realMap(c, res, sub, type, flags, out);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) add(&g_now.mapsBusy, 1);
    return hr;
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

// Constant buffers: every width the game has ever asked for, with a count.
// Widths are 16-byte multiples and a game has a handful of them, so a small
// fixed table is enough; anything past it is counted as "other".
struct Width { UINT bytes; LONG count; };
Width g_cbWidths[32];
int   g_cbWidthCount;
LONG  g_cbTotal, g_cbOther, g_buffersTotal;
UINT  g_cbLargest;
const LONG kMaxCbLines = 96;    // per-creation lines before we stop detailing

CRITICAL_SECTION g_devLock;     // the tables above; device calls may be off the render thread

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
    HRESULT hr = g_realCreateBuffer(dev, desc, init, out);
    add(&g_now.newBuffers, 1);

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
    QueryPerformanceFrequency(&g_qpcFreq);
    tableOpen();

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
          g_realPresent ? "ok" : "NO", g_realDrawIndexed && g_realDraw ? "ok" : "NO", g_realMap ? "ok" : "NO",
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
}

}  // namespace frames
}  // namespace tq
