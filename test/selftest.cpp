#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "arc_cache.h"
#include "dxbc_patch.h"
#include "bloom_hook.h"
#include "frame_overlay.h"
#include "detour.h"
#include "renderer_draw.h"
#include "renderer_draw_sites.h"
#include "engine_probe.h"
#include "engine.h"
#include "secondary_admission.h"
#include "frustum_fix.h"
#include "grass.h"
#include "hdr.h"
#include "probe.h"
#include "shadow_fix.h"
#include "streaming.h"
#include "upload.h"
#include "visual.h"
#include "bloom_shaders.inc"

namespace {

// Default shadow map scale; the tests exercise the shipped default. Measured
// at 3.73 ms a frame against 2.31 ms at scale 2, and kept at 4 because the
// smaller map visibly softens the shadows; the README documents the trade.
const UINT kShadowScale = 4;
// Point and spot maps scale separately; the split does not touch them.
const UINT kPointShadowScale = 2;

FILE* g_report;
int   g_failures;
int   g_presentOrder;
bool  g_presentOrderValid;
IDXGISwapChain* g_presentSwapChain;

void check(bool passed, const char* description) {
    fprintf(g_report, "%s  %s\n", passed ? "ok  " : "FAIL", description);
    if (!passed) ++g_failures;
}

typedef void (*SubmitDrawFn)(ID3D11DeviceContext*, UINT);
typedef void (*SubmitIndexedFn)(ID3D11DeviceContext*, UINT, UINT, INT);
unsigned g_rendererDrawEntries, g_rendererIndexedEntries, g_nativeDraws, g_nativeIndexedA, g_nativeIndexedB;
UINT g_expectedCount, g_expectedStart;
INT g_expectedBase;
bool g_nativeArgs, g_companion, g_suppressDraw;
void* g_rendererDrawReturn;
void* g_rendererIndexedReturn;

void WINAPI rendererNativeDraw(ID3D11DeviceContext*, UINT count, UINT start) {
    ++g_nativeDraws;
    g_nativeArgs &= count == g_expectedCount && start == 0;
}
void WINAPI rendererNativeIndexedB(ID3D11DeviceContext*, UINT count, UINT start, INT base) {
    ++g_nativeIndexedB;
    g_nativeArgs &= count == g_expectedCount && start == g_expectedStart && base == g_expectedBase;
}
void WINAPI rendererNativeIndexedA(ID3D11DeviceContext* context, UINT count, UINT start, INT base) {
    ++g_nativeIndexedA;
    g_nativeArgs &= count == g_expectedCount && start == g_expectedStart && base == g_expectedBase;
    // Simulate native dispatch changing during the first submission, before
    // the hook submits an extra draw. No recovery runs between the two.
    (*(void***)context)[12] = (void*)&rendererNativeIndexedB;
}
void WINAPI rendererDrawHandler(ID3D11DeviceContext* context, UINT count, UINT start) {
    ++g_rendererDrawEntries;
    g_rendererDrawReturn = __builtin_return_address(0);
    if (!g_suppressDraw) context->Draw(count, start);
}
void WINAPI rendererIndexedHandler(ID3D11DeviceContext* context, UINT count, UINT start, INT base) {
    ++g_rendererIndexedEntries;
    g_rendererIndexedReturn = __builtin_return_address(0);
    if (g_suppressDraw) return;
    context->DrawIndexed(count, start, base);
    if (g_companion) context->DrawIndexed(count, start, base);
}

void testRendererDrawHooks() {
    using namespace tq::rendererdraw::sites;
    HMODULE module = LoadLibraryA("Direct3D11.dll");
    typedef BOOL (*PrepareFn)();
    PrepareFn prepare = module ? (PrepareFn)(void*)GetProcAddress(module, "prepare_draw_sites") : nullptr;
    SubmitDrawFn draw = module ? (SubmitDrawFn)(void*)GetProcAddress(module, "submit_draw") : nullptr;
    SubmitIndexedFn indexed = module ? (SubmitIndexedFn)(void*)GetProcAddress(module, "submit_indexed") : nullptr;
    const bool ready = prepare && draw && indexed && prepare();
    check(ready, "prepare executable copies of the renderer's audited draw windows");
    if (!ready) { if (module) FreeLibrary(module); return; }
    BYTE* drawWindow = (BYTE*)module + kDrawWindowRva;
    BYTE* indexedWindow = (BYTE*)module + kIndexedWindowRva;
    BYTE changed = kIndexedWindow[0] ^ 1;
    tq::detour::writeBytes(indexedWindow, kIndexedWindow, &changed, 1);
    check(!tq::rendererdraw::install(module, &rendererDrawHandler, &rendererIndexedHandler)
          && !memcmp(drawWindow, kDrawWindow, sizeof(kDrawWindow)),
          "a mismatch outside the indexed patch rejects both sites before any write");
    tq::detour::writeBytes(indexedWindow, &changed, kIndexedWindow, 1);
    const bool installed = tq::rendererdraw::install(module, &rendererDrawHandler, &rendererIndexedHandler);
    check(installed && tq::rendererdraw::installed(), "install both renderer draw-site patches");
    if (installed) {
        void* vtable[115] = {};
        vtable[12] = (void*)&rendererNativeIndexedA;
        vtable[13] = (void*)&rendererNativeDraw;
        struct FakeContext { void** table; } fake = {vtable};
        ID3D11DeviceContext* context = (ID3D11DeviceContext*)&fake;
        g_nativeArgs = true;
        g_expectedCount = 27; g_expectedStart = 11; g_expectedBase = -7;
        g_companion = true;
        draw(context, g_expectedCount);
        indexed(context, g_expectedCount, g_expectedStart, g_expectedBase);
        check(g_nativeArgs && g_rendererDrawEntries == 1 && g_rendererIndexedEntries == 1
              && g_nativeDraws == 1 && g_nativeIndexedA == 1 && g_nativeIndexedB == 1,
              "preserve draw arguments and use changed native dispatch for a companion without recursion");
        check(g_rendererDrawReturn == drawWindow + kDrawPatchOffset + kPatchSize
              && g_rendererIndexedReturn == indexedWindow + kIndexedPatchOffset + kPatchSize,
              "both replacements return to the renderer's original next instruction");
        check(vtable[12] == (void*)&rendererNativeIndexedB && vtable[13] == (void*)&rendererNativeDraw,
              "renderer interception leaves native Draw and DrawIndexed slots untouched");
        g_companion = false;
        for (unsigned i = 0; i < 1000; ++i) {
            vtable[12] = i % 2 ? (void*)&rendererNativeIndexedA : (void*)&rendererNativeIndexedB;
            indexed(context, g_expectedCount, g_expectedStart, g_expectedBase);
        }
        check(g_nativeArgs && g_rendererIndexedEntries == 1001
              && g_nativeIndexedA + g_nativeIndexedB == 1002,
              "repeated native target switches cannot remove renderer interception or corrupt the stack");
        const unsigned nativeBefore = g_nativeDraws + g_nativeIndexedA + g_nativeIndexedB;
        g_suppressDraw = true;
        draw(context, g_expectedCount);
        indexed(context, g_expectedCount, g_expectedStart, g_expectedBase);
        g_suppressDraw = false;
        check(g_nativeDraws + g_nativeIndexedA + g_nativeIndexedB == nativeBefore,
              "renderer callbacks can suppress both submissions without entering native draw");
        check(tq::rendererdraw::install(module, &rendererDrawHandler, &rendererIndexedHandler),
              "repeated installation does not patch an already redirected call again");
    }
    tq::rendererdraw::shutdown();
    check(!tq::rendererdraw::installed()
          && !memcmp(drawWindow, kDrawWindow, sizeof(kDrawWindow))
          && !memcmp(indexedWindow, kIndexedWindow, sizeof(kIndexedWindow)),
          "shutdown restores both complete renderer windows byte for byte");
    void* nativeTable[115] = {};
    nativeTable[12] = (void*)&rendererNativeIndexedB;
    nativeTable[13] = (void*)&rendererNativeDraw;
    struct RestoredContext { void** table; } restored = {nativeTable};
    const unsigned hookEntries = g_rendererDrawEntries + g_rendererIndexedEntries;
    const unsigned nativeEntries = g_nativeDraws + g_nativeIndexedA + g_nativeIndexedB;
    draw((ID3D11DeviceContext*)&restored, g_expectedCount);
    indexed((ID3D11DeviceContext*)&restored, g_expectedCount, g_expectedStart, g_expectedBase);
    check(g_nativeArgs && g_rendererDrawEntries + g_rendererIndexedEntries == hookEntries
          && g_nativeDraws + g_nativeIndexedA + g_nativeIndexedB == nativeEntries + 2,
          "restored renderer sites execute native draws without entering the removed hooks");
    if (tq::rendererdraw::install(module, &rendererDrawHandler, &rendererIndexedHandler)) {
        BYTE* site = drawWindow + kDrawPatchOffset;
        BYTE owned[7], foreign[7];
        memcpy(owned, site, sizeof(owned));
        memcpy(foreign, owned, sizeof(foreign));
        foreign[0] ^= 1;
        tq::detour::writeBytes(site, owned, foreign, sizeof(foreign));
        tq::rendererdraw::shutdown();
        check(!memcmp(site, foreign, sizeof(foreign))
              && !memcmp(indexedWindow, kIndexedWindow, sizeof(kIndexedWindow)),
              "shutdown preserves a subsequent foreign patch while restoring the untouched companion site");
        tq::detour::writeBytes(site, foreign, kDrawWindow + kDrawPatchOffset, sizeof(foreign));
    } else check(false, "reinstall renderer sites for ownership-aware shutdown test");
    FreeLibrary(module);
}

void onTestPrePresent(IDXGISwapChain* swapChain) {
    g_presentOrderValid &= g_presentOrder == 0;
    g_presentOrder = 1;
    g_presentSwapChain = swapChain;
}

void onTestPostPresent(IDXGISwapChain* swapChain) {
    g_presentOrderValid &= g_presentOrder == 2 && swapChain == g_presentSwapChain;
    g_presentOrder = 3;
}

UINT g_expectedPresentInterval, g_expectedPresentFlags;
unsigned g_nativePresents, g_fullscreenQueries, g_alternatePresents;
BOOL g_testFullscreen;
HRESULT g_fullscreenResult = S_OK, g_testDescResult = S_OK;
HRESULT g_nativePresentResult = S_OK;
bool g_rejectTearing;
DXGI_SWAP_CHAIN_DESC g_testSwapDesc;
UINT g_resizeFlags;
DXGI_FORMAT g_resizeFormat;

HRESULT WINAPI testOriginalPresent(IDXGISwapChain* swapChain, UINT interval,
                                   UINT flags) {
    ++g_nativePresents;
    if (g_rejectTearing && flags == DXGI_PRESENT_ALLOW_TEARING) {
        g_presentOrderValid &= g_presentOrder == 1 && interval == 0;
        return DXGI_ERROR_INVALID_CALL;
    }
    g_presentOrderValid &= g_presentOrder == 1 && swapChain == g_presentSwapChain
                        && interval == g_expectedPresentInterval
                        && flags == g_expectedPresentFlags;
    g_presentOrder = 2;
    return g_nativePresentResult;
}

HRESULT WINAPI testAlternatePresent(IDXGISwapChain* swapChain, UINT interval, UINT flags) {
    ++g_alternatePresents;
    return testOriginalPresent(swapChain, interval, flags);
}

HRESULT WINAPI testFullscreenState(IDXGISwapChain*, BOOL* fullscreen, IDXGIOutput**) {
    ++g_fullscreenQueries;
    *fullscreen = g_testFullscreen;
    return g_fullscreenResult;
}

HRESULT WINAPI testSwapDesc(IDXGISwapChain*, DXGI_SWAP_CHAIN_DESC* desc) {
    *desc = g_testSwapDesc;
    return g_testDescResult;
}

HRESULT WINAPI testResizeBuffers(IDXGISwapChain*, UINT, UINT, UINT,
                                 DXGI_FORMAT format, UINT flags) {
    g_resizeFlags = flags;
    g_resizeFormat = format;
    return S_OK;
}

struct TearingTestFactory {
    void** vtable;
    void* slots[29] = {};
    HRESULT queryResult = S_OK, featureResult = S_OK;
    BOOL supported = TRUE;
    unsigned queries = 0, releases = 0, features = 0;
    bool validArgs = true;
    TearingTestFactory() : vtable(slots) {
        slots[0] = (void*)&query;
        slots[2] = (void*)&release;
        slots[28] = (void*)&feature;
    }
    static HRESULT WINAPI query(IDXGIFactory* self, REFIID iid, void** out) {
        auto* f = (TearingTestFactory*)self;
        ++f->queries;
        f->validArgs &= IsEqualIID(iid, __uuidof(IDXGIFactory5));
        *out = SUCCEEDED(f->queryResult) ? self : nullptr;
        return f->queryResult;
    }
    static ULONG WINAPI release(IDXGIFactory5* self) {
        ++((TearingTestFactory*)self)->releases; return 1;
    }
    static HRESULT WINAPI feature(IDXGIFactory5* self, DXGI_FEATURE kind,
                                   void* data, UINT size) {
        auto* f = (TearingTestFactory*)self;
        ++f->features;
        f->validArgs &= kind == DXGI_FEATURE_PRESENT_ALLOW_TEARING && size == sizeof(BOOL);
        *(BOOL*)data = f->supported;
        return f->featureResult;
    }
};

void testTearingCapability() {
    TearingTestFactory factory;
    auto* f = (IDXGIFactory*)&factory;
    check(tq::hdr::supportsTearing(f) && factory.validArgs && factory.releases == 1,
          "tearing requires the DXGI factory capability and releases its queried interface");
    factory.supported = FALSE;
    check(!tq::hdr::supportsTearing(f), "unsupported tearing falls back without enabling flags");
    factory.supported = TRUE; factory.featureResult = E_FAIL;
    check(!tq::hdr::supportsTearing(f), "failed capability query cannot enable tearing");
    factory.queryResult = E_NOINTERFACE;
    check(!tq::hdr::supportsTearing(f) && factory.features == 3 && factory.releases == 3
          && !tq::hdr::supportsTearing(nullptr),
          "older factories and absent DXGI support retain the non-tearing path");
    DXGI_SWAP_CHAIN_DESC original = {};
    original.BufferDesc.Width = 3840; original.BufferDesc.Height = 2160;
    original.BufferDesc.RefreshRate = {120, 1};
    original.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    original.BufferCount = 2; original.Windowed = TRUE;
    original.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    original.SampleDesc = {4, 2};
    original.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    DXGI_SWAP_CHAIN_DESC expected = original;
    expected.BufferDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    expected.SampleDesc = {1, 0}; expected.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    expected.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    DXGI_SWAP_CHAIN_DESC candidate = tq::hdr::fp16SwapChainDescription(original, true);
    check(!memcmp(&candidate, &expected, sizeof(candidate)),
          "FP16 tearing preserves requested refresh, window mode, buffer count and unrelated flags");
    candidate = tq::hdr::fp16SwapChainDescription(original, false);
    expected.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    check(!memcmp(&candidate, &expected, sizeof(candidate)),
          "non-tearing capability uses the same FP16 description without the optional flag");
}

void testRendererPresentHook() {
    const SIZE_T imageSize = 0x192000;
    BYTE* image = (BYTE*)VirtualAlloc(nullptr, imageSize,
                                     MEM_RESERVE | MEM_COMMIT,
                                     PAGE_EXECUTE_READWRITE);
    if (!image) {
        check(false, "allocate a synthetic Titan Quest renderer image");
        return;
    }
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)image;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x100;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(image + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    // Timestamp is deliberately arbitrary: identical renderer code repackaged
    // with different linker metadata must still be accepted.
    nt->FileHeader.TimeDateStamp = 0xdeadbeefu;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    nt->OptionalHeader.SizeOfImage = (DWORD)imageSize;
    static const BYTE presentCode[] = {
        0x8b, 0x51, 0x34, 0x33, 0xc0, 0x38, 0x81, 0xdb,
        0x05, 0x00, 0x00, 0x56, 0x8b, 0x32, 0x0f, 0x95,
        0xc0, 0x6a, 0x00, 0x50, 0x52, 0xff, 0x56, 0x20,
        0x5e, 0xc2, 0x04, 0x00
    };
    memcpy(image + 0x61190, presentCode, sizeof(presentCode));
    void** rendererPresentSlot = (void**)(image + 0x8625c);
    *rendererPresentSlot = image + 0x61190;

    void* swapVtable[14] = {};
    swapVtable[8] = (void*)&testOriginalPresent;
    swapVtable[11] = (void*)&testFullscreenState;
    swapVtable[12] = (void*)&testSwapDesc;
    swapVtable[13] = (void*)&testResizeBuffers;
    struct FakeSwapChain { void** vtable; } swapChain = {swapVtable};
    BYTE renderer[0x600] = {};
    *(IDXGISwapChain**)(renderer + 0x34) = (IDXGISwapChain*)&swapChain;

    tq::streaming::setPresentCallback(&onTestPrePresent);
    tq::streaming::setPostPresentCallback(&onTestPostPresent);
    bool installed = tq::streaming::installRenderer((HMODULE)image);
    typedef HRESULT (__thiscall* RendererPresentFn)(void*, void*);
    RendererPresentFn present = (RendererPresentFn)*rendererPresentSlot;
    g_presentOrder = 0;
    g_presentOrderValid = true;
    g_presentSwapChain = (IDXGISwapChain*)&swapChain;
    HRESULT result = installed ? present(renderer, nullptr) : E_FAIL;
    check(installed && tq::streaming::presentHookInstalled(),
          "install the signature-gated renderer-level Present hook");
    check(SUCCEEDED(result) && g_presentOrderValid && g_presentOrder == 3,
          "run pre-Present, Steam-safe original Present, and post-Present in order");
    check(swapVtable[8] == (void*)&testOriginalPresent,
          "leave the shared IDXGISwapChain Present slot untouched");

    g_testSwapDesc.BufferDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    g_testSwapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    g_testSwapDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    g_testSwapDesc.Windowed = TRUE;
    tq::streaming::installSwapChain((IDXGISwapChain*)&swapChain);
    auto run = [&](UINT interval, UINT flags) {
        renderer[0x5db] = (BYTE)interval;
        g_expectedPresentInterval = interval ? 1 : 0;
        g_expectedPresentFlags = flags;
        g_presentOrder = 0; g_presentOrderValid = true;
        return present(renderer, nullptr);
    };
    check(run(0, DXGI_PRESENT_ALLOW_TEARING) == S_OK && g_presentOrderValid
          && g_presentOrder == 3, "windowed VSync-off uses tearing with callback order preserved");
    unsigned queries = g_fullscreenQueries;
    check(run(1, 0) == S_OK && g_presentOrderValid && g_fullscreenQueries == queries,
          "VSync-on uses the original interval with no per-frame fullscreen query");
    check(run(255, 0) == S_OK && g_presentOrderValid,
          "any nonzero renderer VSync byte keeps the original normalized interval one");
    g_testFullscreen = TRUE;
    check(run(0, 0) == S_OK && g_presentOrderValid,
          "exclusive fullscreen never receives the windowed tearing flag");
    g_testFullscreen = FALSE; g_fullscreenResult = E_FAIL;
    check(run(0, 0) == S_OK && g_presentOrderValid,
          "unknown fullscreen state fails safely to original presentation");
    g_fullscreenResult = S_OK;
    swapVtable[8] = (void*)&testAlternatePresent;
    check(run(0, DXGI_PRESENT_ALLOW_TEARING) == S_OK && g_presentOrderValid
          && g_alternatePresents == 1 && swapVtable[8] == (void*)&testAlternatePresent,
          "tearing follows a replaced native/overlay Present slot without patching it");
    unsigned calls = g_nativePresents;
    g_rejectTearing = true;
    check(run(0, 0) == S_OK && g_presentOrderValid && g_presentOrder == 3
          && g_nativePresents == calls + 2,
          "a rejected tearing flag retries original Present once with one successful post callback");
    g_rejectTearing = false;
    queries = g_fullscreenQueries;
    check(run(0, 0) == S_OK && g_presentOrderValid && g_fullscreenQueries == queries,
          "a rejected optional flag remains disabled for the rest of the chain lifetime");
    tq::streaming::installSwapChain((IDXGISwapChain*)&swapChain);
    calls = g_nativePresents;
    g_nativePresentResult = DXGI_ERROR_DEVICE_REMOVED;
    check(run(0, DXGI_PRESENT_ALLOW_TEARING) == DXGI_ERROR_DEVICE_REMOVED
          && g_presentOrderValid && g_presentOrder == 2 && g_nativePresents == calls + 1,
          "device removal propagates without a duplicate Present or post callback");
    g_nativePresentResult = S_OK;
    typedef HRESULT (WINAPI* ResizeFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    ResizeFn resize = (ResizeFn)swapVtable[13];
    resize((IDXGISwapChain*)&swapChain, 2, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM,
           DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
    check(g_resizeFormat == DXGI_FORMAT_R16G16B16A16_FLOAT
          && g_resizeFlags == (DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH),
          "ResizeBuffers retains FP16 and immutable tearing support plus caller flags");
    g_testSwapDesc.Flags = 0;
    tq::streaming::installSwapChain((IDXGISwapChain*)&swapChain);
    queries = g_fullscreenQueries;
    check(run(0, 0) == S_OK && g_presentOrderValid && g_fullscreenQueries == queries,
          "a chain created without tearing keeps original presentation without extra queries");
    resize((IDXGISwapChain*)&swapChain, 2, 1920, 1080, DXGI_FORMAT_UNKNOWN,
           DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
    check(g_resizeFlags == 0, "ResizeBuffers cannot add tearing to a chain created without it");
    g_testSwapDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    g_testSwapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tq::streaming::installSwapChain((IDXGISwapChain*)&swapChain);
    check(run(0, 0) == S_OK && g_presentOrderValid,
          "stock eight-bit output remains outside the FP16 tearing change");
    g_testDescResult = E_FAIL;
    tq::streaming::installSwapChain((IDXGISwapChain*)&swapChain);
    check(run(0, 0) == S_OK && g_presentOrderValid,
          "a failed chain descriptor query cannot enable tearing");
    g_testDescResult = S_OK;

    tq::streaming::shutdown();
    check(*rendererPresentSlot == image + 0x61190
          && !tq::streaming::presentHookInstalled(),
          "restore the renderer hook cleanly during shutdown");
    image[0x61190] ^= 1;
    check(!tq::streaming::installRenderer((HMODULE)image)
          && *rendererPresentSlot == image + 0x61190,
          "reject a near-match renderer without changing its vtable");
    tq::streaming::shutdown();
    VirtualFree(image, 0, MEM_RELEASE);
}

void testBloomHook() {
    const SIZE_T imageSize = 0x300000;
    BYTE* image = (BYTE*)VirtualAlloc(nullptr, imageSize,
                                     MEM_RESERVE | MEM_COMMIT,
                                     PAGE_EXECUTE_READWRITE);
    if (!image) {
        check(false, "allocate a synthetic Titan Quest Engine image");
        return;
    }
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)image;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x100;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(image + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER);
    nt->OptionalHeader.SizeOfImage = (DWORD)imageSize;
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    memcpy(section->Name, ".text", 5);
    section->VirtualAddress = 0x1000;
    section->Misc.VirtualSize = 0x2aa69c;

    static const BYTE body[] = {
        0x55,0x8b,0xec,0x83,0xe4,0xf8, // validated prologue
        0x8b,0xe5,0x5d,0xc2,0x14,0x00  // synthetic epilogue
    };
    BYTE* original = image + 0x15d7f0;
    memcpy(original, body, sizeof(body));
    bool hooked = tq::bloomhook::install(
        (HMODULE)image, (tq::bloomhook::HotBlurFn)(void*)original);
    check(hooked && tq::bloomhook::installed()
          && original[0] == 0x68 && original[5] == 0xc3,
          "detour the exact Engine bloom export with one absolute branch");
    tq::bloomhook::shutdown();
    check(!tq::bloomhook::installed()
          && !memcmp(original, body, sizeof(body)),
          "restore the Engine bloom function entry during shutdown");
    original[0] ^= 1;
    check(!tq::bloomhook::install(
              (HMODULE)image, (tq::bloomhook::HotBlurFn)(void*)original)
          && !tq::bloomhook::installed(),
          "reject a near-match bloom prologue without patching it");
    tq::bloomhook::shutdown();
    VirtualFree(image, 0, MEM_RELEASE);
}

// The grass probe writes into Engine.dll's own code, so the parts worth
// testing without the game are the ones that can corrupt it: that the exact
// prologue is required, that the trampoline still reaches the body, that
// suppression is the only thing that stops it, and that every byte comes back.
LONG g_grassRenderBody;

void emitAbsoluteIncrement(BYTE* code, const void* counter) {
    code[0] = 0xff;  // inc dword ptr [imm32]
    code[1] = 0x05;
    uint32_t address = (uint32_t)(uintptr_t)counter;
    memcpy(code + 2, &address, sizeof(address));
}

void testGrassProbe() {
    const SIZE_T imageSize = 0x400000;
    BYTE* image = (BYTE*)VirtualAlloc(nullptr, imageSize,
                                      MEM_RESERVE | MEM_COMMIT,
                                      PAGE_EXECUTE_READWRITE);
    if (!image) {
        check(false, "allocate a synthetic Titan Quest Engine image");
        return;
    }
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)image;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x100;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(image + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER);
    nt->OptionalHeader.SizeOfImage = (DWORD)imageSize;
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    memcpy(section->Name, ".text", 5);
    section->VirtualAddress = 0x1000;
    section->Misc.VirtualSize = 0x2aa69c;

    // void __thiscall RenderGrass(Name&, Canvas&, SceneRenderer&, Pass&)
    BYTE* render = image + 0x23afc0;
    BYTE renderBody[] = {
        0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,  // validated prologue
        0xff, 0x05, 0, 0, 0, 0,              // inc [g_grassRenderBody]
        0x8b, 0xe5, 0x5d, 0xc2, 0x10, 0x00   // mov esp,ebp; pop ebp; ret 0x10
    };
    emitAbsoluteIncrement(renderBody + 6, &g_grassRenderBody);
    memcpy(render, renderBody, sizeof(renderBody));

    tq::grass::Exports exports = {};
    exports.renderGrassRT = render;
    bool hooked = tq::grass::install((HMODULE)image, exports);
    check(hooked && tq::grass::installed()
          && render[0] == 0x68 && render[5] == 0xc3,
          "detour the exported grass render entry with one absolute branch");

    typedef void (__fastcall* RenderFn)(void*, void*, const void*, void*,
                                        const void*, const void*);
    RenderFn callRender = (RenderFn)(void*)render;

    // The detour exists to report that a grass draw is on the stack, and it
    // must still reach the body: this is the game's own rendering, not ours.
    g_grassRenderBody = 0;
    check(!tq::grass::rendering(), "report no grass draw outside RenderGrass");
    callRender(image, nullptr, image, image, image, image);
    check(g_grassRenderBody == 1,
          "reach the grass render body through the trampoline");
    check(!tq::grass::rendering(), "stop reporting once RenderGrass returns");

    tq::grass::shutdown();
    check(!tq::grass::installed()
          && !memcmp(render, renderBody, sizeof(renderBody)),
          "restore the grass render entry during shutdown");

    render[0] ^= 1;
    tq::grass::Exports broken = {};
    broken.renderGrassRT = render;
    check(!tq::grass::install((HMODULE)image, broken) && !tq::grass::installed(),
          "reject a near-match grass prologue without patching it");
    tq::grass::shutdown();
    VirtualFree(image, 0, MEM_RELEASE);
}

// The engine instrumentation writes into another module's .text on paths that
// run thousands of times a second, so the parts worth testing without the game
// are the ones that can corrupt it: that a long signature refuses a target
// whose short prologue matches, that relocated operands are checked rather
// than skipped, that a retargeted call site reaches the replacement, and that
// every byte comes back.
LONG g_detourBody;
LONG g_detourHook;
LONG g_callOriginal;
LONG g_callReplacement;

void __fastcall detourHookBody(void*, void*, int) { InterlockedIncrement(&g_detourHook); }
void __fastcall detourReplaceBody(void*, void*) { InterlockedIncrement(&g_detourHook); }
void __stdcall detourCallTarget() { InterlockedIncrement(&g_callOriginal); }
LONG g_sleepCalls;
void __stdcall sleepCountingReplacement(DWORD) { InterlockedIncrement(&g_sleepCalls); }
void __stdcall detourCallReplacement() { InterlockedIncrement(&g_callReplacement); }

BYTE* allocateSyntheticEngine(SIZE_T imageSize) {
    BYTE* image = (BYTE*)VirtualAlloc(nullptr, imageSize,
                                      MEM_RESERVE | MEM_COMMIT,
                                      PAGE_EXECUTE_READWRITE);
    if (!image) return nullptr;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)image;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x100;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(image + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER);
    nt->OptionalHeader.SizeOfImage = (DWORD)imageSize;
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    memcpy(section->Name, ".text", 5);
    section->VirtualAddress = 0x1000;
    section->Misc.VirtualSize = 0x2aa69c;
    return image;
}

void testDetour() {
    const SIZE_T imageSize = 0x400000;
    BYTE* image = allocateSyntheticEngine(imageSize);
    if (!image) {
        check(false, "allocate a synthetic image for the detour tests");
        return;
    }
    HMODULE module = (HMODULE)image;

    // ---- a long signature over the shared six-byte prologue.
    //
    // Region::LoadLevel, Archive::ReadFromFile, the archive block inflate and
    // Region::GetEntitiesInFrustum all open with these same six bytes, so this
    // is the exact confusion the verify length exists to prevent.
    BYTE* target = image + 0x20bec0;
    BYTE body[] = {
        0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,     // the shared prologue: stolen
        0x83, 0xec, 0x0c, 0x53, 0x8b, 0xd9,     // verified, not stolen
        0x56, 0x8b, 0x43, 0x50,                 // mov eax,[ebx+0x50]
        0xff, 0x05, 0, 0, 0, 0,                 // inc [g_detourBody]
        0x8b, 0xe5, 0x5d, 0xc2, 0x04, 0x00      // mov esp,ebp; pop ebp; ret 4
    };
    emitAbsoluteIncrement(body + 16, &g_detourBody);
    memcpy(target, body, sizeof(body));
    BYTE reference[sizeof(body)];
    memcpy(reference, body, sizeof(body));

    tq::detour::Signature good = {body, 16, nullptr, 0};
    BYTE nearMiss[16];
    memcpy(nearMiss, body, sizeof(nearMiss));
    nearMiss[9] ^= 1;                            // past the six stolen bytes
    tq::detour::Signature wrong = {nearMiss, 16, nullptr, 0};

    tq::detour::Detour detour = {};
    void* trampoline = nullptr;
    check(!tq::detour::attach(detour, module, target, wrong, 6,
                              (const void*)&detourHookBody, &trampoline)
          && !detour.installed && !trampoline
          && !memcmp(target, reference, sizeof(reference)),
          "attach refuses a target whose stolen bytes match but whose"
          " signature does not, and writes nothing");

    check(tq::detour::attach(detour, module, target, good, 6,
                             (const void*)&detourHookBody, &trampoline)
          && detour.installed && trampoline
          && target[0] == 0x68 && target[5] == 0xc3,
          "attach detours a verified target with one absolute branch");

    typedef void (__fastcall* BodyFn)(void*, void*, int);
    g_detourBody = g_detourHook = 0;
    ((BodyFn)(void*)target)(image, nullptr, 0);
    check(g_detourHook == 1 && g_detourBody == 0,
          "the detoured entry reaches the replacement");
    g_detourHook = 0;
    ((BodyFn)trampoline)(image, nullptr, 0);
    check(g_detourBody == 1 && g_detourHook == 0,
          "the trampoline still reaches the original body");

    tq::detour::detach(detour);
    check(!detour.installed && !memcmp(target, reference, sizeof(reference)),
          "detach restores every stolen byte");

    // ---- relocated operands are resolved, not masked out.
    BYTE* relocated = image + 0x213ed0;
    BYTE relocatedBody[] = {
        0x6a, 0xff, 0x68, 0, 0, 0, 0,           // push -1; push <handler>
        0x64, 0xa1, 0x00, 0x00, 0x00, 0x00,
        0x50, 0x83, 0xec, 0x08,
        0xc2, 0x04, 0x00
    };
    const uint32_t handler = (uint32_t)(uintptr_t)(image + 0x2a2db8);
    memcpy(relocatedBody + 3, &handler, sizeof(handler));
    memcpy(relocated, relocatedBody, sizeof(relocatedBody));
    BYTE relocatedPattern[17];
    memcpy(relocatedPattern, relocatedBody, sizeof(relocatedPattern));
    memset(relocatedPattern + 3, 0, sizeof(uint32_t));
    const tq::detour::Relocation right[] = {{3, 0x2a2db8}};
    const tq::detour::Relocation elsewhere[] = {{3, 0x2a29a3}};
    tq::detour::Signature matching = {relocatedPattern, 17, right, 1};
    tq::detour::Signature mismatched = {relocatedPattern, 17, elsewhere, 1};
    check(tq::detour::matches(module, relocated, matching),
          "a signature resolves a relocated operand against the module base");
    check(!tq::detour::matches(module, relocated, mismatched),
          "a relocated operand pointing somewhere else fails the signature");

    // ---- replace, for a target too small to trampoline.
    BYTE* tiny = image + 0x20bde0;
    const BYTE tinyBody[] = {0x80, 0x79, 0x78, 0x01, 0x74, 0xfa, 0xc3};
    memcpy(tiny, tinyBody, sizeof(tinyBody));
    tq::detour::Signature tinySignature = {tinyBody, sizeof(tinyBody), nullptr, 0};
    tq::detour::Detour replaced = {};
    check(tq::detour::replace(replaced, module, tiny, tinySignature,
                              sizeof(tinyBody),
                              (const void*)&detourReplaceBody)
          && replaced.installed && !replaced.trampoline
          && tiny[0] == 0x68 && tiny[5] == 0xc3 && tiny[6] == 0x90,
          "replace overwrites a seven-byte function and nops the tail");
    g_detourHook = 0;
    ((void (__fastcall*)(void*, void*))(void*)tiny)(nullptr, nullptr);
    check(g_detourHook == 1, "the replaced function runs the replacement");
    tq::detour::detach(replaced);
    check(!replaced.installed && !memcmp(tiny, tinyBody, sizeof(tinyBody)),
          "detach restores a replaced function byte for byte");

    // ---- patchCall over an FF 15 site. This is the shape both the loader
    // fence and the region lock use, and the operand it rewrites points at the
    // patch's own cell rather than at the shared import slot -- so every other
    // caller of that import is left alone.
    void** slot = (void**)(image + 0x2ac188);
    *slot = (void*)&detourCallTarget;
    BYTE* indirect = image + 0x14479a;
    BYTE indirectBody[] = {0xff, 0x15, 0, 0, 0, 0, 0xc3};
    const uint32_t slotAddress = (uint32_t)(uintptr_t)slot;
    memcpy(indirectBody + 2, &slotAddress, sizeof(slotAddress));
    memcpy(indirect, indirectBody, sizeof(indirectBody));
    BYTE indirectPattern[sizeof(indirectBody)];
    memcpy(indirectPattern, indirectBody, sizeof(indirectBody));
    memset(indirectPattern + 2, 0, sizeof(uint32_t));
    const tq::detour::Relocation importSlot[] = {{2, 0x2ac188}};
    tq::detour::Signature indirectSignature = {indirectPattern,
                                               sizeof(indirectPattern),
                                               importSlot, 1};
    tq::detour::CallPatch indirectPatch = {};
    check(!tq::detour::patchCall(indirectPatch, module, indirect,
                                 indirectSignature, 0, (const void*)&g_detourBody,
                                 (const void*)&detourCallReplacement)
          && !indirectPatch.installed
          && !memcmp(indirect, indirectBody, sizeof(indirectBody)),
          "patchCall refuses a site whose call resolves somewhere unexpected");
    check(tq::detour::patchCall(indirectPatch, module, indirect,
                                indirectSignature, 0,
                                (const void*)&detourCallTarget,
                                (const void*)&detourCallReplacement)
          && indirectPatch.installed
          && *slot == (void*)&detourCallTarget,
          "patchCall retargets an FF 15 site without touching the import slot");
    g_callOriginal = g_callReplacement = 0;
    ((void (__stdcall*)())(void*)indirect)();
    check(g_callReplacement == 1 && g_callOriginal == 0,
          "the retargeted FF 15 site calls the replacement");
    tq::detour::restoreCall(indirectPatch);
    check(!memcmp(indirect, indirectBody, sizeof(indirectBody)),
          "restoreCall puts an FF 15 operand back exactly");
    g_callOriginal = g_callReplacement = 0;
    ((void (__stdcall*)())(void*)indirect)();
    check(g_callOriginal == 1 && g_callReplacement == 0,
          "the restored FF 15 site calls the original again");

    // ---- patchCall over an E8 site, which is what the seven resource-manager
    // sweeps use.
    BYTE* callee = image + 0x120250;
    BYTE calleeBody[] = {0xff, 0x05, 0, 0, 0, 0, 0xc3};
    emitAbsoluteIncrement(calleeBody, &g_callOriginal);
    memcpy(callee, calleeBody, sizeof(calleeBody));
    BYTE* direct = image + 0x144484;
    BYTE directBody[] = {0xe8, 0, 0, 0, 0, 0xc3};
    const int32_t rel = (int32_t)((uintptr_t)callee - ((uintptr_t)direct + 5));
    memcpy(directBody + 1, &rel, sizeof(rel));
    memcpy(direct, directBody, sizeof(directBody));
    tq::detour::Signature directSignature = {directBody, sizeof(directBody),
                                             nullptr, 0};
    tq::detour::CallPatch directPatch = {};
    check(!tq::detour::patchCall(directPatch, module, direct, directSignature, 0,
                                 (const void*)&g_detourBody,
                                 (const void*)&detourCallReplacement)
          && !memcmp(direct, directBody, sizeof(directBody)),
          "patchCall refuses an E8 site whose displacement resolves elsewhere");
    check(tq::detour::patchCall(directPatch, module, direct, directSignature, 0,
                                callee, (const void*)&detourCallReplacement)
          && directPatch.installed,
          "patchCall retargets a verified E8 site");
    g_callOriginal = g_callReplacement = 0;
    ((void (__stdcall*)())(void*)direct)();
    check(g_callReplacement == 1 && g_callOriginal == 0,
          "the retargeted E8 site calls the replacement");
    tq::detour::restoreCall(directPatch);
    check(!memcmp(direct, directBody, sizeof(directBody)),
          "restoreCall puts an E8 displacement back exactly");
    g_callOriginal = g_callReplacement = 0;
    ((void (__stdcall*)())(void*)direct)();
    check(g_callOriginal == 1 && g_callReplacement == 0,
          "the restored E8 site calls the original again");

    VirtualFree(image, 0, MEM_RELEASE);
}

// The archive block cache. It sits on the one path in this project where being
// wrong is silent -- a block served from the wrong slot is a corrupt texture or
// a corrupt level, not a crash -- so the tests here are about the key being
// strict and the slab never handing back somebody else's bytes.
tq::arccache::Key blockKey(unsigned n) {
    tq::arccache::Key key = {};
    key.archive = (const void*)0x10000000;
    key.handle = (void*)0x40;
    key.offset = n * 0x40000u;
    key.compressed = 1000u + n;
    key.uncompressed = 4096;
    return key;
}

void fillBlock(BYTE* block, uint32_t bytes, unsigned n) {
    for (uint32_t i = 0; i < bytes; ++i) block[i] = (BYTE)(i * 31u + n * 7u + 3u);
}

void testArchiveCache() {
    wchar_t ini[MAX_PATH];
    if (!GetFullPathNameW(L"tqflicker-arccache-selftest.ini", MAX_PATH, ini,
                          nullptr))
        return;
    DeleteFileW(ini);

    // ---- the option. Eight MiB is the measured useful ceiling and now the
    // default; zero remains an explicit stock-path override.
    tq::arccache::readOptions(ini);
    check(tq::arccache::megabytes() == 8 && !tq::arccache::verifying(),
          "archive_cache_mb defaults to the measured eight MiB size");

    WritePrivateProfileStringW(L"performance", L"archive_cache_mb", L"0", ini);
    tq::arccache::readOptions(ini);
    check(!tq::arccache::configured(), "archive_cache_mb=0 is off");

    WritePrivateProfileStringW(L"performance", L"archive_cache_mb", L"8", ini);
    tq::arccache::readOptions(ini);
    check(tq::arccache::megabytes() == 8 && !tq::arccache::verifying(),
          "archive_cache_mb=8 is eight megabytes, serving from the slab");

    WritePrivateProfileStringW(L"performance", L"archive_cache_mb", L"8VeRiFy",
                               ini);
    tq::arccache::readOptions(ini);
    check(tq::arccache::megabytes() == 8 && tq::arccache::verifying(),
          "archive_cache_mb=8verify asks for the measurement boot");

    WritePrivateProfileStringW(L"performance", L"archive_cache_mb", L"8 slots",
                               ini);
    tq::arccache::readOptions(ini);
    check(!tq::arccache::configured(),
          "a value that is not a size stays off rather than being guessed at");

    WritePrivateProfileStringW(L"performance", L"archive_cache_mb", L"9999",
                               ini);
    tq::arccache::readOptions(ini);
    check(tq::arccache::megabytes() == 256,
          "an absurd size is clamped, not taken literally in a 32-bit process");
    DeleteFileW(ini);

    const uint32_t bytes = 4096;
    BYTE* block = (BYTE*)malloc(bytes);
    BYTE* out = (BYTE*)malloc(bytes);
    if (!block || !out) {
        check(false, "allocate the archive cache test buffers");
        free(block);
        free(out);
        return;
    }

    // ---- one megabyte is four slots, which is what makes the clock visible.
    tq::arccache::configureForTest(1, false);
    check(tq::arccache::start() && tq::arccache::running()
          && tq::arccache::slotsForTest() == 4,
          "one megabyte commits four slots of one block each");

    fillBlock(block, bytes, 0);
    tq::arccache::store(blockKey(0), block);
    memset(out, 0, bytes);
    check(tq::arccache::lookup(blockKey(0), out) && !memcmp(out, block, bytes),
          "a stored block comes back byte for byte");

    check(!tq::arccache::lookup(blockKey(1), out),
          "a block that was never stored misses");

    // Every field of the key is load bearing, and the two sizes are the two
    // that a naive {archive, handle, offset} key would drop.
    tq::arccache::Key sameOffset = blockKey(0);
    sameOffset.compressed += 1;
    check(!tq::arccache::lookup(sameOffset, out),
          "a different compressed size is a different block, not a hit");
    sameOffset = blockKey(0);
    sameOffset.handle = (void*)0x41;
    check(!tq::arccache::lookup(sameOffset, out),
          "the same offset in a different file is not a hit");

    // ---- the clock. Five distinct blocks into four slots: one is gone, the
    // rest are intact, and nothing has been handed the wrong bytes.
    for (unsigned n = 0; n < 5; ++n) {
        fillBlock(block, bytes, n);
        tq::arccache::store(blockKey(n), block);
    }
    unsigned resident = 0, wrong = 0;
    for (unsigned n = 0; n < 5; ++n) {
        memset(out, 0, bytes);
        if (!tq::arccache::lookup(blockKey(n), out)) continue;
        ++resident;
        fillBlock(block, bytes, n);
        if (memcmp(out, block, bytes)) ++wrong;
    }
    check(resident == 4 && wrong == 0,
          "five blocks into four slots evicts one and corrupts none");

    // ---- verify mode never serves, and catches a slab that disagrees.
    tq::arccache::configureForTest(1, true);
    check(tq::arccache::start() && tq::arccache::verifying(),
          "the verification boot commits its slab too");
    fillBlock(block, bytes, 9);
    tq::arccache::store(blockKey(9), block);
    check(!tq::arccache::lookup(blockKey(9), out),
          "verify mode never serves a block, so the engine's own inflate runs");
    tq::arccache::store(blockKey(9), block);
    check(tq::arccache::mismatchesForTest() == 0 && tq::arccache::running(),
          "a block that matches what the engine produced is counted, not flagged");
    fillBlock(block, bytes, 10);
    tq::arccache::store(blockKey(9), block);
    check(tq::arccache::mismatchesForTest() == 1 && !tq::arccache::running(),
          "a block that disagrees stops the cache for the rest of the session");

    tq::arccache::stop();
    check(!tq::arccache::lookup(blockKey(0), out) && !tq::arccache::running(),
          "a stopped cache serves nothing");
    tq::arccache::configureForTest(0, false);
    free(block);
    free(out);
}

// The engine trace is gated twice and measured once. The gate is what keeps a
// shipping boot byte-identical to a build without any of this; the region-lock
// thunk is the one hook that sits on a render-path call, so what it costs when
// the section is free is the thing worth pinning down off-game.
struct LockHolder {
    CRITICAL_SECTION* section;
    HANDLE acquired;
};

DWORD WINAPI holdLockBriefly(void* argument) {
    LockHolder* holder = (LockHolder*)argument;
    EnterCriticalSection(holder->section);
    SetEvent(holder->acquired);
    Sleep(100);
    LeaveCriticalSection(holder->section);
    return 0;
}

void testEngineProbe() {
    wchar_t ini[MAX_PATH], csv[MAX_PATH];
    if (!GetFullPathNameW(L"tqflicker-engine-selftest.ini", MAX_PATH, ini, nullptr)
        || !GetFullPathNameW(L"tqflicker-engine-selftest.csv", MAX_PATH, csv,
                             nullptr))
        return;
    DeleteFileW(ini);
    DeleteFileW(csv);

    BYTE* image = allocateSyntheticEngine(0x400000);
    if (!image) {
        check(false, "allocate a synthetic image for the engine trace tests");
        return;
    }

    // The shipping configuration keeps measurement off while the accepted
    // behavior fixes remain independently requested. A non-audited image must
    // still receive no patch.
    tq::probe::readOptions(ini);
    tq::engine::readOptions(ini);
    check(!tq::probe::enabled() && tq::arccache::megabytes() == 8
          && tq::engineprobe::shadowDeferColdResourcesForTest()
          && tq::engineprobe::shadowDeferColdActorPoseForTest()
          && tq::engineprobe::terrainPreloadLayersForTest()
          && tq::engineprobe::secondaryPassAdmissionBudgetForTest() == 8,
          "no INI selects every accepted performance default without tracing");
    check(!tq::engine::install((HMODULE)image)
          && tq::engineprobe::installedForTest() == 0,
          "default fixes still refuse a module that is not Engine.dll");

    // Pin the actual normal-configuration combination too, rather than only
    // inferring it from no-INI defaults. Both debug switches are explicitly
    // off while every accepted Engine-side behavior remains requested; the
    // independently defaulted visual streaming/loose-file paths are checked
    // by their own parser and install-contract tests.
    WritePrivateProfileStringW(L"debug", L"trace", L"0", ini);
    WritePrivateProfileStringW(L"debug", L"performance_trace", L"0", ini);
    WritePrivateProfileStringW(L"performance", L"archive_cache_mb", L"8", ini);
    WritePrivateProfileStringW(L"performance", L"shadow_defer_cold_resources",
                               L"1", ini);
    WritePrivateProfileStringW(L"performance", L"shadow_defer_cold_actor_pose",
                               L"1", ini);
    WritePrivateProfileStringW(L"performance", L"terrain_preload_layers", L"1",
                               ini);
    WritePrivateProfileStringW(L"performance",
                               L"secondary_pass_admission_budget", L"8", ini);
    tq::probe::readOptions(ini);
    tq::engine::readOptions(ini);
    check(!tq::probe::enabled() && tq::arccache::megabytes() == 8
          && tq::engineprobe::shadowDeferColdResourcesForTest()
          && tq::engineprobe::shadowDeferColdActorPoseForTest()
          && tq::engineprobe::terrainPreloadLayersForTest()
          && tq::engineprobe::secondaryPassAdmissionBudgetForTest() == 8,
          "trace=0 and performance_trace=0 keep every accepted Engine-side"
          " performance behavior requested");
    check(!tq::engineprobe::wantsForTest(2)
          && !tq::engineprobe::wantsForTest(4)
          && !tq::engineprobe::wantsForTest(16384)
          && !tq::engineprobe::wantsForTest(32768)
          && !tq::engineprobe::wantsForTest(65536)
          && !tq::engineprobe::wantsForTest(131072),
          "the trace-off accepted behavior set enables no trace group");

    check(tq::engine::exerciseGameUpdateCompatibilityForTest(),
          "stock/HekTo Game Update preserves ABI, callback order and teardown; rejects altered layouts");
    check(tq::engine::exerciseTraceOffHooksForTest(),
          "trace-off shared hooks preload, queue cold roots and budget draws"
          " without entering either recorder");
    tq::engine::readOptions(ini);

    // Per-draw clocks are part of full measurement now. The removed legacy key
    // cannot arm them without full mode, and hitch-only mode stays lightweight.
    WritePrivateProfileStringW(L"debug", L"draw_timing", L"1", ini);
    tq::probe::readOptions(ini);
    check(!tq::probe::enabled() && !tq::probe::drawTimingEnabled(),
          "the removed draw_timing key cannot arm measurement by itself");

    WritePrivateProfileStringW(L"debug", L"performance_trace", L"1", ini);
    tq::probe::readOptions(ini);
    check(tq::probe::enabled() && !tq::probe::drawTimingEnabled(),
          "hitch-only performance_trace omits high-frequency draw clocks");

    WritePrivateProfileStringW(L"debug", L"performance_trace", L"full", ini);
    WritePrivateProfileStringW(L"debug", L"engine_trace", L"0", ini);
    tq::probe::readOptions(ini);
    tq::probe::setOutputPath(csv);
    tq::engine::readOptions(ini);
    check(tq::probe::enabled() && !tq::engine::install((HMODULE)image)
          && tq::engineprobe::installedForTest() == 0,
          "a non-audited module stays untouched with full measurement on");

    // Full mode arms the clocks, and the phase it arms is the one the CSV
    // column is named after -- so a run that reads draw_submit_ms = 0 is the
    // game not spending time there, not a switch that silently did nothing.
    check(tq::probe::enabled() && tq::probe::drawTimingEnabled(),
          "performance_trace=full includes draw and map phases");
    tq::probe::beginFrame(nullptr);
    const int64_t drawStart = tq::probe::now();
    while (tq::probe::now() == drawStart) {}
    tq::probe::addPhase(tq::probe::PhaseDrawSubmit, drawStart);
    tq::probe::endFrame(16.7f);
    check(tq::probe::phaseForTest(0, tq::probe::PhaseDrawSubmit) > 0.0f
          && tq::probe::phaseForTest(0, tq::probe::PhaseMapResource) == 0.0f,
          "draw_submit records into its own column and not its neighbour");

    // The deferred-pass partition reuses the Draw hook's one ending clock
    // sample.  Drive two distinct pass buckets directly so an enum/name shift
    // cannot silently charge the wait to its neighbour.
    tq::probe::beginFrame(nullptr);
    const int64_t partitionStart = tq::probe::now();
    while (tq::probe::now() == partitionStart) {}
    const uint32_t partitionUs = tq::probe::finishPhase(
        tq::probe::PhaseDrawSubmit, partitionStart);
    tq::engineprobe::setDeferredOwnerContextForTest(1, 1);  // setup site
    tq::engineprobe::setDeferredPassForTest(1);  // geometry
    tq::engineprobe::countDeferredDrawForTest(partitionUs);
    tq::engineprobe::noteDeferredCreationForTest(true, 23);
    tq::engineprobe::noteDeferredCreationForTest(false, 17);
    tq::engineprobe::resetOffMainTexturesForTest();
    tq::engineprobe::noteOffMainTextureCreated(
        70, 71, 1234, 99, 2048, 1024, 12, 77, 8, 4, true);
    unsigned offStart = 0, offFinish = 0, offElapsed = 0, offThread = 0;
    unsigned offWidth = 0, offHeight = 0, offMips = 0;
    bool offInitial = false;
    const bool offTexture = tq::engineprobe::latestOffMainTextureForTest(
        &offStart, &offFinish, &offElapsed, &offThread, &offWidth,
        &offHeight, &offMips, &offInitial);
    tq::engineprobe::setDeferredOwnerContextForTest(1, 0);  // owner remainder
    tq::engineprobe::setDeferredPassForTest(6);  // post
    tq::engineprobe::countDeferredDrawForTest(17);
    tq::engineprobe::setDeferredPassForTest(0);
    tq::probe::endFrame(16.7f);
    check(partitionUs > 0
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineDeferredGeometryDrawUs)
                 == partitionUs
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineDeferredPostDrawUs) == 17
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineDeferredLightingDrawUs) == 0,
          "one Draw interval lands in only the active deferred-renderer pass");
    check(offTexture && offStart == 70 && offFinish == 71
          && offElapsed == 1234 && offThread == 99
          && offWidth == 2048 && offHeight == 1024 && offMips == 12
          && offInitial,
          "off-main texture trace publishes exact descriptor and frame extent");
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineDeferredI1GeometrySetupDrawUs)
              == partitionUs
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineDeferredI2GeometrySetupDrawUs) == 0
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineDeferredI1GeometrySetupTexCreate)
                 == 1
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineDeferredI1GeometrySetupTexCreateUs)
                 == 23
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineDeferredI1GeometrySetupBufCreate)
                 == 1
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineDeferredI1GeometrySetupBufCreateUs)
                 == 17,
          "owner invocation and geometry site partition Draw and D3D creation");

    tq::probe::beginFrame(nullptr);
    tq::engineprobe::setReflectionContextForTest(2, 1);
    tq::engineprobe::countDeferredDrawForTest(31);
    tq::engineprobe::noteDeferredCreationForTest(true, 29);
    tq::engineprobe::noteDeferredCreationForTest(false, 19);
    tq::engineprobe::setReflectionContextForTest(0, 0);
    tq::probe::endFrame(16.7f);
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineReflectionManagerDrawUs) == 31
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineReflectionI2DrawUs) == 31
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineReflectionI2P1DrawUs) == 31
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineReflectionI1P1DrawUs) == 0
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineReflectionI2P1TexCreate) == 1
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineReflectionI2P1TexCreateUs) == 29
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineReflectionI2P1BufCreate) == 1
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineReflectionI2P1BufCreateUs) == 19,
          "reflection manager and plane partition Draw and D3D creation");

    // One newly created main-thread vertex buffer is then consumed by the
    // exact reflection-plane, directional-shadow, and deferred-owner classes.
    // The milestone counters must fire only as each new family is reached.
    tq::probe::beginFrame(nullptr);
    const void* const crossBuffer = (const void*)0x12345678;
    tq::engineprobe::setCrossPassTracingForTest(true);
    tq::engineprobe::setReflectionContextForTest(2, 1);
    tq::engineprobe::noteCrossPassBufferForTest(crossBuffer, 256);
    tq::engineprobe::countCrossPassDrawForTest(crossBuffer);
    tq::engineprobe::setReflectionContextForTest(0, 0);
    tq::engineprobe::setDirectionalContextForTest(true);
    tq::engineprobe::countCrossPassDrawForTest(crossBuffer);
    tq::engineprobe::setDirectionalContextForTest(false);
    tq::engineprobe::setDeferredOwnerContextForTest(2, 2);  // scene site
    tq::engineprobe::setDeferredPassForTest(1);  // geometry
    tq::engineprobe::countCrossPassDrawForTest(crossBuffer);
    tq::engineprobe::setDeferredPassForTest(0);
    tq::engineprobe::setDeferredOwnerContextForTest(0, 0);
    tq::engineprobe::setCrossPassTracingForTest(false);
    tq::probe::endFrame(16.7f);
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineCrossPassBufferCreated) == 1
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineCrossPassBufferCreatedBytes) == 256
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineCrossPassReflectionDraw) == 1
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineCrossPassShadowDraw) == 1
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineCrossPassDeferredDraw) == 1
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineShadowDirectionalDraw) == 1,
          "cross-pass trace classifies one draw in each exact family");
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineCrossPassFreshReflectionBuffer) == 1
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineCrossPassFreshShadowBuffer) == 1
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineCrossPassFreshDeferredBuffer) == 1
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineCrossPassJoinReflectionShadow) == 1
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineCrossPassJoinReflectionDeferred) == 1
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineCrossPassJoinShadowDeferred) == 1
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineCrossPassJoinAllThree) == 1
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineCrossPassIndexOverflow) == 0
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineCrossPassRecentEviction) == 0,
          "fresh-buffer joins fire once when each cross-pass relation appears");

    // Run 79 spends the same sixteen timestamp pairs continuously across
    // reflection draws 1--320. Exercise the partition without a device and
    // prove that the retained call stream distinguishes both TerrainBlock
    // and GraphicsMeshInstance while keeping nested terrain work.
    tq::probe::beginFrame(nullptr);
    tq::engineprobe::setGpuChunkTracingForTest(true);
    check(!tq::engineprobe::gpuChunkDrawActive(),
          "ordinary draws bypass sparse GPU chunk helpers");
    tq::engineprobe::armGpuChunksForTest();
    check(tq::engineprobe::gpuChunkDrawActive(),
          "selected reflection RenderLightStyle opens the draw gate");
    tq::engineprobe::recordGpuChunkTerrainCallForTest(
        true, (const void*)2, 31, 19, 17);
    for (unsigned i = 0; i < 18; ++i) {
        tq::engineprobe::beginGpuChunkDraw(nullptr);
        tq::engineprobe::finishGpuChunkDraw(true, 3, nullptr);
    }
    tq::engineprobe::recordGpuChunkMeshCallForTest((const void*)3, 41);
    for (unsigned i = 0; i < 18; ++i) {
        tq::engineprobe::beginGpuChunkDraw(nullptr);
        tq::engineprobe::finishGpuChunkDraw(true, 3, nullptr);
    }
    tq::engineprobe::closeGpuChunksForTest();
    check(!tq::engineprobe::gpuChunkDrawActive(),
          "reflection RenderLightStyle exit closes the draw gate");
    tq::probe::endFrame(16.7f);
    bool terrainBlock = false;
    unsigned terrainFirst = 0, terrainLast = 0, terrainCpu = 0;
    unsigned terrainResources = 0, terrainResourceUs = 0;
    unsigned terrainTextures = 0, terrainTextureUs = 0;
    const bool terrainCall = tq::engineprobe::gpuChunkTerrainCallForTest(
        0, &terrainBlock, &terrainFirst, &terrainLast, &terrainCpu,
        &terrainResources, &terrainResourceUs, &terrainTextures,
        &terrainTextureUs);
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineGpuChunkReflectionArm) == 1
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineGpuChunkReflectionStartDraw) == 1
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineGpuChunkReflectionDraw) == 40
          && tq::engineprobe::gpuChunkBinDrawsForTest(0) == 20
          && tq::engineprobe::gpuChunkBinDrawsForTest(1) == 20,
          "reflection GPU chunks cover draws 1--40 as two 20-draw bins");
    check(terrainCall && terrainBlock && terrainFirst == 1
          && terrainLast == 2 && terrainCpu == 31
          && terrainResources == 1 && terrainResourceUs == 19
          && terrainTextures == 1 && terrainTextureUs == 17,
          "selected TerrainBlock retains draw ordinals and nested work");
    check(tq::engineprobe::gpuChunkRenderableKindForTest(0) == 2
          && tq::engineprobe::gpuChunkRenderableKindForTest(1) == 3,
          "reflection renderable calls distinguish terrain and mesh classes");
    tq::engineprobe::setGpuChunkTracingForTest(false);

    WritePrivateProfileStringW(L"debug", L"engine_trace", L"1", ini);
    tq::engine::readOptions(ini);
    check(!tq::engine::install((HMODULE)image)
          && tq::engineprobe::installedForTest() == 0,
          "a module that is not the audited Engine.dll installs nothing");
    tq::engine::shutdown();

    // The region-lock thunk, both ways round. Uncontended it is one
    // interlocked operation and a branch and records nothing at all, which is
    // what makes it affordable on a call the renderer makes per region per
    // frame; contended it names the wait, which is the audit's section 1b and
    // has never been measured.
    CRITICAL_SECTION section;
    InitializeCriticalSection(&section);
    tq::probe::endFrame(16.7f);
    tq::engineprobe::enterCriticalSectionForTest(&section);
    LeaveCriticalSection(&section);
    tq::probe::endFrame(16.7f);
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineRegionLockHits) == 0
          && tq::probe::counterForTest(
                 0, tq::probe::CounterEngineRegionLockUs) == 0,
          "the region-lock thunk records nothing while the section is free");

    LockHolder holder = {&section,
                         CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HANDLE thread = holder.acquired
        ? CreateThread(nullptr, 0, holdLockBriefly, &holder, 0, nullptr)
        : nullptr;
    if (thread && WaitForSingleObject(holder.acquired, 5000) == WAIT_OBJECT_0) {
        tq::engineprobe::enterCriticalSectionForTest(&section);
        LeaveCriticalSection(&section);
        WaitForSingleObject(thread, 5000);
        tq::probe::endFrame(16.7f);
        const uint32_t hits = tq::probe::counterForTest(
            0, tq::probe::CounterEngineRegionLockHits);
        const uint32_t waited = tq::probe::counterForTest(
            0, tq::probe::CounterEngineRegionLockUs);
        check(hits == 1 && waited >= 10000u && waited < 5000000u,
              "the region-lock thunk names the wait when the section is held");
    } else {
        check(false, "hold the region lock from a second thread");
    }
    if (thread) CloseHandle(thread);
    if (holder.acquired) CloseHandle(holder.acquired);
    DeleteCriticalSection(&section);

    tq::probe::shutdown();
    DeleteFileW(csv);
    tq::probe::resetForTest();
    tq::probe::readOptions(nullptr);
    tq::engine::readOptions(nullptr);
    DeleteFileW(ini);

    // The third way in, added with the block cache. archive_cache_mb is a fix
    // rather than an instrument, so unlike every other hook in that file it
    // installs with the performance probe off -- but the module check is not
    // relaxed with it, and a build that is not the audited Engine.dll still
    // gets nothing.
    WritePrivateProfileStringW(L"performance", L"archive_cache_mb", L"8", ini);
    tq::probe::readOptions(ini);
    tq::engine::readOptions(ini);
    check(!tq::probe::enabled() && tq::arccache::configured()
          && !tq::engine::install((HMODULE)image)
          && tq::engineprobe::installedForTest() == 0,
          "archive_cache_mb reaches install() with the probe off, and still"
          " installs nothing into a module that is not Engine.dll");
    // And it brings none of the instrument with it. engine_trace defaults to
    // 1, so every group would say yes if the mask were consulted on its own;
    // what makes them say no is the probe being off.
    check(!tq::engineprobe::wantsForTest(2) && !tq::engineprobe::wantsForTest(4)
          && !tq::engineprobe::wantsForTest(1024)
          && !tq::engineprobe::wantsForTest(16384)
          && !tq::engineprobe::wantsForTest(65536),
          "a cache-only boot installs no trace group, whatever engine_trace says");
    tq::engine::shutdown();
    tq::engine::readOptions(nullptr);
    DeleteFileW(ini);

    // Rejected behavior keys remain inert even if copied from an old INI.
    check(!tq::engineprobe::asyncLevelLoadForTest(),
          "async_level_load is off with no INI at all");
    WritePrivateProfileStringW(L"performance", L"loose_texture_max", L"4096",
                               ini);
    tq::engine::readOptions(ini);
    check(!tq::engineprobe::asyncLevelLoadForTest(),
          "async_level_load is off in a [performance] section that omits it");
    WritePrivateProfileStringW(L"performance", L"async_level_load", L"0", ini);
    tq::engine::readOptions(ini);
    check(!tq::engineprobe::asyncLevelLoadForTest(),
          "async_level_load=0 is off");

    WritePrivateProfileStringW(L"performance", L"archive_cache_mb", L"0", ini);
    WritePrivateProfileStringW(L"performance", L"async_level_load", L"1", ini);
    tq::probe::readOptions(ini);
    tq::engine::readOptions(ini);
    check(!tq::probe::enabled() && !tq::engineprobe::asyncLevelLoadForTest()
          && !tq::arccache::configured()
          && !tq::engine::install((HMODULE)image)
          && tq::engineprobe::installedForTest() == 0,
          "the removed async_level_load key is ignored");

    // Third, that it brings none of the instrument with it. engine_trace
    // defaults to 1, so every group would say yes if the mask were consulted
    // on its own; what makes them say no is the probe being off.
    check(!tq::engineprobe::wantsForTest(2) && !tq::engineprobe::wantsForTest(16)
          && !tq::engineprobe::wantsForTest(8192)
          && !tq::engineprobe::wantsForTest(16384)
          && !tq::engineprobe::wantsForTest(65536),
          "the removed async key installs no trace group");
    tq::engine::shutdown();
    tq::engine::readOptions(nullptr);
    tq::probe::readOptions(nullptr);
    check(!tq::engineprobe::asyncLevelLoadForTest(),
          "shutting down and re-reading no INI puts async_level_load back off");
    DeleteFileW(ini);

    // The rejected one-frame whole-map reuse key is inert too.
    check(!tq::engineprobe::shadowTransitionReuseForTest(),
          "shadow_transition_reuse is off with no INI at all");
    WritePrivateProfileStringW(L"performance", L"shadow_transition_reuse",
                               L"1", ini);
    tq::probe::readOptions(ini);
    tq::engine::readOptions(ini);
    check(!tq::probe::enabled()
          && !tq::engineprobe::shadowTransitionReuseForTest()
          && !tq::engine::install((HMODULE)image)
          && tq::engineprobe::installedForTest() == 0,
          "the removed shadow_transition_reuse key is ignored");
    check(!tq::engineprobe::wantsForTest(2)
          && !tq::engineprobe::wantsForTest(16384)
          && !tq::engineprobe::wantsForTest(65536),
          "a shadow-reuse-only boot installs no trace group");

    uint32_t oldMatrix[16], outputMatrix[16];
    for (unsigned i = 0; i < 16; ++i) oldMatrix[i] = 0x12340000u + i;
    memset(outputMatrix, 0, sizeof(outputMatrix));
    void* const oldRegion = (void*)0x1000;
    void* const newRegion = (void*)0x2000;
    void* const thirdRegion = (void*)0x3000;
    void* const surface = (void*)0x4000;
    tq::engineprobe::primeShadowReuseForTest(oldRegion, surface, oldMatrix);
    check(!tq::engineprobe::reuseShadowForTest(oldRegion, surface, outputMatrix),
          "shadow reuse does nothing while the region is unchanged");
    tq::engineprobe::primeShadowReuseForTest(oldRegion, surface, oldMatrix);
    check(!tq::engineprobe::reuseShadowForTest(newRegion, (void*)0x5000,
                                               outputMatrix),
          "shadow reuse refuses a different depth target");
    tq::engineprobe::primeShadowReuseForTest(oldRegion, surface, oldMatrix);
    check(!tq::engineprobe::reuseShadowForTest(newRegion, surface, outputMatrix)
          && memcmp(outputMatrix, oldMatrix, sizeof(oldMatrix)),
          "removed whole-map reuse cannot restore a stale shadow matrix");
    check(!tq::engineprobe::reuseShadowForTest(thirdRegion, surface,
                                               outputMatrix),
          "shadow transition reuse never skips two calls in a row");
    tq::engine::shutdown();
    tq::engine::readOptions(nullptr);
    tq::probe::readOptions(nullptr);
    check(!tq::engineprobe::shadowTransitionReuseForTest(),
          "re-reading no INI puts shadow_transition_reuse back off");
    DeleteFileW(ini);

    // The accepted cold-resource shadow mitigation defaults on, can be
    // explicitly disabled, reaches install() with the probe off, and cannot
    // enable a trace group by itself.
    check(tq::engineprobe::shadowDeferColdResourcesForTest(),
          "shadow_defer_cold_resources defaults on with no INI");
    WritePrivateProfileStringW(L"performance", L"shadow_defer_cold_resources",
                               L"0", ini);
    tq::engine::readOptions(ini);
    check(!tq::engineprobe::shadowDeferColdResourcesForTest(),
          "shadow_defer_cold_resources=0 disables the mitigation");
    WritePrivateProfileStringW(L"performance", L"shadow_defer_cold_resources",
                               L"1", ini);
    tq::probe::readOptions(ini);
    tq::engine::readOptions(ini);
    check(!tq::probe::enabled()
          && tq::engineprobe::shadowDeferColdResourcesForTest()
          && !tq::engine::install((HMODULE)image)
          && tq::engineprobe::installedForTest() == 0,
          "shadow_defer_cold_resources reaches install() with the probe off, and"
          " still refuses a module that is not Engine.dll");
    check(!tq::engineprobe::wantsForTest(2)
          && !tq::engineprobe::wantsForTest(16384)
          && !tq::engineprobe::wantsForTest(65536),
          "a cold-resource-shadow-only boot installs no trace group");
    check(tq::engineprobe::shouldDeferShadowAlphaForTest(3, 0)
          && tq::engineprobe::shouldDeferShadowAlphaForTest(4, 1)
          && tq::engineprobe::shouldDeferShadowAlphaForTest(5, 0)
          && !tq::engineprobe::shouldDeferShadowAlphaForTest(0, 0)
          && !tq::engineprobe::shouldDeferShadowAlphaForTest(2, 1)
          && !tq::engineprobe::shouldDeferShadowAlphaForTest(3, 2)
          && !tq::engineprobe::shouldDeferShadowAlphaForTest(6, 0),
          "only alpha-tested styles in resource states 0/1 are deferred");
    check(tq::engineprobe::shouldDeferShadowMeshForTest(0)
          && tq::engineprobe::shouldDeferShadowMeshForTest(1)
          && !tq::engineprobe::shouldDeferShadowMeshForTest(2)
          && !tq::engineprobe::shouldDeferShadowMeshForTest(3),
          "only root meshes in resource states 0/1 are deferred");
    check(!tq::engineprobe::shadowActorPoseQueueConfirmedForTest(0, false)
          && tq::engineprobe::shadowActorPoseQueueConfirmedForTest(0, true)
          && tq::engineprobe::shadowActorPoseQueueConfirmedForTest(1, false)
          && tq::engineprobe::shadowActorPoseQueueConfirmedForTest(1, true)
          && !tq::engineprobe::shadowActorPoseQueueConfirmedForTest(2, false)
          && !tq::engineprobe::shadowActorPoseQueueConfirmedForTest(2, true),
          "cold Actor pose omission requires a confirmed queue and never"
          " skips a resident root");
    tq::engine::shutdown();
    tq::engine::readOptions(nullptr);
    tq::probe::readOptions(nullptr);
    check(tq::engineprobe::shadowDeferColdResourcesForTest(),
          "re-reading no INI restores shadow_defer_cold_resources default");
    DeleteFileW(ini);

    // The eighth independent way in moves the same cold-root decision to the
    // exact Actor::AddToScene call proved by Run 68. It implies the later
    // caster gate, defaults on, and brings no trace group by itself.
    check(tq::engineprobe::shadowDeferColdActorPoseForTest(),
          "shadow_defer_cold_actor_pose defaults on with no INI");
    WritePrivateProfileStringW(L"performance",
                               L"shadow_defer_cold_actor_pose", L"0", ini);
    tq::engine::readOptions(ini);
    check(!tq::engineprobe::shadowDeferColdActorPoseForTest(),
          "shadow_defer_cold_actor_pose=0 disables the early gate");
    WritePrivateProfileStringW(L"performance",
                               L"shadow_defer_cold_actor_pose", L"1", ini);
    tq::probe::readOptions(ini);
    tq::engine::readOptions(ini);
    check(!tq::probe::enabled()
          && tq::engineprobe::shadowDeferColdActorPoseForTest()
          && !tq::engine::install((HMODULE)image)
          && tq::engineprobe::installedForTest() == 0,
          "shadow_defer_cold_actor_pose reaches install() with the probe off,"
          " and still refuses a module that is not Engine.dll");
    check(!tq::engineprobe::wantsForTest(2)
          && !tq::engineprobe::wantsForTest(16384)
          && !tq::engineprobe::wantsForTest(65536),
          "an actor-pose-defer-only boot installs no trace group");
    tq::engine::shutdown();
    tq::engine::readOptions(nullptr);
    tq::probe::readOptions(nullptr);
    check(tq::engineprobe::shadowDeferColdActorPoseForTest(),
          "re-reading no INI restores shadow_defer_cold_actor_pose default");
    DeleteFileW(ini);

    // The ninth independent way in queues runtime terrain-layer textures at
    // their exact post-LoadTextures boundary. It defaults on, is reachable
    // with the probe off, and cannot bring a trace group.
    check(tq::engineprobe::terrainPreloadLayersForTest(),
          "terrain_preload_layers defaults on with no INI");
    WritePrivateProfileStringW(L"performance", L"terrain_preload_layers",
                               L"0", ini);
    tq::engine::readOptions(ini);
    check(!tq::engineprobe::terrainPreloadLayersForTest(),
          "terrain_preload_layers=0 disables the preload");
    WritePrivateProfileStringW(L"performance", L"terrain_preload_layers",
                               L"1", ini);
    tq::probe::readOptions(ini);
    tq::engine::readOptions(ini);
    check(!tq::probe::enabled()
          && tq::engineprobe::terrainPreloadLayersForTest()
          && !tq::engine::install((HMODULE)image)
          && tq::engineprobe::installedForTest() == 0,
          "terrain_preload_layers reaches install() with the probe off, and"
          " still refuses a module that is not Engine.dll");
    check(!tq::engineprobe::wantsForTest(2)
          && !tq::engineprobe::wantsForTest(16384)
          && !tq::engineprobe::wantsForTest(32768)
          && !tq::engineprobe::wantsForTest(65536),
          "a terrain-layer-preload-only boot installs no trace group");
    tq::engine::shutdown();
    tq::engine::readOptions(nullptr);
    tq::probe::readOptions(nullptr);
    check(tq::engineprobe::terrainPreloadLayersForTest(),
          "re-reading no INI restores terrain_preload_layers default");
    DeleteFileW(ini);

    // The two rejected one-consumer reflection omission keys are ignored.
    check(!tq::engineprobe::reflectionDeferAdmissionMeshForTest(),
          "reflection_defer_admission_mesh is off with no INI at all");
    WritePrivateProfileStringW(L"performance",
                               L"reflection_defer_admission_mesh", L"1", ini);
    tq::probe::readOptions(ini);
    tq::engine::readOptions(ini);
    check(!tq::probe::enabled()
          && !tq::engineprobe::reflectionDeferAdmissionMeshForTest()
          && !tq::engine::install((HMODULE)image)
          && tq::engineprobe::installedForTest() == 0,
          "the removed reflection_defer_admission_mesh key is ignored");
    check(!tq::engineprobe::wantsForTest(2)
          && !tq::engineprobe::wantsForTest(32768)
          && !tq::engineprobe::wantsForTest(65536)
          && !tq::engineprobe::wantsForTest(131072),
          "a reflection-admission-only boot installs no trace group");
    check(!tq::engineprobe::reflectionAdmissionTriggeredForTest(31)
          && !tq::engineprobe::reflectionAdmissionTriggeredForTest(32),
          "removed reflection omission cannot arm on buffer count");
    tq::engine::shutdown();
    tq::engine::readOptions(nullptr);
    tq::probe::readOptions(nullptr);
    check(!tq::engineprobe::reflectionDeferAdmissionMeshForTest(),
          "re-reading no INI puts reflection_defer_admission_mesh back off");
    DeleteFileW(ini);

    check(!tq::engineprobe::reflectionDeferAdmissionAllForTest(),
          "reflection_defer_admission_all is off with no INI at all");
    WritePrivateProfileStringW(L"performance",
                               L"reflection_defer_admission_all", L"1", ini);
    tq::probe::readOptions(ini);
    tq::engine::readOptions(ini);
    check(!tq::probe::enabled()
          && !tq::engineprobe::reflectionDeferAdmissionAllForTest()
          && !tq::engineprobe::reflectionDeferAdmissionMeshForTest()
          && !tq::engine::install((HMODULE)image)
          && tq::engineprobe::installedForTest() == 0,
          "the removed reflection_defer_admission_all key is ignored");
    check(!tq::engineprobe::wantsForTest(2)
          && !tq::engineprobe::wantsForTest(32768)
          && !tq::engineprobe::wantsForTest(65536)
          && !tq::engineprobe::wantsForTest(131072),
          "a whole-reflection-admission-only boot installs no trace group");
    check(!tq::engineprobe::reflectionAdmissionTriggeredForTest(31)
          && !tq::engineprobe::reflectionAdmissionTriggeredForTest(32),
          "removed whole-reflection omission cannot arm on buffer count");
    tq::engine::shutdown();
    tq::engine::readOptions(nullptr);
    tq::probe::readOptions(nullptr);
    check(!tq::engineprobe::reflectionDeferAdmissionAllForTest(),
          "re-reading no INI puts reflection_defer_admission_all back off");
    DeleteFileW(ini);

    // The next independent behavior path keeps normal colour stock but
    // budgets first reflection/directional GPU participation as one object
    // population. It must work with the probe off and imply no trace group.
    check(tq::engineprobe::secondaryPassAdmissionBudgetForTest() == 8,
          "secondary_pass_admission_budget defaults to eight with no INI");
    WritePrivateProfileStringW(L"performance",
                               L"secondary_pass_admission_budget", L"0", ini);
    tq::engine::readOptions(ini);
    check(tq::engineprobe::secondaryPassAdmissionBudgetForTest() == 0,
          "secondary_pass_admission_budget=0 restores stock admission");
    WritePrivateProfileStringW(L"performance",
                               L"secondary_pass_admission_budget", L"8", ini);
    tq::probe::readOptions(ini);
    tq::engine::readOptions(ini);
    check(!tq::probe::enabled()
          && tq::engineprobe::secondaryPassAdmissionBudgetForTest() == 8
          && !tq::engine::install((HMODULE)image)
          && tq::engineprobe::installedForTest() == 0,
          "secondary_pass_admission_budget reaches install() with the probe"
          " off, and still refuses a module that is not Engine.dll");
    check(!tq::engineprobe::wantsForTest(2)
          && !tq::engineprobe::wantsForTest(16384)
          && !tq::engineprobe::wantsForTest(32768)
          && !tq::engineprobe::wantsForTest(65536)
          && !tq::engineprobe::wantsForTest(131072),
          "a secondary-pass-admission-only boot installs no trace group");
    check(!tq::engineprobe::reflectionAdmissionTriggeredForTest(31)
          && !tq::engineprobe::reflectionAdmissionTriggeredForTest(32)
          && !tq::engineprobe::reflectionAdmissionBufferTrackingRequested(),
          "secondary-pass admission does not use reflection buffers");
    tq::engine::shutdown();
    tq::engine::readOptions(nullptr);
    tq::probe::readOptions(nullptr);
    check(tq::engineprobe::secondaryPassAdmissionBudgetForTest() == 8,
          "re-reading no INI restores secondary admission budget eight");
    DeleteFileW(ini);

    // The next passive trace must distinguish an object's first visit to each
    // exact consumer, without mistaking a repeat call or another class for it.
    int admissionObject = 0;
    tq::engineprobe::resetAdmissionRenderableIdentitiesForTest();
    check(tq::engineprobe::admissionRenderableFirstForTest(
              &admissionObject, 1, 1)
          && !tq::engineprobe::admissionRenderableFirstForTest(
              &admissionObject, 1, 1)
          && tq::engineprobe::admissionRenderableFirstForTest(
              &admissionObject, 1, 4)
          && tq::engineprobe::admissionRenderableFirstForTest(
              &admissionObject, 2, 1),
          "admission identities are first once per renderable class and exact consumer");

    // The controlled population is its own signal: two new identities fit
    // this synthetic frame's budget without arming, while the third self-arms
    // and remains pending. Admission is global across reflection and
    // directional shadow, so an admitted object spends no second slot there.
    int secondaryObjects[4] = {};
    tq::engineprobe::resetSecondaryAdmissionForTest(2, false);
    check(!tq::engineprobe::secondaryAdmissionRenderableDeferredForTest(
              &secondaryObjects[0], 3, true, false)
          && !tq::engineprobe::secondaryAdmissionRenderableDeferredForTest(
              &secondaryObjects[0], 3, false, true)
          && !tq::engineprobe::secondaryAdmissionRenderableDeferredForTest(
              &secondaryObjects[1], 3, false, true)
          && !tq::engineprobe::secondaryAdmissionArmedForTest(),
          "one frame admits two shared secondary identities without arming");
    check(tq::engineprobe::secondaryAdmissionRenderableDeferredForTest(
              &secondaryObjects[2], 3, true, false)
          && tq::engineprobe::secondaryAdmissionArmedForTest()
          && tq::engineprobe::secondaryAdmissionRenderableDeferredForTest(
              &secondaryObjects[2], 3, false, true),
          "identity budget plus one self-arms and remains pending globally");
    tq::secondaryadmission::secondaryAdmissionFrameBoundary();
    check(!tq::engineprobe::secondaryAdmissionRenderableDeferredForTest(
              &secondaryObjects[2], 3, true, false),
          "a pending secondary identity can enter on the next frame's budget");

    // The slow-LoadLevel caller table. Five calls a session decide where
    // Stage 5.1 should point, so a slot bug costs a boot rather than a build;
    // this drives the aggregator directly.
    {
        BYTE* const base = (BYTE*)0x10000000;
        unsigned long rva = 0;
        long calls = 0, mainCalls = 0, us = 0, worst = 0;
        tq::engineprobe::slowLoadResetForTest(base);
        check(!tq::engineprobe::slowLoadSlotForTest(0, &rva, &calls, &mainCalls,
                                                    &us, &worst)
              && tq::engineprobe::slowLoadLostForTest() == 0,
              "the slow-load table starts empty");

        tq::engineprobe::slowLoadRecordForTest(base + 0x20e7bc, 100000, true);
        tq::engineprobe::slowLoadRecordForTest(base + 0x20e7bc, 60000, false);
        tq::engineprobe::slowLoadRecordForTest(base + 0x20aebc, 5000, true);
        tq::engineprobe::slowLoadRecordForTest(base + 0x20e7bc, 80000, true);
        check(tq::engineprobe::slowLoadSlotForTest(0, &rva, &calls, &mainCalls,
                                                   &us, &worst)
              && rva == 0x20e7bc && calls == 3 && mainCalls == 2
              && us == 240000 && worst == 100000,
              "repeat callers share a slot and accumulate calls, main, total"
              " and worst");
        check(tq::engineprobe::slowLoadSlotForTest(1, &rva, &calls, &mainCalls,
                                                   &us, &worst)
              && rva == 0x20aebc && calls == 1 && mainCalls == 1
              && us == 5000 && worst == 5000,
              "a second caller takes the next slot");

        // The module bound is what keeps an RVA meaningful. A caller below the
        // base or past SizeOfImage is a different module's, and recording it
        // against Engine.dll would name an address that means nothing.
        tq::engineprobe::slowLoadRecordForTest(base - 0x1000, 9000, true);
        tq::engineprobe::slowLoadRecordForTest(base + 0x44b000, 9000, true);
        check(tq::engineprobe::slowLoadLostForTest() == 2
              && !tq::engineprobe::slowLoadSlotForTest(2, &rva, &calls,
                                                       &mainCalls, &us, &worst),
              "a caller outside Engine.dll is counted as lost, not recorded");

        // Sixteen slots, and the seventeenth distinct caller is dropped rather
        // than overwriting one that has already been recorded.
        for (unsigned i = 0; i < 20; ++i)
            tq::engineprobe::slowLoadRecordForTest(base + 0x100000 + i * 0x10,
                                                   2000, false);
        check(tq::engineprobe::slowLoadSlotForTest(15, &rva, &calls, &mainCalls,
                                                   &us, &worst)
              && tq::engineprobe::slowLoadLostForTest() > 2,
              "the table fills to sixteen and drops the rest rather than"
              " overwriting");
        tq::engineprobe::slowLoadResetForTest(nullptr);
        check(!tq::engineprobe::slowLoadSlotForTest(0, &rva, &calls, &mainCalls,
                                                    &us, &worst),
              "and it resets");
    }

    // The stack scan. Without frame pointers the only thing separating a
    // return address from a stale dword is "the bytes before it are a call",
    // so that filter is the whole instrument -- and a false negative on a
    // virtual call would drop exactly the frames the render path is made of.
    {
        // A synthetic .text: every candidate is an offset into this buffer, so
        // the filter can be driven over all five encodings and their misses
        // without needing a real module.
        const SIZE_T textSize = 0x400;
        BYTE* text = (BYTE*)VirtualAlloc(nullptr, textSize, MEM_COMMIT,
                                         PAGE_READWRITE);
        if (!text) {
            check(false, "allocate a synthetic .text for the stack scan");
        } else {
            memset(text, 0xcc, textSize);
            tq::engineprobe::slowLoadResetForTest(text);
            tq::engineprobe::chainTextForTest(text, textSize, 'E');

            // E8 rel32 at 0x100, calling 0x200: return address is 0x105.
            text[0x100] = 0xe8;
            *(int32_t*)(text + 0x101) = 0x200 - 0x105;
            check(tq::engineprobe::precededByCallForTest(text + 0x105),
                  "E8 rel32 whose destination is in .text is a call");

            // Same shape, but the displacement leaves the section. That is the
            // half of the E8 test that rejects a coincidence.
            text[0x120] = 0xe8;
            *(int32_t*)(text + 0x121) = 0x40000000;
            check(!tq::engineprobe::precededByCallForTest(text + 0x125),
                  "E8 whose destination is outside .text is not");

            // The four indirect forms, which are how the render path calls
            // anything virtual.
            text[0x140] = 0xff; text[0x141] = 0x15;      // call [disp32]
            check(tq::engineprobe::precededByCallForTest(text + 0x146),
                  "FF 15 disp32 is a call");
            text[0x160] = 0xff; text[0x161] = 0x90;      // call [eax+disp32]
            check(tq::engineprobe::precededByCallForTest(text + 0x166),
                  "FF 90 disp32 is a call");
            text[0x180] = 0xff; text[0x181] = 0x50;      // call [eax+disp8]
            check(tq::engineprobe::precededByCallForTest(text + 0x183),
                  "FF 50 disp8 is a call");
            text[0x1a0] = 0xff; text[0x1a1] = 0xd0;      // call eax
            check(tq::engineprobe::precededByCallForTest(text + 0x1a2),
                  "FF D0 (call reg) is a call");

            check(!tq::engineprobe::precededByCallForTest(text + 0x300),
                  "a run of int3 is not a call");
            check(!tq::engineprobe::precededByCallForTest(text + 2),
                  "and a candidate too close to the start of .text is refused"
                  " rather than read behind");

            // A synthetic stack: two real return addresses with junk around
            // them, one of the junk values pointing into .text but not after
            // a call. The scan must keep the two and drop the third.
            uintptr_t frame[8];
            frame[0] = (uintptr_t)(text + 0x105);   // a call return address
            frame[1] = 0xdeadbeef;                  // not in .text
            frame[2] = (uintptr_t)(text + 0x300);   // in .text, not after a call
            frame[3] = (uintptr_t)(text + 0x105);   // repeat, collapsed
            frame[4] = (uintptr_t)(text + 0x146);   // a second call site
            frame[5] = 0;
            frame[6] = 0;
            frame[7] = 0;
            unsigned depth = 0;
            char tags[32] = {};
            const unsigned first =
                tq::engineprobe::captureChainForTest(frame, &depth, tags);
            check(first == 0x105 && depth == 2 && tags[0] == 'E'
                  && tags[1] == 'E',
                  "the scan keeps call-preceded addresses in stack order,"
                  " drops the rest, and collapses repeats");

            // The second module, which is the whole point of run 32: the
            // chain leaves Engine.dll, and a frame in Game.dll or TQ.exe has
            // to be kept and labelled rather than dropped. Its RVAs are
            // against its own base, so the same offset in two modules is two
            // different frames.
            BYTE* other = (BYTE*)VirtualAlloc(nullptr, textSize, MEM_COMMIT,
                                              PAGE_READWRITE);
            if (!other) {
                check(false, "allocate a second synthetic module");
            } else {
                memset(other, 0xcc, textSize);
                tq::engineprobe::chainTextForTest(other, textSize, 'T');
                other[0x100] = 0xe8;
                *(int32_t*)(other + 0x101) = 0x200 - 0x105;
                check(tq::engineprobe::precededByCallForTest(other + 0x105),
                      "a call site in the second module is recognised there");
                // An E8 in one module whose displacement lands in the other is
                // not a call within either -- the destination check is
                // per-module, which is what keeps it strong.
                text[0x1c0] = 0xe8;
                *(int32_t*)(text + 0x1c1) =
                    (int32_t)((other + 0x200) - (text + 0x1c5));
                check(!tq::engineprobe::precededByCallForTest(text + 0x1c5),
                      "an E8 pointing into a different module is not accepted");

                uintptr_t mixed[4];
                mixed[0] = (uintptr_t)(text + 0x105);
                mixed[1] = (uintptr_t)(other + 0x105);
                mixed[2] = 0;
                mixed[3] = 0;
                unsigned mixedDepth = 0;
                char mixedTags[32] = {};
                const unsigned head = tq::engineprobe::captureChainForTest(
                    mixed, &mixedDepth, mixedTags);
                // Depth is >= rather than ==, and that is the instrument
                // being honest rather than the test being loose: the scan
                // walks 8 KiB upward, so it also finds the previous check's
                // array further up this same stack. That is exactly the
                // superset behaviour the design accepts and the reason the
                // log is read as ranked candidates, not as a backtrace.
                check(head == 0x105 && mixedDepth >= 2 && mixedTags[0] == 'E'
                      && mixedTags[1] == 'T',
                      "a chain that crosses modules keeps both frames and"
                      " labels each with the module it came from");
                VirtualFree(other, 0, MEM_RELEASE);
            }

            tq::engineprobe::chainTextForTest(nullptr, 0, 0);
            tq::engineprobe::slowLoadResetForTest(nullptr);
            VirtualFree(text, 0, MEM_RELEASE);
        }
    }
    // ---- patchImport, which is how the game's own main loop is instrumented.
    // It writes four bytes into an import table rather than into anybody's
    // code, and it is scoped to one module -- so the test that matters is
    // that it finds the right slot, refuses a slot holding something else,
    // and leaves the real function reachable everywhere else.
    {
        // winmm.dll is loaded by the harness and imports from kernel32, so
        // there is a real import table to walk without inventing one.
        HMODULE self = GetModuleHandleW(nullptr);
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        void* realSleep = kernel ? (void*)GetProcAddress(kernel, "Sleep")
                                 : nullptr;
        tq::detour::CallPatch importPatch = {};
        check(realSleep && !tq::detour::patchImport(
                  importPatch, self, "kernel32.dll", "Sleep",
                  (const void*)&detourCallReplacement,
                  (const void*)&detourCallReplacement)
              && !importPatch.installed,
              "patchImport refuses an import slot holding something else");
        check(!tq::detour::patchImport(importPatch, self, "kernel32.dll",
                                       "NoSuchExportedFunction", realSleep,
                                       (const void*)&detourCallReplacement),
              "patchImport refuses a name the module does not import");
        // The real redirect, and back again. Sleep(0) is a yield, so calling
        // through the patched slot is safe and observable.
        if (realSleep
            && tq::detour::patchImport(importPatch, self, "kernel32.dll",
                                       "Sleep", realSleep,
                                       (const void*)&sleepCountingReplacement)) {
            check(importPatch.installed
                  && *(void**)importPatch.operand
                         == (void*)&sleepCountingReplacement,
                  "patchImport redirects the import slot it was given");
            g_sleepCalls = 0;
            Sleep(0);
            check(g_sleepCalls == 1,
                  "the module's own call reaches the replacement");
            check((void*)GetProcAddress(kernel, "Sleep") == realSleep,
                  "the exporting module still hands out the real function");
            tq::detour::restoreCall(importPatch);
            g_sleepCalls = 0;
            Sleep(0);
            check(g_sleepCalls == 0,
                  "restoreCall puts the import slot back");
        } else {
            check(false, "redirect the harness's own Sleep import");
        }
    }

    VirtualFree(image, 0, MEM_RELEASE);
}

