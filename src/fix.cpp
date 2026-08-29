#include <windows.h>
#include <d3d11.h>

#include <string.h>

#include "dxbc_patch.h"
#include "frustum_fix.h"
#include "visual.h"

extern "C" void* tq_winmm_targets[];

namespace {

const char* const kWinmmNames[] = {
#define TQ_WINMM_NAME(name) name,
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

    const int count = (int)(sizeof(kWinmmNames) / sizeof(kWinmmNames[0]));
    for (int i = 0; i < count; ++i) {
        FARPROC target = GetProcAddress(real, kWinmmNames[i]);
        if (!target || belongsTo((HMODULE)self, (const void*)target)) return false;
        tq_winmm_targets[i] = (void*)target;
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
        InterlockedExchange(&g_devicePatched, 0);
        return;
    }
    // Publish the call-through before making the hook reachable from another
    // thread through the shared DXMT vtable.
    g_createVertexShader = (CreateVertexShaderFn)*slot;
    if (!rememberPatch(slot, (void*)&hookCreateVertexShader)) {
        g_createVertexShader = nullptr;
        InterlockedExchange(&g_devicePatched, 0);
    }
}

void installHooks(ID3D11Device* device, ID3D11DeviceContext* context) {
    patchDevice(device);
    if (device) tq::visual::install(device, context);
}

HRESULT WINAPI hookCreateDevice(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdkVersion,
    ID3D11Device** device, D3D_FEATURE_LEVEL* selectedLevel,
    ID3D11DeviceContext** context) {
    HRESULT result = g_createDevice(adapter, driverType, software, flags, levels,
                                    levelCount, sdkVersion, device, selectedLevel, context);
    if (SUCCEEDED(result) && device) installHooks(*device, context ? *context : nullptr);
    return result;
}

HRESULT WINAPI hookCreateDeviceAndSwapChain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* description, IDXGISwapChain** swapChain,
    ID3D11Device** device, D3D_FEATURE_LEVEL* selectedLevel,
    ID3D11DeviceContext** context) {
    HRESULT result = g_createDeviceAndSwapChain(
        adapter, driverType, software, flags, levels, levelCount, sdkVersion,
        description, swapChain, device, selectedLevel, context);
    if (SUCCEEDED(result) && device) installHooks(*device, context ? *context : nullptr);
    return result;
}

bool installDeviceHook(HMODULE renderer) {
    void** slot = importSlot(renderer, "d3d11.dll", "D3D11CreateDeviceAndSwapChain");
    if (slot && readable(*slot)) {
        g_createDeviceAndSwapChain = (CreateDeviceAndSwapChainFn)*slot;
        if (rememberPatch(slot, (void*)&hookCreateDeviceAndSwapChain)) {
            return true;
        }
        g_createDeviceAndSwapChain = nullptr;
    }

    slot = importSlot(renderer, "d3d11.dll", "D3D11CreateDevice");
    if (slot && readable(*slot)) {
        g_createDevice = (CreateDeviceFn)*slot;
        if (rememberPatch(slot, (void*)&hookCreateDevice)) {
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
    for (DWORD waited = 0; waited < timeoutMs; waited += stepMs) {
        if (WaitForSingleObject(g_stop, stepMs) != WAIT_TIMEOUT) break;
        if (!frustumAttempted) {
            HMODULE game = GetModuleHandleW(L"Game.dll");
            if (game) {
                tq::frustum::install(game);
                frustumAttempted = true;
            }
        }
        if (!rendererInstalled) {
            HMODULE renderer = GetModuleHandleW(L"Direct3D11.dll");
            rendererInstalled = renderer && installDeviceHook(renderer);
        }
        if (rendererInstalled && frustumAttempted) break;
    }
    SetEvent(g_done);
    return 0;
}

}  // namespace

extern "C" unsigned long tq_winmm_unresolved(void) { return 0; }

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
        tq::visual::shutdown();
        restorePatches();
        if (g_thread) CloseHandle(g_thread);
        if (g_done) CloseHandle(g_done);
        if (g_stop) CloseHandle(g_stop);
    }
    return TRUE;
}
