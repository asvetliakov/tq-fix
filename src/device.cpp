#include "device.h"

#include <d3d11_1.h>
#include <stdio.h>

#include "log.h"
#include "modules.h"
#include "patch.h"

namespace tq {
namespace device {

namespace {

// ------------------------------------------------------------------- state

ID3D11Device*        g_device;
ID3D11DeviceContext* g_context;
IDXGISwapChain*      g_swapChain;
D3D_FEATURE_LEVEL    g_featureLevel;

volatile LONG g_creates;      // times a hook fired
volatile LONG g_succeeded;    // times it returned a device
int           g_hooked;       // entry points redirected

// The log is a shared file and this is a per-creation block of a dozen lines.
// Device creation happens once or twice per process, but a driver-type fallback
// loop could make it many more, and a log that is mostly our own repetition is
// the log nobody reads. After this many, the count in `report` carries it.
const LONG kMaxDetailed = 8;

typedef HRESULT(WINAPI* CreateDeviceFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                        const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                        D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

typedef HRESULT(WINAPI* CreateDeviceAndSwapChainFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                    const D3D_FEATURE_LEVEL*, UINT, UINT,
                                                    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
                                                    ID3D11Device**, D3D_FEATURE_LEVEL*,
                                                    ID3D11DeviceContext**);

CreateDeviceFn             g_realCreate;
CreateDeviceAndSwapChainFn g_realCreateAndSwapChain;

// ------------------------------------------------------------- log helpers

const char* featureLevelName(D3D_FEATURE_LEVEL fl) {
    switch (fl) {
        case D3D_FEATURE_LEVEL_9_1:  return "9_1";
        case D3D_FEATURE_LEVEL_9_2:  return "9_2";
        case D3D_FEATURE_LEVEL_9_3:  return "9_3";
        case D3D_FEATURE_LEVEL_10_0: return "10_0";
        case D3D_FEATURE_LEVEL_10_1: return "10_1";
        case D3D_FEATURE_LEVEL_11_0: return "11_0";
        case D3D_FEATURE_LEVEL_11_1: return "11_1";
        default:                     return "?";
    }
}

const char* driverTypeName(D3D_DRIVER_TYPE t) {
    switch (t) {
        case D3D_DRIVER_TYPE_UNKNOWN:   return "UNKNOWN";
        case D3D_DRIVER_TYPE_HARDWARE:  return "HARDWARE";
        case D3D_DRIVER_TYPE_REFERENCE: return "REFERENCE";
        case D3D_DRIVER_TYPE_NULL:      return "NULL";
        case D3D_DRIVER_TYPE_SOFTWARE:  return "SOFTWARE";
        case D3D_DRIVER_TYPE_WARP:      return "WARP";
        default:                        return "?";
    }
}

/** The create flags, spelled out. Only the ones that could matter to us. */
void describeFlags(UINT f, char* out, size_t n) {
    _snprintf(out, n, "0x%08x%s%s%s%s", f,
              (f & D3D11_CREATE_DEVICE_DEBUG) ? " DEBUG" : "",
              (f & D3D11_CREATE_DEVICE_SINGLETHREADED) ? " SINGLETHREADED" : "",
              (f & D3D11_CREATE_DEVICE_BGRA_SUPPORT) ? " BGRA_SUPPORT" : "",
              (f & D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS)
                  ? " NO_INTERNAL_THREADING" : "");
    out[n - 1] = 0;
}

/** The vtable pointer of a COM object, or null. Stage 4 patches one of these. */
void* vtableOf(void* obj) {
    if (!patch::readable(obj, sizeof(void*))) return nullptr;
    return *(void**)obj;
}

// ------------------------------------------------------------ the reporting

/**
 * Everything worth knowing about one successful creation, logged where it
 * happens.
 *
 * **Not accumulated for a summary at exit.** O17: the first `TQ.exe` process
 * logged no detach at all, so it was terminated rather than unloaded, and
 * anything held back for a shutdown report in that process is simply lost.
 */
void logResult(const char* which, UINT flags, D3D_DRIVER_TYPE driver, UINT sdk,
               const D3D_FEATURE_LEVEL* wanted, UINT wantedCount,
               ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGISwapChain* sc,
               D3D_FEATURE_LEVEL got) {
    char flagText[128];
    describeFlags(flags, flagText, sizeof(flagText));

    tqlog("d3d11:    %s succeeded", which);
    tqlog("  device   %p  (vtable %p)", (void*)dev, vtableOf(dev));
    tqlog("  context  %p  (vtable %p)", (void*)ctx, vtableOf(ctx));
    if (sc) tqlog("  swapchain %p  (vtable %p)", (void*)sc, vtableOf(sc));
    tqlog("  feature level %s", featureLevelName(got));
    tqlog("  driver %s, flags %s, SDK version %u", driverTypeName(driver), flagText, sdk);

    // What the game *asked* for, which is not the same as what it got and is the
    // thing that explains a second process settling on 10_0 (O16).
    if (wanted && wantedCount) {
        char list[128] = "";
        int at = 0;
        for (UINT i = 0; i < wantedCount && at < (int)sizeof(list) - 8; i++) {
            int w = _snprintf(list + at, sizeof(list) - at, "%s%s", i ? ", " : "",
                              featureLevelName(wanted[i]));
            if (w < 0) break;   // MSVCRT returns -1 on truncation, not the length it wanted
            at += w;
        }
        list[sizeof(list) - 1] = 0;
        tqlog("  it asked for: %s", list);
    } else {
        tqlog("  it asked for: nothing specific (null feature-level array - D3D picks)");
    }
}

/**
 * Risk 3, asked now while it is cheap: does `ID3D11Device1` come back as a
 * *different object* with a *different vtable*?
 *
 * Stage 4 patches the vtable of whatever pointer the game actually calls
 * through. If DXMT implements `ID3D11Device1` on a separate object, patching the
 * wrong one is a hook that never fires — and there is no way to tell that apart
 * from a hook installed too late. Answer it here, in one launch, rather than
 * guessing next stage.
 *
 * `QueryInterface` AddRefs, so the result is released immediately. We keep no
 * reference to anything: a device the game thinks it destroyed but we are
 * keeping alive is a hang at exit, and a hang at exit is hard to attribute.
 */
void probeDevice1(ID3D11Device* dev) {
    ID3D11Device1* dev1 = nullptr;
    HRESULT hr = dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&dev1);
    if (FAILED(hr) || !dev1) {
        tqlog("  ID3D11Device1: not supported (hr 0x%08lx) - one interface, one vtable",
              (unsigned long)hr);
        return;
    }
    tqlog("  ID3D11Device1 %p (vtable %p) - %s object as ID3D11Device%s",
          (void*)dev1, vtableOf(dev1),
          ((void*)dev1 == (void*)dev) ? "the SAME" : "a DIFFERENT",
          ((void*)dev1 == (void*)dev) ? "" : "  <-- Risk 3 is real; Stage 4 must patch the"
                                             " pointer the game calls through");
    dev1->Release();
}

/** The shared tail of both hooks. */
void onCreated(const char* which, UINT flags, D3D_DRIVER_TYPE driver, UINT sdk,
               const D3D_FEATURE_LEVEL* wanted, UINT wantedCount,
               ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGISwapChain* sc,
               const D3D_FEATURE_LEVEL* levelOut) {
    LONG n = InterlockedIncrement(&g_succeeded);

    // The game may pass null for the context, in which case D3D creates one
    // anyway and we have to ask. `GetImmediateContext` AddRefs, so it is
    // released straight away: the device holds its own reference to its
    // immediate context for as long as it lives, so the raw pointer stays valid
    // and we are not keeping the device alive behind the game's back.
    ID3D11DeviceContext* owned = nullptr;
    if (!ctx && dev) {
        dev->GetImmediateContext(&owned);
        ctx = owned;
    }

    D3D_FEATURE_LEVEL got = levelOut ? *levelOut
                                     : (dev ? dev->GetFeatureLevel() : (D3D_FEATURE_LEVEL)0);

    if (n <= kMaxDetailed) {
        logResult(which, flags, driver, sdk, wanted, wantedCount, dev, ctx, sc, got);
        if (dev) probeDevice1(dev);
        if (!levelOut) tqlog("  (the game passed no pFeatureLevel; asked the device instead)");
        if (owned) tqlog("  (the game passed no context; asked the device, released our ref)");
    } else if (n == kMaxDetailed + 1) {
        tqlog("d3d11:    %s succeeded again - further creations counted, not detailed", which);
    }

    // The first one wins. A capability probe that creates a device and destroys
    // it (O16 saw one settle on 10_0) must not replace the device the renderer
    // is actually using, and the renderer's is the first real one in its process.
    if (!g_device && dev) {
        g_device       = dev;
        g_context      = ctx;
        g_swapChain    = sc;
        g_featureLevel = got;
    }

    if (owned) owned->Release();
}

// ------------------------------------------------------------------- hooks

HRESULT WINAPI hookCreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver, HMODULE software,
                                UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount,
                                UINT sdk, ID3D11Device** ppDevice,
                                D3D_FEATURE_LEVEL* pLevel, ID3D11DeviceContext** ppContext) {
    InterlockedIncrement(&g_creates);
    if (!g_realCreate) return E_FAIL;

    // Call through first, and change nothing on the way in. This stage is an
    // observer; the arguments the game passed are the evidence, and altering one
    // would make every measurement that follows a measurement of us.
    HRESULT hr = g_realCreate(adapter, driver, software, flags, levels, levelCount, sdk,
                              ppDevice, pLevel, ppContext);

    if (SUCCEEDED(hr)) {
        onCreated("D3D11CreateDevice", flags, driver, sdk, levels, levelCount,
                  ppDevice ? *ppDevice : nullptr, ppContext ? *ppContext : nullptr, nullptr,
                  pLevel);
    } else {
        tqlog("d3d11:    D3D11CreateDevice FAILED, hr 0x%08lx (driver %s)",
              (unsigned long)hr, driverTypeName(driver));
    }
    return hr;
}

HRESULT WINAPI hookCreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver,
                                            HMODULE software, UINT flags,
                                            const D3D_FEATURE_LEVEL* levels, UINT levelCount,
                                            UINT sdk, const DXGI_SWAP_CHAIN_DESC* scDesc,
                                            IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
                                            D3D_FEATURE_LEVEL* pLevel,
                                            ID3D11DeviceContext** ppContext) {
    InterlockedIncrement(&g_creates);
    if (!g_realCreateAndSwapChain) return E_FAIL;

    HRESULT hr = g_realCreateAndSwapChain(adapter, driver, software, flags, levels, levelCount,
                                          sdk, scDesc, ppSwapChain, ppDevice, pLevel, ppContext);

    if (SUCCEEDED(hr)) {
        onCreated("D3D11CreateDeviceAndSwapChain", flags, driver, sdk, levels, levelCount,
                  ppDevice ? *ppDevice : nullptr, ppContext ? *ppContext : nullptr,
                  ppSwapChain ? *ppSwapChain : nullptr, pLevel);
        // The swapchain description, because Stage 4 hooks `Present` and needs to
        // know what it is presenting: the buffer count is how many frames can be
        // in flight, which is the number every "one frame late" hypothesis in
        // docs/rev/observed.md is really about.
        if (scDesc && g_succeeded <= kMaxDetailed) {
            tqlog("  swapchain: %ux%u, format %d, %u buffer(s), %u sample(s), %s, swap effect %d",
                  (unsigned)scDesc->BufferDesc.Width, (unsigned)scDesc->BufferDesc.Height,
                  (int)scDesc->BufferDesc.Format, (unsigned)scDesc->BufferCount,
                  (unsigned)scDesc->SampleDesc.Count, scDesc->Windowed ? "windowed" : "fullscreen",
                  (int)scDesc->SwapEffect);
            tqlog("  refresh requested: %u/%u Hz",
                  (unsigned)scDesc->BufferDesc.RefreshRate.Numerator,
                  (unsigned)scDesc->BufferDesc.RefreshRate.Denominator);
        }
    } else {
        tqlog("d3d11:    D3D11CreateDeviceAndSwapChain FAILED, hr 0x%08lx (driver %s)",
              (unsigned long)hr, driverTypeName(driver));
    }
    return hr;
}

// --------------------------------------------------------------- installing

/**
 * Has the loader finished snapping `Direct3D11.dll`'s imports?
 *
 * `GetModuleHandleW` answers as soon as the module is in the loader's list,
 * which is **before** its import table is filled in. An IAT entry patched in
 * that window is one the loader then overwrites with the real address, and the
 * result is a hook that is installed, reported as installed, and never fires.
 *
 * The test is the slot itself: an unsnapped entry holds the RVA of the
 * hint/name table entry, which is a small number and not a mapped address. Once
 * it points at readable memory the loader has been there.
 */
bool importsSnapped(void** slot) {
    return slot && patch::readable(slot, sizeof(void*)) && patch::readable(*slot, 1);
}

/** Hook one entry point, if `Direct3D11.dll` imports it. Returns true if hooked. */
template <typename T>
bool hookOne(HMODULE d3d, const char* name, void* replacement, T* realOut, HANDLE cancel) {
    void** slot = patch::iatSlot(d3d, "d3d11.dll", name);
    if (!slot) {
        // Not an error, and worth a line saying so: Direct3D11.dll imports
        // exactly one of the two (checked with objdump, and confirmed here at
        // runtime). A silent absence would read as a failed patch.
        tqlog("d3d11:    Direct3D11.dll does not import d3d11.dll!%s - nothing to hook", name);
        return false;
    }

    for (int i = 0; i < 200 && !importsSnapped(slot); i++)
        if (!modules::nap(cancel, 5)) return false;
    if (!importsSnapped(slot)) {
        tqlog("!! d3d11: %s's IAT slot at %p still holds %p after 1s - not snapped, refusing"
              " to patch it", name, (void*)slot, *slot);
        return false;
    }

    void* original = patch::iat(d3d, "Direct3D11.dll", "d3d11.dll", name, replacement);
    if (!original) return false;
    *realOut = (T)original;

    // The loader can still snap an import after we have written the slot if the
    // module was only part-loaded when we found it. Look once more, a beat
    // later: a slot that no longer holds our pointer is a hook that will never
    // fire, and this is the only moment at which that is cheap to notice.
    modules::nap(cancel, 20);
    if (*slot != replacement) {
        tqlog("!! d3d11: %s's IAT slot was overwritten after we patched it (holds %p, ours is"
              " %p). The hook will NOT fire.", name, *slot, replacement);
        return false;
    }
    return true;
}

}  // namespace

