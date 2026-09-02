#include <windows.h>
#include <d3d11.h>

#include <string.h>

#include "dxbc_patch.h"
#include "frustum_fix.h"
#include "grass.h"
#include "hdr.h"
#include "shadow_fix.h"
#include "streaming.h"
#include "visual.h"

extern "C" void* tq_winmm_targets[];

namespace {

struct WinmmExport {
    const char* name;
    bool required;
};

const WinmmExport kWinmmExports[] = {
#define TQ_WINMM_NAME(name, required) {name, required},
#include "winmm_names.inc"
#undef TQ_WINMM_NAME
};

typedef HRESULT(WINAPI* CreateDeviceFn)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
    UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT(WINAPI* CreateDeviceAndSwapChainFn)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
    UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
    D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT(WINAPI* CreateVertexShaderFn)(
    ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);

CreateDeviceFn             g_createDevice;
CreateDeviceAndSwapChainFn g_createDeviceAndSwapChain;
CreateVertexShaderFn       g_createVertexShader;

struct Patch {
    void** slot;
    void*  original;
    void*  replacement;
};

Patch  g_patches[3];
int    g_patchCount;
LONG   g_devicePatched;
HANDLE g_stop;
HANDLE g_done;
HANDLE g_thread;
int    g_winmmResolved;
int    g_winmmOptionalMissing;

bool readable(const void* address) {
    MEMORY_BASIC_INFORMATION info;
    if (!address || !VirtualQuery(address, &info, sizeof(info))) return false;
    DWORD protection = info.Protect & 0xff;
    return info.State == MEM_COMMIT && !(info.Protect & PAGE_GUARD)
        && protection != PAGE_NOACCESS;
}

bool belongsTo(HMODULE module, const void* address) {
    MEMORY_BASIC_INFORMATION info;
    return address && VirtualQuery(address, &info, sizeof(info))
        && info.AllocationBase == module;
}

bool writePointer(void** slot, void* value, void** oldValue = nullptr) {
    if (!slot || !value) return false;
    DWORD oldProtection;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtection)) return false;
    void* previous = InterlockedExchangePointer((PVOID volatile*)slot, value);
    DWORD ignored;
    VirtualProtect(slot, sizeof(*slot), oldProtection, &ignored);
    if (oldValue) *oldValue = previous;
    return true;
}

bool rememberPatch(void** slot, void* replacement) {
    if (!slot || !replacement || g_patchCount >= (int)(sizeof(g_patches) / sizeof(g_patches[0])))
        return false;
    if (*slot == replacement) return true;
    void* old = nullptr;
    if (!writePointer(slot, replacement, &old)) return false;
    g_patches[g_patchCount++] = {slot, old, replacement};
    return true;
}

void restorePatches() {
    for (int i = g_patchCount - 1; i >= 0; --i) {
        Patch& patch = g_patches[i];
        if (readable(patch.slot) && *patch.slot == patch.replacement)
            writePointer(patch.slot, patch.original);
    }
    g_patchCount = 0;
}

void** importSlot(HMODULE module, const char* importedDll, const char* importedName) {
    if (!module) return nullptr;
    BYTE* base = (BYTE*)module;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size) return nullptr;

    IMAGE_IMPORT_DESCRIPTOR* descriptor =
        (IMAGE_IMPORT_DESCRIPTOR*)(base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        if (_stricmp((const char*)(base + descriptor->Name), importedDll)) continue;
        if (!descriptor->OriginalFirstThunk) return nullptr;

        IMAGE_THUNK_DATA* names =
            (IMAGE_THUNK_DATA*)(base + descriptor->OriginalFirstThunk);
        IMAGE_THUNK_DATA* addresses =
            (IMAGE_THUNK_DATA*)(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* name =
                (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (!strcmp((const char*)name->Name, importedName))
                return (void**)&addresses->u1.Function;
        }
    }
    return nullptr;
}

bool resolveWinmm(HINSTANCE self) {
    wchar_t systemDirectory[MAX_PATH];
    UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (!length || length >= MAX_PATH - 11) return false;

    wchar_t path[MAX_PATH];
    lstrcpyW(path, systemDirectory);
    lstrcatW(path, L"\\winmm.dll");
    HMODULE real = LoadLibraryW(path);
    if (!real || real == (HMODULE)self) return false;

    const int count = (int)(sizeof(kWinmmExports) / sizeof(kWinmmExports[0]));
    g_winmmResolved = 0;
    g_winmmOptionalMissing = 0;
    for (int i = 0; i < count; ++i) {
        const WinmmExport& entry = kWinmmExports[i];
        FARPROC target = GetProcAddress(real, entry.name);
        if (!target || belongsTo((HMODULE)self, (const void*)target)) {
            if (entry.required) return false;
            ++g_winmmOptionalMissing;
            continue;
        }
        tq_winmm_targets[i] = (void*)target;
        ++g_winmmResolved;
    }
    return true;
}