// The captured first grass plane, verbatim from the bound stream during a
// live draw: position, normal, uv per vertex, wound top-left, top-right,
// bottom-right, bottom-left. Using the real bytes means the fingerprint is
// tested against what the game actually writes rather than against an idea of
// it.
const uint32_t kCapturedPlane[32] = {
    0x43139465, 0x41c0d49a, 0x41fa311f, 0xbe3b9187, 0x3f7b44d5, 0x3d62e2d8,
        0x00000000, 0x00000000,
    0x431489d5, 0x41c0d49a, 0x42000629, 0xbe3b9187, 0x3f7b44d5, 0x3d62e2d8,
        0x3f000000, 0x00000000,
    0x431489d5, 0x41b7315d, 0x42000629, 0xbe3b9187, 0x3f7b44d5, 0x3d62e2d8,
        0x3f000000, 0x3f800000,
    0x43139465, 0x41b7315d, 0x41fa311f, 0xbe3b9187, 0x3f7b44d5, 0x3d62e2d8,
        0x00000000, 0x3f800000
};

void loadPlane(float plane[32]) {
    memcpy(plane, kCapturedPlane, sizeof(kCapturedPlane));
}

float kCapturedPlaneFloat(unsigned index) {
    float value;
    memcpy(&value, kCapturedPlane + index, sizeof(value));
    return value;
}

