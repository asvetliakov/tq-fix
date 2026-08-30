#include "streaming.h"
#include "hdr.h"

#include <windows.h>
#include <string.h>

namespace tq {
namespace streaming {
namespace {

typedef HRESULT (__thiscall* RendererPresentFn)(void*, void*);
typedef HRESULT (WINAPI* ResizeBuffersFn)(IDXGISwapChain*, UINT, UINT, UINT,
                                          DXGI_FORMAT, UINT);

// Verified against the Steam and GOG 2022 renderer. Both distributions carry
// the same PE timestamp, image size, wrapper bytes, and renderer vtable slot.
const DWORD kRendererTimestamp = 0x62da9e96;
const DWORD kRendererImageSize = 0x192000;
const DWORD kRendererPresentRva = 0x61190;
const DWORD kRendererPresentSlotRva = 0x8625c;
const DWORD kRendererSwapChainOffset = 0x34;
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
volatile LONG g_resizeInstalled;
ResizeBuffersFn g_resizeBuffers;
void** g_resizeSlot;
void (*g_presentCallback)(IDXGISwapChain*);
void (*g_postPresentCallback)(IDXGISwapChain*);
void (*g_preResizeCallback)(IDXGISwapChain*);
void (*g_resizeCallback)(IDXGISwapChain*);

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
    HRESULT result = g_rendererPresent(renderer, parameter);
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
        && current.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
        format = DXGI_FORMAT_R16G16B16A16_FLOAT;
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
                 && nt->FileHeader.TimeDateStamp == kRendererTimestamp
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
    if (!swapChain) {
        tq::hdr::log("Swap-chain Resize hook skipped: no swap chain\r\n");
        return;
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
    InterlockedExchange(&g_rendererInstalled, 0);
    if (g_resizeSlot && g_resizeBuffers && readable(g_resizeSlot)
        && *g_resizeSlot == (void*)&hookResizeBuffers)
        writePointer(g_resizeSlot, (void*)g_resizeBuffers);
    g_resizeSlot = nullptr;
    g_resizeBuffers = nullptr;
    InterlockedExchange(&g_resizeInstalled, 0);
}

}  // namespace streaming
}  // namespace tq
