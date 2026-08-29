#include "streaming.h"

#include <windows.h>

namespace tq {
namespace streaming {
namespace {

typedef HRESULT (WINAPI* PresentFn)(IDXGISwapChain*, UINT, UINT);

volatile LONG g_installed;
PresentFn g_present;
void** g_presentSlot;
void (*g_presentCallback)();

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
    if (g_presentCallback) g_presentCallback();
    return g_present(swapChain, interval, flags);
}

}  // namespace

bool optimizationEnabled(const wchar_t* value) {
    return !value || _wcsicmp(value, L"original") != 0;
}

void installSwapChain(IDXGISwapChain* swapChain) {
    if (!swapChain || !g_presentCallback
        || InterlockedCompareExchange(&g_installed, 1, 0)) return;
    void** vtable = *(void***)swapChain;
    if (!readable(vtable) || !readable(vtable + 8) || !readable(vtable[8])) {
        InterlockedExchange(&g_installed, 0);
        return;
    }
    g_presentSlot = &vtable[8];
    g_present = (PresentFn)vtable[8];
    if (!writePointer(g_presentSlot, (void*)&hookPresent)) {
        g_presentSlot = nullptr;
        g_present = nullptr;
        InterlockedExchange(&g_installed, 0);
    }
}

void setPresentCallback(void (*callback)()) {
    g_presentCallback = callback;
}

void shutdown() {
    g_presentCallback = nullptr;
    if (g_presentSlot && g_present && readable(g_presentSlot)
        && *g_presentSlot == (void*)&hookPresent)
        writePointer(g_presentSlot, (void*)g_present);
    g_presentSlot = nullptr;
    g_present = nullptr;
    InterlockedExchange(&g_installed, 0);
}

}  // namespace streaming
}  // namespace tq