// COM lifetime model: objects remain inspectable after their last Release so
// the test can verify exactly when a native allocator would recycle them.
struct GrassTestBuffer : ID3D11Buffer {
    ULONG refs = 1;
    unsigned descCalls = 0;
    D3D11_BUFFER_DESC desc = {};
    GrassTestBuffer() {
        desc.ByteWidth = 44800;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** out) override { *out = nullptr; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* out) override { *out = D3D11_RESOURCE_DIMENSION_BUFFER; }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
    void STDMETHODCALLTYPE GetDesc(D3D11_BUFFER_DESC* out) override { ++descCalls; *out = desc; }
};

// Only the slots used by grass are supplied. Unexpected driver calls fail the
// test instead of silently returning a plausible result.
struct GrassTestDevice {
    void** vtable;
    void* slots[4] = {};
    GrassTestBuffer staging, twin;
    GrassTestDevice() : vtable(slots) {
        staging.refs = twin.refs = 0;
        slots[2] = (void*)&release;
        slots[3] = (void*)&create;
    }
    static ULONG WINAPI release(ID3D11Device*) { return 1; }
    static HRESULT WINAPI create(ID3D11Device* self, const D3D11_BUFFER_DESC* desc,
                                  const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer** out) {
        auto* device = (GrassTestDevice*)self;
        GrassTestBuffer* buffer = desc->Usage == D3D11_USAGE_STAGING
            ? &device->staging : &device->twin;
        buffer->desc = *desc;
        buffer->AddRef();
        *out = buffer;
        return S_OK;
    }
};