HRESULT WINAPI hookCreateVertexShader(ID3D11Device* device, const void* bytecode,
                                      SIZE_T size, ID3D11ClassLinkage* linkage,
                                      ID3D11VertexShader** shader) {
    tq::dxbc::PatchResult patched = {};
    bool changed = tq::dxbc::clampBoneIndices(bytecode, size, &patched);
    HRESULT result = g_createVertexShader(
        device, changed ? patched.data : bytecode, changed ? patched.size : size,
        linkage, shader);
    if (changed && FAILED(result))
        result = g_createVertexShader(device, bytecode, size, linkage, shader);
    tq::dxbc::release(&patched);
    return result;
}

void patchDevice(ID3D11Device* device) {
    if (!device || InterlockedCompareExchange(&g_devicePatched, 1, 0)) return;
    void** vtable = *(void***)device;
    void** slot = &vtable[12];  // ID3D11Device::CreateVertexShader
    if (!readable(slot) || !readable(*slot)) {
        tq::hdr::log("Device vertex-shader slot is unreadable: device=%p slot=%p\r\n",
                     device, slot);
        InterlockedExchange(&g_devicePatched, 0);
        return;
    }
    // Publish the call-through before making the hook reachable from another
    // thread through the shared DXMT vtable.
    g_createVertexShader = (CreateVertexShaderFn)*slot;
    if (!rememberPatch(slot, (void*)&hookCreateVertexShader)) {
        tq::hdr::log("Device vertex-shader hook failed: slot=%p target=%p\r\n",
                     slot, *slot);
        g_createVertexShader = nullptr;
        InterlockedExchange(&g_devicePatched, 0);
    } else {
        tq::hdr::log("Device vertex-shader hook installed: device=%p\r\n", device);
    }
}

void installHooks(ID3D11Device* device, ID3D11DeviceContext* context,
                  IDXGISwapChain* swapChain = nullptr) {
    tq::hdr::log("Installing hooks: device=%p context=%p swapChain=%p\r\n",
                 device, context, swapChain);
    patchDevice(device);
    // The directional shadow split is an Engine.dll constant redirect, so it
    // is installed once the module is loaded and is independent of the device.
    HMODULE engine = GetModuleHandleW(L"Engine.dll");
    tq::shadow::install(engine);
    // Terrain grass lives entirely in Engine.dll and never reaches the device,
    // so it installs from the same handle. The detour is what lets a draw hook
    // tell a grass draw from every other draw in the frame.
    tq::grass::installFromModule(engine);
    if (device) tq::visual::install(device, context, swapChain);
    if (swapChain) tq::streaming::installSwapChain(swapChain);
    tq::hdr::log("Hook installation returned\r\n");
}

void releaseCreation(IDXGISwapChain** swapChain, ID3D11Device** device,
                     ID3D11DeviceContext** context) {
    if (context && *context) { (*context)->Release(); *context = nullptr; }
    if (device && *device) { (*device)->Release(); *device = nullptr; }
    if (swapChain && *swapChain) { (*swapChain)->Release(); *swapChain = nullptr; }
}

HRESULT WINAPI hookCreateDevice(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdkVersion,
    ID3D11Device** device, D3D_FEATURE_LEVEL* selectedLevel,
    ID3D11DeviceContext** context) {
    tq::hdr::log("D3D11CreateDevice entered: flags=0x%x levels=%u\r\n",
                 flags, levelCount);
    HRESULT result = g_createDevice(adapter, driverType, software, flags, levels,
                                    levelCount, sdkVersion, device, selectedLevel, context);
    tq::hdr::log("D3D11CreateDevice returned: hr=0x%08lx device=%p context=%p\r\n",
                 (unsigned long)result, device ? *device : nullptr,
                 context ? *context : nullptr);
    if (SUCCEEDED(result) && device)
        installHooks(*device, context ? *context : nullptr);
    return result;
}

HRESULT WINAPI hookCreateDeviceAndSwapChain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* description, IDXGISwapChain** swapChain,
    ID3D11Device** device, D3D_FEATURE_LEVEL* selectedLevel,
    ID3D11DeviceContext** context) {
    if (description)
        tq::hdr::log("D3D11CreateDeviceAndSwapChain entered: %ux%u format=%u buffers=%u effect=%u flags=0x%x\r\n",
                     description->BufferDesc.Width, description->BufferDesc.Height,
                     (unsigned)description->BufferDesc.Format, description->BufferCount,
                     (unsigned)description->SwapEffect, flags);
    else
        tq::hdr::log("D3D11CreateDeviceAndSwapChain entered without a description\r\n");
    DXGI_SWAP_CHAIN_DESC candidate = {};
    bool tryFloatOutput = tq::streaming::presentHookInstalled() && description
                       && tq::hdr::makeSwapChainCandidate(*description, &candidate);
    HRESULT result = g_createDeviceAndSwapChain(
        adapter, driverType, software, flags, levels, levelCount, sdkVersion,
        tryFloatOutput ? &candidate : description, swapChain, device, selectedLevel, context);
    tq::hdr::log("D3D11CreateDeviceAndSwapChain returned: candidate=%u hr=0x%08lx swapChain=%p device=%p context=%p\r\n",
                 tryFloatOutput ? 1u : 0u, (unsigned long)result,
                 swapChain ? *swapChain : nullptr, device ? *device : nullptr,
                 context ? *context : nullptr);
    if (tryFloatOutput && (FAILED(result) || !swapChain || !*swapChain
                           || !tq::hdr::activateSwapChain(*swapChain))) {
        tq::hdr::log("FP16 swap-chain attempt failed; retrying original description\r\n");
        releaseCreation(swapChain, device, context);
        result = g_createDeviceAndSwapChain(
            adapter, driverType, software, flags, levels, levelCount, sdkVersion,
            description, swapChain, device, selectedLevel, context);
        tq::hdr::log("Original swap-chain retry returned: hr=0x%08lx swapChain=%p device=%p context=%p\r\n",
                     (unsigned long)result, swapChain ? *swapChain : nullptr,
                     device ? *device : nullptr, context ? *context : nullptr);
    }
    if (SUCCEEDED(result) && device)
        installHooks(*device, context ? *context : nullptr,
                     swapChain ? *swapChain : nullptr);
    return result;
}

