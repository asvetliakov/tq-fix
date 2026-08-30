#include "streaming.h"

#include <windows.h>

namespace tq {
namespace streaming {
namespace {

typedef HRESULT (WINAPI* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (WINAPI* ResizeBuffersFn)(IDXGISwapChain*, UINT, UINT, UINT,
                                          DXGI_FORMAT, UINT);

volatile LONG g_installed;
PresentFn g_present;
void** g_presentSlot;
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

HRESULT WINAPI hookPresent(IDXGISwapChain* swapChain, UINT interval, UINT flags) {
    if (g_presentCallback) g_presentCallback(swapChain);
    HRESULT result = g_present(swapChain, interval, flags);
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

void installSwapChain(IDXGISwapChain* swapChain) {
    if (!swapChain || !g_presentCallback
        || InterlockedCompareExchange(&g_installed, 1, 0)) return;
    void** vtable = *(void***)swapChain;
    if (!readable(vtable) || !readable(vtable + 13)
        || !readable(vtable[8]) || !readable(vtable[13])) {
        InterlockedExchange(&g_installed, 0);
        return;
    }
    g_presentSlot = &vtable[8];
    g_present = (PresentFn)vtable[8];
    g_resizeSlot = &vtable[13];
    g_resizeBuffers = (ResizeBuffersFn)vtable[13];
    if (!writePointer(g_presentSlot, (void*)&hookPresent)
        || !writePointer(g_resizeSlot, (void*)&hookResizeBuffers)) {
        if (g_presentSlot && g_present && *g_presentSlot == (void*)&hookPresent)
            writePointer(g_presentSlot, (void*)g_present);
        g_presentSlot = nullptr;
        g_present = nullptr;
        g_resizeSlot = nullptr;
        g_resizeBuffers = nullptr;
        InterlockedExchange(&g_installed, 0);
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
    if (g_presentSlot && g_present && readable(g_presentSlot)
        && *g_presentSlot == (void*)&hookPresent)
        writePointer(g_presentSlot, (void*)g_present);
    g_presentSlot = nullptr;
    g_present = nullptr;
    if (g_resizeSlot && g_resizeBuffers && readable(g_resizeSlot)
        && *g_resizeSlot == (void*)&hookResizeBuffers)
        writePointer(g_resizeSlot, (void*)g_resizeBuffers);
    g_resizeSlot = nullptr;
    g_resizeBuffers = nullptr;
    InterlockedExchange(&g_installed, 0);
}

}  // namespace streaming
}  // namespace tq