struct GrassTestContext {
    void** vtable;
    void* slots[48] = {};
    GrassTestDevice device;
    unsigned maps = 0, copies = 0;
    UINT readFlags = 0;
    HRESULT readResult = S_OK;
    BYTE data[44800] = {};
    GrassTestContext() : vtable(slots) {
        slots[3] = (void*)&getDevice;
        slots[14] = (void*)&map;
        slots[15] = (void*)&unmap;
        slots[47] = (void*)&copy;
        memcpy(data, kCapturedPlane, sizeof(kCapturedPlane));
    }
    ID3D11DeviceContext* get() { return (ID3D11DeviceContext*)this; }
    static void WINAPI getDevice(ID3D11DeviceContext* self, ID3D11Device** out) {
        *out = (ID3D11Device*)&((GrassTestContext*)self)->device;
    }
    static HRESULT WINAPI map(ID3D11DeviceContext* self, ID3D11Resource*, UINT,
                               D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* out) {
        auto* context = (GrassTestContext*)self;
        ++context->maps;
        if (type == D3D11_MAP_READ) {
            context->readFlags = flags;
            if (FAILED(context->readResult)) return context->readResult;
        }
        out->pData = context->data;
        return S_OK;
    }
    static void WINAPI unmap(ID3D11DeviceContext*, ID3D11Resource*, UINT) {}
    static void WINAPI copy(ID3D11DeviceContext* self, ID3D11Resource*, ID3D11Resource*) {
        ++((GrassTestContext*)self)->copies;
    }
};

void testGrassBufferLifetime() {
    tq::grass::installBuffers();
    GrassTestBuffer source;
    tq::grass::noteBufferCreated(&source, &source.desc);
    tq::grass::noteBufferCreated(&source, &source.desc);
    check(source.refs == 2, "grass candidate owns one reference, including duplicate notifications");

    BYTE* memory = (BYTE*)VirtualAlloc(nullptr, 49152, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(memory != nullptr, "allocate protected grass mapped-memory fixture");
    if (!memory) { tq::grass::shutdown(); return; }
    memcpy(memory, kCapturedPlane, sizeof(kCapturedPlane));
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    mapped.pData = memory;
    tq::grass::noteMap(&source, 0, &mapped);
    tq::grass::noteUnmap(&source, 0);
    GrassTestContext context;
    tq::grass::afterUnmap(context.get());
    check(source.refs == 2 && tq::grass::crossedBuffer(&source) == &context.device.twin,
          "grass promotion transfers ownership and produces a usable crossing");
    const unsigned descriptorCalls = source.descCalls;
    tq::grass::noteMap(&source, 0, &mapped);
    tq::grass::noteUnmap(&source, 0);
    tq::grass::afterUnmap(context.get());
    check(source.descCalls == descriptorCalls, "tracked grass Map/Unmap adds no descriptor queries");
    DWORD previous = 0;
    const bool protectedMemory = VirtualProtect(memory, 49152, PAGE_NOACCESS, &previous) != 0;
    check(protectedMemory, "revoke access to unmapped grass memory");
    tq::grass::noteUnmap(&source, 0);
    check(true, "a repeated Unmap cannot read a stale candidate mapping");
    check(source.Release() == 1, "a released game buffer remains alive while grass tracks its address");
    tq::grass::shutdown();
    check(source.refs == 0 && context.device.twin.refs == 0,
          "grass shutdown releases both source and twin ownership");

    // The same address is now eligible for reuse by a tiny unrelated buffer.
    source.refs = 1;
    source.desc.ByteWidth = 16;
    tq::grass::installBuffers();
    tq::grass::noteBufferCreated(&source, &source.desc);
    tq::grass::noteMap(&source, 0, &mapped);
    tq::grass::noteUnmap(&source, 0);
    check(source.refs == 1, "a recycled small buffer never reads protected mapped memory");
    tq::grass::shutdown();

    tq::grass::installBuffers();
    GrassTestBuffer candidates[600];
    for (auto& buffer : candidates) {
        tq::grass::noteBufferCreated(&buffer, &buffer.desc);
        buffer.Release();
    }
    unsigned retained = 0;
    bool balanced = true;
    for (auto& buffer : candidates) {
        retained += buffer.refs;
        balanced &= buffer.refs <= 1;
    }
    check(balanced && retained <= 256 && candidates[0].refs == 0,
          "candidate churn releases evictions and keeps ownership bounded");
    tq::grass::shutdown();
    for (auto& buffer : candidates) balanced &= buffer.refs == 0;
    check(balanced, "candidate shutdown leaves no retained references");

    tq::grass::installBuffers();
    GrassTestBuffer seeded, replacements[700];
    GrassTestContext seedContext;
    tq::grass::crossedBuffer(&seeded);
    tq::grass::seedFromDraw(seedContext.get(), &seeded);
    seeded.Release();
    tq::grass::onPresent(seedContext.get());
    for (auto& buffer : replacements) tq::grass::crossedBuffer(&buffer);
    seedContext.readResult = DXGI_ERROR_WAS_STILL_DRAWING;
    tq::grass::onPresent(seedContext.get());
    for (auto& buffer : replacements) tq::grass::crossedBuffer(&buffer);
    check(seeded.refs == 1 && seedContext.copies == 1 && seedContext.maps == 1,
          "cache pressure preserves an in-flight seed across frames without waiting");
    seedContext.readResult = S_OK;
    tq::grass::onPresent(seedContext.get());
    check(seedContext.maps == 3 && seedContext.device.twin.refs == 1,
          "a protected seed completes despite a population exceeding the cache cap");
    // Once the readback finishes, a source absent for multiple frames is cold.
    tq::grass::onPresent(seedContext.get());
    for (auto& buffer : replacements) tq::grass::crossedBuffer(&buffer);
    check(seeded.refs == 0 && seedContext.device.twin.refs == 0,
          "cold completed streams release both source and twin on eviction");
    seeded.refs = 1;
    seeded.desc.ByteWidth = 16;
    tq::grass::noteBufferCreated(&seeded, &seeded.desc);
    tq::grass::noteMap(&seeded, 0, &mapped);
    tq::grass::noteUnmap(&seeded, 0);
    check(seeded.refs == 1, "address reuse after eviction is safe while the grass cache remains active");
    tq::grass::shutdown();
    balanced = seedContext.device.staging.refs == 0;
    for (auto& buffer : replacements) balanced &= buffer.refs == 1;
    check(balanced, "stream churn and shutdown release every cache reference");

    tq::grass::installBuffers();
    GrassTestBuffer refilled;
    GrassTestContext refillContext;
    tq::grass::crossedBuffer(&refilled);
    tq::grass::seedFromDraw(refillContext.get(), &refilled);
    BYTE empty[44800] = {};
    mapped.pData = empty;
    tq::grass::noteMap(&refilled, 0, &mapped);
    tq::grass::noteUnmap(&refilled, 0);
    tq::grass::afterUnmap(refillContext.get());
    tq::grass::onPresent(refillContext.get());
    tq::grass::onPresent(refillContext.get());
    check(refillContext.maps == 0, "a refill cancels an older queued seed even when no cards remain");
    tq::grass::shutdown();

    tq::grass::installBuffers();
    GrassTestBuffer normalSeed;
    GrassTestContext normalContext;
    tq::grass::crossedBuffer(&normalSeed);
    tq::grass::seedFromDraw(normalContext.get(), &normalSeed);
    tq::grass::onPresent(normalContext.get());
    check(normalContext.maps == 0, "grass seeding still waits for a Present before attempting readback");
    normalContext.readResult = DXGI_ERROR_WAS_STILL_DRAWING;
    tq::grass::onPresent(normalContext.get());
    check(normalContext.maps == 1 && normalContext.readFlags == D3D11_MAP_FLAG_DO_NOT_WAIT,
          "busy grass readback retries without a GPU wait");
    normalContext.readResult = S_OK;
    tq::grass::onPresent(normalContext.get());
    check(normalContext.maps == 3 && tq::grass::crossedBuffer(&normalSeed) == &normalContext.device.twin,
          "completed grass readback still publishes a crossing");
    tq::grass::shutdown();
    check(normalSeed.refs == 1 && normalContext.device.twin.refs == 0
          && normalContext.device.staging.refs == 0,
          "completed seeding releases all owned resources at shutdown");
    VirtualFree(memory, 0, MEM_RELEASE);
}

void testGrassCachePressure() {
    tq::grass::installBuffers();
    GrassTestBuffer sources[700];
    GrassTestContext context;
    BYTE cards[44800] = {};
    memcpy(cards, kCapturedPlane, sizeof(kCapturedPlane));
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    mapped.pData = cards;
    // Reproduce the >330-buffer live scene using real captured card bytes.
    for (unsigned i = 0; i < 400; ++i) {
        tq::grass::noteBufferCreated(&sources[i], &sources[i].desc);
        tq::grass::noteMap(&sources[i], 0, &mapped);
        tq::grass::noteUnmap(&sources[i], 0);
        tq::grass::afterUnmap(context.get());
    }
    unsigned descriptors = 0;
    for (auto& source : sources) descriptors += source.descCalls;
    bool stable = true;
    for (unsigned frame = 0; frame < 8; ++frame) {
        tq::grass::onPresent(context.get());
        for (unsigned i = 0; i < 400; ++i) {
            const unsigned at = (i + frame * 31) % 400;
            stable &= tq::grass::crossedBuffer(&sources[at]) == &context.device.twin;
        }
    }
    unsigned after = 0;
    for (auto& source : sources) after += source.descCalls;
    check(stable && after == descriptors,
          "400 visible grass streams retain every crossing across reordered frames without descriptor queries");

    // A draw population larger than the new cap must degrade to stable stock
    // blocks, not repeatedly destroy crossings visited earlier in the frame.
    for (unsigned i = 400; i < 700; ++i) tq::grass::crossedBuffer(&sources[i]);
    unsigned held = 0;
    for (auto& source : sources) held += source.refs == 2;
    check(held == 512, "grass source ownership stays bounded at 512 under overload");
    descriptors = 0;
    for (auto& source : sources) descriptors += source.descCalls;
    for (unsigned pass = 0; pass < 4; ++pass)
        for (unsigned i = 512; i < 700; ++i) tq::grass::crossedBuffer(&sources[i]);
    after = 0;
    for (auto& source : sources) after += source.descCalls;
    check(after == descriptors, "a saturated grass cache stops repeated admission queries within the frame");
    // Visit overflow first next frame, before any retained stream is touched.
    tq::grass::onPresent(context.get());
    for (unsigned i = 512; i < 700; ++i) tq::grass::crossedBuffer(&sources[i]);
    stable = true;
    for (unsigned i = 0; i < 400; ++i)
        stable &= tq::grass::crossedBuffer(&sources[i]) == &context.device.twin;
    check(stable, "previous-frame protection prevents early overflow draws evicting visible crossings");

    // Stop using the first scene. Two boundaries allow cold entries to leave.
    tq::grass::onPresent(context.get());
    tq::grass::onPresent(context.get());
    for (unsigned i = 512; i < 700; ++i) tq::grass::crossedBuffer(&sources[i]);
    unsigned admitted = 0;
    held = 0;
    for (unsigned i = 0; i < 700; ++i) {
        held += sources[i].refs == 2;
        if (i >= 512) admitted += sources[i].refs == 2;
    }
    check(admitted == 188 && held == 512,
          "new scenery replaces cold streams once recent-frame protection expires");
    tq::grass::shutdown();
    bool balanced = context.device.twin.refs == 0;
    for (auto& source : sources) balanced &= source.refs == 1;
    check(balanced, "cache pressure and shutdown release all retained sources and twins");
}

void testGrassCrossed() {
    float plane[32];
    loadPlane(plane);

    // The fingerprint, against bytes captured from the running game rather
    // than against an idea of what a card looks like. Everything else here
    // depends on this recognising a real card and nothing else.
    check(tq::grass::isGrassPlane(plane),
          "recognise a captured grass card by its exact shape");
    check(!memcmp(plane + 3, plane + 11, 3 * sizeof(float))
          && !memcmp(plane + 3, plane + 19, 3 * sizeof(float))
          && !memcmp(plane + 3, plane + 27, 3 * sizeof(float)),
          "confirm the captured card shares one normal across four vertices");

    const float* v0 = plane;
    const float* v1 = plane + 8;
    const float* v2 = plane + 16;
    const float* v3 = plane + 24;

    const double cx = ((double)v0[0] + v1[0]) * 0.5;
    const double cz = ((double)v0[2] + v1[2]) * 0.5;
    const double wx = (double)v1[0] - v0[0];
    const double wz = (double)v1[2] - v0[2];
    const double width = sqrt(wx * wx + wz * wz);
    const double top = v0[1];
    const double bottom = v2[1];

    check(tq::grass::rotatePlane(plane), "turn a captured card a quarter turn");

    // A crossing card is only a crossing card if it still stands where the
    // original stood: same centre, same size, same height.
    const double cx2 = ((double)v0[0] + v1[0]) * 0.5;
    const double cz2 = ((double)v0[2] + v1[2]) * 0.5;
    check(fabs(cx2 - cx) < 1e-4 && fabs(cz2 - cz) < 1e-4,
          "keep the turned card on the original card's centre");

    const double wx2 = (double)v1[0] - v0[0];
    const double wz2 = (double)v1[2] - v0[2];
    const double width2 = sqrt(wx2 * wx2 + wz2 * wz2);
    check(fabs(width2 - width) < 1e-3, "keep the turned card's width");
    check(v0[1] == top && v1[1] == top && v2[1] == bottom && v3[1] == bottom,
          "keep the turned card's height untouched");

    // Perpendicular is the whole point: a card turned by anything less would
    // still vanish edge-on at the same angle the original does.
    const double along = (wx * wx2 + wz * wz2) / (width * width2);
    check(fabs(along) < 1e-4, "turn the card perpendicular to the original");

    check(tq::grass::isGrassPlane(plane),
          "leave the turned card recognisable as a grass card");

    // Untouched apart from position: the copy shares the original's texture
    // column and its normal, so only the geometry differs.
    check(!memcmp(plane + 3, kCapturedPlane + 3, 3 * sizeof(float))
          && !memcmp(plane + 6, kCapturedPlane + 6, 2 * sizeof(float))
          && !memcmp(plane + 30, kCapturedPlane + 30, 2 * sizeof(float)),
          "leave the turned card's normal and uv exactly as they were");

    // Two quarter turns are a half turn, which is the same card seen from the
    // other side -- so the operation cannot drift the geometry.
    check(tq::grass::rotatePlane(plane), "turn a card that has already turned");
    check(fabs((double)v0[0] - kCapturedPlaneFloat(8)) < 1e-3
          && fabs((double)v1[0] - kCapturedPlaneFloat(0)) < 1e-3,
          "return a twice-turned card to the original corners, swapped");

    float zero[32];
    memset(zero, 0, sizeof(zero));
    check(!tq::grass::rotatePlane(zero), "leave an unwritten buffer slot unturned");
}

// Counted but not printed: a batch check would otherwise report sixty-four
// identical lines.
void check_quiet(bool passed) {
    if (!passed) ++g_failures;
}

void testGrassPointerIndex() {
    static tq::grass::PointerIndex index;
    tq::grass::indexReset(index);

    void* a = (void*)0x10004000;
    void* b = (void*)0x10008000;
    unsigned value = 0;
    check(!tq::grass::indexLookup(index, a, &value), "miss on an empty index");
    check(tq::grass::indexInsert(index, a, 7)
          && tq::grass::indexLookup(index, a, &value) && value == 7,
          "find a key that was inserted");
    check(!tq::grass::indexLookup(index, b, &value), "miss a key never inserted");
    check(tq::grass::indexInsert(index, a, 9)
          && tq::grass::indexLookup(index, a, &value) && value == 9,
          "replace the value of an existing key");
    check(tq::grass::indexRemove(index, a) && !tq::grass::indexLookup(index, a, &value),
          "miss a key after it is removed");

    // Removal leaves a tombstone precisely so a key that probed past the
    // removed one is still reachable. Buffers are recycled constantly, so this
    // is the ordinary case, not an edge case.
    tq::grass::indexReset(index);
    void* keys[64];
    for (unsigned i = 0; i < 64; ++i) {
        keys[i] = (void*)(uintptr_t)(0x20000000 + i * 0x100);
        check_quiet(tq::grass::indexInsert(index, keys[i], i));
    }
    bool all = true;
    for (unsigned i = 0; i < 64; ++i)
        all = all && tq::grass::indexLookup(index, keys[i], &value) && value == i;
    check(all, "find all of a batch of realistic buffer addresses");

    for (unsigned i = 0; i < 64; i += 2) tq::grass::indexRemove(index, keys[i]);
    bool survivors = true;
    for (unsigned i = 1; i < 64; i += 2)
        survivors = survivors && tq::grass::indexLookup(index, keys[i], &value)
                 && value == i;
    check(survivors, "keep the rest reachable after removing every other key");
    bool gone = true;
    for (unsigned i = 0; i < 64; i += 2)
        gone = gone && !tq::grass::indexLookup(index, keys[i], &value);
    check(gone, "leave no removed key findable");

    // A tombstone has to be reusable or a long session of streaming blocks
    // would fill the table with them and stop tracking anything.
    check(tq::grass::indexInsert(index, keys[0], 111)
          && tq::grass::indexLookup(index, keys[0], &value) && value == 111,
          "reuse a tombstoned slot for a later key");

    check(!tq::grass::indexInsert(index, nullptr, 1)
          && !tq::grass::indexLookup(index, nullptr, &value),
          "refuse a null key rather than storing one");

    // Insertion is allowed to fail, and the caller treats that as untracked.
    // What it must never do is report success without storing the key.
    tq::grass::indexReset(index);
    unsigned stored = 0;
    for (unsigned i = 0; i < 4096; ++i) {
        void* key = (void*)(uintptr_t)(0x30000000 + i * 0x40);
        if (tq::grass::indexInsert(index, key, i)) ++stored;
    }
    bool honest = true;
    unsigned found = 0;
    for (unsigned i = 0; i < 4096; ++i) {
        void* key = (void*)(uintptr_t)(0x30000000 + i * 0x40);
        if (tq::grass::indexLookup(index, key, &value)) {
            ++found;
            honest = honest && value == i;
        }
    }
    check(found == stored && honest,
          "store exactly the keys whose insertion was reported successful");
}

// Crossing and bending are independent settings that can both be on, so the
// order they compose in matters: a bent card must still be turnable, and a
// turned card must still be bendable.
void testBloomExtraction() {
    bool monotonic = true;
    float previous = -1.0f;
    for (unsigned i = 0; i <= 2048; ++i) {
        float input = i * (8.0f / 2048.0f);
        float output = tq::bloomhook::extractBrightness(input, 1.0f, 0.25f);
        monotonic &= output == output && output >= 0.0f
                  && output + 0.00001f >= previous;
        previous = output;
    }
    float dark = tq::bloomhook::extractBrightness(0.5f, 1.0f, 0.25f);
    float shoulder = tq::bloomhook::extractBrightness(0.9f, 1.0f, 0.25f);
    float white = tq::bloomhook::extractBrightness(1.0f, 1.0f, 0.25f);
    float highlight = tq::bloomhook::extractBrightness(2.0f, 1.0f, 0.25f);
    check(monotonic && dark == 0.0f && shoulder > 0.0f && shoulder < 0.1f
          && white > shoulder && white < 0.1f
          && highlight > 0.999f && highlight < 1.001f,
          "global HDR bloom extraction is soft, monotonic, and unclipped");
}

bool overlayCompile(const char* source, const char* target, ID3DBlob** result) {
    static HMODULE compiler;
    if (!compiler) compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!compiler) compiler = LoadLibraryW(L"d3dcompiler_43.dll");
    typedef HRESULT(WINAPI* CompileFn)(
        LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
        LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    CompileFn compile = compiler
        ? (CompileFn)(void*)GetProcAddress(compiler, "D3DCompile") : nullptr;
    if (!compile) return false;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile(source, strlen(source), "overlay-selftest", nullptr,
                         nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3,
                         0, result, &errors);
    if (errors) errors->Release();
    return SUCCEEDED(hr) && result && *result;
}

HRESULT WINAPI overlayCreateTexture2D(ID3D11Device* device,
                                      const D3D11_TEXTURE2D_DESC* desc,
                                      const D3D11_SUBRESOURCE_DATA* initial,
                                      ID3D11Texture2D** out) {
    return device->CreateTexture2D(desc, initial, out);
}

HRESULT WINAPI overlayCreateShaderResourceView(
    ID3D11Device* device, ID3D11Resource* resource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, ID3D11ShaderResourceView** out) {
    return device->CreateShaderResourceView(resource, desc, out);
}

HRESULT WINAPI overlayCreateSamplerState(ID3D11Device* device,
                                         const D3D11_SAMPLER_DESC* desc,
                                         ID3D11SamplerState** out) {
    return device->CreateSamplerState(desc, out);
}

HRESULT WINAPI overlayCreatePixelShader(ID3D11Device* device, const void* bytes,
                                        SIZE_T size, ID3D11ClassLinkage* linkage,
                                        ID3D11PixelShader** out) {
    return device->CreatePixelShader(bytes, size, linkage, out);
}

// The overlay is a debug instrument, so what matters most is that a shipped
// configuration never reaches it: with the setting absent or 0 it must not
// measure, allocate, build, or draw. The rest of the test proves it still
// works when it is asked for.
// Reads a whole small file, so the probe's CSV can be asserted on rather than
// merely assumed to exist.
char* readTextFile(const wchar_t* path, long* size) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    DWORD bytes = GetFileSize(file, nullptr);
    char* text = (char*)calloc(bytes + 1, 1);
    DWORD read = 0;
    if (text) ReadFile(file, text, bytes, &read, nullptr);
    CloseHandle(file);
    if (size) *size = (long)read;
    return text;
}

// Runs on a thread that is deliberately not the render thread, so the two
// counting channels can be told apart by where their writes end up.
DWORD WINAPI engineChannelWorker(void*) {
    tq::probe::count(tq::probe::CounterDrawIndexed, 5);
    tq::probe::engineCount(tq::probe::CounterEngineTexCreateOff, 3);
    tq::probe::engineCount(tq::probe::CounterEngineTexCreateOffUs, 900);
    return 0;
}

volatile LONG g_offThreadGpuContextResult;

DWORD WINAPI gpuContextWorker(void*) {
    InterlockedExchange(&g_offThreadGpuContextResult,
                        tq::probe::currentGpuContext() ? 2 : 1);
    return 0;
}

// Fields in a CSV line, counting the separators rather than parsing: the only
// field that can contain a space is `unusual`, and none contains a comma.
unsigned csvFieldCount(const char* line) {
    unsigned fields = 1;
    for (const char* p = line; *p && *p != '\r' && *p != '\n'; ++p)
        if (*p == ',') ++fields;
    return fields;
}

// Whether the line's last field is exactly `name`.
bool csvLastFieldIs(const char* line, const char* name) {
    const char* end = line;
    while (*end && *end != '\r' && *end != '\n') ++end;
    const char* last = line;
    for (const char* p = line; p < end; ++p) if (*p == ',') last = p + 1;
    size_t length = strlen(name);
    return (size_t)(end - last) == length && strncmp(last, name, length) == 0;
}

// ---------------------------------------------------------------------------
// The progressive texture uploader, driven entirely off-game. The module takes
// its device entry points and its clock by injection precisely so this is
// possible: the chunk controller is a feedback loop over measured time, and
// against a clock that only ever tells the truth there is nothing to assert.

struct FakeCom {
    void** vtable;
    LONG refs;
};

ULONG __stdcall fakeAddRef(void* self) { return ++((FakeCom*)self)->refs; }
ULONG __stdcall fakeRelease(void* self) { return --((FakeCom*)self)->refs; }

void* g_fakeVtable[8];

void initFakeCom(FakeCom* object) {
    g_fakeVtable[1] = (void*)&fakeAddRef;
    g_fakeVtable[2] = (void*)&fakeRelease;
    object->vtable = g_fakeVtable;
    object->refs = 1;
}

struct UploadFake {
    FakeCom texture;
    FakeCom fullView;
    FakeCom lowView;
    unsigned creates, views, updates, retains, releases;
    // What the last UpdateSubresource was asked to do.
    D3D11_BOX box;
    UINT mip, pitch;
    const void* source;
    UINT lastRows;
    UINT lastBytes;
    // The clock the module sees, and what the next chunk will appear to cost.
    int64_t clock;
    double nextChunkMs;
    // Which mips the fake was handed at creation, so the low-mip staging can
    // be asserted rather than assumed.
    const void* staged[tq::upload::kMaxTextureMips];
    UINT stagedCount;
};

UploadFake g_upload;

HRESULT WINAPI fakeCreateTexture2D(ID3D11Device*, const D3D11_TEXTURE2D_DESC* desc,
                                   const D3D11_SUBRESOURCE_DATA* initial,
                                   ID3D11Texture2D** texture) {
    ++g_upload.creates;
    g_upload.stagedCount = desc ? desc->MipLevels : 0;
    for (UINT i = 0; i < g_upload.stagedCount && i < tq::upload::kMaxTextureMips; ++i)
        g_upload.staged[i] = initial ? initial[i].pSysMem : nullptr;
    initFakeCom(&g_upload.texture);
    *texture = (ID3D11Texture2D*)&g_upload.texture;
    return S_OK;
}

