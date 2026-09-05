#include "streaming.h"
#include "hdr.h"
#include "probe.h"

#include <windows.h>
#include <string.h>

namespace tq {
namespace streaming {
namespace {

typedef HRESULT (__thiscall* RendererPresentFn)(void*, void*);
typedef HRESULT (WINAPI* ResizeBuffersFn)(IDXGISwapChain*, UINT, UINT, UINT,
                                          DXGI_FORMAT, UINT);

// Verified against the Steam and GOG 2022 renderer. Validate the executable
// layout and exact wrapper instructions, not linker timestamp metadata.
const DWORD kRendererImageSize = 0x192000;
const DWORD kRendererPresentRva = 0x61190;
const DWORD kRendererPresentSlotRva = 0x8625c;
const DWORD kRendererSwapChainOffset = 0x34;
// The complete verified Present wrapper normalizes this byte to interval 0/1.
const DWORD kRendererVsyncOffset = 0x5db;
const BYTE kRendererPresentCode[] = {
    0x8b, 0x51, 0x34, 0x33, 0xc0, 0x38, 0x81, 0xdb,
    0x05, 0x00, 0x00, 0x56, 0x8b, 0x32, 0x0f, 0x95,
    0xc0, 0x6a, 0x00, 0x50, 0x52, 0xff, 0x56, 0x20,
    0x5e, 0xc2, 0x04, 0x00
};

volatile LONG g_rendererInstalled;
RendererPresentFn g_rendererPresent;
void** g_rendererPresentSlot;
volatile LONG g_firstPresentReturned;
// Milestone frames for the trace log; hdr::log is a no-op while trace is off,
// so this costs one interlocked increment per present.
volatile LONG g_presentReturnCount;
volatile LONG g_resizeInstalled;
ResizeBuffersFn g_resizeBuffers;
void** g_resizeSlot;
void (*g_presentCallback)(IDXGISwapChain*);
void (*g_postPresentCallback)(IDXGISwapChain*);
void (*g_preResizeCallback)(IDXGISwapChain*);
void (*g_resizeCallback)(IDXGISwapChain*);
IDXGISwapChain* g_tearingSwapChain; // borrowed; reset on chain installation/shutdown
bool g_tearingPresentLogged;

bool readable(const void* address) {
    MEMORY_BASIC_INFORMATION info = {};
    if (!address || !VirtualQuery(address, &info, sizeof(info))) return false;
    DWORD protection = info.Protect & 0xff;
    return info.State == MEM_COMMIT && !(info.Protect & PAGE_GUARD)
        && protection != PAGE_NOACCESS;
}

bool writePointer(void** slot, void* value) {
    if (!slot || !value || !readable(slot)) return false;
    DWORD oldProtection;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtection))
        return false;
    InterlockedExchangePointer((PVOID volatile*)slot, value);
    DWORD ignored;
    VirtualProtect(slot, sizeof(*slot), oldProtection, &ignored);
    return true;
}

HRESULT __fastcall hookRendererPresent(void* renderer, void*, void* parameter) {
    IDXGISwapChain* swapChain = nullptr;
    BYTE* field = (BYTE*)renderer + kRendererSwapChainOffset;
    if (renderer && readable(field)) swapChain = *(IDXGISwapChain**)field;
    if (g_presentCallback) g_presentCallback(swapChain);
    // Timed separately from everything else: this is where a frame waits when
    // the GPU is behind, and telling that apart from work on this thread is
    // the difference between a CPU problem and a GPU one.
    HRESULT result;
    {
        tq::probe::Scope timing(tq::probe::PhasePresentCall);
        bool tear = false;
        if (swapChain && swapChain == g_tearingSwapChain
            && !*((BYTE*)renderer + kRendererVsyncOffset)) {
            // Query current state to handle Alt+Enter. A failed query must not
            // accidentally pass the windowed-only flag in exclusive fullscreen.
            BOOL fullscreen = TRUE;
            tear = SUCCEEDED(swapChain->GetFullscreenState(&fullscreen, nullptr))
                && !fullscreen;
        }
        if (tear) {
            // The verified wrapper does only Present(interval, 0). Preserve
            // its live DXGI/overlay dispatch while supplying the extra flag;
            // never patch or cache a shared DXGI Present function pointer.
            result = swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
            if (result == DXGI_ERROR_INVALID_CALL || result == E_INVALIDARG) {
                // A runtime/overlay may reject the optional flag despite the
                // capability report. Failed calls presented nothing; retry
                // once through the original path, then leave tearing disabled.
                g_tearingSwapChain = nullptr;
                tq::hdr::log("Tearing Present rejected: hr=0x%08lx; reverting to original presentation\r\n",
                             (unsigned long)result);
                result = g_rendererPresent(renderer, parameter);
            } else if (result == S_OK && !g_tearingPresentLogged) {
                g_tearingPresentLogged = true;
                tq::hdr::log("Tearing Present active: syncInterval=0 flags=0x%x\r\n",
                             DXGI_PRESENT_ALLOW_TEARING);
            }
        } else {
            result = g_rendererPresent(renderer, parameter);
        }
    }
    LONG present = InterlockedIncrement(&g_presentReturnCount);
    if (present == 1 || present == 2 || present == 30 || present == 120
        || present == 180 || present == 240 || present == 300
        || present == 600 || present == 1200 || result != S_OK)
        tq::hdr::log("Renderer Present result: frame=%ld hr=0x%08lx\r\n",
                     present, (unsigned long)result);
    if (!InterlockedCompareExchange(&g_firstPresentReturned, 1, 0))
        tq::hdr::log("First renderer Present returned through the overlay chain: hr=0x%08lx\r\n",
                     (unsigned long)result);
    if (SUCCEEDED(result) && g_postPresentCallback)
        g_postPresentCallback(swapChain);
    return result;
}

HRESULT WINAPI hookResizeBuffers(IDXGISwapChain* swapChain, UINT count,
                                 UINT width, UINT height, DXGI_FORMAT format,
                                 UINT flags) {
    DXGI_SWAP_CHAIN_DESC current = {};
    if (SUCCEEDED(swapChain->GetDesc(&current))
        && current.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        // ALLOW_TEARING is immutable across ResizeBuffers.
        flags = (flags & ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)
              | (current.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
    }
    if (g_preResizeCallback) g_preResizeCallback(swapChain);
    HRESULT hr = g_resizeBuffers(swapChain, count, width, height, format, flags);
    if (SUCCEEDED(hr) && g_resizeCallback) g_resizeCallback(swapChain);
    return hr;
}

}  // namespace

bool optimizationEnabled(const wchar_t* value) {
    return !value || _wcsicmp(value, L"original") != 0;
}

bool installRenderer(HMODULE renderer) {
    if (g_rendererInstalled == 2) return true;
    if (!renderer || InterlockedCompareExchange(&g_rendererInstalled, 1, 0))
        return g_rendererInstalled == 2;

    BYTE* base = (BYTE*)renderer;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = nullptr;
    if (readable(dos) && dos->e_magic == IMAGE_DOS_SIGNATURE)
        nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    bool identity = nt && readable(nt) && nt->Signature == IMAGE_NT_SIGNATURE
                 && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386
                 && nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC
                 && nt->OptionalHeader.SizeOfImage == kRendererImageSize;
    BYTE* original = identity ? base + kRendererPresentRva : nullptr;
    void** slot = identity ? (void**)(base + kRendererPresentSlotRva) : nullptr;
    bool signature = original && readable(original)
                  && readable(original + sizeof(kRendererPresentCode) - 1)
                  && !memcmp(original, kRendererPresentCode,
                             sizeof(kRendererPresentCode));
    if (!signature || !slot || !readable(slot) || *slot != original) {
        tq::hdr::log("Renderer Present hook skipped: incompatible renderer identity/signature\r\n");
        InterlockedExchange(&g_rendererInstalled, 0);
        return false;
    }

    g_rendererPresent = (RendererPresentFn)(void*)original;
    g_rendererPresentSlot = slot;
    if (!writePointer(slot, (void*)&hookRendererPresent)) {
        g_rendererPresent = nullptr;
        g_rendererPresentSlot = nullptr;
        InterlockedExchange(&g_rendererInstalled, 0);
        tq::hdr::log("Renderer Present hook failed while patching its vtable\r\n");
        return false;
    }
    InterlockedExchange(&g_rendererInstalled, 2);
    tq::hdr::log("Renderer Present wrapper hooked without modifying the DXGI Present slot\r\n");
    return true;
}

bool presentHookInstalled() {
    return g_rendererInstalled == 2;
}

void installSwapChain(IDXGISwapChain* swapChain) {
    g_tearingSwapChain = nullptr;
    g_tearingPresentLogged = false;
    if (!swapChain) {
        tq::hdr::log("Swap-chain Resize hook skipped: no swap chain\r\n");
        return;
    }
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (SUCCEEDED(swapChain->GetDesc(&desc))) {
        if (desc.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
            && desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD
            && (desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))
            g_tearingSwapChain = swapChain;
        tq::hdr::log("Swap-chain presentation: tearingEligible=%u windowed=%u refresh=%u/%u flags=0x%x\r\n",
                     g_tearingSwapChain ? 1u : 0u, desc.Windowed ? 1u : 0u,
                     desc.BufferDesc.RefreshRate.Numerator,
                     desc.BufferDesc.RefreshRate.Denominator, desc.Flags);
    }
    // Steam may re-establish its DXGI hooks after device creation. Never join
    // that shared vtable chain: Present is handled above the overlay, and the
    // game renderer recreates rather than ResizeBuffers its primary chain.
    if (GetModuleHandleW(L"gameoverlayrenderer.dll")) {
        tq::hdr::log("Swap-chain Resize hook skipped: Steam overlay loaded\r\n");
        return;
    }
    if (InterlockedCompareExchange(&g_resizeInstalled, 1, 0)) {
        tq::hdr::log("Swap-chain Resize hook skipped: already installed\r\n");
        return;
    }
    void** vtable = *(void***)swapChain;
    if (!readable(vtable) || !readable(vtable + 13)
        || !readable(vtable[13])) {
        tq::hdr::log("Swap-chain Resize hook failed: unreadable vtable=%p\r\n", vtable);
        InterlockedExchange(&g_resizeInstalled, 0);
        return;
    }
    g_resizeSlot = &vtable[13];
    g_resizeBuffers = (ResizeBuffersFn)vtable[13];
    if (!writePointer(g_resizeSlot, (void*)&hookResizeBuffers)) {
        g_resizeSlot = nullptr;
        g_resizeBuffers = nullptr;
        InterlockedExchange(&g_resizeInstalled, 0);
        tq::hdr::log("Swap-chain Resize hook failed while patching its slot\r\n");
    } else {
        tq::hdr::log("Swap-chain Resize hook installed; DXGI Present left untouched\r\n");
    }
}

void setPresentCallback(void (*callback)(IDXGISwapChain*)) {
    g_presentCallback = callback;
}

void setPostPresentCallback(void (*callback)(IDXGISwapChain*)) {
    g_postPresentCallback = callback;
}

void setPreResizeCallback(void (*callback)(IDXGISwapChain*)) {
    g_preResizeCallback = callback;
}

void setResizeCallback(void (*callback)(IDXGISwapChain*)) {
    g_resizeCallback = callback;
}

void shutdown() {
    releaseSwapChain();
    g_presentCallback = nullptr;
    g_postPresentCallback = nullptr;
    g_preResizeCallback = nullptr;
    g_resizeCallback = nullptr;
    if (g_rendererPresentSlot && g_rendererPresent
        && readable(g_rendererPresentSlot)
        && *g_rendererPresentSlot == (void*)&hookRendererPresent)
        writePointer(g_rendererPresentSlot, (void*)g_rendererPresent);
    g_rendererPresentSlot = nullptr;
    g_rendererPresent = nullptr;
    InterlockedExchange(&g_firstPresentReturned, 0);
    InterlockedExchange(&g_presentReturnCount, 0);
    InterlockedExchange(&g_rendererInstalled, 0);
}

void releaseSwapChain() {
    g_tearingSwapChain = nullptr;
    g_tearingPresentLogged = false;
    if (g_resizeSlot && g_resizeBuffers && readable(g_resizeSlot)
        && *g_resizeSlot == (void*)&hookResizeBuffers)
        writePointer(g_resizeSlot, (void*)g_resizeBuffers);
    g_resizeSlot = nullptr;
    g_resizeBuffers = nullptr;
    InterlockedExchange(&g_resizeInstalled, 0);
}

}  // namespace streaming
}  // namespace tq