bool install(HANDLE cancel) {
    // Bounded, because of O17: `TQ.exe` runs as two processes and one of them
    // may never load a renderer at all. An unbounded wait there is a thread
    // parked forever and a log that says nothing, which is exactly what a broken
    // hook looks like. Three minutes is far longer than this game's load and
    // short enough to be over before anyone starts diagnosing.
    const DWORD kTimeoutMs = 180000;

    DWORD waited = 0;
    bool cancelled = false;
    HMODULE d3d = modules::waitFor(L"Direct3D11.dll", kTimeoutMs, cancel, &waited, &cancelled);
    if (!d3d) {
        if (cancelled) {
            tqlog("d3d11:    stopped waiting for Direct3D11.dll after %ums - we are being"
                  " unloaded.", (unsigned)waited);
            return false;
        }
        tqlog("d3d11:    Direct3D11.dll never loaded in this process after %ums. This is NOT a"
              " broken hook - O17: TQ.exe runs as two processes and only one renders.",
              (unsigned)waited);
        modules::logLoaded("gave up waiting for Direct3D11.dll");
        return false;
    }
    tqlog("d3d11:    Direct3D11.dll at %p after %ums", (void*)d3d, (unsigned)waited);

    if (hookOne(d3d, "D3D11CreateDeviceAndSwapChain",
                (void*)&hookCreateDeviceAndSwapChain, &g_realCreateAndSwapChain, cancel))
        g_hooked++;
    if (hookOne(d3d, "D3D11CreateDevice", (void*)&hookCreateDevice, &g_realCreate, cancel))
        g_hooked++;

    if (!g_hooked) {
        tqlog("!! d3d11: neither entry point could be hooked. Stage 4 has no device.");
        return false;
    }
    tqlog("d3d11:    %d of 2 entry point(s) hooked; waiting for the game to create a device",
          g_hooked);
    return true;
}

void report(const char* when) {
    LONG calls = g_creates, ok = g_succeeded;
    if (!g_hooked) {
        tqlog("d3d11 (%s): nothing hooked in this process.", when);
        return;
    }
    if (!calls) {
        // Two readings, and the log cannot tell them apart on its own - so it
        // says both rather than picking one. Stage 4 can settle it by taking the
        // device from the swapchain instead, the way the THQ overlay does.
        tqlog("!! d3d11 (%s): hooked, but never called. Either this process does not create a"
              " device, or we were too late and it was created before the hook landed.", when);
        return;
    }
    tqlog("d3d11 (%s): %ld call(s), %ld succeeded. device %p, context %p, feature level %s",
          when, calls, ok, (void*)g_device, (void*)g_context, featureLevelName(g_featureLevel));
}

}  // namespace device
}  // namespace tq