HRESULT WINAPI fakeCreateShaderResourceView(ID3D11Device*, ID3D11Resource*,
                                            const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                            ID3D11ShaderResourceView** view) {
    ++g_upload.views;
    initFakeCom(&g_upload.lowView);
    *view = (ID3D11ShaderResourceView*)&g_upload.lowView;
    return S_OK;
}

void WINAPI fakeUpdateSubresource(ID3D11DeviceContext*, ID3D11Resource*, UINT mip,
                                  const D3D11_BOX* box, const void* source,
                                  UINT pitch, UINT) {
    ++g_upload.updates;
    if (box) g_upload.box = *box;
    g_upload.mip = mip;
    g_upload.pitch = pitch;
    g_upload.source = source;
    g_upload.lastRows = box ? (box->bottom - box->top + 3u) / 4u : 0u;
    g_upload.lastBytes = g_upload.lastRows * pitch;
    // Charge the configured cost to the clock the module reads back.
    g_upload.clock += (int64_t)(g_upload.nextChunkMs * 1000.0);
}

int64_t fakeNow() { return g_upload.clock; }
double fakeMillisecondsSince(int64_t start) {
    return (double)(g_upload.clock - start) / 1000.0;
}
bool fakeRetain(void*, void** token) { ++g_upload.retains; *token = &g_upload; return true; }
void fakeRelease2(void*) { ++g_upload.releases; }

tq::upload::Calls fakeUploadCalls() {
    tq::upload::Calls calls = {};
    calls.createTexture2D = &fakeCreateTexture2D;
    calls.createShaderResourceView = &fakeCreateShaderResourceView;
    calls.updateSubresource = &fakeUpdateSubresource;
    calls.now = &fakeNow;
    calls.millisecondsSince = &fakeMillisecondsSince;
    calls.retain = &fakeRetain;
    calls.release = &fakeRelease2;
    return calls;
}

// A 2048x2048 BC1 with a full mip chain, which is the shape of the terrain
// textures this path exists for: 8 bytes per 4x4 block, so mip 0 is a 4096
// byte pitch over 512 block rows.
struct SyntheticTexture {
    D3D11_TEXTURE2D_DESC desc;
    D3D11_SUBRESOURCE_DATA initial[tq::upload::kMaxTextureMips];
    BYTE* bytes;
    size_t size;
    uint64_t topBytes;
};

bool buildSyntheticTexture(SyntheticTexture* out) {
    memset(out, 0, sizeof(*out));
    out->desc.Width = out->desc.Height = 2048;
    out->desc.MipLevels = 12;
    out->desc.ArraySize = 1;
    out->desc.Format = DXGI_FORMAT_BC1_UNORM;
    out->desc.SampleDesc.Count = 1;
    out->desc.Usage = D3D11_USAGE_DEFAULT;
    out->desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    size_t total = 0;
    for (UINT mip = 0; mip < out->desc.MipLevels; ++mip) {
        UINT width = out->desc.Width >> mip, height = out->desc.Height >> mip;
        if (!width) width = 1;
        if (!height) height = 1;
        total += (size_t)((width + 3u) / 4u) * 8u * ((height + 3u) / 4u);
    }
    out->bytes = (BYTE*)malloc(total);
    if (!out->bytes) return false;
    out->size = total;
    // A pattern that is a function of position, so a copy can be checked byte
    // for byte against what the engine's buffer held.
    for (size_t i = 0; i < total; ++i) out->bytes[i] = (BYTE)(i * 31u + 7u);
    size_t offset = 0;
    for (UINT mip = 0; mip < out->desc.MipLevels; ++mip) {
        UINT width = out->desc.Width >> mip, height = out->desc.Height >> mip;
        if (!width) width = 1;
        if (!height) height = 1;
        UINT pitch = ((width + 3u) / 4u) * 8u;
        UINT rows = (height + 3u) / 4u;
        out->initial[mip].pSysMem = out->bytes + offset;
        out->initial[mip].SysMemPitch = pitch;
        offset += (size_t)pitch * rows;
        if (!mip) out->topBytes = (uint64_t)pitch * rows;
    }
    return true;
}

// The dimension gate. The first case is not synthetic: these are the real
// first 32 bytes of XPack3/Scenery/atlantis/06garden/nature/trees/
// gardens_bigtree01normal.tex from the installed texture pack, which is the
// 341 MiB 16384x16384 DXT5 that motivated the cap.
void testTextureDimensions() {
    static const BYTE kRealTexHeader[32] = {
        0x54,0x45,0x58,0x01, 0x00,0x00,0x00,0x00, 0xf0,0x55,0x55,0x15,
        0x44,0x44,0x53,0x52, 0x7c,0x00,0x00,0x00, 0x07,0x10,0x0a,0x00,
        0x00,0x40,0x00,0x00, 0x00,0x40,0x00,0x00
    };
    UINT w = 0, h = 0;
    check(tq::upload::textureDimensions(kRealTexHeader, sizeof(kRealTexHeader),
                                        &w, &h) && w == 16384 && h == 16384,
          "read 16384x16384 out of a real TEX container header");

    // A TEX whose payload magic is the stock "DDS " rather than this pack's
    // "DDSR", at a size that must be kept.
    BYTE tex[64] = {};
    memcpy(tex, "TEX\x01", 4);
    memcpy(tex + 12, "DDS ", 4);
    UINT size = 124, height = 2048, width = 4096;
    memcpy(tex + 16, &size, 4);
    memcpy(tex + 24, &height, 4);
    memcpy(tex + 28, &width, 4);
    w = h = 0;
    check(tq::upload::textureDimensions(tex, sizeof(tex), &w, &h)
          && w == 4096 && h == 2048,
          "read a TEX container whose payload carries the stock DDS magic");

    // A bare .dds, where the header starts at zero instead of twelve.
    BYTE dds[64] = {};
    memcpy(dds, "DDS ", 4);
    memcpy(dds + 4, &size, 4);
    memcpy(dds + 12, &height, 4);
    memcpy(dds + 16, &width, 4);
    w = h = 0;
    check(tq::upload::textureDimensions(dds, sizeof(dds), &w, &h)
          && w == 4096 && h == 2048, "read a bare DDS header");

    // Everything that is not a texture has to be refused, because the gate
    // runs over every file the loose source opens, not just textures.
    BYTE junk[64];
    memset(junk, 0xab, sizeof(junk));
    check(!tq::upload::textureDimensions(junk, sizeof(junk), &w, &h),
          "refuse a file that is not a texture container");
    check(!tq::upload::textureDimensions(kRealTexHeader, 8, &w, &h),
          "refuse a header too short to carry dimensions");
    BYTE bad[64] = {};
    memcpy(bad, tex, sizeof(bad));
    UINT wrongSize = 100;
    memcpy(bad + 16, &wrongSize, 4);
    check(!tq::upload::textureDimensions(bad, sizeof(bad), &w, &h),
          "refuse a DDS header whose dwSize is not 124");
    BYTE zero[64] = {};
    memcpy(zero, tex, sizeof(zero));
    UINT none = 0;
    memcpy(zero + 28, &none, 4);
    check(!tq::upload::textureDimensions(zero, sizeof(zero), &w, &h),
          "refuse a texture claiming a zero dimension");

    // The policy itself, over the real distribution: 4096 keeps a 4K asset and
    // refuses everything above it on either axis.
    struct Case { UINT w, h; bool refused; };
    static const Case kCases[] = {
        {4096, 4096, false}, {2048, 2048, false}, {4096, 2048, false},
        {8192, 8192, true},  {16384, 16384, true},
        {4096, 16384, true}, {16384, 4096, true}, {8000, 16384, true},
    };
    bool policy = true;
    for (unsigned i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
        bool over = kCases[i].w > 4096 || kCases[i].h > 4096;
        policy &= over == kCases[i].refused;
    }
    check(policy, "a 4096 cap refuses exactly the shapes over it on either axis");
}

void testUpload() {
    SyntheticTexture texture;
    if (!buildSyntheticTexture(&texture)) {
        check(false, "build a synthetic 2048x2048 BC1 for the upload tests");
        return;
    }
    ID3D11DeviceContext* context = (ID3D11DeviceContext*)&g_upload;

    // --- one job, and only the low mips reach the driver at creation ---
    memset(&g_upload, 0, sizeof(g_upload));
    g_upload.nextChunkMs = 1.0;
    tq::upload::resetRateForTest();
    check(tq::upload::install(fakeUploadCalls()) && tq::upload::ready(),
          "the upload module installs against injected device calls");
    ID3D11Texture2D* created = nullptr;
    bool handled = false;
    HRESULT hr = tq::upload::create(nullptr, &texture.desc, texture.initial,
                                    &created, texture.topBytes, &g_upload,
                                    &handled);
    UINT lowMip = tq::upload::lowMipFor(&texture.desc);
    bool staged = g_upload.stagedCount == texture.desc.MipLevels;
    for (UINT mip = 0; mip < texture.desc.MipLevels && staged; ++mip)
        staged = mip < lowMip ? g_upload.staged[mip] == nullptr
                              : g_upload.staged[mip] == texture.initial[mip].pSysMem;
    check(SUCCEEDED(hr) && handled && created && tq::upload::runningJobsForTest() == 1
          && g_upload.retains == 1,
          "create starts one progressive job and takes a hold on the source");
    check(lowMip == 2 && staged,
          "the texture is created with the large mips withheld from the driver");

    // --- one chunk, block-aligned, out of the mip the job is working on ---
    tq::upload::advance(context);
    bool aligned = g_upload.updates == 1
                && g_upload.mip == 0
                && g_upload.box.left == 0 && g_upload.box.right == 2048
                && g_upload.box.top == 0
                && g_upload.box.bottom % 4 == 0
                && g_upload.box.bottom <= 2048
                && g_upload.box.front == 0 && g_upload.box.back == 1
                && g_upload.pitch == texture.initial[0].SysMemPitch
                && g_upload.source == texture.initial[0].pSysMem;
    check(aligned, "advance issues one chunk as a block-aligned D3D11_BOX");

    // --- an expensive chunk shrinks the next one ---
    // A fresh job, so both chunks come out of mip 0 and their sizes are
    // comparable: across a mip boundary the pitch changes and a comparison in
    // block rows means nothing.
    tq::upload::shutdown();
    memset(&g_upload, 0, sizeof(g_upload));
    g_upload.nextChunkMs = 40.0;
    tq::upload::resetRateForTest();
    tq::upload::install(fakeUploadCalls());
    created = nullptr;
    handled = false;
    tq::upload::create(nullptr, &texture.desc, texture.initial, &created,
                       texture.topBytes, &g_upload, &handled);
    tq::upload::advance(context);
    UINT expensive = g_upload.lastBytes;
    tq::upload::advance(context);
    check(expensive && g_upload.lastBytes * 2 <= expensive,
          "a chunk that overran its budget at least halves the one after it");

    // --- cheap chunks grow the budget back to the ceiling ---
    // The rate is process-global by design -- it is a property of the driver
    // and the format, not of a texture -- so this starts from the ratcheted
    // value the expensive run just produced, which is the case that matters.
    tq::upload::shutdown();
    memset(&g_upload, 0, sizeof(g_upload));
    g_upload.nextChunkMs = 0.1;
    tq::upload::resetRateForTest();
    tq::upload::install(fakeUploadCalls());
    UINT peakAfterFirst = 0;
    for (unsigned i = 0; i < 12; ++i) {
        if (!tq::upload::runningJobsForTest()) {
            created = nullptr;
            handled = false;
            tq::upload::create(nullptr, &texture.desc, texture.initial, &created,
                               texture.topBytes, &g_upload, &handled);
            tq::upload::advance(context);   // the opening chunk, uncapped
            continue;
        }
        tq::upload::advance(context);
        if (g_upload.lastBytes > peakAfterFirst) peakAfterFirst = g_upload.lastBytes;
    }
    check(tq::upload::chunkBytesForTargetMs() == 2u * 1024u * 1024u,
          "chunks that cost nothing grow the budget back to its ceiling");
    check(peakAfterFirst && peakAfterFirst <= 1024u * 1024u,
          "a job's own chunks stay under the per-texture cap however cheap they get");

    // The full view has to be referenced while a job holds it, or the engine
    // can release it, D3D can reuse the allocation, and the substitution
    // matches an unrelated texture by raw pointer.
    tq::upload::shutdown();
    memset(&g_upload, 0, sizeof(g_upload));
    g_upload.nextChunkMs = 0.5;
    tq::upload::resetRateForTest();
    tq::upload::install(fakeUploadCalls());
    created = nullptr;
    handled = false;
    tq::upload::create(nullptr, &texture.desc, texture.initial, &created,
                       texture.topBytes, &g_upload, &handled);
    initFakeCom(&g_upload.fullView);
    LONG before = g_upload.fullView.refs;
    tq::upload::noteShaderResourceView(nullptr, (ID3D11Resource*)created, nullptr,
                                       (ID3D11ShaderResourceView*)&g_upload.fullView);
    check(g_upload.fullView.refs == before + 1,
          "a job takes a reference on the full view it will substitute for");
    for (unsigned i = 0; i < 12 && tq::upload::runningJobsForTest(); ++i)
        tq::upload::advance(context);
    check(g_upload.fullView.refs == before,
          "and gives it back when the job retires");

    tq::upload::shutdown();
    check(!tq::upload::ready() && tq::upload::runningJobsForTest() == 0,
          "shutdown clears the job pool");
    free(texture.bytes);
}

// Whether this device retires timestamp queries at all, and under which of the
// three ways of asking. Two in-game runs of eight thousand frames each resolved
// not one, so the capability has to be established here rather than assumed:
// the probe's GPU columns are only worth having if the answer is yes.
void testTimestampCapability(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context) return;
    D3D11_QUERY_DESC disjointDesc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC stampDesc = {D3D11_QUERY_TIMESTAMP, 0};
    ID3D11Query *disjoint = nullptr, *begin = nullptr, *end = nullptr;
    bool created = SUCCEEDED(device->CreateQuery(&disjointDesc, &disjoint))
                && SUCCEEDED(device->CreateQuery(&stampDesc, &begin))
                && SUCCEEDED(device->CreateQuery(&stampDesc, &end));
    check(created, "the device creates timestamp and disjoint queries");
    if (!created) {
        if (disjoint) disjoint->Release();
        if (begin) begin->Release();
        if (end) end->Release();
        return;
    }

    // Something for the GPU to actually do between the two timestamps.
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = desc.Height = 512;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *source = nullptr, *destination = nullptr;
    bool ready = SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &source))
              && SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &destination));

    context->Begin(disjoint);
    context->End(begin);
    if (ready) for (unsigned i = 0; i < 32; ++i)
        context->CopyResource(destination, source);
    context->End(end);
    context->End(disjoint);

    // First the way the render path asks: never flush, never wait.
    bool quiet = false;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data = {};
    for (unsigned i = 0; i < 200 && !quiet; ++i) {
        quiet = context->GetData(disjoint, &data, sizeof(data),
                                 D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (!quiet) Sleep(1);
    }
    // Then the way that permits the runtime to flush first.
    context->Flush();
    bool flushed = quiet;
    for (unsigned i = 0; i < 200 && !flushed; ++i) {
        flushed = context->GetData(disjoint, &data, sizeof(data), 0) == S_OK;
        if (!flushed) Sleep(1);
    }
    UINT64 first = 0, last = 0;
    bool stamps = flushed
               && context->GetData(begin, &first, sizeof(first), 0) == S_OK
               && context->GetData(end, &last, sizeof(last), 0) == S_OK;

    char detail[192];
    snprintf(detail, sizeof(detail),
             "timestamp queries retire on this device "
             "(donotflush=%u afterflush=%u stamps=%u disjoint=%u freq=%llu)",
             quiet ? 1u : 0u, flushed ? 1u : 0u, stamps ? 1u : 0u,
             data.Disjoint ? 1u : 0u, (unsigned long long)data.Frequency);
    check(flushed && stamps && data.Frequency != 0, detail);

    if (source) source->Release();
    if (destination) destination->Release();
    disjoint->Release();
    begin->Release();
    end->Release();
}