bool installDeviceHook(HMODULE renderer) {
    tq::streaming::installRenderer(renderer);
    void** slot = importSlot(renderer, "d3d11.dll", "D3D11CreateDeviceAndSwapChain");
    if (slot && readable(*slot)) {
        g_createDeviceAndSwapChain = (CreateDeviceAndSwapChainFn)*slot;
        if (rememberPatch(slot, (void*)&hookCreateDeviceAndSwapChain)) {
            tq::hdr::log("Renderer device-and-swap-chain import hooked: renderer=%p\r\n",
                         renderer);
            return true;
        }
        g_createDeviceAndSwapChain = nullptr;
    }

    slot = importSlot(renderer, "d3d11.dll", "D3D11CreateDevice");
    if (slot && readable(*slot)) {
        g_createDevice = (CreateDeviceFn)*slot;
        if (rememberPatch(slot, (void*)&hookCreateDevice)) {
            tq::hdr::log("Renderer device import hooked: renderer=%p\r\n", renderer);
            return true;
        }
        g_createDevice = nullptr;
    }
    return false;
}

DWORD WINAPI installerThread(void*) {
    const DWORD stepMs = 10;
    const DWORD timeoutMs = 180000;
    bool rendererInstalled = false;
    bool frustumAttempted = false;
    bool rendererSeen = false;
    bool gameSeen = false;
    tq::hdr::log("Proxy initialized: pid=%lu winmmResolved=%d optionalMissing=%d\r\n",
                 GetCurrentProcessId(), g_winmmResolved, g_winmmOptionalMissing);
    for (DWORD waited = 0; waited < timeoutMs; waited += stepMs) {
        if (WaitForSingleObject(g_stop, stepMs) != WAIT_TIMEOUT) break;
        if (!frustumAttempted) {
            HMODULE game = GetModuleHandleW(L"Game.dll");
            if (game) {
                if (!gameSeen) {
                    tq::hdr::log("Game.dll detected: module=%p\r\n", game);
                    gameSeen = true;
                }
                tq::frustum::install(game);
                frustumAttempted = true;
                tq::hdr::log("Frustum hook attempt returned\r\n");
            }
        }
        if (!rendererInstalled) {
            HMODULE renderer = GetModuleHandleW(L"Direct3D11.dll");
            if (renderer && !rendererSeen) {
                tq::hdr::log("Direct3D11.dll detected: module=%p\r\n", renderer);
                rendererSeen = true;
            }
            rendererInstalled = renderer && installDeviceHook(renderer);
        }
        if (rendererInstalled && frustumAttempted) break;
    }
    tq::hdr::log("Installer finished: rendererInstalled=%u presentInstalled=%u frustumAttempted=%u\r\n",
                 rendererInstalled ? 1u : 0u,
                 tq::streaming::presentHookInstalled() ? 1u : 0u,
                 frustumAttempted ? 1u : 0u);
    SetEvent(g_done);
    return 0;
}

}  // namespace

extern "C" __attribute__((noreturn)) void tq_winmm_unresolved(void) {
    TerminateProcess(GetCurrentProcess(), ERROR_PROC_NOT_FOUND);
    for (;;) {}
}

extern "C" BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        if (!resolveWinmm(self)) return FALSE;

        g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_stop || !g_done) return FALSE;
        g_thread = CreateThread(nullptr, 0, installerThread, nullptr, 0, nullptr);
        return g_thread != nullptr;
    }

    if (reason == DLL_PROCESS_DETACH && !reserved) {
        if (g_stop) SetEvent(g_stop);
        if (g_done) WaitForSingleObject(g_done, 2000);
        tq::frustum::shutdown();
        tq::shadow::shutdown();
        tq::grass::shutdown();
        tq::visual::shutdown();
        restorePatches();
        tq::streaming::shutdown();
        tq::hdr::shutdown();
        if (g_thread) CloseHandle(g_thread);
        if (g_done) CloseHandle(g_done);
        if (g_stop) CloseHandle(g_stop);
    }
    return TRUE;
}