void testProbe(ID3D11Device* device, ID3D11DeviceContext* context) {
    wchar_t ini[MAX_PATH], csv[MAX_PATH];
    if (!GetFullPathNameW(L"tqflicker-probe-selftest.ini", MAX_PATH, ini, nullptr)
        || !GetFullPathNameW(L"tqflicker-probe-selftest.csv", MAX_PATH, csv, nullptr))
        return;
    DeleteFileW(ini);
    DeleteFileW(csv);

    // Off by default, and silent: the probe must never write a file the user
    // did not ask for, which is the same invariant the two logs hold.
    tq::probe::readOptions(ini);
    check(!tq::probe::enabled(), "probe stays off when the INI has no setting");
    tq::probe::count(tq::probe::CounterDraw);
    tq::probe::engineCount(tq::probe::CounterEngineTexCreateOff, 11);
    tq::probe::endFrame(33.0f);
    check(tq::probe::frameCountForTest() == 0,
          "probe records nothing while it is off");
    check(tq::probe::microsecondsSince(tq::probe::now()) == 0,
          "microsecondsSince reports nothing while the probe is off");
    check(!tq::probe::createResources(device),
          "probe creates no device objects while it is off");

    WritePrivateProfileStringW(L"debug", L"performance_trace", L"0", ini);
    WritePrivateProfileStringW(L"debug", L"stutter_marker", L"1", ini);
    tq::probe::readOptions(ini);
    check(!tq::probe::enabled() && !tq::probe::stutterMarkerEnabled(),
          "performance_trace=0 leaves the probe and marker off");

    WritePrivateProfileStringW(L"debug", L"performance_trace", L"1", ini);
    tq::probe::readOptions(ini);
    check(tq::probe::enabled() && !tq::probe::logsEveryFrame()
          && tq::probe::stutterMarkerEnabled(),
          "performance_trace=1 records hitching frames and arms its marker");
    tq::probe::resetForTest();

    WritePrivateProfileStringW(L"debug", L"performance_trace", L"full", ini);
    WritePrivateProfileStringW(L"debug", L"stutter_marker", L"1", ini);
    tq::probe::readOptions(ini);
    tq::probe::setOutputPath(csv);
    check(tq::probe::enabled() && tq::probe::logsEveryFrame(),
          "performance_trace=full turns the probe on for every frame");

    // A phase interval the clock can actually resolve, and a counter beside it.
    int64_t start = tq::probe::now();
    Sleep(4);
    tq::probe::addPhase(tq::probe::PhaseGrassPresent, start);
    tq::probe::count(tq::probe::CounterGrassSeedQueued);
    tq::probe::count(tq::probe::CounterDraw, 7);
    tq::probe::markStutter();
    tq::probe::endFrame(16.7f);
    float measured = tq::probe::phaseMillisecondsForTest(0, tq::probe::PhaseGrassPresent);
    check(measured >= 2.0f && measured < 500.0f,
          "probe times a phase with the high-resolution clock");
    check(tq::probe::counterForTest(0, tq::probe::CounterDraw) == 7
          && tq::probe::counterForTest(0, tq::probe::CounterGrassSeedQueued) == 1
          && tq::probe::counterForTest(0, tq::probe::CounterStutterMarker) == 1,
          "probe counts local, engine and marker events");
    check(tq::probe::phaseMillisecondsForTest(0, tq::probe::PhaseBloom) == 0.0f,
          "a phase that did not run stays at zero");

    // The engine channel. The frame above taught the probe which thread is the
    // render thread, so from here a write from anywhere else is refused by
    // count() and accepted by engineCount() -- which is the entire reason the
    // second channel exists, and the invariant the first one must not lose.
    HANDLE worker = CreateThread(nullptr, 0, &engineChannelWorker, nullptr, 0,
                                 nullptr);
    if (worker) { WaitForSingleObject(worker, 5000); CloseHandle(worker); }
    tq::probe::endFrame(16.7f);
    check(worker != nullptr
          && tq::probe::counterForTest(0, tq::probe::CounterEngineTexCreateOff) == 3
          && tq::probe::counterForTest(0, tq::probe::CounterEngineTexCreateOffUs) == 900,
          "engineCount from the game's own thread lands in the frame that closed");
    check(tq::probe::counterForTest(0, tq::probe::CounterDrawIndexed) == 0,
          "count() from a thread that is not the render thread still records nothing");
    tq::probe::endFrame(16.7f);
    check(tq::probe::counterForTest(0, tq::probe::CounterEngineTexCreateOff) == 0,
          "the engine channel drains, so a count is charged to one frame only");

    // Run 48's shadow-resource lifecycle split. The four loaded-state buckets
    // partition the nested calls; in_queue is deliberately overlapping and
    // must carry the same call duration rather than stealing it from state 1.
    tq::engineprobe::countShadowResourceStateForTest(0, false, 1100);
    tq::engineprobe::countShadowResourceStateForTest(1, true, 2200);
    tq::engineprobe::countShadowResourceStateForTest(2, false, 3300);
    tq::engineprobe::countShadowResourceStateForTest(7, true, 4400);
    tq::probe::endFrame(16.7f);
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowResState0) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowResState0Us) == 1100
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowResState1) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowResState1Us) == 2200
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowResState2) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowResState2Us) == 3300
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowResStateOther) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowResStateOtherUs) == 4400,
          "shadow resource loaded-state buckets partition calls and durations");
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowResInQueue) == 2
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowResInQueueUs) == 6600,
          "shadow resource queue flag overlaps its loaded-state buckets");

    // Run 49's engine-native filename partition. These are the resource
    // classes observed at LoadResource, not guesses from which renderer was
    // active; mixed-case suffixes remain the same engine file type.
    tq::engineprobe::countShadowResourceTypeForTest(
        "Creatures/Monster.msh", 1100);
    tq::engineprobe::countShadowResourceTypeForTest(
        "Shaders/Pieces/Shadow.SSH", 2200);
    tq::engineprobe::countShadowResourceTypeForTest(
        "Items/Weapon.tex", 3300);
    tq::engineprobe::countShadowResourceTypeForTest(nullptr, 4400);
    tq::probe::endFrame(16.7f);
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowResMesh) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowResMeshUs) == 1100
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowResShader) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowResShaderUs) == 2200
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowResTexture) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowResTextureUs) == 3300
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowResTypeOther) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowResTypeOtherUs) == 4400,
          "shadow resource filename classes partition calls and durations");

    // Run 61's complement of the directional-shadow population. Phase and
    // filename type are two independent partitions of the same main-thread
    // LoadResource calls, so each must add back to the common total.
    tq::engineprobe::countOutsideDirResourceForTest(0, 0, 1100);
    tq::engineprobe::countOutsideDirResourceForTest(1, 1, 2200);
    tq::engineprobe::countOutsideDirResourceForTest(2, 2, 3300);
    tq::engineprobe::countOutsideDirResourceForTest(3, 0, 4400);
    tq::probe::endFrame(16.7f);
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineResOutsideDir) == 4
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineResOutsideDirUs) == 11000,
          "outside-directional resource calls retain one common population");
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineResOutsideDirRender) == 2
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineResOutsideDirRenderUs) == 5500
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineResOutsideDirUpdate) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineResOutsideDirUpdateUs) == 2200
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineResOutsideDirOther) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineResOutsideDirOtherUs) == 3300,
          "outside-directional resource phases partition calls and durations");
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineResOutsideDirMesh) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineResOutsideDirMeshUs) == 1100
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineResOutsideDirShader) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineResOutsideDirShaderUs) == 2200
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineResOutsideDirTexture) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineResOutsideDirTextureUs) == 3300
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineResOutsideDirTypeOther) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineResOutsideDirTypeOtherUs) == 4400,
          "outside-directional resource types partition calls and durations");

    tq::engineprobe::outsideDirResourceResetForTest();
    tq::engineprobe::outsideDirResourceRememberForTest(10);
    tq::engineprobe::outsideDirResourceRememberForTest(50);
    tq::engineprobe::outsideDirResourceRememberForTest(251);
    bool resourceWindowTruncated = true;
    check(tq::engineprobe::outsideDirResourceWindowForTest(
              130, &resourceWindowTruncated) == 2
          && !resourceWindowTruncated,
          "the reaction window includes its 120-frame boundary and rejects"
          " future loads");
    tq::engineprobe::outsideDirResourceResetForTest();
    for (unsigned i = 0; i < 129; ++i)
        tq::engineprobe::outsideDirResourceRememberForTest(100);
    check(tq::engineprobe::outsideDirResourceWindowForTest(
              100, &resourceWindowTruncated) == 128
          && resourceWindowTruncated,
          "the reaction window reports when one recent Resource load was"
          " overwritten");
    tq::engineprobe::outsideDirResourceResetForTest();

    tq::engineprobe::shadowMeshResourceResetForTest();
    tq::engineprobe::shadowMeshResourceRememberForTest(10);
    tq::engineprobe::shadowMeshResourceRememberForTest(50);
    tq::engineprobe::shadowMeshResourceRememberForTest(251);
    resourceWindowTruncated = true;
    check(tq::engineprobe::shadowMeshResourceWindowForTest(
              130, &resourceWindowTruncated) == 2
          && !resourceWindowTruncated,
          "the directional-mesh reaction window includes its boundary and"
          " rejects future loads");
    tq::engineprobe::shadowMeshResourceResetForTest();
    for (unsigned i = 0; i < 129; ++i)
        tq::engineprobe::shadowMeshResourceRememberForTest(100);
    check(tq::engineprobe::shadowMeshResourceWindowForTest(
              100, &resourceWindowTruncated) == 128
          && resourceWindowTruncated,
          "the directional-mesh window reports a recent overwritten load");
    tq::engineprobe::shadowMeshResourceResetForTest();

    // Run 63's TerrainType association retains per-object preload history,
    // including frame zero without confusing it with "never".
    const void* const terrainA = (const void*)0x12340000;
    const void* const terrainB = (const void*)0x12348000;
    tq::engineprobe::terrainPreloadResetForTest();
    tq::engineprobe::terrainPreloadRememberForTest(terrainA, true, 0);
    tq::engineprobe::terrainPreloadRememberForTest(terrainA, false, 17);
    tq::engineprobe::terrainPreloadRememberForTest(terrainA, true, 22);
    unsigned preloadTrue = 0, preloadFalse = 0;
    unsigned lastPreloadTrue = 0, lastPreloadFalse = 0;
    tq::engineprobe::terrainPreloadSnapshotForTest(
        terrainA, &preloadTrue, &preloadFalse, &lastPreloadTrue,
        &lastPreloadFalse);
    check(preloadTrue == 2 && preloadFalse == 1
          && lastPreloadTrue == 23 && lastPreloadFalse == 18,
          "TerrainType preload history is identity-specific and preserves"
          " frame zero");
    tq::engineprobe::terrainPreloadSnapshotForTest(
        terrainB, &preloadTrue, &preloadFalse, &lastPreloadTrue,
        &lastPreloadFalse);
    check(preloadTrue == 0 && preloadFalse == 0
          && lastPreloadTrue == 0 && lastPreloadFalse == 0,
          "an unseen TerrainType cannot inherit another object's preload");

    // Run 64 distinguishes the runtime terrain owner's three relevant
    // boundaries for each layer TerrainType. Counts and both endpoints are
    // retained because a later owner preload must not erase an earlier attach
    // or texture-admission event.
    tq::engineprobe::terrainRtEventRememberForTest(terrainA, 0, 4);
    tq::engineprobe::terrainRtEventRememberForTest(terrainA, 0, 9);
    tq::engineprobe::terrainRtEventRememberForTest(terrainA, 1, 12);
    tq::engineprobe::terrainRtEventRememberForTest(terrainA, 2, 15);
    tq::engineprobe::terrainRtEventRememberForTest(terrainA, 2, 18);
    unsigned attachCount = 0, attachFirst = 0, attachLast = 0;
    unsigned texturesCount = 0, texturesFirst = 0, texturesLast = 0;
    unsigned ownerPreloadCount = 0, ownerPreloadFirst = 0;
    unsigned ownerPreloadLast = 0;
    tq::engineprobe::terrainRtEventSnapshotForTest(
        terrainA, &attachCount, &attachFirst, &attachLast,
        &texturesCount, &texturesFirst, &texturesLast,
        &ownerPreloadCount, &ownerPreloadFirst, &ownerPreloadLast);
    check(attachCount == 2 && attachFirst == 5 && attachLast == 10
          && texturesCount == 1 && texturesFirst == 13
          && texturesLast == 13 && ownerPreloadCount == 2
          && ownerPreloadFirst == 16 && ownerPreloadLast == 19,
          "TerrainRT layer history retains each boundary's first, last, and"
          " count independently");
    tq::engineprobe::terrainRtEventSnapshotForTest(
        terrainB, &attachCount, &attachFirst, &attachLast,
        &texturesCount, &texturesFirst, &texturesLast,
        &ownerPreloadCount, &ownerPreloadFirst, &ownerPreloadLast);
    check(attachCount == 0 && attachFirst == 0 && attachLast == 0
          && texturesCount == 0 && texturesFirst == 0 && texturesLast == 0
          && ownerPreloadCount == 0 && ownerPreloadFirst == 0
          && ownerPreloadLast == 0,
          "an unseen TerrainType cannot inherit TerrainRT owner history");
    tq::engineprobe::terrainPreloadResetForTest();

    // Run 52's texture-load caller split. The raw word scan stops at the
    // nearest exact E8 return address, and an out-of-range semantic value is
    // retained in the explicit unresolved bucket rather than indexing past
    // the counter arrays.
    const uintptr_t fakeEngineBase = 0x10000000u;
    const void* textureStack[] = {
        (const void*)(fakeEngineBase + 0x120f37u + 5u),
        (const void*)(fakeEngineBase + 0x18a90eu + 5u)
    };
    check(tq::engineprobe::shadowTextureCallerFromWordsForTest(
              textureStack, 2, (const void*)fakeEngineBase) == 2,
          "shadow texture stack scan selects the nearest verified caller");

    // Run 53 replaces run 52's unsafe re-entry from the material loop with a
    // frame-local identity table populated at the already exercised record
    // gate. Prove both pass keys and generation expiry without calling any
    // engine method from inside GraphicsMesh::SetShaderParameters.
    void* const recordInstance = (void*)0x12345000;
    const void* const recordBase = (const void*)0x76543000;
    tq::engineprobe::resetShadowRecordContextsForTest();
    tq::engineprobe::rememberShadowRecordContextForTest(
        recordInstance, 0, 3, true, true, recordBase);
    tq::engineprobe::rememberShadowRecordContextForTest(
        recordInstance, 1, 4, false, false, nullptr);
    unsigned recordStyle = 0;
    bool recordStyleKnown = false;
    bool recordBaseKnown = false;
    const void* foundBase = nullptr;
    check(tq::engineprobe::findShadowRecordContextForTest(
              recordInstance, 0, &recordStyle, &recordStyleKnown,
              &recordBaseKnown, &foundBase)
          && recordStyle == 3 && recordStyleKnown && recordBaseKnown
          && foundBase == recordBase,
          "shadow record context retains exact instance/pass identity");
    check(tq::engineprobe::findShadowRecordContextForTest(
              recordInstance, 1, &recordStyle, &recordStyleKnown,
              &recordBaseKnown, &foundBase)
          && recordStyle == 4 && !recordStyleKnown && !recordBaseKnown
          && foundBase == nullptr,
          "shadow record context keeps an overridden class explicit");
    check(tq::engineprobe::explainShadowRecordMissForTest(
              recordInstance, 2) == 2
          && tq::engineprobe::explainShadowRecordMissForTest(
                 (void*)0x11111000, 0) == 3
          && tq::engineprobe::explainShadowRecordMissForTest(nullptr, 0) == 3,
          "shadow record misses distinguish pass mismatch from every missing instance");
    tq::engineprobe::resetShadowRecordContextsForTest();
    check(!tq::engineprobe::findShadowRecordContextForTest(
              recordInstance, 0, nullptr, nullptr, nullptr, nullptr),
          "a new directional generation expires prior record identities");

    // The fixed table must fail visibly rather than silently losing the join.
    // These are identity-only values: the implementation never dereferences
    // anything retained here.
    for (uintptr_t i = 0; i < 4096; ++i)
        tq::engineprobe::rememberShadowRecordContextForTest(
            (void*)(0x20000000u + i * 16u), 0, 0, true, false, nullptr);
    tq::engineprobe::rememberShadowRecordContextForTest(
        (void*)0x30000000u, 0, 0, true, false, nullptr);
    tq::probe::endFrame(16.7f);
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowContextTableOverflow) == 1,
          "shadow record context table reports an explicit overflow");
    tq::engineprobe::resetShadowRecordContextsForTest();

    tq::engineprobe::countShadowTextureCallerForTest(0, 1100);
    tq::engineprobe::countShadowTextureCallerForTest(2, 2200);
    tq::engineprobe::countShadowTextureCallerForTest(99, 3300);
    tq::probe::endFrame(16.7f);
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowTexFromMeshMaterial) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowTexFromMeshMaterialUs) == 1100
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowTexFromBillboard) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowTexFromBillboardUs) == 2200
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowTexFromUnresolved) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowTexFromUnresolvedUs) == 3300,
          "shadow texture callers retain counts and durations by exact site");

    tq::engineprobe::countShadowMaterialTextureForTest(true, true, 1100);
    tq::engineprobe::countShadowMaterialTextureForTest(true, false, 2200);
    tq::engineprobe::countShadowMaterialTextureForTest(false, false, 3300);
    tq::probe::endFrame(16.7f);
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialTex) == 3
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialTexUs) == 6600
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialTexUsed) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialTexUsedUs) == 1100
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialTexUnused) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialTexUnusedUs) == 2200
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialTexUnknown) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialTexUnknownUs) == 3300,
          "shadow material textures partition by active-shader use");

    tq::engineprobe::countShadowMaterialUsedContextForTest(
        true, true, true, 0, 0, true, true, 0, true, 1100);
    tq::engineprobe::countShadowMaterialUsedContextForTest(
        true, true, true, 0, 3, true, false, 2, true, 2200);
    tq::engineprobe::countShadowMaterialUsedContextForTest(
        true, true, true, 0, 1, false, false, 0, true, 4400);
    tq::engineprobe::countShadowMaterialUsedContextForTest(
        true, true, false, 1, 0, false, false, 1, true, 5500);
    tq::engineprobe::countShadowMaterialUsedContextForTest(
        true, false, false, 2, 0, false, false, 0, true, 6600);
    tq::engineprobe::countShadowMaterialUsedContextForTest(
        true, false, false, 3, 0, false, false, 0, true, 3300);
    tq::engineprobe::countShadowMaterialUsedContextForTest(
        false, false, false, 3, 0, false, false, 0, false, 7700);
    tq::probe::endFrame(16.7f);
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialUsedStyle0) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialUsedStyle0Us) == 1100
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialUsedStyle3) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialUsedStyle3Us) == 2200
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialUsedContextUnknown) == 4
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialUsedContextUnknownUs)
                 == 23100,
          "cold used material textures partition by style or explicit unknown");
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialUsedBaseMatch) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialUsedBaseMatchUs) == 1100
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialUsedBaseOther) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialUsedBaseOtherUs) == 2200
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialUsedBaseUnknown) == 5
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialUsedBaseUnknownUs)
                 == 27500
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialUsedPass0) == 4
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialUsedPass0Us) == 15400
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialUsedPassOther) == 2
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialUsedPassOtherUs) == 7700
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialUsedPassUnknown) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialUsedPassUnknownUs)
                 == 7700,
          "cold used material textures partition by base identity and pass");
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialLookupExact) == 3
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialLookupExactUs) == 7700
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialLookupClassOther) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialLookupClassOtherUs)
                 == 5500
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialLookupPassMismatch) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialLookupPassMismatchUs)
                 == 6600
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialLookupInstanceMissing)
                 == 2
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialLookupInstanceMissingUs)
                 == 11000,
          "material context lookup partitions exact, class, pass, and missing");
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialOuterInstanceSite) == 6
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialOuterInstanceSiteUs)
                 == 23100
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialOuterOtherSite) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMaterialOuterOtherSiteUs)
                 == 7700,
          "used shadow materials partition by their enclosing mesh caller");

    for (unsigned status = 0; status < 7; ++status)
        tq::engineprobe::countShadowMeshContextPatchStatusForTest(status);
    tq::probe::endFrame(16.7f);
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowContextPatchActive) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowContextPatchDependencyMissing)
                 == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowContextPatchFrameMismatch) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowContextPatchEntryMismatch) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowContextPatchContextMismatch)
                 == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowContextPatchCallFailed) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowContextPatchReverted) == 1,
          "directional rows expose the exact mesh-context patch status");

    tq::engineprobe::countDeferredShadowAlphaForTest(0, true, false);
    tq::engineprobe::countDeferredShadowAlphaForTest(1, false, false);
    tq::engineprobe::countDeferredShadowAlphaForTest(0, false, true);
    tq::engineprobe::countDeferredShadowMeshForTest(0, true, false);
    tq::engineprobe::countDeferredShadowMeshForTest(1, false, false);
    tq::engineprobe::countDeferredShadowMeshForTest(0, false, true);
    tq::engineprobe::countDeferredShadowActorPoseForTest(0, true, false);
    tq::engineprobe::countDeferredShadowActorPoseForTest(1, false, false);
    tq::engineprobe::countDeferredShadowActorPoseForTest(0, false, true);
    tq::probe::endFrame(16.7f);
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowAlphaOmitted) == 3
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowAlphaState0) == 2
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowAlphaState1) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowAlphaEnqueued) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowAlphaEnqueueFailed) == 1,
          "cold alpha-shadow counters partition state and enqueue outcome");
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMeshOmitted) == 3
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMeshOmittedState0) == 2
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMeshOmittedState1) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMeshOmittedEnqueued) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowMeshOmittedEnqueueFailed) == 1,
          "cold root-mesh counters partition state and enqueue outcome");
    check(tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowActorPoseDeferred) == 3
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowActorPoseState0) == 2
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowActorPoseState1) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowActorPoseEnqueued) == 1
          && tq::probe::counterForTest(
              0, tq::probe::CounterEngineShadowActorPoseEnqueueFailed) == 1,
          "cold Actor pose counters partition state and enqueue outcome");

    // A steady baseline, then one frame that spikes in a single phase. The row
    // for that frame has to name the phase, not merely report the frame time.
    for (unsigned i = 0; i < 90; ++i) tq::probe::endFrame(16.7f);
    int64_t hitchStart = tq::probe::now();
    Sleep(30);
    tq::probe::addPhase(tq::probe::PhaseGrassPresent, hitchStart);
    tq::probe::endFrame(48.0f);
    for (unsigned i = 0; i < 16; ++i) tq::probe::endFrame(16.7f);

    // Run 44 caught the writer opening the CSV once per emitted frame. Under
    // CrossOver those opens share wineserver with the render thread's
    // PeekMessage calls and can manufacture the pump stalls being measured.
    // Multiple batches must therefore reuse exactly one session handle.
    Sleep(300);
    for (unsigned i = 0; i < 16; ++i) tq::probe::endFrame(16.7f);
    Sleep(300);
    check(tq::probe::logFileOpensForTest() == 1,
          "the probe reuses one CSV handle across writer batches");

    char summary[80] = {};
    tq::probe::summarize(summary, sizeof(summary));
    check(strstr(summary, "GRASS-PRES") != nullptr,
          "the overlay summary names the phase that dominated the hitch");

    if (device && context) {
        check(tq::probe::createResources(device),
              "probe builds its timestamp queries on the live device");

        // Run 64 reached TerrainRT::LoadRenderData from save loading while a
        // render-frame query slot was live. The immediate context is owned by
        // the render thread: exposing it to a loader can deadlock the device.
        tq::probe::beginFrame(context);
        check(tq::probe::currentGpuContext() == context,
              "the render thread can borrow its current GPU context");
        InterlockedExchange(&g_offThreadGpuContextResult, 0);
        HANDLE gpuWorker = CreateThread(nullptr, 0, &gpuContextWorker,
                                        nullptr, 0, nullptr);
        if (gpuWorker) {
            WaitForSingleObject(gpuWorker, 5000);
            CloseHandle(gpuWorker);
        }
        check(gpuWorker != nullptr
              && InterlockedCompareExchange(&g_offThreadGpuContextResult,
                                            0, 0) == 1,
              "a loader thread cannot borrow the render thread GPU context");
        tq::probe::endFrame(16.7f);

        // Drive frames with real GPU work in them until a timestamp comes
        // back. Two eight-thousand-frame runs reported no GPU timing at all
        // because the frame's disjoint query was begun and never ended, and
        // every timestamp is gated on that disjoint result. Asserting only
        // that issuing the queries does not crash could not see it; this
        // asserts that a number actually arrives.
        D3D11_TEXTURE2D_DESC surface = {};
        surface.Width = surface.Height = 512;
        surface.MipLevels = surface.ArraySize = 1;
        surface.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        surface.SampleDesc.Count = 1;
        surface.Usage = D3D11_USAGE_DEFAULT;
        surface.BindFlags = D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D *from = nullptr, *to = nullptr;
        bool copies = SUCCEEDED(device->CreateTexture2D(&surface, nullptr, &from))
                   && SUCCEEDED(device->CreateTexture2D(&surface, nullptr, &to));
        unsigned before = tq::probe::frameCountForTest();
        for (unsigned i = 0; i < 40; ++i) {
            tq::probe::beginFrame(context);
            tq::probe::gpuBegin(context, tq::probe::GpuSmaa);
            if (copies) for (unsigned c = 0; c < 16; ++c)
                context->CopyResource(to, from);
            tq::probe::gpuEnd(context, tq::probe::GpuSmaa);
            tq::probe::endFrame(16.7f);
            context->Flush();
            Sleep(2);
        }
        bool resolved = false;
        for (unsigned back = 0;
             back < tq::probe::frameCountForTest() - before && !resolved; ++back)
            resolved = tq::probe::gpuResolvedForTest(back);
        check(copies && resolved,
              "probe reads back a GPU timestamp for a frame it timed");
        if (from) from->Release();
        if (to) to->Release();
        tq::probe::releaseResources();
    }

    tq::probe::shutdown();
    check(!tq::probe::stutterMarkerEnabled(),
          "shutting down the probe disarms the stutter marker");
    long size = 0;
    char* csvText = readTextFile(csv, &size);
    bool header = csvText
               && strstr(csvText, "# performance_trace=full") == csvText
               && strstr(csvText, "# draw_timing=") != nullptr
               && strstr(csvText, "# stutter_marker=F12") != nullptr
               && strstr(csvText, "draw_submit_ms") != nullptr
               && strstr(csvText, "map_resource_ms") != nullptr
               && strstr(csvText, "stutter_marker") != nullptr
               && strstr(csvText, "frame,ms") != nullptr
               && strstr(csvText, "grass_present_ms") != nullptr
               && strstr(csvText, "gpu_shadow_dir_ms") != nullptr
               && strstr(csvText, ",unusual") != nullptr;
    check(header, "the probe writes its mode and a header naming every column");
    check(csvText && strstr(csvText, "engine_tex_create_off_us") != nullptr,
          "the header carries the engine channel's columns");
    check(csvText && strstr(csvText, "engine_terrain_preload_us") != nullptr
          && strstr(csvText, "engine_terrain_preload_true") != nullptr
          && strstr(csvText, "engine_terrain_preload_false") != nullptr
          && strstr(csvText, "engine_terrain_preload_table_overflow") != nullptr
          && strstr(csvText, "engine_terrain_shader_params") != nullptr
          && strstr(csvText, "engine_terrain_grass_params") != nullptr
          && strstr(csvText, "engine_terrain_ground_us") != nullptr
          && strstr(csvText, "gpu_terrain_ground_ms") != nullptr,
          "the header carries TerrainType preload and RT ground diagnostics");
    check(csvText && strstr(csvText, "engine_terrain_rt_load_us") != nullptr
          && strstr(csvText, "engine_terrain_rt_load_render_us") != nullptr
          && strstr(csvText, "engine_terrain_rt_load_render_main_us")
                 != nullptr
          && strstr(csvText, "engine_terrain_rt_load_render_other_us")
                 != nullptr
          && strstr(csvText, "engine_terrain_rt_load_textures_us") != nullptr
          && strstr(csvText, "engine_terrain_rt_preload_us") != nullptr
          && strstr(csvText, "engine_terrain_rt_preload_layers") != nullptr
          && strstr(csvText, "engine_terrain_rt_layer_overflow") != nullptr
          && strstr(csvText, "engine_terrain_plug_us") != nullptr
          && strstr(csvText, "engine_terrain_block_us") != nullptr
          && strstr(csvText, "gpu_terrain_rt_load_render_ms") != nullptr
          && strstr(csvText, "gpu_terrain_plug_ms") == nullptr
          && strstr(csvText, "gpu_terrain_block_ms") == nullptr,
          "the header carries the TerrainRT load and colour-render chain");
    check(csvText
          && strstr(csvText, "engine_deferred_geometry_us") != nullptr
          && strstr(csvText, "engine_deferred_geometry_draw_us") != nullptr
          && strstr(csvText, "engine_deferred_shadows_draw_us") != nullptr
          && strstr(csvText, "engine_deferred_lighting_draw_us") != nullptr
          && strstr(csvText, "engine_deferred_resolve_draw_us") != nullptr
          && strstr(csvText, "engine_deferred_late_scene_draw_us") != nullptr
          && strstr(csvText, "engine_deferred_post_draw_us") != nullptr
          && strstr(csvText, "engine_deferred_owner_overflow") != nullptr
          && strstr(csvText, "engine_deferred_i1_geometry_setup_us") != nullptr
          && strstr(csvText, "engine_deferred_i1_geometry_scene_draw_us")
                 != nullptr
          && strstr(csvText, "engine_deferred_i2_geometry_setup_res_load_us")
                 != nullptr
          && strstr(csvText, "engine_deferred_i2_geometry_scene_tex_create_us")
                 != nullptr
          && strstr(csvText, "engine_deferred_i2_other_buf_create_us")
                 != nullptr
          && strstr(csvText, "gpu_deferred_i1_geometry_setup_ms") != nullptr
          && strstr(csvText, "gpu_deferred_i1_geometry_scene_ms") != nullptr
          && strstr(csvText, "gpu_deferred_i2_geometry_setup_ms") != nullptr
          && strstr(csvText, "gpu_deferred_i2_geometry_scene_ms") != nullptr
          && strstr(csvText, "gpu_deferred_geometry_ms") == nullptr,
          "the header carries the per-invocation geometry partition");
    check(csvText && strstr(csvText, "engine_res_outside_dir_us") != nullptr
          && strstr(csvText, "engine_res_outside_dir_render_us") != nullptr
          && strstr(csvText, "engine_res_outside_dir_update_us") != nullptr
          && strstr(csvText, "engine_res_outside_dir_other_us") != nullptr
          && strstr(csvText, "engine_res_outside_dir_mesh_us") != nullptr
          && strstr(csvText, "engine_res_outside_dir_shader_us") != nullptr
          && strstr(csvText, "engine_res_outside_dir_texture_us") != nullptr
          && strstr(csvText, "engine_res_outside_dir_type_other_us") != nullptr
          && strstr(csvText, "engine_res_outside_dir_marker_truncated")
                 != nullptr,
          "the header carries outside-directional Resource attribution");
    check(csvText && strstr(csvText, "engine_shadow_render_us") != nullptr
          && strstr(csvText, "engine_shadow_region_change") != nullptr
          && strstr(csvText, "engine_shadow_reuse") != nullptr
          && strstr(csvText, "engine_shadow_res_load_us") != nullptr
          && strstr(csvText, "engine_shadow_res_state0_us") != nullptr
          && strstr(csvText, "engine_shadow_res_state1_us") != nullptr
          && strstr(csvText, "engine_shadow_res_state2_us") != nullptr
          && strstr(csvText, "engine_shadow_res_state_other_us") != nullptr
          && strstr(csvText, "engine_shadow_res_in_queue_us") != nullptr,
          "the header carries the directional-shadow attribution columns");
    check(csvText && strstr(csvText, "engine_shadow_res_mesh_us") != nullptr
          && strstr(csvText, "engine_shadow_res_shader_us") != nullptr
          && strstr(csvText, "engine_shadow_res_texture_us") != nullptr
          && strstr(csvText, "engine_shadow_res_type_other_us") != nullptr
          && strstr(csvText, "engine_shadow_mesh_cold_us") != nullptr
          && strstr(csvText, "engine_shadow_mesh_omitted") != nullptr
          && strstr(csvText, "engine_shadow_mesh_omitted_state0") != nullptr
          && strstr(csvText, "engine_shadow_mesh_omitted_state1") != nullptr
          && strstr(csvText, "engine_shadow_mesh_omitted_enqueued") != nullptr
          && strstr(csvText, "engine_shadow_mesh_omitted_enqueue_failed")
                 != nullptr
          && strstr(csvText, "engine_shadow_actor_pose_deferred") != nullptr
          && strstr(csvText, "engine_shadow_actor_pose_state0") != nullptr
          && strstr(csvText, "engine_shadow_actor_pose_state1") != nullptr
          && strstr(csvText, "engine_shadow_actor_pose_enqueued") != nullptr
          && strstr(csvText, "engine_shadow_actor_pose_enqueue_failed")
                 != nullptr
          && strstr(csvText, "engine_shadow_material_tex_us") != nullptr
          && strstr(csvText, "engine_shadow_material_tex_used_us") != nullptr
          && strstr(csvText, "engine_shadow_material_tex_unused_us") != nullptr
          && strstr(csvText, "engine_shadow_material_tex_unknown_us") != nullptr
          && strstr(csvText, "engine_shadow_tex_from_mesh_material_us") != nullptr
          && strstr(csvText, "engine_shadow_tex_from_billboard_us") != nullptr
          && strstr(csvText, "engine_shadow_tex_from_unresolved_us") != nullptr
          && strstr(csvText, "engine_shadow_material_used_style0_us") != nullptr
          && strstr(csvText, "engine_shadow_material_used_style5_us") != nullptr
          && strstr(csvText, "engine_shadow_material_used_context_unknown_us")
                 != nullptr
          && strstr(csvText, "engine_shadow_material_used_base_match_us")
                 != nullptr
          && strstr(csvText, "engine_shadow_material_used_base_other_us")
                 != nullptr
          && strstr(csvText, "engine_shadow_material_used_base_unknown_us")
                 != nullptr
          && strstr(csvText, "engine_shadow_material_used_pass0_us") != nullptr
          && strstr(csvText, "engine_shadow_material_used_pass_other_us")
                 != nullptr
          && strstr(csvText, "engine_shadow_material_used_pass_unknown_us")
                 != nullptr
          && strstr(csvText, "engine_shadow_material_lookup_exact_us")
                 != nullptr
          && strstr(csvText, "engine_shadow_material_lookup_class_other_us")
                 != nullptr
          && strstr(csvText, "engine_shadow_material_lookup_pass_mismatch_us")
                 != nullptr
          && strstr(csvText,
                    "engine_shadow_material_lookup_instance_missing_us")
                 != nullptr
          && strstr(csvText, "engine_shadow_context_table_overflow") != nullptr
          && strstr(csvText,
                    "engine_shadow_material_outer_instance_site_us") != nullptr
          && strstr(csvText,
                    "engine_shadow_material_outer_other_site_us") != nullptr
          && strstr(csvText, "engine_shadow_context_patch_active") != nullptr
          && strstr(csvText,
                    "engine_shadow_context_patch_dependency_missing") != nullptr
          && strstr(csvText,
                    "engine_shadow_context_patch_frame_mismatch") != nullptr
          && strstr(csvText,
                    "engine_shadow_context_patch_entry_mismatch") != nullptr
          && strstr(csvText,
                    "engine_shadow_context_patch_context_mismatch") != nullptr
          && strstr(csvText,
                    "engine_shadow_context_patch_call_failed") != nullptr
          && strstr(csvText,
                    "engine_shadow_context_patch_reverted") != nullptr
          && strstr(csvText, "engine_shadow_material_tex_skipped") != nullptr
          && strstr(csvText, "engine_shadow_material_tex_skipped_cold") != nullptr
          && strstr(csvText, "engine_shadow_bump_tex_skipped") != nullptr
          && strstr(csvText, "engine_shadow_bump_tex_skipped_cold") != nullptr
          && strstr(csvText, "engine_shadow_base_override_skipped") != nullptr
          && strstr(csvText, "engine_shadow_base_override_skipped_cold")
                 != nullptr
          && strstr(csvText, "engine_shadow_alpha_omitted") != nullptr
          && strstr(csvText, "engine_shadow_alpha_state0") != nullptr
          && strstr(csvText, "engine_shadow_alpha_state1") != nullptr
          && strstr(csvText, "engine_shadow_alpha_enqueued") != nullptr
          && strstr(csvText, "engine_shadow_alpha_enqueue_failed") != nullptr,
          "the header carries shadow resource types and cold-mesh boundary");
    // The permanent regression test for the header buffer. snprintf truncation
    // is silent -- `n += snprintf(...)` returns the length it wanted, so an
    // overrun writes a short, unterminated header and nothing reports it. A
    // header with fewer fields than its rows is the shape that failure takes.
    bool widths = false;
    if (csvText) {
        // Skip every leading comment rather than assuming one: the file opens
        // with full performance_trace today, and a reader that
        // counts comment lines breaks the next time one is added. frames.py
        // drops all of them the same way.
        const char* headerLine = csvText;
        while (*headerLine == '#') {
            const char* next = strchr(headerLine, '\n');
            if (!next) { headerLine = nullptr; break; }
            headerLine = next + 1;
        }
        const char* firstRow = headerLine ? strchr(headerLine, '\n') : nullptr;
        firstRow = firstRow ? firstRow + 1 : nullptr;
        widths = headerLine && firstRow && *firstRow
              && csvFieldCount(headerLine) == csvFieldCount(firstRow)
              && csvLastFieldIs(headerLine, "unusual");
    }
    check(widths, "the CSV header has exactly as many fields as a row, ending in unusual");
    free(csvText);

    DeleteFileW(csv);
    tq::probe::resetForTest();
    tq::probe::readOptions(nullptr);
    check(!tq::probe::enabled(), "probe stays off without an INI path");
    DeleteFileW(ini);
}

void testFrameOverlay(ID3D11Device* device, ID3D11DeviceContext* context) {
    wchar_t ini[MAX_PATH];
    if (!GetFullPathNameW(L"tqflicker-overlay-selftest.ini", MAX_PATH, ini, nullptr))
        return;
    DeleteFileW(ini);

    tq::frameoverlay::DeviceCalls calls = {};
    calls.createTexture2D = nullptr;
    calls.compile = &overlayCompile;

    tq::frameoverlay::readOptions(ini);
    check(!tq::frameoverlay::enabled(),
          "frame overlay stays off when the INI has no setting");
    tq::frameoverlay::recordFrame();
    check(!tq::frameoverlay::createResources(device, calls),
          "frame overlay builds nothing while it is off");
    tq::frameoverlay::draw(device, context, nullptr, 1920);

    WritePrivateProfileStringW(L"debug", L"frame_overlay", L"0", ini);
    tq::frameoverlay::readOptions(ini);
    check(!tq::frameoverlay::enabled(), "frame_overlay=0 leaves the overlay off");

    // An INI from before the key moved out of [performance] keeps working,
    // and an explicit [debug] value wins over the legacy one.
    WritePrivateProfileStringW(L"debug", L"frame_overlay", nullptr, ini);
    WritePrivateProfileStringW(L"performance", L"frame_overlay", L"1", ini);
    tq::frameoverlay::readOptions(ini);
    check(tq::frameoverlay::enabled(),
          "the key's old [performance] home is still honoured");
    WritePrivateProfileStringW(L"debug", L"frame_overlay", L"0", ini);
    tq::frameoverlay::readOptions(ini);
    check(!tq::frameoverlay::enabled(),
          "a [debug] value overrides the legacy [performance] one");
    WritePrivateProfileStringW(L"performance", L"frame_overlay", nullptr, ini);

    WritePrivateProfileStringW(L"debug", L"frame_overlay", L"1", ini);
    tq::frameoverlay::readOptions(ini);
    check(tq::frameoverlay::enabled(), "frame_overlay=1 turns the overlay on");

    if (device && context) {
        calls.createTexture2D = &overlayCreateTexture2D;
        calls.createShaderResourceView = &overlayCreateShaderResourceView;
        calls.createSamplerState = &overlayCreateSamplerState;
        calls.createPixelShader = &overlayCreatePixelShader;
        check(tq::frameoverlay::createResources(device, calls),
              "build the overlay's shaders and panels on the live device");

        for (unsigned i = 0; i < 8; ++i) { tq::frameoverlay::recordFrame(); Sleep(16); }

        D3D11_TEXTURE2D_DESC targetDesc = {};
        targetDesc.Width = 960; targetDesc.Height = 540;
        targetDesc.MipLevels = targetDesc.ArraySize = 1;
        targetDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        targetDesc.SampleDesc.Count = 1;
        targetDesc.Usage = D3D11_USAGE_DEFAULT;
        targetDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        D3D11_TEXTURE2D_DESC stagingDesc = targetDesc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* surface = nullptr;
        ID3D11Texture2D* staging = nullptr;
        ID3D11RenderTargetView* rtv = nullptr;
        bool ready = SUCCEEDED(device->CreateTexture2D(&targetDesc, nullptr, &surface))
                  && SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))
                  && SUCCEEDED(device->CreateRenderTargetView(surface, nullptr, &rtv));
        check(ready, "allocate a scratch surface for the overlay draw");
        if (ready) {
            const FLOAT clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            context->ClearRenderTargetView(rtv, clear);
            tq::frameoverlay::draw(device, context, rtv, targetDesc.Width);
            context->CopyResource(staging, surface);
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            bool mappedOk = SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
            bool panelDrawn = false, graphDrawn = false, outsideClean = true;
            if (mappedOk) {
                const BYTE* rows = (const BYTE*)mapped.pData;
                panelDrawn = *(const uint32_t*)(rows + 40 * mapped.RowPitch + 40 * 4) != 0;
                graphDrawn = *(const uint32_t*)(rows + 150 * mapped.RowPitch + 40 * 4) != 0;
                outsideClean = *(const uint32_t*)(rows + 400 * mapped.RowPitch + 400 * 4) == 0;
                context->Unmap(staging, 0);
            }
            check(mappedOk && panelDrawn, "the overlay writes its statistics panel");
            check(mappedOk && graphDrawn, "the overlay writes its pacing graph");
            check(mappedOk && outsideClean,
                  "the overlay leaves the rest of the frame untouched");
        }
        if (rtv) rtv->Release();
        if (staging) staging->Release();
        if (surface) surface->Release();
        tq::frameoverlay::releaseResources();
    }

    tq::frameoverlay::reset();
    tq::frameoverlay::readOptions(nullptr);
    check(!tq::frameoverlay::enabled(), "frame overlay stays off without an INI path");
    DeleteFileW(ini);
}

void testBloomShaders() {
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!compiler) compiler = LoadLibraryW(L"d3dcompiler_43.dll");
    typedef HRESULT(WINAPI* CompileFn)(
        LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
        LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    CompileFn compile = compiler
        ? (CompileFn)(void*)GetProcAddress(compiler, "D3DCompile") : nullptr;
    const char* sources[] = {
        kBloomExtractSource, kBloomDownsampleSource,
        kBloomUpsampleSource, kBloomCompositeSource
    };
    bool accepted = compile != nullptr;
    for (unsigned i = 0; accepted && i < sizeof(sources) / sizeof(sources[0]); ++i) {
        ID3DBlob *shader = nullptr, *errors = nullptr;
        HRESULT hr = compile(sources[i], strlen(sources[i]), "bloom-selftest",
                             nullptr, nullptr, "main", "ps_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                             &shader, &errors);
        accepted &= SUCCEEDED(hr) && shader && shader->GetBufferSize() > 0;
        if (shader) shader->Release();
        if (errors) errors->Release();
    }
    check(accepted, "compile all HDR bloom shaders with the runtime compiler");
    if (compiler) FreeLibrary(compiler);
}

void testShadowSplitRedirect() {
    const SIZE_T imageSize = 0x0044b000u;
    BYTE* image = (BYTE*)VirtualAlloc(nullptr, imageSize,
                                      MEM_RESERVE | MEM_COMMIT,
                                      PAGE_EXECUTE_READWRITE);
    check(image != nullptr, "allocate a synthetic Engine image");
    if (!image) return;
    memset(image, 0, imageSize);

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)image;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(image + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    // Timestamp is deliberately arbitrary: identical code repackaged with
    // different linker metadata must still be accepted.
    nt->FileHeader.TimeDateStamp = 0xdeadbeefu;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    nt->OptionalHeader.SizeOfImage = (DWORD)imageSize;

    const struct { DWORD rva; BYTE reg, opcode; } references[] = {
        {0x0018e40du, 0x15, 0x59}, {0x0018e42eu, 0x0d, 0x59},
        {0x0018e446u, 0x05, 0x59}, {0x0018e503u, 0x15, 0x59},
        {0x0018e51bu, 0x0d, 0x59}, {0x0018e533u, 0x05, 0x59},
        {0x0018e5ddu, 0x15, 0x59}, {0x0018e609u, 0x0d, 0x59},
        {0x0018e618u, 0x05, 0x59}, {0x0018e6fcu, 0x05, 0x10},
        {0x0018f556u, 0x0d, 0x10},
    };
    const unsigned count = sizeof(references) / sizeof(references[0]);
    uint32_t cropAddress = (uint32_t)(uintptr_t)(image + 0x002f9550u);
    for (unsigned i = 0; i < count; ++i) {
        BYTE* instruction = image + references[i].rva;
        instruction[0] = 0xf3; instruction[1] = 0x0f;
        instruction[2] = references[i].opcode;
        instruction[3] = references[i].reg;
        memcpy(instruction + 4, &cropAddress, sizeof(cropAddress));
    }

    check(tq::shadow::validateSupportedImageForTest((HMODULE)image),
          "accept the audited Engine layout with a different PE timestamp");
    check(tq::shadow::redirectCropRoundTripForTest((HMODULE)image),
          "redirect and restore all eleven crop operands exactly");

    // The fit stabiliser retargets one relative call rather than rewriting an
    // immediate, so the site is identified by its five literal bytes: a
    // relative displacement does not depend on where the image is loaded.
    const DWORD fitCallRva = 0x0018ec69u;
    const BYTE fitCall[5] = {0xe8, 0xc2, 0x51, 0xf9, 0xff};
    memcpy(image + fitCallRva, fitCall, sizeof(fitCall));
    const DWORD basisCallRva = 0x0018e7fau;
    const BYTE basisCall[5] = {0xe8, 0xf1, 0x55, 0x0f, 0x00};
    memcpy(image + basisCallRva, basisCall, sizeof(basisCall));
    check(tq::shadow::validateFitCameraCallForTest((HMODULE)image),
          "accept the audited Camera setup call site");
    check(tq::shadow::validateBasisCallForTest((HMODULE)image),
          "accept the audited light-basis call site");
    check(tq::shadow::retargetFitCameraCallRoundTripForTest((HMODULE)image),
          "retarget the Camera setup call to the thunk and restore it exactly");
    image[fitCallRva + 1] ^= 1;
    check(!tq::shadow::validateFitCameraCallForTest((HMODULE)image),
          "reject a Camera setup call with an unexpected target");
    image[fitCallRva + 1] ^= 1;
    image[basisCallRva + 1] ^= 1;
    check(!tq::shadow::validateBasisCallForTest((HMODULE)image),
          "reject a light-basis call with an unexpected target");
    image[basisCallRva + 1] ^= 1;

    image[references[6].rva + 3] ^= 1;
    check(!tq::shadow::validateSupportedImageForTest((HMODULE)image),
          "reject a near-match crop instruction");
    image[references[6].rva + 3] ^= 1;

    uint32_t wrong = cropAddress + 4;
    memcpy(image + references[10].rva + 4, &wrong, sizeof(wrong));
    check(!tq::shadow::validateSupportedImageForTest((HMODULE)image),
          "reject a crop read pointing at an unexpected constant");
    memcpy(image + references[10].rva + 4, &cropAddress, sizeof(cropAddress));

    nt->OptionalHeader.SizeOfImage = (DWORD)imageSize + 0x1000;
    check(!tq::shadow::validateSupportedImageForTest((HMODULE)image),
          "reject an Engine image of unexpected size");

    VirtualFree(image, 0, MEM_RELEASE);
}

void* readFile(const char* path, long* size) {
    *size = 0;
    FILE* file = fopen(path, "rb");
    if (!file) return nullptr;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    void* bytes = length > 0 ? malloc((size_t)length) : nullptr;
    if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        bytes = nullptr;
    } else {
        *size = length;
    }
    fclose(file);
    return bytes;
}

}  // namespace

// The fitted light camera as RenderDirectional lays it out on its stack. The
// offsets are the contract between this test and shadow_fix.cpp; keeping them
// spelled out here means a layout mistake fails the build rather than the game.
struct FitCameraImage {
    int32_t type;       // +0x00
    float basis[9];     // +0x04
    float position[3];  // +0x28
    float reserved;     // +0x34
    float extentRow0;   // +0x38
    float extentRow1;   // +0x3c
    float nearDepth;    // +0x40
    float farDepth;     // +0x44
};

FitCameraImage makeFit(float x, float y, float z, float e0, float e1) {
    FitCameraImage fit = {};
    fit.type = 1;
    fit.basis[0] = 1.0f; fit.basis[4] = 1.0f; fit.basis[8] = 1.0f;
    fit.position[0] = x; fit.position[1] = y; fit.position[2] = z;
    fit.extentRow0 = e0;
    fit.extentRow1 = e1;
    fit.farDepth = 300.0f;
    return fit;
}

void testShadowBasisReference() {
    const float fallback[3] = {9.0f, 9.0f, 9.0f};

    // A light mostly along -Y must be crossed with a world axis it is least
    // aligned with, or the cross product degenerates and the basis collapses.
    const float steep[3] = {0.10f, -0.98f, 0.17f};
    const float* up = tq::shadow::chooseReferenceUpForTest(steep, fallback);
    check(up && up[0] == 1.0f && up[1] == 0.0f && up[2] == 0.0f,
          "the pinned reference picks the axis the light is least aligned with");

    const float acrossX[3] = {0.97f, -0.20f, 0.14f};
    up = tq::shadow::chooseReferenceUpForTest(acrossX, fallback);
    check(up && up[1] == 0.0f && up[2] == 1.0f && up[0] == 0.0f,
          "a light along X is crossed with a different axis");

    // Determinism is the whole point: the same light must produce the same
    // basis every frame or nothing downstream can be snapped to it.
    const float* again = tq::shadow::chooseReferenceUpForTest(steep, fallback);
    check(again && again[0] == 1.0f && again[1] == 0.0f && again[2] == 0.0f,
          "the pinned reference is a function of the light alone");

    const float zero[3] = {0.0f, 0.0f, 0.0f};
    check(tq::shadow::chooseReferenceUpForTest(zero, fallback) == fallback,
          "a degenerate light direction keeps the engine's own reference");
    check(tq::shadow::chooseReferenceUpForTest(nullptr, fallback) == fallback,
          "a missing light direction keeps the engine's own reference");
}

void testShadowFitStabilizer() {
    const unsigned texels = 1024;
    const unsigned steps = 8;

    FitCameraImage fit = makeFit(123.456f, 77.75f, 5.0f, 100.0f, 60.0f);
    const FitCameraImage original = fit;
    tq::shadow::stabilizeFitForTest(&fit, texels, steps);

    check(fit.extentRow0 >= original.extentRow0
              && fit.extentRow1 >= original.extentRow1,
          "quantising the fit never shrinks the box below the tight fit");

    const double t0 = (double)fit.extentRow0 / texels;
    const double t1 = (double)fit.extentRow1 / texels;
    const double p0 = fit.position[0] / t0;
    const double p1 = fit.position[1] / t1;
    check(fabs(p0 - floor(p0 + 0.5)) < 1.0e-3
              && fabs(p1 - floor(p1 + 0.5)) < 1.0e-3,
          "the stabilised centre lands on the shadow map texel grid");

    // Snapping moves the box, so the quantised extent has to carry at least
    // that much slack or the fit would stop covering what it enclosed.
    check(fit.extentRow0 >= original.extentRow0 + 2.0 * t0
              && fit.extentRow1 >= original.extentRow1 + 2.0 * t1,
          "the quantised box still covers the tight fit after snapping");
    check(fabs(fit.position[0] - original.position[0]) <= t0 + 1.0e-4
              && fabs(fit.position[1] - original.position[1]) <= t1 + 1.0e-4,
          "snapping moves the centre by less than one texel");
    check(fit.position[2] == original.position[2],
          "snapping leaves the depth axis alone");

    // The point of the exercise: a camera that has crept a fraction of a texel
    // must produce the same grid, which is what stops shadow edges crawling.
    FitCameraImage crept = makeFit(123.456f + (float)(t0 * 0.3),
                                   77.75f + (float)(t1 * 0.4), 5.0f,
                                   100.0f, 60.0f);
    tq::shadow::stabilizeFitForTest(&crept, texels, steps);
    check(crept.extentRow0 == fit.extentRow0 && crept.extentRow1 == fit.extentRow1,
          "a sub-texel camera creep does not change the fitted extents");
    const double shift0 = (crept.position[0] - fit.position[0]) / t0;
    const double shift1 = (crept.position[1] - fit.position[1]) / t1;
    check(fabs(shift0 - floor(shift0 + 0.5)) < 1.0e-3
              && fabs(shift1 - floor(shift1 + 0.5)) < 1.0e-3,
          "a sub-texel camera creep moves the grid by whole texels only");

    // A basis the projection would not read as light-space axes is left alone
    // rather than moved onto a grid that is not where the texels are.
    FitCameraImage skewed = makeFit(123.456f, 77.75f, 5.0f, 100.0f, 60.0f);
    skewed.basis[0] = 2.0f;
    const FitCameraImage skewedBefore = skewed;
    tq::shadow::stabilizeFitForTest(&skewed, texels, steps);
    check(!memcmp(&skewed, &skewedBefore, sizeof(skewed)),
          "a non-orthonormal light basis is left untouched");

    FitCameraImage perspective = makeFit(123.456f, 77.75f, 5.0f, 100.0f, 60.0f);
    perspective.type = 0;
    const FitCameraImage perspectiveBefore = perspective;
    tq::shadow::stabilizeFitForTest(&perspective, texels, steps);
    check(!memcmp(&perspective, &perspectiveBefore, sizeof(perspective)),
          "a camera that is not the orthographic fit is left untouched");

    FitCameraImage degenerate = makeFit(1.0f, 2.0f, 3.0f, 0.0f, 60.0f);
    const FitCameraImage degenerateBefore = degenerate;
    tq::shadow::stabilizeFitForTest(&degenerate, texels, steps);
    check(!memcmp(&degenerate, &degenerateBefore, sizeof(degenerate)),
          "a degenerate fit extent is left untouched");
}

// Runs in its own fixture directory/process because production options and
// device hooks are initialized once. Verify the actual DLL's renderer patches,
// with every other Draw-hook consumer disabled.
int testGrassOnlyHooks(bool enhanced, bool rejectRenderer = false) {
    g_report = fopen("report.txt", "w");
    if (!g_report) return 99;
    FILE* ini = fopen("tqflicker.ini", "w");
    check(ini != nullptr, "create isolated grass-only configuration");
    if (ini) {
        fprintf(ini,
            "[graphics]\ngrass=%s\naa=fxaa\nshadows=original\n"
            "tonemap=original\nhdr=off\nbloom=original\nanisotropy=1\n"
            "[performance]\nstreaming=original\narchive_cache_mb=0\n"
            "loose_texture_max=0\nshadow_defer_cold_resources=0\n"
            "shadow_defer_cold_actor_pose=0\nterrain_preload_layers=0\n"
            "secondary_pass_admission_budget=0\n"
            "[debug]\ntrace=0\nperformance_trace=0\nframe_overlay=0\n",
            enhanced ? "enhanced" : "original");
        fclose(ini);
    }
    char path[MAX_PATH];
    GetFullPathNameA("winmm.dll", MAX_PATH, path, nullptr);
    HMODULE proxy = LoadLibraryA(path);
    GetFullPathNameA("Direct3D11.dll", MAX_PATH, path, nullptr);
    HMODULE host = LoadLibraryA(path);
    typedef HRESULT (*MakeDeviceFn)(ID3D11Device**, ID3D11DeviceContext**);
    MakeDeviceFn makeDevice = host
        ? (MakeDeviceFn)(void*)GetProcAddress(host, "make_device") : nullptr;
    if (rejectRenderer) {
        typedef BOOL (*PrepareFn)();
        PrepareFn prepare = host ? (PrepareFn)(void*)GetProcAddress(host, "prepare_draw_sites") : nullptr;
        bool rejected = prepare && prepare();
        BYTE* signature = (BYTE*)host + tq::rendererdraw::sites::kDrawWindowRva;
        DWORD protection = 0;
        rejected = rejected && VirtualProtect(signature, 1, PAGE_EXECUTE_READWRITE, &protection);
        if (rejected) {
            *signature ^= 0xff; // Outside the overwritten call site: reject the audited signature.
            DWORD ignored;
            VirtualProtect(signature, 1, protection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), signature, 1);
        }
        check(rejected, "prepare an unsupported renderer signature to exercise visual rollback");
    }
    Sleep(250);
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT result = makeDevice ? makeDevice(&device, &context) : E_FAIL;
    check(proxy && SUCCEEDED(result) && device && context,
          "create a device through the production grass-only installation path");
    if (proxy && context) {
        void** slots = *(void***)context;
        const unsigned indices[] = {12, 13, 14, 15};
        bool expected = true;
        for (unsigned slot : indices) {
            MEMORY_BASIC_INFORMATION info = {};
            expected &= VirtualQuery(slots[slot], &info, sizeof(info)) != 0
                && (info.AllocationBase == proxy) == (enhanced && !rejectRenderer && slot >= 14);
        }
        check(expected, rejectRenderer
            ? "visual rollback restores the native context slots"
            : enhanced
            ? "grass alone installs Map and Unmap while leaving native draw slots untouched"
            : "original grass leaves DrawIndexed, Draw, Map and Unmap unhooked when other consumers are off");
        using namespace tq::rendererdraw::sites;
        bool rendererPatched = true;
        const DWORD patchRvas[] = {kDrawWindowRva + kDrawPatchOffset,
                                  kIndexedWindowRva + kIndexedPatchOffset};
        for (DWORD rva : patchRvas) {
            BYTE* site = (BYTE*)host + rva;
            if (enhanced && !rejectRenderer) {
                void* target = site + 7 + *(int32_t*)(site + 3);
                MEMORY_BASIC_INFORMATION info = {};
                rendererPatched &= site[2] == 0xe8
                    && VirtualQuery(target, &info, sizeof(info)) && info.AllocationBase == proxy;
            } else rendererPatched &= site[0] == 0x8b && site[1] == 0x08;
        }
        check(rendererPatched, enhanced && !rejectRenderer ? "grass alone installs both production renderer submission hooks"
                                       : "original grass leaves renderer submission sites unchanged");
        MEMORY_BASIC_INFORMATION core = {};
        check(device && VirtualQuery((*(void***)device)[12], &core, sizeof(core))
              && core.AllocationBase == proxy,
              "the skinning shader hook remains installed regardless of optional visual activation");
    }
    if (context) context->Release();
    if (device) device->Release();
    if (proxy) FreeLibrary(proxy);
    if (host) FreeLibrary(host);
    check(GetFileAttributesA("tqflicker-debug.log") == INVALID_FILE_ATTRIBUTES
          && GetFileAttributesA("tqflicker-frames.csv") == INVALID_FILE_ATTRIBUTES,
          "grass-only hook installation does not enable tracing");
    fprintf(g_report, "\nRESULT: %d failure(s)\n", g_failures);
    fclose(g_report);
    return g_failures ? 1 : 0;
}

struct RecreationLifetime : IUnknown {
    LONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&refs); }
};

int testDeviceRecreation() {
    g_report = fopen("report.txt", "w");
    if (!g_report) return 99;
    FILE* ini = fopen("tqflicker.ini", "w");
    if (!ini) return 99;
    fprintf(ini, "[debug]\ntrace=1\nperformance_trace=full\n");
    fclose(ini);
    char path[MAX_PATH];
    GetFullPathNameA("Direct3D11.dll", MAX_PATH, path, nullptr);
    HMODULE host = LoadLibraryA(path);
    typedef BOOL (*PrepareFn)();
    typedef HRESULT (*CreateFn)(HWND, UINT, ID3D11Device**, ID3D11DeviceContext**, IDXGISwapChain**);
    typedef HRESULT (*PresentFn)(IDXGISwapChain*, BOOL);
    PrepareFn prepare = host ? (PrepareFn)(void*)GetProcAddress(host, "prepare_draw_sites") : nullptr;
    CreateFn create = host ? (CreateFn)(void*)GetProcAddress(host, "make_swap_chain") : nullptr;
    PresentFn present = host ? (PresentFn)(void*)GetProcAddress(host, "submit_present") : nullptr;
    bool prepared = prepare && prepare();
    GetFullPathNameA("winmm.dll", MAX_PATH, path, nullptr);
    HMODULE proxy = prepared ? LoadLibraryA(path) : nullptr;
    Sleep(250);
    HWND window = CreateWindowExA(0, "STATIC", "TQ recreation selftest", WS_OVERLAPPEDWINDOW,
        0, 0, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(prepared && proxy && create && present && window,
          "prepare the production swap-chain creation and Present fixture");
    long gammaSize = 0;
    void* gammaBytes = readFile("tq-dxbc-PS-gamma.dxbc", &gammaSize);
    const GUID lifetimeId = {0xaec28e07, 0x70f2, 0x462f, {0xb6,0x53,0x68,0x71,0x1a,0x12,0x98,0x2f}};
    RecreationLifetime chains[4], grasses[4];
    unsigned completed = 0;
    for (unsigned pass = 0; prepared && proxy && create && present && window && pass < 4; ++pass) {
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        IDXGISwapChain* chain = nullptr;
        HRESULT hr = create(window, pass % 2 ? 1 : 2, &device, &context, &chain);
        check(SUCCEEDED(hr) && device && context && chain,
              "create the next device through the production renderer import");
        if (FAILED(hr) || !device || !context || !chain) break;
        bool hooksReady = true;
        const unsigned deviceSlots[] = {3, 5, 7, 9, 12, 15, 23};
        const unsigned contextSlots[] = {8, 9, 14, 15, 33, 44, 45, 50};
        for (unsigned slot : deviceSlots) {
            MEMORY_BASIC_INFORMATION info = {};
            hooksReady &= VirtualQuery((*(void***)device)[slot], &info, sizeof(info))
                && info.AllocationBase == proxy;
        }
        for (unsigned slot : contextSlots) {
            MEMORY_BASIC_INFORMATION info = {};
            hooksReady &= VirtualQuery((*(void***)context)[slot], &info, sizeof(info))
                && info.AllocationBase == proxy;
        }
        check(hooksReady, "all required visual device/context hooks are installed on this generation");
        using namespace tq::rendererdraw::sites;
        bool drawReady = true;
        const DWORD drawRvas[] = {kDrawWindowRva + kDrawPatchOffset,
                                 kIndexedWindowRva + kIndexedPatchOffset};
        for (DWORD rva : drawRvas) {
            BYTE* site = (BYTE*)host + rva;
            void* target = site + 7 + *(int32_t*)(site + 3);
            MEMORY_BASIC_INFORMATION info = {};
            drawReady &= site[2] == 0xe8 && VirtualQuery(target, &info, sizeof(info))
                && info.AllocationBase == proxy;
        }
        const unsigned nativeDrawSlots[] = {12, 13};
        for (unsigned slot : nativeDrawSlots) {
            MEMORY_BASIC_INFORMATION info = {};
            drawReady &= VirtualQuery((*(void***)context)[slot], &info, sizeof(info))
                && info.AllocationBase != proxy;
        }
        check(drawReady, "both renderer draw hooks are installed without patching native Draw slots");
        if (pass) {
            check(chains[pass - 1].refs == 1,
                  "recreation releases the previous swap chain before returning the replacement");
            check(grasses[pass - 1].refs == 1,
                  "recreation releases the previous device's retained grass streams");
        }
        DXGI_SWAP_CHAIN_DESC desc = {};
        chain->GetDesc(&desc);
        check(desc.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
              && desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD && desc.BufferCount >= 2,
              "repeated buffer-count changes preserve FP16 flip presentation");
        check(SUCCEEDED(chain->SetPrivateDataInterface(lifetimeId, &chains[pass])),
              "attach a lifetime observer to the current swap chain");
        ID3D11PixelShader* gamma = nullptr;
        hr = gammaBytes ? device->CreatePixelShader(gammaBytes, gammaSize, nullptr, &gamma) : E_FAIL;
        bool replaced = false;
        for (unsigned wait = 0; gamma && wait < 250 && !replaced; ++wait) {
            context->PSSetShader(gamma, nullptr, 0);
            ID3D11PixelShader* active = nullptr;
            context->PSGetShader(&active, nullptr, nullptr);
            ID3D11Device* owner = nullptr;
            if (active) active->GetDevice(&owner);
            replaced = active && active != gamma && owner == device;
            if (owner) owner->Release();
            if (active) active->Release();
            if (!replaced) Sleep(20);
        }
        check(SUCCEEDED(hr) && replaced,
              "the replacement gamma shader belongs to the current device");
        context->PSSetShader(nullptr, nullptr, 0);
        if (gamma) gamma->Release();

        if (!pass) {
            HWND auxiliary = CreateWindowExA(0, "STATIC", "Auxiliary", WS_OVERLAPPEDWINDOW,
                0, 0, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            ID3D11Device* auxDevice = nullptr;
            ID3D11DeviceContext* auxContext = nullptr;
            IDXGISwapChain* auxChain = nullptr;
            HRESULT auxResult = auxiliary
                ? create(auxiliary, 1, &auxDevice, &auxContext, &auxChain) : E_FAIL;
            DXGI_SWAP_CHAIN_DESC auxDesc = {};
            if (auxChain) auxChain->GetDesc(&auxDesc);
            check(SUCCEEDED(auxResult) && auxDesc.BufferDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM
                  && chains[pass].refs == 2,
                  "an auxiliary window keeps its original output and leaves the primary chain tracked");
            if (auxChain) auxChain->Release();
            if (auxContext) auxContext->Release();
            if (auxDevice) auxDevice->Release();
            if (auxiliary) DestroyWindow(auxiliary);
        }

        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 44800; bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Buffer* grass = nullptr;
        hr = device->CreateBuffer(&bd, nullptr, &grass);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        bool mappedGrass = grass && SUCCEEDED(context->Map(grass, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        if (mappedGrass) {
            for (unsigned plane = 0; plane < 350; ++plane)
                memcpy((BYTE*)mapped.pData + plane * sizeof(kCapturedPlane), kCapturedPlane, sizeof(kCapturedPlane));
            context->Unmap(grass, 0);
        }
        check(SUCCEEDED(hr) && mappedGrass,
              "grass creation and Map/Unmap remain usable on the replacement device");
        if (grass) {
            grass->SetPrivateDataInterface(lifetimeId, &grasses[pass]);
            grass->Release();
            check(grasses[pass].refs == 2, "the current grass stream is retained by the new tracking table");
        }
        ID3D11Texture2D* back = nullptr;
        ID3D11RenderTargetView* target = nullptr;
        if (SUCCEEDED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)))
            device->CreateRenderTargetView(back, nullptr, &target);
        if (back) back->Release();
        check(target != nullptr, "create the replacement back-buffer view");
        if (target) {
            const float color[4] = {0.2f, 0.3f, 0.4f, 1.0f};
            for (unsigned frame = 0; frame < 3; ++frame) {
                context->OMSetRenderTargets(1, &target, nullptr);
                context->ClearRenderTargetView(target, color);
                check(SUCCEEDED(present(chain, pass % 2 == 0)),
                      "Present succeeds through the reinitialized FP16 callbacks");
            }
            target->Release();
        }
        check(device->GetDeviceRemovedReason() == S_OK,
              "recreation does not use resources from the retired device");
        // Leave the output bound, just as a renderer can do at teardown. The
        // mod must clear it and drop its own references before the next create.
        chain->Release(); context->Release(); device->Release();
        ++completed;
    }
    check(completed == 4, "complete repeated VSync-on/off-style recreations");
    free(gammaBytes);
    if (proxy) FreeLibrary(proxy);
    if (window) DestroyWindow(window);
    long logSize = 0;
    void* logBytes = readFile("tqflicker-debug.log", &logSize);
    char* log = (char*)calloc((size_t)logSize + 1, 1);
    if (log && logBytes) memcpy(log, logBytes, (size_t)logSize);
    check(log && logBytes && !strstr(log, "FP16 swap-chain attempt failed")
          && strstr(log, "Device recreation: old graphics state released"),
          "recreation trace confirms cleanup with no original-output fallback");
    free(log);
    free(logBytes);
    fprintf(g_report, "\nRESULT: %d failure(s)\n", g_failures);
    fclose(g_report);
    return g_failures ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--device-recreation"))
        return testDeviceRecreation();
    if (argc == 3 && !strcmp(argv[1], "--grass-hooks"))
        return testGrassOnlyHooks(strcmp(argv[2], "original") != 0,
                                  !strcmp(argv[2], "rollback"));
    const char* dll = argc > 1 ? argv[1] : "winmm.dll";
    const char* report = argc > 2 ? argv[2] : "C:\\tqflicker-selftest.txt";
    g_report = fopen(report, "w");
    if (!g_report) return 99;

    check(tq::streaming::optimizationEnabled(nullptr),
          "streaming optimization defaults on when the setting is absent");
    check(tq::streaming::optimizationEnabled(L"optimized"),
          "streaming=optimized enables progressive uploads");
    check(!tq::streaming::optimizationEnabled(L"original"),
          "streaming=original restores synchronous uploads");
    testTearingCapability();
    testRendererPresentHook();
    testRendererDrawHooks();
    testBloomHook();
    testGrassProbe();
    testDetour();
    testGrassPointerIndex();
    testGrassCrossed();
    testGrassBufferLifetime();
    testGrassCachePressure();
    testBloomExtraction();
    testBloomShaders();
    testShadowSplitRedirect();
    testShadowFitStabilizer();
    testShadowBasisReference();
    testTextureDimensions();
    testUpload();

    tq::hdr::Settings defaultHdr = tq::hdr::readSettings();
    check(defaultHdr.requestHdr && defaultHdr.toneMap == tq::hdr::ToneFrostbite
          && defaultHdr.paperWhiteNits == 203.0f
          && defaultHdr.peakNitsOverride == 0.0f
          && !defaultHdr.debug && !defaultHdr.trace,
          "HDR defaults to auto/frostbite/203 nits with diagnostics disabled");

    const tq::hdr::ToneMap outputModes[] = {
        tq::hdr::ToneAgx, tq::hdr::ToneFrostbite
    };
    bool sdrCurvesValid = true;
    bool hdrCurvesValid = true;
    for (unsigned mode = 0; mode < sizeof(outputModes) / sizeof(outputModes[0]); ++mode) {
        float previousSdr = -1.0f;
        float previousHdr = -1.0f;
        for (unsigned i = 0; i <= 1024; ++i) {
            float input = i * (32.0f / 1024.0f);
            float sdr = tq::hdr::toneMapLuminance(outputModes[mode], input, 1.0f);
            float hdr = tq::hdr::toneMapLuminance(outputModes[mode], input, 4.926108f);
            sdrCurvesValid &= sdr == sdr && sdr >= 0.0f && sdr <= 1.00001f
                           && sdr + 0.00001f >= previousSdr;
            hdrCurvesValid &= hdr == hdr && hdr >= 0.0f && hdr <= 4.92612f
                           && hdr + 0.00001f >= previousHdr;
            previousSdr = sdr;
            previousHdr = hdr;
        }
    }
    float agxWhite = tq::hdr::toneMapLuminance(tq::hdr::ToneAgx, 1.0f, 1.0f);
    float agxHighlight = tq::hdr::toneMapLuminance(tq::hdr::ToneAgx, 4.0f, 1.0f);
    float frostbiteMid = tq::hdr::toneMapLuminance(
        tq::hdr::ToneFrostbite, 0.5f, 1.0f);
    float frostbiteWhite = tq::hdr::toneMapLuminance(
        tq::hdr::ToneFrostbite, 1.0f, 1.0f);
    float frostbiteHighlight = tq::hdr::toneMapLuminance(
        tq::hdr::ToneFrostbite, 4.0f, 1.0f);
    check(sdrCurvesValid && agxWhite > 0.4f && agxWhite < 0.9f
          && frostbiteMid == 0.5f
          && frostbiteWhite > 0.90f && frostbiteWhite < 0.92f
          && agxHighlight > agxWhite && agxHighlight < 1.0f
          && frostbiteHighlight > frostbiteWhite && frostbiteHighlight < 1.0f,
          "all output curves monotonically roll extended highlights into SDR");
    float agxHdr = tq::hdr::toneMapLuminance(tq::hdr::ToneAgx, 4.0f, 4.926108f);
    float frostbiteHdrWhite = tq::hdr::toneMapLuminance(
        tq::hdr::ToneFrostbite, 1.0f, 4.926108f);
    float frostbiteHdr = tq::hdr::toneMapLuminance(
        tq::hdr::ToneFrostbite, 4.0f, 4.926108f);
    check(hdrCurvesValid && agxHdr > 1.0f && agxHdr < 4.926108f
          && frostbiteHdrWhite == 1.0f
          && frostbiteHdr > 3.9f && frostbiteHdr < 4.0f,
          "all output curves preserve extended luminance for HDR output");
    check(frostbiteWhite != agxWhite && frostbiteHighlight != agxHighlight,
          "AgX and Frostbite select different curves");

    unsigned char colorGrade[1288] = {};
    const unsigned char colorChecksum[16] = {
        0x15,0x07,0x85,0xe4,0xfb,0xb5,0xca,0x43,
        0x79,0xfc,0x92,0xf9,0x64,0x2c,0x0c,0x9b
    };
    memcpy(colorGrade, "DXBC", 4);
    memcpy(colorGrade + 4, colorChecksum, sizeof(colorChecksum));
    *(uint32_t*)(colorGrade + 24) = sizeof(colorGrade);
    memcpy(colorGrade + 64, "SceneColor", 11);
    memcpy(colorGrade + 96, "ColorLut", 9);
    check(tq::hdr::isColorGradingShader(colorGrade, sizeof(colorGrade)),
          "recognize the exact Titan Quest color-grading shader signature");
    colorGrade[4] ^= 1;
    check(!tq::hdr::isColorGradingShader(colorGrade, sizeof(colorGrade)),
          "reject a near-match color-grading shader signature");

    unsigned char gamma[1108] = {};
    const unsigned char gammaChecksum[16] = {
        0xa2,0x0f,0xf7,0xb0,0xe5,0x78,0x2f,0x87,
        0x20,0x5c,0x22,0x36,0xb1,0xf7,0xe2,0x05
    };
    memcpy(gamma, "DXBC", 4);
    memcpy(gamma + 4, gammaChecksum, sizeof(gammaChecksum));
    *(uint32_t*)(gamma + 24) = sizeof(gamma);
    memcpy(gamma + 64, "screenSampler", 14);
    memcpy(gamma + 96, "gammaSampler", 13);
    check(tq::hdr::isGammaShader(gamma, sizeof(gamma)),
          "recognize the exact Titan Quest gamma shader signature");
    gamma[24] ^= 1;
    check(!tq::hdr::isGammaShader(gamma, sizeof(gamma)),
          "reject a malformed gamma shader container");

    const uintptr_t viewportSlot = 0x12345678u;
    const uintptr_t frustumSlot = 0x23456789u;
    BYTE updateSignature[] = {
        0x68, 0x00, 0x03, 0x00, 0x00,
        0x68, 0x00, 0x04, 0x00, 0x00,
        0x6a, 0x00, 0x6a, 0x00,
        0x8d, 0x4c, 0x24, 0x18, 0xff, 0x15,
        0, 0, 0, 0,
        0x8d, 0x44, 0x24, 0x08, 0x50,
        0x8d, 0x84, 0x24, 0x5c, 0x06, 0x00, 0x00, 0x50,
        0x8d, 0x4c, 0x24, 0x20, 0xff, 0x15,
        0, 0, 0, 0,
        0xb9, 0x02, 0x01, 0x00, 0x00, 0x8b, 0xf0, 0xf3, 0xa5
    };
    memcpy(updateSignature + 20, &viewportSlot, sizeof(uint32_t));
    memcpy(updateSignature + 43, &frustumSlot, sizeof(uint32_t));
    BYTE signatureBuffer[160] = {};
    memcpy(signatureBuffer + 16, updateSignature, sizeof(updateSignature));
    unsigned matches = 0;
    const BYTE* callSite = tq::frustum::findUpdateViewportCall(
        signatureBuffer, sizeof(signatureBuffer), viewportSlot, frustumSlot, &matches);
    check(matches == 1 && callSite == signatureBuffer + 16 + 24,
          "find the unique fixed 4:3 entity-update frustum");
    signatureBuffer[16 + 55] ^= 1;
    callSite = tq::frustum::findUpdateViewportCall(
        signatureBuffer, sizeof(signatureBuffer), viewportSlot, frustumSlot, &matches);
    check(!callSite && matches == 0, "reject a near-match update-frustum signature");
    signatureBuffer[16 + 55] ^= 1;
    memcpy(signatureBuffer + 88, updateSignature, sizeof(updateSignature));
    callSite = tq::frustum::findUpdateViewportCall(
        signatureBuffer, sizeof(signatureBuffer), viewportSlot, frustumSlot, &matches);
    check(!callSite && matches == 2, "reject ambiguous update-frustum signatures");

    int selectedWidth = 0, selectedHeight = 0;
    bool expanded169 = tq::frustum::selectViewportSize(
        true, true, 1024, 768, 1920, 1080, &selectedWidth, &selectedHeight);
    check(expanded169 && selectedWidth == 1920 && selectedHeight == 1080,
          "expand entity updates to a 16:9 viewport");
    bool expanded219 = tq::frustum::selectViewportSize(
        true, true, 1024, 768, 3440, 1440, &selectedWidth, &selectedHeight);
    check(expanded219 && selectedWidth == 3440 && selectedHeight == 1440,
          "expand entity updates to a 21:9 viewport");
    bool expanded329 = tq::frustum::selectViewportSize(
        true, true, 1024, 768, 5120, 1440, &selectedWidth, &selectedHeight);
    check(expanded329 && selectedWidth == 5120 && selectedHeight == 1440,
          "replace the centered 4:3 update aspect with the full 32:9 aspect");
    bool expanded43 = tq::frustum::selectViewportSize(
        true, true, 1024, 768, 1600, 1200, &selectedWidth, &selectedHeight);
    check(!expanded43 && selectedWidth == 1024 && selectedHeight == 768,
          "retain the original update frustum at 4:3");
    bool wrongCaller = tq::frustum::selectViewportSize(
        true, false, 1024, 768, 3440, 1440, &selectedWidth, &selectedHeight);
    check(!wrongCaller && selectedWidth == 1024 && selectedHeight == 768,
          "leave identical viewport construction from other callers untouched");
    bool disabled = tq::frustum::selectViewportSize(
        false, true, 1024, 768, 3440, 1440, &selectedWidth, &selectedHeight);
    check(!disabled && selectedWidth == 1024 && selectedHeight == 768,
          "restore the original frustum when edge updates are disabled");
    bool invalid = tq::frustum::selectViewportSize(
        true, true, 1024, 768, 20000, 1440, &selectedWidth, &selectedHeight);
    check(!invalid && selectedWidth == 1024 && selectedHeight == 768,
          "reject invalid live display dimensions");

    check(tq::visual::isFp16SceneTargetOrdinal(5)
          && tq::visual::isFp16SceneTargetOrdinal(7)
          && tq::visual::isFp16SceneTargetOrdinal(9)
          && tq::visual::isFp16SceneTargetOrdinal(11)
          && tq::visual::isFp16SceneTargetOrdinal(12)
          && tq::visual::isFp16SceneTargetOrdinal(13)
          && !tq::visual::isFp16SceneTargetOrdinal(4)
          && !tq::visual::isFp16SceneTargetOrdinal(6)
          && !tq::visual::isFp16SceneTargetOrdinal(10)
          && !tq::visual::isFp16SceneTargetOrdinal(14),
          "keep every confirmed scene/post target, including the alternate gamma snapshot, in FP16");

    HMODULE proxy = LoadLibraryA(dll);
    check(proxy != nullptr, "load the winmm proxy");
    if (proxy) {
        static const char* const names[] = {
#define TQ_WINMM_NAME(name, required) name,
#include "winmm_names.inc"
#undef TQ_WINMM_NAME
        };
        bool complete = true;
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
            if (!GetProcAddress(proxy, names[i])) complete = false;
        check(complete, "all winmm exports are present");

        FARPROC address = GetProcAddress(proxy, "timeGetTime");
        typedef DWORD(WINAPI* TimeGetTimeFn)();
        DWORD before = address ? ((TimeGetTimeFn)address)() : 0;
        Sleep(30);
        DWORD after = address ? ((TimeGetTimeFn)address)() : 0;
        check(address && before && after >= before, "timeGetTime forwards to the real winmm");
    }

    HMODULE host = LoadLibraryA("C:\\tqflicker-selftest\\Direct3D11.dll");
    check(host != nullptr, "load the production-path Direct3D11 host");
    typedef HRESULT (*MakeDeviceFn)(ID3D11Device**, ID3D11DeviceContext**);
    MakeDeviceFn makeDevice = host
        ? (MakeDeviceFn)(void*)GetProcAddress(host, "make_device") : nullptr;
    check(makeDevice != nullptr, "find the host's device-creation entry point");

    // The production DLL polls every 10 ms because the game's renderer is
    // loaded after winmm. Give the same path ample time in this off-game test.
    Sleep(250);
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT result = makeDevice ? makeDevice(&device, &context) : E_FAIL;
    check(SUCCEEDED(result) && device && context,
          "create a 32-bit DXMT D3D11 device through the hooked host");
    if (device && proxy) {
        void* createVertexShader = (*(void***)device)[12];
        void* createTexture2D = (*(void***)device)[5];
        void* createPixelShader = (*(void***)device)[15];
        void* createSamplerState = (*(void***)device)[23];
        MEMORY_BASIC_INFORMATION info = {};
        bool queried = VirtualQuery(createVertexShader, &info, sizeof(info)) != 0;
        check(queried && info.AllocationBase == proxy,
              "CreateVertexShader is redirected into the minimal proxy");
        queried = VirtualQuery(createTexture2D, &info, sizeof(info)) != 0;
        check(queried && info.AllocationBase == proxy,
              "CreateTexture2D is redirected into the visual proxy");
        queried = VirtualQuery(createPixelShader, &info, sizeof(info)) != 0;
        check(queried && info.AllocationBase == proxy,
              "CreatePixelShader is redirected into the visual proxy");
        queried = VirtualQuery(createSamplerState, &info, sizeof(info)) != 0;
        check(queried && info.AllocationBase == proxy,
              "CreateSamplerState is redirected into the visual proxy");
        void* draw = (*(void***)context)[13];
        queried = VirtualQuery(draw, &info, sizeof(info)) != 0;
        check(queried && info.AllocationBase != proxy,
              "native Draw remains outside the visual proxy");
    }

    if (device) {
        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW
                             = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11SamplerState* sampler = nullptr;
        D3D11_SAMPLER_DESC observedSampler = {};
        HRESULT samplerResult = device->CreateSamplerState(&samplerDesc, &sampler);
        if (sampler) sampler->GetDesc(&observedSampler);
        check(SUCCEEDED(samplerResult) && sampler
              && observedSampler.Filter == D3D11_FILTER_ANISOTROPIC
              && observedSampler.MaxAnisotropy == 16,
              "trilinear wrap sampling is upgraded to 16x anisotropy");
        if (sampler) sampler->Release();

        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler = nullptr;
        memset(&observedSampler, 0, sizeof(observedSampler));
        samplerResult = device->CreateSamplerState(&samplerDesc, &sampler);
        if (sampler) sampler->GetDesc(&observedSampler);
        check(SUCCEEDED(samplerResult) && sampler
              && observedSampler.Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR,
              "clamped post-process sampling retains its original filter");
        if (sampler) sampler->Release();

        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler = nullptr;
        memset(&observedSampler, 0, sizeof(observedSampler));
        samplerResult = device->CreateSamplerState(&samplerDesc, &sampler);
        if (sampler) sampler->GetDesc(&observedSampler);
        check(SUCCEEDED(samplerResult) && sampler
              && observedSampler.Filter == D3D11_FILTER_MIN_MAG_MIP_POINT,
              "point sampling retains its original filter");
        if (sampler) sampler->Release();

        D3D11_TEXTURE2D_DESC shadow = {};
        shadow.Width = shadow.Height = 512;
        shadow.MipLevels = shadow.ArraySize = 1;
        shadow.Format = DXGI_FORMAT_R32_TYPELESS;
        shadow.SampleDesc.Count = 1;
        shadow.Usage = D3D11_USAGE_DEFAULT;
        shadow.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* texture = nullptr;
        HRESULT textureResult = E_FAIL;
        D3D11_TEXTURE2D_DESC actual = {};
        const UINT shadowSizes[] = {512, 1024, 2048};
        bool allShadowSizes = true;
        for (UINT i = 0; i < sizeof(shadowSizes) / sizeof(shadowSizes[0]); ++i) {
            shadow.Width = shadow.Height = shadowSizes[i];
            texture = nullptr;
            textureResult = device->CreateTexture2D(&shadow, nullptr, &texture);
            memset(&actual, 0, sizeof(actual));
            if (texture) texture->GetDesc(&actual);
            UINT scale = shadowSizes[i] >= 2048 ? kShadowScale : kPointShadowScale;
            UINT expected = shadowSizes[i] * scale;
            while (expected > 8192) expected /= 2;
            allShadowSizes &= SUCCEEDED(textureResult) && texture
                           && actual.Width == expected
                           && actual.Height == expected;
            if (texture) texture->Release();
        }
        check(allShadowSizes,
              "enhanced shadows scale Low/Medium/High map dimensions");

        shadow.Width = shadow.Height = 512;
        shadow.Format = DXGI_FORMAT_R24G8_TYPELESS;
        texture = nullptr;
        textureResult = device->CreateTexture2D(&shadow, nullptr, &texture);
        if (texture) texture->GetDesc(&actual);
        check(SUCCEEDED(textureResult) && texture && actual.Width == 512 && actual.Height == 512,
              "other square depth/SRV targets retain their requested dimensions");
        if (texture) texture->Release();

        shadow.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        shadow.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texture = nullptr;
        textureResult = device->CreateTexture2D(&shadow, nullptr, &texture);
        if (texture) texture->GetDesc(&actual);
        check(SUCCEEDED(textureResult) && texture && actual.Width == 512 && actual.Height == 512,
              "non-shadow square targets retain their requested dimensions");
        if (texture) texture->Release();

        // A water-reflection pass has both a color target and a square depth
        // target. Even if that depth texture resembles a shadow map, its
        // viewport must remain at the reflection target's dimensions.
        shadow.Format = DXGI_FORMAT_R32_TYPELESS;
        shadow.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* passDepth = nullptr;
        ID3D11DepthStencilView* passDSV = nullptr;
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        bool passTargets = SUCCEEDED(device->CreateTexture2D(&shadow, nullptr, &passDepth));
        if (passTargets) passTargets = SUCCEEDED(device->CreateDepthStencilView(
            passDepth, &dsvDesc, &passDSV));
        D3D11_TEXTURE2D_DESC passColorDesc = {};
        passColorDesc.Width = passColorDesc.Height = 512;
        passColorDesc.MipLevels = passColorDesc.ArraySize = 1;
        passColorDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        passColorDesc.SampleDesc.Count = 1;
        passColorDesc.Usage = D3D11_USAGE_DEFAULT;
        passColorDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D* passColor = nullptr;
        ID3D11RenderTargetView* passRTV = nullptr;
        if (passTargets) passTargets = SUCCEEDED(device->CreateTexture2D(
            &passColorDesc, nullptr, &passColor));
        if (passTargets) passTargets = SUCCEEDED(device->CreateRenderTargetView(
            passColor, nullptr, &passRTV));
        D3D11_VIEWPORT passViewport = {0, 0, 512, 512, 0, 1};
        if (passTargets && context) {
            context->RSSetViewports(1, &passViewport);
            context->OMSetRenderTargets(1, &passRTV, passDSV);
            UINT viewportCount = 1;
            D3D11_VIEWPORT observed = {};
            context->RSGetViewports(&viewportCount, &observed);
            check(viewportCount == 1 && observed.Width == 512 && observed.Height == 512,
                  "reflection color/depth passes keep their original viewport");
            context->OMSetRenderTargets(0, nullptr, passDSV);
            viewportCount = 1;
            context->RSGetViewports(&viewportCount, &observed);
            check(viewportCount == 1
                      && observed.Width == 512.0f * kPointShadowScale
                      && observed.Height == 512.0f * kPointShadowScale,
                  "depth-only shadow passes receive the scaled viewport");
            context->OMSetRenderTargets(0, nullptr, nullptr);
        } else {
            check(false, "create reflection/shadow viewport test targets");
            check(false, "run the depth-only shadow viewport test");
        }
        if (passRTV) passRTV->Release();
        if (passColor) passColor->Release();
        if (passDSV) passDSV->Release();
        if (passDepth) passDepth->Release();

        long fxaaSize = 0;
        void* fxaaBytes = readFile("C:\\tqflicker-selftest\\tq-dxbc-PS-fxaa.dxbc", &fxaaSize);
        ID3D11PixelShader* fxaa = nullptr;
        HRESULT fxaaResult = fxaaBytes ? device->CreatePixelShader(
            fxaaBytes, (SIZE_T)fxaaSize, nullptr, &fxaa) : E_FAIL;
        check(SUCCEEDED(fxaaResult) && fxaa,
              "the captured Titan Quest FXAA shader is accepted through the visual hook");
        if (fxaa && context) {
            Sleep(1000);
            context->PSSetShader(fxaa, nullptr, 0);
            ID3D11PixelShader* rebound = nullptr;
            context->PSGetShader(&rebound, nullptr, nullptr);
            check(rebound == fxaa, "the FXAA marker shader remains bindable before draw replacement");
            if (rebound) rebound->Release();

            UINT pixels[64];
            for (UINT i = 0; i < 64; ++i) pixels[i] = ((i + i / 8) & 1) ? 0xffffffffu : 0xff000000u;
            D3D11_TEXTURE2D_DESC color = {};
            color.Width = color.Height = 8; color.MipLevels = color.ArraySize = 1;
            color.Format = DXGI_FORMAT_R8G8B8A8_UNORM; color.SampleDesc.Count = 1;
            color.Usage = D3D11_USAGE_DEFAULT; color.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init = {pixels, 8 * sizeof(UINT), 0};
            ID3D11Texture2D *input = nullptr, *output = nullptr;
            ID3D11ShaderResourceView* inputView = nullptr;
            ID3D11RenderTargetView* outputView = nullptr;
            ID3D11VertexShader* fullscreenVS = nullptr;
            ID3D11InputLayout* fullscreenLayout = nullptr;
            ID3D11Buffer* fullscreenVB = nullptr;
            long vsSize = 0;
            void* vsBytes = readFile("C:\\tqflicker-selftest\\tq-dxbc-VS-fxaa.dxbc", &vsSize);
            bool rendered = SUCCEEDED(device->CreateTexture2D(&color, &init, &input));
            if (rendered) rendered = SUCCEEDED(device->CreateShaderResourceView(input, nullptr, &inputView));
            color.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (rendered) rendered = SUCCEEDED(device->CreateTexture2D(&color, nullptr, &output));
            if (rendered) rendered = SUCCEEDED(device->CreateRenderTargetView(output, nullptr, &outputView));
            if (rendered) rendered = vsBytes && SUCCEEDED(device->CreateVertexShader(
                vsBytes, (SIZE_T)vsSize, nullptr, &fullscreenVS));
            D3D11_INPUT_ELEMENT_DESC elements[2] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                 D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12,
                 D3D11_INPUT_PER_VERTEX_DATA, 0}
            };
            if (rendered) rendered = SUCCEEDED(device->CreateInputLayout(
                elements, 2, vsBytes, (SIZE_T)vsSize, &fullscreenLayout));
            struct Vertex { float x, y, z, u, v; } vertices[3] = {
                {-1, -1, 0, 0, 1}, {-1, 3, 0, 0, -1}, {3, -1, 0, 2, 1}
            };
            D3D11_BUFFER_DESC vbDesc = {};
            vbDesc.ByteWidth = sizeof(vertices); vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
            vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA vbData = {vertices, 0, 0};
            if (rendered) rendered = SUCCEEDED(device->CreateBuffer(&vbDesc, &vbData, &fullscreenVB));
            if (rendered) {
                FLOAT magenta[4] = {1, 0, 1, 1};
                D3D11_VIEWPORT vp = {0, 0, 8, 8, 0, 1};
                context->ClearRenderTargetView(outputView, magenta);
                context->OMSetRenderTargets(1, &outputView, nullptr);
                context->RSSetViewports(1, &vp);
                context->PSSetShaderResources(0, 1, &inputView);
                UINT stride = sizeof(Vertex), offset = 0;
                context->IASetInputLayout(fullscreenLayout);
                context->IASetVertexBuffers(0, 1, &fullscreenVB, &stride, &offset);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->VSSetShader(fullscreenVS, nullptr, 0);
                context->PSSetShader(fxaa, nullptr, 0);
                SubmitDrawFn submit = (SubmitDrawFn)(void*)GetProcAddress(host, "submit_draw");
                check(submit != nullptr, "resolve the renderer submission fixture for SMAA");
                if (submit) submit(context, 3);
                ID3D11PixelShader* restoredPS = nullptr;
                ID3D11ShaderResourceView* restoredSRV = nullptr;
                ID3D11RenderTargetView* restoredRTV = nullptr;
                UINT restoredViewportCount = 1;
                D3D11_VIEWPORT restoredViewport = {};
                context->PSGetShader(&restoredPS, nullptr, nullptr);
                context->PSGetShaderResources(0, 1, &restoredSRV);
                context->OMGetRenderTargets(1, &restoredRTV, nullptr);
                context->RSGetViewports(&restoredViewportCount, &restoredViewport);
                check(restoredPS == fxaa && restoredSRV == inputView && restoredRTV == outputView
                      && restoredViewportCount == 1 && restoredViewport.Width == 8,
                      "the AA replacement restores the game's pipeline state");
                if (restoredPS) restoredPS->Release();
                if (restoredSRV) restoredSRV->Release();
                if (restoredRTV) restoredRTV->Release();
                ID3D11ShaderResourceView* noView = nullptr;
                ID3D11RenderTargetView* noTarget = nullptr;
                context->PSSetShaderResources(0, 1, &noView);
                context->OMSetRenderTargets(1, &noTarget, nullptr);
                check(device->GetDeviceRemovedReason() == S_OK,
                      "the captured FXAA draw executes the three-pass AA pipeline");
            } else {
                check(false, "create the off-game AA render targets");
            }
            if (outputView) outputView->Release();
            if (inputView) inputView->Release();
            if (fullscreenVB) fullscreenVB->Release();
            if (fullscreenLayout) fullscreenLayout->Release();
            if (fullscreenVS) fullscreenVS->Release();
            if (output) output->Release();
            if (input) input->Release();
            free(vsBytes);
        }
        if (fxaa) fxaa->Release();

        long gradeSize = 0, gammaSize = 0;
        void* gradeBytes = readFile(
            "C:\\tqflicker-selftest\\tq-dxbc-PS-colorgrading.dxbc", &gradeSize);
        void* gammaBytes = readFile(
            "C:\\tqflicker-selftest\\tq-dxbc-PS-gamma.dxbc", &gammaSize);
        ID3D11PixelShader *gradeShader = nullptr, *gammaShader = nullptr;
        bool postShaders = gradeBytes && gammaBytes
            && SUCCEEDED(device->CreatePixelShader(gradeBytes, gradeSize, nullptr, &gradeShader))
            && SUCCEEDED(device->CreatePixelShader(gammaBytes, gammaSize, nullptr, &gammaShader));
        check(postShaders && gradeShader && gammaShader,
              "the exact color-grading and gamma shaders pass validation");
        if (postShaders && context) {
            Sleep(3000);
            context->PSSetShader(gradeShader, nullptr, 0);
            ID3D11PixelShader* reboundGrade = nullptr;
            context->PSGetShader(&reboundGrade, nullptr, nullptr);
            check(reboundGrade && reboundGrade != gradeShader,
                  "the enhanced color-grading pass is active by default");
            if (reboundGrade) reboundGrade->Release();
            context->PSSetShader(gammaShader, nullptr, 0);
            ID3D11PixelShader* reboundGamma = nullptr;
            context->PSGetShader(&reboundGamma, nullptr, nullptr);
            check(reboundGamma && reboundGamma != gammaShader,
                  "the Frostbite output transform is active by default");
            if (reboundGamma) reboundGamma->Release();
        } else {
            check(false, "replace the exact color-grading pass");
            check(false, "replace the exact gamma pass");
        }
        if (gradeShader) gradeShader->Release();
        if (gammaShader) gammaShader->Release();
        free(gradeBytes); free(gammaBytes);

        tq::dxbc::PatchResult notShadow = {};
        check(!tq::dxbc::enhanceShadowPcf(fxaaBytes, (SIZE_T)fxaaSize, &notShadow),
              "the shadow transformer rejects the FXAA shader");
        tq::dxbc::release(&notShadow);
        free(fxaaBytes);

        long shadowSize = 0;
        void* shadowBytes = readFile("C:\\tqflicker-selftest\\tq-dxbc-PS-shadow.dxbc", &shadowSize);
        tq::dxbc::PatchResult nearShadow = {};
        bool rejectedNearShadow = false;
        if (shadowBytes && shadowSize > 0) {
            unsigned char* nearBytes = (unsigned char*)malloc((size_t)shadowSize);
            memcpy(nearBytes, shadowBytes, (size_t)shadowSize);
            const char marker[] = "shadowBluriness";
            for (long i = 0; nearBytes && i + (long)sizeof(marker) <= shadowSize; ++i) {
                if (!memcmp(nearBytes + i, marker, sizeof(marker) - 1)) {
                    nearBytes[i] ^= 1;
                    break;
                }
            }
            rejectedNearShadow = !tq::dxbc::enhanceShadowPcf(
                nearBytes, (SIZE_T)shadowSize, &nearShadow);
            free(nearBytes);
        }
        check(rejectedNearShadow, "the shadow transformer rejects a near-match shader");
        tq::dxbc::release(&nearShadow);
        tq::dxbc::PatchResult shadowPatch = {};
        bool shadowChanged = shadowBytes && tq::dxbc::enhanceShadowPcf(
            shadowBytes, (SIZE_T)shadowSize, &shadowPatch);
        check(shadowChanged && shadowPatch.size == (SIZE_T)shadowSize,
              "transform one captured Titan Quest shadow receiver shader");
        if (shadowChanged) {
            ID3D11PixelShader* receiver = nullptr;
            HRESULT receiverResult = device->CreatePixelShader(
                shadowPatch.data, shadowPatch.size, nullptr, &receiver);
            check(SUCCEEDED(receiverResult) && receiver,
                  "DXMT accepts the enhanced shadow receiver shader");
            if (receiver) receiver->Release();
        }
        tq::dxbc::release(&shadowPatch);

        // The deferred screen-space receiver is the shader Titan Quest
        // actually uses to apply directional shadows. Its taps must be
        // retuned, and no other shader's may be: the per-material receivers
        // and the point-light one share the tap shape but were not widened.
        long deferredSize = 0;
        void* deferredBytes = readFile(
            "C:\\tqflicker-selftest\\tq-dxbc-PS-deferred-shadow.dxbc", &deferredSize);
        check(deferredBytes && deferredSize > 0,
              "read the captured deferred shadow receiver");
        tq::dxbc::PatchResult tuned = {};
        bool retuned = deferredBytes && tq::dxbc::tuneDeferredShadowFilter(
            deferredBytes, (SIZE_T)deferredSize, 0.38f, 0.695f, true, &tuned);
        check(retuned && tuned.size == (SIZE_T)deferredSize,
              "retune the deferred receiver's PCF taps in place");
        if (retuned && device) {
            ID3D11PixelShader* shader = nullptr;
            HRESULT hr = device->CreatePixelShader(tuned.data, tuned.size,
                                                   nullptr, &shader);
            check(SUCCEEDED(hr) && shader,
                  "DXMT accepts the retuned deferred receiver");
            if (shader) shader->Release();
        }
        tq::dxbc::release(&tuned);

        tq::dxbc::PatchResult widened = {};
        check(deferredBytes && !tq::dxbc::tuneDeferredShadowFilter(
                  deferredBytes, (SIZE_T)deferredSize, 1.5f, 1.0f, true, &widened),
              "refuse an offset scale that would widen the blur");
        tq::dxbc::PatchResult loosened = {};
        check(deferredBytes && !tq::dxbc::tuneDeferredShadowFilter(
                  deferredBytes, (SIZE_T)deferredSize, 0.38f, 1.5f, true, &loosened),
              "refuse a bias scale that would loosen the depth test");
        tq::dxbc::release(&loosened);
        tq::dxbc::release(&widened);

        tq::dxbc::PatchResult legacyTuned = {};
        check(shadowBytes && !tq::dxbc::tuneDeferredShadowFilter(
                  shadowBytes, (SIZE_T)shadowSize, 0.38f, 0.695f, true, &legacyTuned),
              "leave a per-material receiver's taps untouched");
        tq::dxbc::release(&legacyTuned);
        free(deferredBytes);
        free(shadowBytes);
    }

    testTimestampCapability(device, context);
    testProbe(device, context);
    testEngineProbe();
    testArchiveCache();
    testFrameOverlay(device, context);

    int transformed = 0;
    if (device) {
        for (int i = 3; i < argc; ++i) {
            long size;
            void* original = readFile(argv[i], &size);
            tq::dxbc::PatchResult patch = {};
            bool changed = original && tq::dxbc::clampBoneIndices(original, (SIZE_T)size, &patch);
            check(changed && patch.size == (SIZE_T)size + 40,
                  "transform one captured Titan Quest skinning shader");
            if (changed) {
                ID3D11VertexShader* shader = nullptr;
                HRESULT shaderResult = device->CreateVertexShader(
                    patch.data, patch.size, nullptr, &shader);
                check(SUCCEEDED(shaderResult) && shader,
                      "DXMT accepts the transformed shader");
                if (shader) shader->Release();
                ++transformed;
            }
            tq::dxbc::release(&patch);
            free(original);
        }
    }
    if (argc > 3)
        check(transformed == argc - 3, "all captured shader variants were transformed");

    if (context) context->Release();
    if (device) device->Release();
    // The proxy restores its IAT and vtable slots on an explicit unload, so the
    // host must remain mapped until after this call.
    if (proxy) FreeLibrary(proxy);
    if (host) FreeLibrary(host);

    check(GetFileAttributesA("tqflicker-hdr.log") == INVALID_FILE_ATTRIBUTES,
          "HDR logging creates no file when hdr_debug is absent");
    check(GetFileAttributesA("tqflicker-debug.log") == INVALID_FILE_ATTRIBUTES,
          "startup tracing creates no file when trace is absent");
    check(GetFileAttributesA("tqflicker-frames.csv") == INVALID_FILE_ATTRIBUTES,
          "the probe creates no CSV when probe is absent");
    fprintf(g_report, "\nRESULT: %d failure(s)\n", g_failures);
    fclose(g_report);
    return g_failures ? 1 : 0;
}
