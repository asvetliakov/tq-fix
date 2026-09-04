#include "visual.h"
#include "bloom_hook.h"
#include "grass.h"
#include "dxbc_patch.h"
#include "frame_overlay.h"
#include "hdr.h"
#include "probe.h"
#include "shadow_fix.h"
#include "engine_probe.h"
#include "engine.h"
#include "secondary_admission.h"

#include "streaming.h"
#include "upload.h"

#include <d3dcompiler.h>
#include <math.h>
#include <stdint.h>
#include <string>
#include <string.h>

#include "AreaTex.h"
#include "SearchTex.h"
#include "smaa_source.h"

namespace tq {
namespace visual {
namespace {

enum BloomMode { BloomOriginal, BloomEnhanced, BloomOff };

struct Options {
    bool smaa, shadows, streaming, bloomToggle;
    BloomMode bloom;
    float bloomStrength;
    UINT anisotropy;
    UINT shadowMapScale;
    UINT pointShadowMapScale;
    // Largest dimension a loose texture may have before it is refused and the
    // game's own archive copy is used instead. 0 leaves loose files alone,
    // which is the stock behaviour and the default.
    UINT looseTextureMax;
};
Options g_options = {true, true, true, false, BloomEnhanced, 0.85f, 16, 4, 2, 0};

// The smallest square the game requests for its directional shadow map. Point
// and spot maps are requested below this.
const UINT kDirectionalShadowSize = 2048;
bool g_bloomToggleKeyDown;
bool g_bloomEnhancedRuntime = true;
bool g_globalBloomEnabled;

struct Patch { void** slot; void* original; void* replacement; };
Patch g_patches[32];
int g_patchCount;
LONG g_installed;
LONG g_firstPresentLogged;

typedef HRESULT(WINAPI* CreateTexture2DFn)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                           const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
typedef HRESULT(WINAPI* CreatePixelShaderFn)(ID3D11Device*, const void*, SIZE_T,
                                             ID3D11ClassLinkage*, ID3D11PixelShader**);
typedef HRESULT(WINAPI* CreateBufferFn)(ID3D11Device*, const D3D11_BUFFER_DESC*,
                                        const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
typedef HRESULT(WINAPI* MapFn)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                               D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
typedef void (WINAPI* UnmapFn)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
typedef HRESULT(WINAPI* CreateSamplerStateFn)(ID3D11Device*, const D3D11_SAMPLER_DESC*,
                                              ID3D11SamplerState**);
typedef HRESULT(WINAPI* CreateShaderResourceViewFn)(ID3D11Device*, ID3D11Resource*,
                                                     const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                                     ID3D11ShaderResourceView**);
typedef HRESULT(WINAPI* CreateRenderTargetViewFn)(ID3D11Device*, ID3D11Resource*,
                                                   const D3D11_RENDER_TARGET_VIEW_DESC*,
                                                   ID3D11RenderTargetView**);
typedef void(WINAPI* PSSetShaderFn)(ID3D11DeviceContext*, ID3D11PixelShader*,
                                    ID3D11ClassInstance* const*, UINT);
typedef void(WINAPI* VSSetShaderFn)(ID3D11DeviceContext*, ID3D11VertexShader*,
                                    ID3D11ClassInstance* const*, UINT);
typedef void(WINAPI* PSSetShaderResourcesFn)(ID3D11DeviceContext*, UINT, UINT,
                                             ID3D11ShaderResourceView* const*);
typedef void(WINAPI* IASetVertexBuffersFn)(ID3D11DeviceContext*, UINT, UINT,
                                           ID3D11Buffer* const*, const UINT*,
                                           const UINT*);
typedef void(WINAPI* IASetIndexBufferFn)(ID3D11DeviceContext*, ID3D11Buffer*,
                                         DXGI_FORMAT, UINT);
typedef void(WINAPI* DrawFn)(ID3D11DeviceContext*, UINT, UINT);
typedef void(WINAPI* DrawIndexedFn)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void(WINAPI* ClearRenderTargetViewFn)(ID3D11DeviceContext*,
                                              ID3D11RenderTargetView*,
                                              const FLOAT[4]);
typedef void(WINAPI* OMSetRenderTargetsFn)(ID3D11DeviceContext*, UINT,
                                           ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
typedef void(WINAPI* RSSetViewportsFn)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
typedef void(WINAPI* RSSetScissorsFn)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);
typedef void(WINAPI* UpdateSubresourceFn)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                          const D3D11_BOX*, const void*, UINT, UINT);

CreateTexture2DFn g_createTexture2D;
CreatePixelShaderFn g_createPixelShader;
CreateSamplerStateFn g_createSamplerState;
CreateBufferFn       g_createBuffer;
bool                 g_grassEnhanced;
MapFn                g_map;
UnmapFn              g_unmap;
CreateShaderResourceViewFn g_createShaderResourceView;
CreateRenderTargetViewFn g_createRenderTargetView;
PSSetShaderFn g_psSetShader;
VSSetShaderFn g_vsSetShader;
PSSetShaderResourcesFn g_psSetShaderResources;
IASetVertexBuffersFn g_iaSetVertexBuffers;
IASetIndexBufferFn g_iaSetIndexBuffer;
DrawFn g_draw;
DrawIndexedFn g_drawIndexed;
ClearRenderTargetViewFn g_clearRenderTargetView;
OMSetRenderTargetsFn g_omSetRenderTargets;
RSSetViewportsFn g_rsSetViewports;
RSSetScissorsFn g_rsSetScissors;
UpdateSubresourceFn g_updateSubresource;

tq::engineprobe::DeferredDrawBindings g_deferredBindings;
bool g_deferredBindingTracing;

ID3D11Device* g_device;
ID3D11DeviceContext* g_context;
IDXGISwapChain* g_swapChain;
void* g_backBufferIdentity;
UINT g_backBufferWidth, g_backBufferHeight;
ID3D11PixelShader* g_fxaa[8];
unsigned g_fxaaCount;
ID3D11PixelShader* g_colorGrading[8];
unsigned g_colorGradingCount;
ID3D11PixelShader* g_gamma[8];
unsigned g_gammaCount;
ID3D11PixelShader* g_alphaClampCopies[8];
unsigned g_alphaClampCopyCount;
ID3D11PixelShader* g_gamePixelShader;
bool g_fxaaBound;
bool g_colorGradingBound;
bool g_gammaBound;
bool g_backBufferNeedsRestore;
UINT g_flipBackBufferSlots;
LONG g_firstFlipOutputRestoreLogged;

struct ScreenTargetInfo {
    void* identity;
    unsigned id;
    D3D11_TEXTURE2D_DESC desc;
    bool upgraded;
};
ScreenTargetInfo g_screenTargets[32];
unsigned g_screenTargetCount;

struct PostProcessBinding {
    unsigned stage;
    void* source;
    void* destination;
};
PostProcessBinding g_postProcessBindings[24];
unsigned g_postProcessBindingCount;
bool g_inside;
LONG g_programState;  // 0 idle, 1 building, 2 ready, 3 failed
HANDLE g_programThread;
HMODULE g_compiler;

// The job pool, the chunk controller and the substitution table live in
// src/upload.cpp. What stays here is what is specific to this game: which
// textures are candidates, which File object owns their bytes, and the
// mapping lease that keeps a loose file's view alive while a job runs.
const unsigned kMaxMappingLeases = 128;

typedef void (__thiscall* ArchiveUnmapFn)(void*);
ArchiveUnmapFn g_archiveUnmap;
LONG g_archiveVtablePatched;

struct MappingLease {
    bool used;
    bool sealed;
    void* source;
    void* mappedBase;
    LONG jobs;
    // What the view costs in address space, so the pool can be judged in bytes
    // rather than in slots. FileDirectory keeps its length at +0x28, which is
    // all GetLength (`8b 41 28 c3`) returns.
    unsigned bytes;
};
// Summed over live leases and sampled once a frame into upload_leased_mib.
volatile LONG g_leasedBytes;

MappingLease g_mappingLeases[kMaxMappingLeases];

bool upgradedIdentity(void* identity);

struct ShadowTexture {
    void* identity;
    UINT originalWidth, originalHeight;
    UINT scale;
};
// The engine allocates nine shadow targets per CreateRenderTargets -- one
// directional and eight point/spot -- and calls it again on a resolution or
// shadow-quality change. Sixteen slots overflowed on the second call, and an
// unrecorded map is still created enlarged while its viewport is left at the
// size the game asked for, so the pass renders into a quarter of it. The table
// is also cleared when the renderer rebuilds its targets, which is what stops
// stale identities accumulating.
ShadowTexture g_shadowTextures[48];
unsigned g_shadowTextureCount;
LONG g_shadowTableFullLogged;
// Which kind of shadow map is bound, so the probe charges the directional pass
// and the point passes separately.
bool g_shadowDirectional;
bool g_shadowBound;
UINT g_shadowScale = 1;
D3D11_VIEWPORT g_gameViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
UINT g_gameViewportCount;
D3D11_RECT g_gameScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
UINT g_gameScissorCount;

struct SmaaResources {
    UINT width, height;
    ID3D11PixelShader* edgePS;
    ID3D11PixelShader* weightPS;
    ID3D11PixelShader* blendPS;
    ID3D11Texture2D* edgeTex;
    ID3D11RenderTargetView* edgeRTV;
    ID3D11ShaderResourceView* edgeSRV;
    ID3D11Texture2D* weightTex;
    ID3D11RenderTargetView* weightRTV;
    ID3D11ShaderResourceView* weightSRV;
    ID3D11Texture2D* areaTex;
    ID3D11ShaderResourceView* areaSRV;
    ID3D11Texture2D* searchTex;
    ID3D11ShaderResourceView* searchSRV;
    ID3D11Buffer* metrics;
    ID3D11SamplerState* linearSampler;
    ID3D11SamplerState* pointSampler;
    ID3D11BlendState* blend;
    ID3D11DepthStencilState* depth;
    ID3D11RasterizerState* raster;
} g_smaa;

struct HdrResources {
    ID3D11PixelShader* colorGradingPS;
    ID3D11PixelShader* gammaPS;
    ID3D11PixelShader* tonePS;
    ID3D11PixelShader* presentPS;
    ID3D11PixelShader* alphaClampPS;
    ID3D11VertexShader* fullscreenVS;
    ID3D11Texture2D* presentCopy;
    ID3D11ShaderResourceView* presentCopySRV;
    ID3D11ShaderResourceView* backBufferSRV;
    ID3D11RenderTargetView* backBufferRTV;
    ID3D11SamplerState* sampler;
    ID3D11BlendState* blend;
    ID3D11DepthStencilState* depth;
    ID3D11RasterizerState* raster;
    UINT width, height;
} g_hdr;

const unsigned kBloomMaxLevels = 5;
const unsigned kBloomTimingSlots = 4;

struct BloomTiming {
    ID3D11Query* disjoint;
    ID3D11Query* begin;
    ID3D11Query* end;
    bool pending;
};

struct BloomResources {
    ID3D11PixelShader* extractPS;
    ID3D11PixelShader* downsamplePS;
    ID3D11PixelShader* upsamplePS;
    ID3D11PixelShader* compositePS;
    ID3D11Buffer* constants;
    ID3D11SamplerState* sampler;
    ID3D11BlendState* opaqueBlend;
    ID3D11BlendState* additiveBlend;
    ID3D11DepthStencilState* depth;
    ID3D11RasterizerState* raster;
    ID3D11Texture2D* downTexture;
    ID3D11Texture2D* upTexture;
    ID3D11ShaderResourceView* downSRV[kBloomMaxLevels];
    ID3D11RenderTargetView* downRTV[kBloomMaxLevels];
    ID3D11ShaderResourceView* upSRV[kBloomMaxLevels];
    ID3D11RenderTargetView* upRTV[kBloomMaxLevels];
    BloomTiming timing[kBloomTimingSlots];
    UINT width, height, levels;
    DXGI_FORMAT format;
    UINT rejectedWidth, rejectedHeight;
    unsigned timingCursor, timingSamples;
    double timingTotalMs, timingMaxMs;
    unsigned calls;
    bool freshForPresent;
    bool rejectionLogged;
} g_bloom;


template <typename T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool readable(const void* address) {
    MEMORY_BASIC_INFORMATION info;
    if (!address || !VirtualQuery(address, &info, sizeof(info))) return false;
    DWORD p = info.Protect & 0xff;
    return info.State == MEM_COMMIT && !(info.Protect & PAGE_GUARD) && p != PAGE_NOACCESS;
}

bool patchSlot(void** slot, void* replacement, void** original) {
    if (!slot || !replacement || !readable(slot) || !readable(*slot)) return false;
    DWORD oldProtection;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtection)) return false;
    void* old = InterlockedExchangePointer((PVOID volatile*)slot, replacement);
    DWORD ignored;
    VirtualProtect(slot, sizeof(*slot), oldProtection, &ignored);
    if (original) *original = old;
    if (old != replacement && g_patchCount < (int)(sizeof(g_patches) / sizeof(g_patches[0])))
        g_patches[g_patchCount++] = {slot, old, replacement};
    return true;
}

void restoreSlots() {
    for (int i = g_patchCount - 1; i >= 0; --i) {
        Patch& p = g_patches[i];
        if (!readable(p.slot) || *p.slot != p.replacement) continue;
        DWORD oldProtection;
        if (!VirtualProtect(p.slot, sizeof(*p.slot), PAGE_READWRITE, &oldProtection)) continue;
        InterlockedExchangePointer((PVOID volatile*)p.slot, p.original);
        DWORD ignored;
        VirtualProtect(p.slot, sizeof(*p.slot), oldProtection, &ignored);
    }
    g_patchCount = 0;
}

void readOptions() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    lstrcpyW(slash + 1, L"tqflicker.ini");
    wchar_t value[32];
    GetPrivateProfileStringW(L"graphics", L"aa", L"smaa", value, 32, path);
    g_options.smaa = _wcsicmp(value, L"fxaa") != 0;
    GetPrivateProfileStringW(L"graphics", L"shadows", L"enhanced", value, 32, path);
    g_options.shadows = _wcsicmp(value, L"original") != 0;
    GetPrivateProfileStringW(L"graphics", L"bloom", L"enhanced", value, 32, path);
    g_options.bloom = !_wcsicmp(value, L"original") ? BloomOriginal
                    : !_wcsicmp(value, L"off") ? BloomOff : BloomEnhanced;
    GetPrivateProfileStringW(L"graphics", L"bloom_strength", L"0.85",
                             value, 32, path);
    wchar_t* strengthEnd = nullptr;
    double bloomStrength = wcstod(value, &strengthEnd);
    g_options.bloomStrength = strengthEnd != value && *strengthEnd == 0
                           && isfinite(bloomStrength)
                           && bloomStrength >= 0.0 && bloomStrength <= 4.0
                            ? (float)bloomStrength : 0.85f;
    g_options.bloomToggle = GetPrivateProfileIntW(
        L"debug", L"bloom_toggle", 0, path) != 0;
    GetPrivateProfileStringW(L"performance", L"streaming", L"optimized",
                             value, 32, path);
    g_options.streaming = tq::streaming::optimizationEnabled(value);
    // A texture pack that ships assets larger than the engine ever asks for is
    // not a rendering problem, it is a loading one: this install carries 984
    // loose textures over 4096 on a side, up to 16384x16384, and between them
    // they are 46% of the pack's bytes. Refusing those hands the game its own
    // archive copy, which for the same assets is 6.4% of the size.
    int looseMax = GetPrivateProfileIntW(L"performance", L"loose_texture_max",
                                         4096, path);
    g_options.looseTextureMax = looseMax >= 64 && looseMax <= 16384
                              ? (UINT)looseMax : 0;
    if (g_options.streaming && !tq::streaming::presentHookInstalled()) {
        g_options.streaming = false;
        tq::hdr::log("Progressive streaming disabled: renderer Present hook unavailable\r\n");
    }
    tq::frameoverlay::readOptions(path);
    tq::probe::readOptions(path);
    tq::engine::readOptions(path);
    int anisotropy = GetPrivateProfileIntW(L"graphics", L"anisotropy", 16, path);
    g_options.anisotropy = anisotropy == 1 ? 1
                         : anisotropy >= 2 && anisotropy <= 16 ? (UINT)anisotropy : 16;
    // A wider shadow split spreads the map over more world, so the map has to
    // grow with it to keep texel density. Powers of two only; the scale
    // multiplies the square size the game asks for.
    //
    // The default costs real GPU time and the cost is measured, not guessed:
    // on a 5120x1440 display the directional pass takes 3.73 ms a frame at 4
    // and 2.31 ms at 2, out of frames averaging 13.9 ms. The default stays at
    // 4 because 2 visibly softens the shadows; `shadow_map_scale=2` is the
    // documented way to buy the 1.4 ms back.
    int shadowScale = GetPrivateProfileIntW(L"graphics", L"shadow_map_scale", 4, path);
    g_options.shadowMapScale = shadowScale == 1 ? 1
                             : shadowScale == 2 ? 2
                             : shadowScale == 8 ? 8 : 4;
    // Point and spot maps gain nothing from the wider directional split, so
    // they scale separately and more modestly.
    int pointScale = GetPrivateProfileIntW(L"graphics", L"shadow_point_map_scale",
                                           2, path);
    g_options.pointShadowMapScale = pointScale == 1 ? 1
                                  : pointScale == 4 ? 4
                                  : pointScale == 8 ? 8 : 2;
}

bool contains(const BYTE* bytes, SIZE_T size, const char* text) {
    SIZE_T n = strlen(text);
    if (!n || n > size) return false;
    for (SIZE_T i = 0; i <= size - n; ++i)
        if (!memcmp(bytes + i, text, n)) return true;
    return false;
}

bool isFxaa(const void* bytecode, SIZE_T size) {
    static const BYTE checksum[16] = {
        0x9f, 0x62, 0xb0, 0xf1, 0x3e, 0xba, 0xb2, 0xa9,
        0x3b, 0xf7, 0xa7, 0x70, 0x97, 0x0a, 0x4d, 0x39
    };
    if (!bytecode || size != 7208 || memcmp(bytecode, "DXBC", 4)
        || memcmp((const BYTE*)bytecode + 4, checksum, sizeof(checksum))
        || *(const uint32_t*)((const BYTE*)bytecode + 24) != size) return false;
    const BYTE* b = (const BYTE*)bytecode;
    return contains(b, size, "AASettings") && contains(b, size, "PixelStep")
        && contains(b, size, "texFrame");
}

bool isAlphaClampCopy(const void* bytecode, SIZE_T size) {
    static const BYTE checksum[16] = {
        0x47, 0x92, 0x15, 0x61, 0x8a, 0xaf, 0xca, 0xb4,
        0x1c, 0x14, 0x61, 0xbc, 0x45, 0x1c, 0x23, 0x5a
    };
    return bytecode && size == 748 && !memcmp(bytecode, "DXBC", 4)
        && !memcmp((const BYTE*)bytecode + 4, checksum, sizeof(checksum))
        && *(const uint32_t*)((const BYTE*)bytecode + 24) == size
        && contains((const BYTE*)bytecode, size, "baseSamplerTex");
}

void* identityOf(IUnknown* object) {
    if (!object) return nullptr;
    IUnknown* identity = nullptr;
    if (FAILED(object->QueryInterface(__uuidof(IUnknown), (void**)&identity)) || !identity)
        return nullptr;
    void* result = identity;
    identity->Release();
    return result;
}

ScreenTargetInfo* screenTarget(void* identity) {
    if (!identity) return nullptr;
    for (unsigned i = 0; i < g_screenTargetCount; ++i)
        if (g_screenTargets[i].identity == identity) return &g_screenTargets[i];
    return nullptr;
}

unsigned screenTargetId(void* identity) {
    if (!identity) return 0;
    if (identity == g_backBufferIdentity) return 1000;
    ScreenTargetInfo* target = screenTarget(identity);
    return target ? target->id : 0;
}

void tracePostProcessBinding(ID3D11DeviceContext* context) {
    unsigned stage = g_colorGradingBound ? 1u : g_gammaBound ? 2u : g_fxaaBound ? 3u : 0u;
    if (!tq::hdr::runtime().settings.debug || !stage || !context
        || g_postProcessBindingCount >= 24) return;
    ID3D11ShaderResourceView* sourceView = nullptr;
    ID3D11RenderTargetView* destinationView = nullptr;
    context->PSGetShaderResources(0, 1, &sourceView);
    context->OMGetRenderTargets(1, &destinationView, nullptr);
    ID3D11Resource *sourceResource = nullptr, *destinationResource = nullptr;
    if (sourceView) sourceView->GetResource(&sourceResource);
    if (destinationView) destinationView->GetResource(&destinationResource);
    void* source = identityOf(sourceResource);
    void* destination = identityOf(destinationResource);
    for (unsigned i = 0; i < g_postProcessBindingCount; ++i) {
        const PostProcessBinding& seen = g_postProcessBindings[i];
        if (seen.stage == stage && seen.source == source && seen.destination == destination) {
            release(sourceResource); release(destinationResource);
            release(sourceView); release(destinationView);
            return;
        }
    }
    D3D11_TEXTURE2D_DESC sourceDesc = {}, destinationDesc = {};
    ID3D11Texture2D *sourceTexture = nullptr, *destinationTexture = nullptr;
    if (sourceResource) sourceResource->QueryInterface(
        __uuidof(ID3D11Texture2D), (void**)&sourceTexture);
    if (destinationResource) destinationResource->QueryInterface(
        __uuidof(ID3D11Texture2D), (void**)&destinationTexture);
    if (sourceTexture) sourceTexture->GetDesc(&sourceDesc);
    if (destinationTexture) destinationTexture->GetDesc(&destinationDesc);
    g_postProcessBindings[g_postProcessBindingCount++] = {stage, source, destination};
    const char* name = stage == 1 ? "color" : stage == 2 ? "gamma" : "fxaa";
    tq::hdr::log("Post binding: stage=%s src=%u fmt=%u dst=%u fmt=%u targets=%u\r\n",
        name, screenTargetId(source), (unsigned)sourceDesc.Format,
        screenTargetId(destination), (unsigned)destinationDesc.Format,
        g_screenTargetCount);
    release(sourceTexture); release(destinationTexture);
    release(sourceResource); release(destinationResource);
    release(sourceView); release(destinationView);
}

// The one identity-match over the shadow table; every question about a bound
// map (is it ours, what scale, directional or point) reads fields off the
// entry this returns, so the matching rule cannot fork.
const ShadowTexture* shadowTextureForIdentity(void* identity) {
    for (unsigned i = 0; i < g_shadowTextureCount; ++i)
        if (g_shadowTextures[i].identity == identity)
            return &g_shadowTextures[i];
    return nullptr;
}

bool shadowDepthDesc(const D3D11_TEXTURE2D_DESC* d) {
    if (!d || d->Width != d->Height || d->MipLevels != 1 || d->ArraySize != 1
        || d->SampleDesc.Count != 1 || d->Usage != D3D11_USAGE_DEFAULT
        || d->CPUAccessFlags || d->MiscFlags) return false;
    if (d->Width != 256 && d->Width != 512 && d->Width != 1024
        && d->Width != 2048 && d->Width != 4096) return false;
    const UINT need = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    return d->BindFlags == need && d->Format == DXGI_FORMAT_R32_TYPELESS;
}

bool blockCompressedFormat(DXGI_FORMAT format, UINT* blockBytes) {
    UINT bytes = 0;
    switch (format) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
            bytes = 8;
            break;
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
            bytes = 16;
            break;
        default:
            return false;
    }
    if (blockBytes) *blockBytes = bytes;
    return true;
}

struct TextureOwner {
    void* source;
    void* mappedBase;
    void** vtable;
};

bool findTextureOwner(const void* dds, TextureOwner* owner) {
    if (!dds || !owner) return false;
    BYTE* engine = (BYTE*)GetModuleHandleW(L"Engine.dll");
    if (!engine || !readable(engine + 0x213cad)) return false;
    const uintptr_t parserReturn = (uintptr_t)(engine + 0x213cad);

    uintptr_t* stack = nullptr;
    __asm__ __volatile__("movl %%esp, %0" : "=r"(stack));
    MEMORY_BASIC_INFORMATION memory = {};
    if (!stack || !VirtualQuery(stack, &memory, sizeof(memory))
        || memory.State != MEM_COMMIT) return false;
    const BYTE* regionEnd = (const BYTE*)memory.BaseAddress + memory.RegionSize;
    const BYTE* scanEnd = (const BYTE*)stack + 16 * 1024;
    if (scanEnd > regionEnd) scanEnd = regionEnd;

    for (uintptr_t* slot = stack; (const BYTE*)(slot + 8) <= scanEnd; ++slot) {
        if (*slot != parserReturn) continue;
        BYTE* source = (BYTE*)slot[7];
        if (!readable(source) || !readable(source + 0x34)) continue;
        void** vtable = *(void***)source;
        if (!readable(vtable + 4) || vtable != (void**)(engine + 0x2f71ec)
            || vtable[2] != engine + 0x14e560) continue;
        const BYTE* data = (const BYTE*)slot[1];
        uint32_t bytes = (uint32_t)slot[2];
        if (!data || !bytes || (const BYTE*)dds < data
            || (const BYTE*)dds >= data + bytes) continue;
        owner->source = source;
        owner->mappedBase = *(void**)(source + 0x34);
        owner->vtable = vtable;
        return true;
    }
    return false;
}

MappingLease* findLease(void* source) {
    MappingLease* sealed = nullptr;
    for (unsigned i = 0; i < kMaxMappingLeases; ++i) {
        MappingLease& lease = g_mappingLeases[i];
        if (!lease.used || lease.source != source) continue;
        if (!lease.sealed) return &lease;
        sealed = &lease;
    }
    return sealed;
}

MappingLease* createLease(void* source, void* mappedBase) {
    if (!source || !mappedBase) return nullptr;
    for (unsigned i = 0; i < kMaxMappingLeases; ++i) {
        MappingLease& lease = g_mappingLeases[i];
        if (lease.used) continue;
        lease.used = true;
        lease.sealed = false;
        lease.source = source;
        lease.mappedBase = mappedBase;
        lease.jobs = 0;
        return &lease;
    }
    return nullptr;
}

void __fastcall hookArchiveUnmap(void* source, void*) {
    bool retained = false;
    void* unmap = nullptr;
    if (tq::upload::ready()) {
        tq::upload::lock();
        MappingLease* lease = findLease(source);
        if (lease && *(void**)((BYTE*)source + 0x34) == nullptr) {
            lease->sealed = true;
            retained = true;
            if (!lease->jobs) {
                unmap = lease->mappedBase;
                InterlockedExchangeAdd(&g_leasedBytes, -(LONG)lease->bytes);
                memset(lease, 0, sizeof(*lease));
            }
        }
        tq::upload::unlock();
    }
    if (unmap) UnmapViewOfFile(unmap);
    if (!retained && g_archiveUnmap) g_archiveUnmap(source);
}

bool ensureArchiveUnmapHook(void** vtable) {
    if (!vtable || !tq::upload::ready()) return false;
    if (InterlockedCompareExchange(&g_archiveVtablePatched, 1, 1))
        return vtable[4] == (void*)&hookArchiveUnmap;
    if (vtable[4] != (void*)((BYTE*)GetModuleHandleW(L"Engine.dll") + 0x14e540))
        return false;
    g_archiveUnmap = (ArchiveUnmapFn)vtable[4];
    if (!patchSlot(&vtable[4], (void*)&hookArchiveUnmap, nullptr)) {
        g_archiveUnmap = nullptr;
        return false;
    }
    InterlockedExchange(&g_archiveVtablePatched, 1);
    return true;
}

// ---------------------------------------------------------------------------
// The loose-file size gate.
//
// GAME::FileSystem::OpenFile (0x10151460) asks each registered source in turn
// and returns the first non-NULL answer:
//
//     101514f8  CALL [EAX + 0x8]    ; source->vtbl[2](path, ...) == OpenFile
//     101514ff  JNZ  0x10151527     ; non-NULL -> that is the answer
//     10151525  XOR  EBX,EBX        ; nothing anywhere -> NULL
//
// so a directory source that declines falls through to the archive source with
// no fallback logic of our own, and a file with no archive copy comes back
// NULL, which is the engine's own file-not-found path. The whole redirect is
// "return NULL", and everything below exists only to decide when.
typedef void* (__thiscall* SourceOpenFileFn)(void*, const void*, const void*);
typedef void (__thiscall* SourceCloseFileFn)(void*, void**);
typedef void* (__thiscall* FileLockFn)(void*, unsigned, unsigned);
typedef void (__thiscall* FileUnlockFn)(void*);
typedef unsigned (__thiscall* FileGetLengthFn)(void*);

SourceOpenFileFn g_directoryOpenFile;
SourceOpenFileFn g_archiveOpenFile;
LONG g_looseTraceLines;
LONG g_looseRedirects;
LONG g_looseBiggest;

// The source is handed the characters, not the std::string that holds them:
//
//     101514eb  LEA    EDX,[ESP + 0x18]        ; the inline buffer
//     101514ef  CMOVNC EDX,dword ptr [ESP+0x18] ; or the heap pointer, if long
//     101514f7  PUSH   EDX                      ; <- this is arg1
//
// so FileSystem::OpenFile unwraps it and what arrives is a plain NUL-terminated
// const char*. Run 7 logged sixty-four redirects as `?` because this read it as
// a string object instead. Best-effort and fully guarded: it only names a file
// in the log.
const char* resourcePath(const void* path, unsigned* length) {
    *length = 0;
    const char* text = (const char*)path;
    if (!text || !readable(text)) return nullptr;
    unsigned n = 0;
    while (n < 1024) {
        const char* at = text + n;
        // Re-checked at each page boundary rather than once, since the string
        // may end anywhere and the next page need not be mapped.
        if (((uintptr_t)at & 0xfff) == 0 && !readable(at)) return nullptr;
        if (!*at) break;
        ++n;
    }
    if (!n || n >= 1024) return nullptr;
    *length = n;
    return text;
}

void* __fastcall hookDirectoryOpenFile(void* self, void*, const void* path,
                                       const void* extra) {
    void* file = g_directoryOpenFile ? g_directoryOpenFile(self, path, extra)
                                     : nullptr;
    const UINT limit = g_options.looseTextureMax;
    if (!file || !limit || !readable(file)) return file;
    BYTE* engine = (BYTE*)GetModuleHandleW(L"Engine.dll");
    void** vtable = *(void***)file;
    // Only the audited loose-file class, whose Lock/Unlock/GetLength are the
    // three slots used below. Anything else is handed back untouched.
    // Slot 4 is deliberately not compared, and slot 4 is deliberately not
    // called: ensureArchiveUnmapHook replaces it with hookArchiveUnmap on the
    // first progressive job, so requiring the original there disabled this
    // gate for every file after the first. Run 6 caught it exactly --
    // loose_probe read 1, on the same frame as the first job. The class is
    // already pinned by the vtable address and slots 2 and 6, so Unlock is
    // called at its known address instead.
    if (!engine || !readable(vtable) || !readable(vtable + 6)
        || vtable != (void**)(engine + 0x2f71ec)
        || vtable[2] != engine + 0x14e560
        || vtable[6] != engine + 0x14e500) return file;
    tq::probe::engineCount(tq::probe::CounterLooseOpen);

    unsigned length = ((FileGetLengthFn)vtable[6])(file);
    // Run 7 measured the probe at 357 us -- a MapViewOfFile/UnmapViewOfFile
    // pair is a wineserver round trip, not the memory read it looks like -- so
    // it is worth not paying on files that cannot possibly be over the limit.
    // The floor is an eighth of a square texture at the limit -- 2 MiB at
    // 4096, and half of a DXT1 square at the limit, so a texture that trips
    // the cap has to be pathologically thin to fall under it. On the measured
    // pack it skips 40% of opens while the smallest genuinely oversize file is
    // 10.67 MiB, five times the floor. GetLength is one load.
    const unsigned floorBytes = (limit * limit) / 8u;
    if (length < 32 || length < floorBytes) return file;
    const unsigned want = length < 128 ? length : 128;
    // FileDirectory::Unlock is `UnmapViewOfFile(this[0x34]); this[0x34] = 0`
    // and nothing else -- no lock flag -- so the object is left exactly as
    // OpenFile returned it and the caller's own Lock behaves normally.
    const int64_t started = tq::probe::now();
    const void* head = ((FileLockFn)vtable[2])(file, 0, want);
    UINT width = 0, height = 0;
    const bool texture = head && readable(head)
        && tq::upload::textureDimensions(head, want, &width, &height);
    ((FileUnlockFn)(engine + 0x14e540))(file);
    tq::probe::engineCount(tq::probe::CounterLooseProbe);
    tq::probe::engineCount(tq::probe::CounterLooseProbeUs,
                           tq::probe::microsecondsSince(started));
    if (!texture || (width <= limit && height <= limit)) return file;

    void** sourceVtable = *(void***)self;
    if (!readable(sourceVtable + 3)
        || sourceVtable[3] != engine + 0x14fdc0) return file;
    ((SourceCloseFileFn)sourceVtable[3])(self, &file);
    tq::probe::engineCount(tq::probe::CounterLooseRejectOversize);
    InterlockedIncrement(&g_looseRedirects);
    LONG largest = (LONG)(width > height ? width : height);
    if (largest > g_looseBiggest) g_looseBiggest = largest;
    // Named individually for the first few, so a redirect can be checked
    // against the file it was meant to apply to rather than trusted.
    if (InterlockedIncrement(&g_looseTraceLines) <= 64) {
        unsigned n = 0;
        const char* name = resourcePath(path, &n);
        tq::hdr::log("Loose texture %ux%u over %u: %.*s -> archive\r\n",
                     width, height, limit, name ? (int)n : 1,
                     name ? name : "?");
    }
    return nullptr;
}

void* __fastcall hookArchiveOpenFile(void* self, void*, const void* path,
                                     const void* extra) {
    void* file = g_archiveOpenFile ? g_archiveOpenFile(self, path, extra)
                                   : nullptr;
    if (file) tq::probe::engineCount(tq::probe::CounterArcOpen);
    return file;
}

// Patches two vtable slots and writes no code into Engine.dll. Both are
// verified against the audited build first, and restoreSlots() puts them back.
bool installFileSourceGate() {
    BYTE* engine = (BYTE*)GetModuleHandleW(L"Engine.dll");
    if (!engine) return false;
    void** directory = (void**)(engine + 0x2f722c);
    void** archive = (void**)(engine + 0x2f7208);
    if (!readable(directory) || !readable(directory + 3)
        || !readable(archive) || !readable(archive + 2)
        || directory[2] != engine + 0x14fde0
        || directory[3] != engine + 0x14fdc0
        || archive[2] != engine + 0x14ed30) {
        tq::hdr::log("Loose texture cap not installed: the file sources are not"
                     " the audited build\r\n");
        return false;
    }
    bool ok = patchSlot(&directory[2], (void*)&hookDirectoryOpenFile,
                        (void**)&g_directoryOpenFile);
    ok &= patchSlot(&archive[2], (void*)&hookArchiveOpenFile,
                    (void**)&g_archiveOpenFile);
    tq::hdr::log("Loose texture cap: limit=%u installed=%u\r\n",
                 g_options.looseTextureMax, ok ? 1u : 0u);
    return ok;
}

// ---------------------------------------------------------------------------
// The deferred unmap, moved off the render thread.
//
// Run 5 measured 1.03 s of UnmapViewOfFile inside Present across 307 retiring
// jobs, up to 37.8 ms in one frame, and 92-98% of every one of the six worst
// stream_step frames in the session. Tearing down the view has to happen; it
// does not have to happen on the thread that is trying to present a frame.
const unsigned kUnmapQueueSlots = 64;
CRITICAL_SECTION g_unmapLock;
bool g_unmapReady;
HANDLE g_unmapSemaphore;
HANDLE g_unmapStop;
HANDLE g_unmapThread;
void* g_unmapRing[kUnmapQueueSlots];
unsigned g_unmapWrite, g_unmapRead, g_unmapPending;

void unmapNow(void* view, bool inlineOnRenderThread) {
    int64_t started = tq::probe::now();
    UnmapViewOfFile(view);
    uint32_t microseconds = tq::probe::microsecondsSince(started);
    tq::probe::engineCount(tq::probe::CounterUploadUnmap);
    if (inlineOnRenderThread) {
        tq::probe::engineCount(tq::probe::CounterUploadUnmapInline);
        tq::probe::engineCount(tq::probe::CounterUploadUnmapInlineUs,
                               microseconds);
    } else {
        tq::probe::engineCount(tq::probe::CounterUploadUnmapUs, microseconds);
    }
}

bool takeQueuedUnmap(void** view) {
    *view = nullptr;
    EnterCriticalSection(&g_unmapLock);
    if (g_unmapPending) {
        *view = g_unmapRing[g_unmapRead];
        g_unmapRead = (g_unmapRead + 1) % kUnmapQueueSlots;
        --g_unmapPending;
    }
    LeaveCriticalSection(&g_unmapLock);
    return *view != nullptr;
}

DWORD WINAPI unmapThread(void*) {
    HANDLE waits[2] = {g_unmapStop, g_unmapSemaphore};
    for (;;) {
        DWORD woke = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        // Drain whatever is queued whichever handle woke us, so a stop that
        // arrives with work outstanding still finishes it.
        void* view = nullptr;
        while (takeQueuedUnmap(&view)) unmapNow(view, false);
        if (woke == WAIT_OBJECT_0) return 0;
    }
}

void startUnmapWorker() {
    if (g_unmapReady) return;
    InitializeCriticalSection(&g_unmapLock);
    g_unmapWrite = g_unmapRead = g_unmapPending = 0;
    g_unmapStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_unmapSemaphore = CreateSemaphoreW(nullptr, 0, kUnmapQueueSlots, nullptr);
    if (g_unmapStop && g_unmapSemaphore)
        g_unmapThread = CreateThread(nullptr, 0, &unmapThread, nullptr, 0,
                                     nullptr);
    if (!g_unmapThread) {
        // Fail open: without the worker every unmap simply stays inline, which
        // is what the shipped code did, and upload_unmap_inline_us says so.
        if (g_unmapStop) { CloseHandle(g_unmapStop); g_unmapStop = nullptr; }
        if (g_unmapSemaphore) {
            CloseHandle(g_unmapSemaphore);
            g_unmapSemaphore = nullptr;
        }
        DeleteCriticalSection(&g_unmapLock);
        return;
    }
    g_unmapReady = true;
}

void stopUnmapWorker() {
    if (!g_unmapReady) return;
    g_unmapReady = false;
    if (g_unmapStop) SetEvent(g_unmapStop);
    if (g_unmapThread) {
        WaitForSingleObject(g_unmapThread, 2000);
        CloseHandle(g_unmapThread);
        g_unmapThread = nullptr;
    }
    // Anything the worker did not reach is unmapped here; at teardown this
    // thread is the only one left.
    void* view = nullptr;
    while (takeQueuedUnmap(&view)) UnmapViewOfFile(view);
    if (g_unmapStop) { CloseHandle(g_unmapStop); g_unmapStop = nullptr; }
    if (g_unmapSemaphore) {
        CloseHandle(g_unmapSemaphore);
        g_unmapSemaphore = nullptr;
    }
    DeleteCriticalSection(&g_unmapLock);
}

// Hands the view to the worker, or reports that it could not so the caller
// unmaps it itself rather than leaking it.
bool queueUnmap(void* view) {
    if (!g_unmapReady || !view) return false;
    bool queued = false;
    EnterCriticalSection(&g_unmapLock);
    if (g_unmapPending < kUnmapQueueSlots) {
        g_unmapRing[g_unmapWrite] = view;
        g_unmapWrite = (g_unmapWrite + 1) % kUnmapQueueSlots;
        ++g_unmapPending;
        queued = true;
    }
    LeaveCriticalSection(&g_unmapLock);
    if (queued) ReleaseSemaphore(g_unmapSemaphore, 1, nullptr);
    return queued;
}

// The mapping lease, expressed as the upload module's retain/release pair.
// Called with that module's lock held, on whichever thread is loading.
bool retainMapping(void* ownerPointer, void** token) {
    TextureOwner& owner = *(TextureOwner*)ownerPointer;
    MappingLease* lease = findLease(owner.source);
    if (owner.mappedBase
        && (!lease || lease->sealed || lease->mappedBase != owner.mappedBase))
        lease = createLease(owner.source, owner.mappedBase);
    bool hooked = lease && ensureArchiveUnmapHook(owner.vtable);
    if (hooked && owner.mappedBase) {
        void** field = (void**)((BYTE*)owner.source + 0x34);
        void* prior = InterlockedCompareExchangePointer(field, nullptr,
                                                        owner.mappedBase);
        hooked = prior == owner.mappedBase;
    }
    if (hooked && !owner.mappedBase)
        hooked = lease->mappedBase != nullptr;
    if (!hooked) {
        if (lease && !lease->jobs) {
            InterlockedExchangeAdd(&g_leasedBytes, -(LONG)lease->bytes);
            memset(lease, 0, sizeof(*lease));
        }
        return false;
    }
    if (!lease->bytes && readable((BYTE*)owner.source + 0x28)) {
        lease->bytes = *(const unsigned*)((BYTE*)owner.source + 0x28);
        InterlockedExchangeAdd(&g_leasedBytes, (LONG)lease->bytes);
    }
    ++lease->jobs;
    *token = lease;
    return true;
}

// Called on the render thread when a job retires, with the upload module's
// lock dropped -- which is what lets the unmap happen outside it, as it did
// before the extraction. Retaking the lock here is safe: it is a recursive
// CRITICAL_SECTION and this thread does not hold it.
void releaseMapping(void* token) {
    MappingLease* lease = (MappingLease*)token;
    if (!lease) return;
    void* unmap = nullptr;
    tq::upload::lock();
    if (lease->jobs > 0) --lease->jobs;
    if (lease->sealed && !lease->jobs) {
        unmap = lease->mappedBase;
        InterlockedExchangeAdd(&g_leasedBytes, -(LONG)lease->bytes);
        memset(lease, 0, sizeof(*lease));
    }
    tq::upload::unlock();
    // Measured at 1.03 s inside Present over one session, so it goes to the
    // worker. If the queue is full it is done here anyway -- a leaked view in
    // a 32-bit address space is a worse failure than a long frame.
    if (unmap && !queueUnmap(unmap)) unmapNow(unmap, true);
}

int64_t uploadNow() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

double uploadMillisecondsSince(int64_t start) {
    static LARGE_INTEGER frequency;   // invariant for the process
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER after;
    QueryPerformanceCounter(&after);
    return frequency.QuadPart
         ? (double)(after.QuadPart - start) * 1000.0 / (double)frequency.QuadPart
         : 0.0;
}

tq::upload::Calls uploadCalls() {
    tq::upload::Calls calls = {};
    calls.createTexture2D = g_createTexture2D;
    calls.createShaderResourceView = g_createShaderResourceView;
    calls.updateSubresource = g_updateSubresource;
    calls.now = &uploadNow;
    calls.millisecondsSince = &uploadMillisecondsSince;
    calls.retain = &retainMapping;
    calls.release = &releaseMapping;
    return calls;
}

bool progressiveTextureCandidate(const D3D11_TEXTURE2D_DESC* desc,
                                 const D3D11_SUBRESOURCE_DATA* initial,
                                 const void* caller, uint64_t* topBytes) {
    if (!g_options.streaming || !desc || !initial || !initial[0].pSysMem
        || desc->Usage != D3D11_USAGE_DEFAULT || desc->ArraySize != 1
        || !desc->MipLevels || desc->MipLevels > tq::upload::kMaxTextureMips
        || desc->BindFlags != D3D11_BIND_SHADER_RESOURCE) return false;
    UINT blockBytes = 0;
    if (!blockCompressedFormat(desc->Format, &blockBytes)) return false;
    uint64_t rows = (desc->Height + 3u) / 4u;
    if (!rows) rows = 1;
    uint64_t bytes = (uint64_t)initial[0].SysMemPitch * rows;
    if (bytes < 2ull * 1024ull * 1024ull) return false;
    BYTE* renderer = (BYTE*)GetModuleHandleW(L"Direct3D11.dll");
    if (!renderer || caller != renderer + 0x6aa5c) return false;
    if (topBytes) *topBytes = bytes;
    return true;
}

HRESULT createProgressiveTexture(ID3D11Device* device,
                                 const D3D11_TEXTURE2D_DESC* desc,
                                 const D3D11_SUBRESOURCE_DATA* initial,
                                 ID3D11Texture2D** texture,
                                 const void* caller, bool* handled) {
    *handled = false;
    uint64_t topBytes = 0;
    if (!progressiveTextureCandidate(desc, initial, caller, &topBytes)
        || !tq::upload::ready()) return E_FAIL;
    TextureOwner owner = {};
    const BYTE* dds = (const BYTE*)initial[0].pSysMem - 0x80;
    // Past this point the texture is one this path wanted, so every decline
    // below is a fact about the install rather than about the texture -- and
    // each is counted separately, because "rejected" conflating three
    // unrelated outcomes is what made the previous runs unreadable. All of
    // them go through the engine channel: this function runs on the game's
    // loader thread, where probe::count writes nothing.
    if (!findTextureOwner(dds, &owner)) {
        // The audit's prediction, if it fires: a texture read out of a .arc,
        // whose File is not the loose-file class this scan accepts.
        tq::probe::engineCount(tq::probe::CounterUploadSrcNone);
        tq::probe::engineCount(tq::probe::CounterUploadRejectScan);
        tq::probe::engineCount(tq::probe::CounterUploadRejected);
        return E_FAIL;
    }
    tq::probe::engineCount(tq::probe::CounterUploadSrcLoose);
    return tq::upload::create(device, desc, initial, texture, topBytes, &owner,
                              handled);
}

HRESULT WINAPI hookCreateShaderResourceView(
    ID3D11Device* device, ID3D11Resource* resource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* description,
    ID3D11ShaderResourceView** view) {
    D3D11_SHADER_RESOURCE_VIEW_DESC translated = {};
    const D3D11_SHADER_RESOURCE_VIEW_DESC* used = description;
    void* identity = identityOf(resource);
    if (description && description->Format == DXGI_FORMAT_R8G8B8A8_UNORM
        && upgradedIdentity(identity)) {
        translated = *description;
        translated.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        used = &translated;
    }
    HRESULT hr = g_createShaderResourceView(device, resource, used, view);
    if (FAILED(hr) || !view || !*view) return hr;
    tq::upload::noteShaderResourceView(device, resource, description, *view);
    return hr;
}

void WINAPI hookPSSetShaderResources(ID3D11DeviceContext* context, UINT start,
                                     UINT count,
                                     ID3D11ShaderResourceView* const* views) {
    if (!views || !count || count > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
        || !tq::upload::ready()) {
        if (g_deferredBindingTracing && !g_inside && context == g_context
            && views && start < tq::engineprobe::DeferredTracePixelResourceSlots) {
            UINT kept = count;
            if (kept > tq::engineprobe::DeferredTracePixelResourceSlots - start)
                kept = tq::engineprobe::DeferredTracePixelResourceSlots - start;
            for (UINT i = 0; i < kept; ++i)
                g_deferredBindings.pixelResources[start + i] = views[i];
        }
        g_psSetShaderResources(context, start, count, views);
        return;
    }
    tq::probe::count(tq::probe::CounterPsSetSrv);
    ID3D11ShaderResourceView* substituted[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
    memcpy(substituted, views, count * sizeof(*views));
    // The lock is held across the substitution *and* the device call, which is
    // what the shipped code did. It is a real cost on a path that runs
    // thousands of times a frame, and Stage 2.6 of the mitigation plan is
    // where it goes; keeping it here keeps this commit a move.
    tq::upload::lock();
    tq::upload::substituteLocked(count, substituted);
    if (g_deferredBindingTracing && !g_inside && context == g_context
        && start < tq::engineprobe::DeferredTracePixelResourceSlots) {
        UINT kept = count;
        if (kept > tq::engineprobe::DeferredTracePixelResourceSlots - start)
            kept = tq::engineprobe::DeferredTracePixelResourceSlots - start;
        for (UINT i = 0; i < kept; ++i)
            g_deferredBindings.pixelResources[start + i] = substituted[i];
    }
    g_psSetShaderResources(context, start, count, substituted);
    tq::upload::unlock();
}

void advanceTextureUploadsInternal() {
    if (!g_context) return;
    tq::upload::advance(g_context);
    // Once a frame, on the render thread, so the column is a gauge: what the
    // leases are holding right now, not what they have ever held.
    LONG leased = g_leasedBytes;
    if (leased > 0)
        tq::probe::count(tq::probe::CounterUploadLeasedMib,
                         (uint32_t)(leased / (1024 * 1024)));
    // Free address space, on the same terms and for the same reason. This is
    // a 32-bit process holding ~336 MiB of shadow targets, and exhaustion is
    // the one resource failure that would look like the unexplained stalls
    // run 12 measured. One call a frame settles it instead of costing a run.
    if (tq::probe::enabled()) {
        MEMORYSTATUSEX memory = {};
        memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory))
            tq::probe::count(tq::probe::CounterProcAvailVaMib,
                             (uint32_t)(memory.ullAvailVirtual / (1024 * 1024)));
    }
}


bool createProgramResources(ID3D11Device* device);

DWORD WINAPI programThread(void* argument) {
    ID3D11Device* device = (ID3D11Device*)argument;
    bool ok = createProgramResources(device);
    InterlockedExchange(&g_programState, ok ? 2 : 3);
    device->Release();
    return 0;
}

void startProgramBuild(ID3D11Device* device) {
    if (!device || InterlockedCompareExchange(&g_programState, 1, 0)) return;
    device->AddRef();
    g_programThread = CreateThread(nullptr, 0, programThread, device, 0, nullptr);
    if (!g_programThread) {
        device->Release();
        InterlockedExchange(&g_programState, 3);
    }
}

bool gameScreenTarget(const D3D11_TEXTURE2D_DESC* desc, const void* caller) {
    BYTE* renderer = (BYTE*)GetModuleHandleW(L"Direct3D11.dll");
    return desc && renderer && caller == renderer + 0x67341
        && g_backBufferWidth && g_backBufferHeight
        && desc->Width == g_backBufferWidth && desc->Height == g_backBufferHeight
        && (desc->BindFlags & D3D11_BIND_RENDER_TARGET);
}

void recordScreenTarget(const D3D11_TEXTURE2D_DESC* desc,
                        ID3D11Texture2D* texture, const void* caller) {
    if (!gameScreenTarget(desc, caller) || !texture
        || g_screenTargetCount >= sizeof(g_screenTargets) / sizeof(g_screenTargets[0]))
        return;
    ScreenTargetInfo& target = g_screenTargets[g_screenTargetCount++];
    memset(&target, 0, sizeof(target));
    target.identity = identityOf(texture);
    target.id = g_screenTargetCount;
    texture->GetDesc(&target.desc);
    target.upgraded = desc->Format == DXGI_FORMAT_R8G8B8A8_UNORM
                   && target.desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool fp16SceneTarget(const D3D11_TEXTURE2D_DESC* desc,
                     const D3D11_SUBRESOURCE_DATA* initial, const void* caller) {
    // Exact Engine-build/runtime match for confirmed gameplay color surfaces.
    // The ordinal is creation order at Direct3D11.dll+0x67341. Captured pass
    // bindings plus decoded DXBC give this full-resolution target map:
    //   1: G-buffer base/ambient-lit diffuse color (R8; unchanged).
    //   2: G-buffer packed world normal plus material coverage (R8; unchanged).
    //   3: G-buffer diffuse/material reflectance color (R8; unchanged).
    //   4: G-buffer specular color plus packed gloss power (R8; unchanged).
    //   5: composed scene color, including transparent/effect draws (FP16).
    //   6: auxiliary visibility/mask surface; only clear/no read was observed
    //      on the captured route, so it remains R8 rather than being guessed HDR.
    //   7: scene/background copy sampled by refraction effects (FP16).
    //   8: linear scene-depth surface (R32_FLOAT; unchanged).
    //   9: primary deferred-light accumulation/post-color surface (FP16).
    //  10: screen-space occlusion/visibility result from the depth-neighborhood
    //      filter (R8; scalar data rather than display color).
    //  11: alternate deferred-light accumulation/post-color surface (FP16).
    //  12: late post-process color consumed by FXAA/SMAA and gamma (FP16).
    //  13: lazily-created copy of the completed frame used as gamma input by
    //      the alternate sector/portal route (FP16). Leaving this one R8 clips
    //      every extended highlight at once before tone mapping.
    const unsigned nextId = g_screenTargetCount + 1;
    const bool colorSurface = isFp16SceneTargetOrdinal(nextId);
    return tq::hdr::runtime().fp16Active && colorSurface && !initial
        && gameScreenTarget(desc, caller)
        && desc->Format == DXGI_FORMAT_R8G8B8A8_UNORM
        && desc->MipLevels == 1 && desc->ArraySize == 1
        && desc->SampleDesc.Count == 1 && desc->SampleDesc.Quality == 0
        && desc->Usage == D3D11_USAGE_DEFAULT && !desc->CPUAccessFlags
        && !desc->MiscFlags
        && desc->BindFlags == (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
}

bool upgradedIdentity(void* identity) {
    ScreenTargetInfo* target = screenTarget(identity);
    return target && target->upgraded;
}

HRESULT createOriginalTexture(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc,
                              const D3D11_SUBRESOURCE_DATA* initial,
                              ID3D11Texture2D** texture, const void* caller) {
    D3D11_TEXTURE2D_DESC fp16 = {};
    const D3D11_TEXTURE2D_DESC* requested = desc;
    bool upgrade = fp16SceneTarget(desc, initial, caller);
    if (upgrade) { fp16 = *desc; fp16.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; }
    HRESULT hr = g_createTexture2D(device, upgrade ? &fp16 : desc, initial, texture);
    if (upgrade && FAILED(hr)) {
        tq::hdr::log("FP16 scene-target creation failed: hr=0x%08lx; retaining R8\r\n",
                     (unsigned long)hr);
        hr = g_createTexture2D(device, desc, initial, texture);
    }
    if (SUCCEEDED(hr) && texture && *texture) {
        recordScreenTarget(requested, *texture, caller);
    }
    return hr;
}

// The body of the CreateTexture2D hook, split out so the hook itself can be a
// thin frame that decides which of the probe's two channels the call belongs
// in. This function has several returns, and re-deciding that at each of them
// by hand is exactly the bookkeeping that rots.
HRESULT createTexture2DDispatch(ID3D11Device* device,
                                const D3D11_TEXTURE2D_DESC* desc,
                                const D3D11_SUBRESOURCE_DATA* initial,
                                ID3D11Texture2D** texture, const void* caller) {
    bool progressivelyHandled = false;
    HRESULT progressive = createProgressiveTexture(device, desc, initial, texture,
                                                    caller, &progressivelyHandled);
    if (progressivelyHandled) return progressive;
    if (!g_options.shadows || !shadowDepthDesc(desc) || initial)
        return createOriginalTexture(device, desc, initial, texture, caller);
    // Only the directional map benefits from the wider split, and it is one
    // texture among a dozen: a run allocates one large square for the
    // directional light and many smaller ones for point and spot lights.
    // Scaling all of them alike costs several hundred megabytes for shadows
    // the split never touches, which matters because Titan Quest is a 32-bit
    // process with a bounded address space.
    //
    // Size is the only discriminator available at creation. The directional
    // map is the largest request the game makes, so anything at or above
    // kDirectionalShadowSize takes the directional scale. At the lowest shadow
    // quality that request can fall below the threshold and be treated as a
    // point map; the split still applies, but with less density than intended.
    const bool directional = desc->Width >= kDirectionalShadowSize;
    // 8192 is the largest square this path allocates; a failed allocation
    // steps down a scale rather than dropping to the unscaled map, since the
    // largest sizes are a real amount of memory.
    D3D11_TEXTURE2D_DESC scaled = *desc;
    UINT scale = directional ? g_options.shadowMapScale
                             : g_options.pointShadowMapScale;
    while (scale > 1 && desc->Width * scale > 8192) scale /= 2;
    HRESULT hr = E_FAIL;
    for (;; scale /= 2) {
        scaled.Width = desc->Width * scale;
        scaled.Height = desc->Height * scale;
        hr = g_createTexture2D(device, &scaled, initial, texture);
        if ((SUCCEEDED(hr) && texture && *texture) || scale == 1) break;
    }
    if (FAILED(hr) || !texture || !*texture) {
        return createOriginalTexture(device, desc, initial, texture, caller);
    }
    // The fit stabiliser snaps the directional projection onto the shadow map
    // texel grid, so it has to know a size that was actually created. Every
    // map is reported, not just the directional one: at the lowest shadow
    // quality the directional request can fall below the classification
    // threshold, and the stabiliser wants a size it can trust over one that
    // depends on this guess being right.
    tq::shadow::noteShadowMapSize(scaled.Width, directional);
    tq::hdr::log("Shadow map: %ux%u requested, %ux%u created (%s, scale %u,"
                 " %u MiB)\r\n",
                 desc->Width, desc->Height, scaled.Width, scaled.Height,
                 directional ? "directional" : "point/spot", scale,
                 (scaled.Width * scaled.Height * 4u) / (1024u * 1024u));
    if (g_shadowTextureCount < sizeof(g_shadowTextures) / sizeof(g_shadowTextures[0])) {
        ShadowTexture& s = g_shadowTextures[g_shadowTextureCount++];
        s.identity = identityOf(*texture);
        s.originalWidth = desc->Width;
        s.originalHeight = desc->Height;
        s.scale = scale;
    } else if (!InterlockedCompareExchange(&g_shadowTableFullLogged, 1, 0)) {
        // Never silently: an unrecorded map keeps its unscaled viewport and
        // renders into a fraction of itself, which looks like a shadow bug
        // rather than a bookkeeping one.
        tq::hdr::log("Shadow map table full at %u entries; %ux%u will render"
                     " unscaled\r\n", g_shadowTextureCount,
                     scaled.Width, scaled.Height);
    }
    return hr;
}

HRESULT WINAPI hookCreateTexture2D(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc,
                                   const D3D11_SUBRESOURCE_DATA* initial,
                                   ID3D11Texture2D** texture) {
    const void* caller = __builtin_return_address(0);
    // Texture loads run on the game's loader thread, and the frame record
    // cannot accept a write from there: probe::count and probe::addPhase both
    // drop it. So every texture the game streamed in -- which is to say all of
    // them, and precisely the ones under investigation -- was being discarded
    // by the instrument meant to be watching it. Off-thread calls go to the
    // engine channel instead, where they survive.
    if (!tq::probe::enabled())
        return createTexture2DDispatch(device, desc, initial, texture, caller);
    const bool renderThread = tq::probe::isRenderThread();
    const int64_t started = tq::probe::enabled() ? tq::probe::now() : 0;
    const unsigned startFrame = started ? tq::probe::currentFrameIndex() : 0;
    if (renderThread) tq::probe::count(tq::probe::CounterTextureCreate);
    else tq::probe::engineCount(tq::probe::CounterEngineTexCreateOff);
    HRESULT hr = createTexture2DDispatch(device, desc, initial, texture, caller);
    uint32_t elapsed = 0;
    if (started) {
        if (renderThread)
            elapsed = tq::probe::finishPhase(
                tq::probe::PhaseTextureCreate, started);
        else {
            elapsed = tq::probe::microsecondsSince(started);
            tq::probe::engineCount(tq::probe::CounterEngineTexCreateOffUs,
                                   elapsed);
        }
    }
    if (SUCCEEDED(hr) && texture && *texture && desc)
        tq::engineprobe::noteDeferredTextureCreated(
            *texture, elapsed, desc->Width, desc->Height, desc->MipLevels,
            (unsigned)desc->Format, desc->BindFlags, desc->MiscFlags);
    if (!renderThread && SUCCEEDED(hr) && texture && *texture && desc)
        tq::engineprobe::noteOffMainTextureCreated(
            startFrame, tq::probe::currentFrameIndex(), elapsed,
            GetCurrentThreadId(), desc->Width, desc->Height, desc->MipLevels,
            (unsigned)desc->Format, desc->BindFlags, desc->MiscFlags,
            initial != nullptr);
    return hr;
}

HRESULT WINAPI hookCreatePixelShader(ID3D11Device* device, const void* bytecode, SIZE_T size,
                                     ID3D11ClassLinkage* linkage, ID3D11PixelShader** shader) {
    tq::probe::Scope timing(tq::probe::PhaseShaderCreate);
    tq::probe::count(tq::probe::CounterShaderCreate);
    tq::dxbc::PatchResult patch = {};
    bool enhanced = g_options.shadows && tq::dxbc::enhanceShadowPcf(bytecode, size, &patch);
    // enhanceShadowPcf matches the per-material receivers, which this renderer
    // does not use for directional light. The deferred screen-space receiver
    // carries the PCF that actually runs, and its taps need the same 3x3
    // placement plus an offset scale that keeps world-space softness constant
    // as the shadow split widens.
    if (!enhanced && g_options.shadows) {
        const float offsets = tq::shadow::blurCompensation();
        const float bias = tq::shadow::biasCompensation();
        const bool corners = tq::shadow::cornerFilterEnabled();
        if (tq::dxbc::tuneDeferredShadowFilter(bytecode, size, offsets, bias,
                                               corners, &patch)) {
            enhanced = true;
            tq::hdr::log("Deferred shadow filter: taps=%s offsetScale=%.3f"
                         " biasScale=%.3f\r\n",
                         corners ? "3x3 corners" : "native cross",
                         offsets, bias);
        }
    }
    HRESULT hr = g_createPixelShader(device, enhanced ? patch.data : bytecode,
                                     enhanced ? patch.size : size, linkage, shader);
    if (enhanced && FAILED(hr)) hr = g_createPixelShader(device, bytecode, size, linkage, shader);
    tq::dxbc::release(&patch);
    if (SUCCEEDED(hr) && shader && *shader) {
        bool outputTransform = tq::hdr::runtime().settings.toneMap != tq::hdr::ToneOriginal;
        bool fxaa = g_options.smaa && isFxaa(bytecode, size);
        bool color = outputTransform && tq::hdr::isColorGradingShader(bytecode, size);
        bool gamma = outputTransform && tq::hdr::isGammaShader(bytecode, size);
        bool alphaClampCopy = outputTransform && isAlphaClampCopy(bytecode, size);
        if (fxaa && g_fxaaCount < sizeof(g_fxaa) / sizeof(g_fxaa[0]))
            g_fxaa[g_fxaaCount++] = *shader;
        if (color && g_colorGradingCount < sizeof(g_colorGrading) / sizeof(g_colorGrading[0]))
            g_colorGrading[g_colorGradingCount++] = *shader;
        if (gamma && g_gammaCount < sizeof(g_gamma) / sizeof(g_gamma[0]))
            g_gamma[g_gammaCount++] = *shader;
        if (alphaClampCopy && g_alphaClampCopyCount
                < sizeof(g_alphaClampCopies) / sizeof(g_alphaClampCopies[0]))
            g_alphaClampCopies[g_alphaClampCopyCount++] = *shader;
        if (fxaa || color || gamma || alphaClampCopy) {
        // DXMT deadlocks if a device shader is created re-entrantly from either
        // CreatePixelShader or Draw. A one-shot device worker builds the fixed
        // program after this call returns; FXAA remains active until it is ready.
            startProgramBuild(device);
        }
    }
    return hr;
}

HRESULT WINAPI hookCreateRenderTargetView(
    ID3D11Device* device, ID3D11Resource* resource,
    const D3D11_RENDER_TARGET_VIEW_DESC* description,
    ID3D11RenderTargetView** view) {
    void* identity = identityOf(resource);
    const tq::hdr::Runtime& runtime = tq::hdr::runtime();
    bool translate = description && description->Format == DXGI_FORMAT_R8G8B8A8_UNORM
        && ((runtime.fp16Active && identity == g_backBufferIdentity)
            || upgradedIdentity(identity));
    D3D11_RENDER_TARGET_VIEW_DESC translated = {};
    const D3D11_RENDER_TARGET_VIEW_DESC* used = nullptr;
    if (!translate)
        used = description;
    else {
        translated = *description;
        translated.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        used = &translated;
    }
    HRESULT hr = g_createRenderTargetView(device, resource, used, view);
    return hr;
}

HRESULT WINAPI hookCreateSamplerState(ID3D11Device* device, const D3D11_SAMPLER_DESC* desc,
                                      ID3D11SamplerState** sampler) {
    if (!desc || desc->Filter != D3D11_FILTER_MIN_MAG_MIP_LINEAR
        || desc->AddressU != D3D11_TEXTURE_ADDRESS_WRAP
        || desc->AddressV != D3D11_TEXTURE_ADDRESS_WRAP)
        return g_createSamplerState(device, desc, sampler);
    D3D11_SAMPLER_DESC enhanced = *desc;
    enhanced.Filter = D3D11_FILTER_ANISOTROPIC;
    enhanced.MaxAnisotropy = g_options.anisotropy;
    HRESULT hr = g_createSamplerState(device, &enhanced, sampler);
    return FAILED(hr) ? g_createSamplerState(device, desc, sampler) : hr;
}

void WINAPI hookPSSetShader(ID3D11DeviceContext* context, ID3D11PixelShader* shader,
                            ID3D11ClassInstance* const* classes, UINT count) {
    if (context == g_context) g_gamePixelShader = shader;
    g_fxaaBound = g_colorGradingBound = g_gammaBound = false;
    for (unsigned i = 0; i < g_fxaaCount; ++i)
        if (shader == g_fxaa[i]) g_fxaaBound = true;
    for (unsigned i = 0; i < g_colorGradingCount; ++i)
        if (shader == g_colorGrading[i]) g_colorGradingBound = true;
    for (unsigned i = 0; i < g_gammaCount; ++i)
        if (shader == g_gamma[i]) g_gammaBound = true;
    ID3D11PixelShader* replacement = shader;
    if (InterlockedCompareExchange(&g_programState, 2, 2) == 2) {
        if (g_colorGradingBound && g_hdr.colorGradingPS)
            replacement = g_hdr.colorGradingPS;
        else if (g_gammaBound) {
            if (tq::hdr::runtime().fp16Active && g_hdr.gammaPS)
                replacement = g_hdr.gammaPS;
            else if (!tq::hdr::runtime().fp16Active && g_hdr.tonePS)
                replacement = g_hdr.tonePS;
        }
    }
    if (g_deferredBindingTracing && !g_inside && context == g_context)
        g_deferredBindings.pixelShader = replacement;
    g_psSetShader(context, replacement, classes, count);
}

void WINAPI hookVSSetShader(ID3D11DeviceContext* context,
                            ID3D11VertexShader* shader,
                            ID3D11ClassInstance* const* classes, UINT count) {
    if (g_deferredBindingTracing && !g_inside && context == g_context)
        g_deferredBindings.vertexShader = shader;
    g_vsSetShader(context, shader, classes, count);
}

void WINAPI hookIASetVertexBuffers(ID3D11DeviceContext* context, UINT start,
                                   UINT count, ID3D11Buffer* const* buffers,
                                   const UINT* strides, const UINT* offsets) {
    if (g_deferredBindingTracing && !g_inside && context == g_context
        && start < tq::engineprobe::DeferredTraceVertexBufferSlots) {
        UINT kept = count;
        if (kept > tq::engineprobe::DeferredTraceVertexBufferSlots - start)
            kept = tq::engineprobe::DeferredTraceVertexBufferSlots - start;
        for (UINT i = 0; i < kept; ++i) {
            g_deferredBindings.vertexBuffers[start + i] =
                buffers ? buffers[i] : nullptr;
            g_deferredBindings.vertexStrides[start + i] =
                strides ? strides[i] : 0;
            g_deferredBindings.vertexOffsets[start + i] =
                offsets ? offsets[i] : 0;
        }
    }
    g_iaSetVertexBuffers(context, start, count, buffers, strides, offsets);
}

void WINAPI hookIASetIndexBuffer(ID3D11DeviceContext* context,
                                 ID3D11Buffer* buffer, DXGI_FORMAT format,
                                 UINT offset) {
    if (g_deferredBindingTracing && !g_inside && context == g_context) {
        g_deferredBindings.indexBuffer = buffer;
        g_deferredBindings.indexFormat = (unsigned)format;
        g_deferredBindings.indexOffset = offset;
    }
    g_iaSetIndexBuffer(context, buffer, format, offset);
}

const ShadowTexture* dsvShadowTexture(ID3D11DepthStencilView* dsv) {
    if (!dsv) return nullptr;
    ID3D11Resource* resource = nullptr;
    dsv->GetResource(&resource);
    void* identity = identityOf(resource);
    release(resource);
    return shadowTextureForIdentity(identity);
}

void applyViewports(ID3D11DeviceContext* context) {
    if (!g_gameViewportCount) return;
    D3D11_VIEWPORT v[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    memcpy(v, g_gameViewports, g_gameViewportCount * sizeof(*v));
    if (g_shadowBound && g_shadowScale > 1) {
        const float scale = (float)g_shadowScale;
        for (UINT i = 0; i < g_gameViewportCount; ++i) {
            v[i].TopLeftX *= scale; v[i].TopLeftY *= scale;
            v[i].Width *= scale; v[i].Height *= scale;
        }
    }
    g_rsSetViewports(context, g_gameViewportCount, v);
}

void applyScissors(ID3D11DeviceContext* context) {
    if (!g_gameScissorCount) return;
    D3D11_RECT r[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    memcpy(r, g_gameScissors, g_gameScissorCount * sizeof(*r));
    if (g_shadowBound && g_shadowScale > 1) for (UINT i = 0; i < g_gameScissorCount; ++i) {
        r[i].left *= (LONG)g_shadowScale; r[i].top *= (LONG)g_shadowScale;
        r[i].right *= (LONG)g_shadowScale; r[i].bottom *= (LONG)g_shadowScale;
    }
    g_rsSetScissors(context, g_gameScissorCount, r);
}

void WINAPI hookOMSetRenderTargets(ID3D11DeviceContext* context, UINT count,
                                   ID3D11RenderTargetView* const* rtvs,
                                   ID3D11DepthStencilView* dsv) {
    bool hasColorTarget = false;
    if (rtvs) for (UINT i = 0; i < count; ++i) {
        if (!rtvs[i]) continue;
        hasColorTarget = true;
    }
    const bool was = g_shadowBound;
    const bool wasDirectional = g_shadowDirectional;
    const UINT wasScale = g_shadowScale;
    tq::probe::count(tq::probe::CounterSetRenderTargets);
    // TQ's water-reflection pass can allocate a square R32 depth/SRV texture
    // resembling the shadow map. Its DSV is paired with a color target; the
    // actual shadow-map pass is depth-only. Never scale reflection viewports.
    const ShadowTexture* shadow = g_options.shadows && !hasColorTarget
                                ? dsvShadowTexture(dsv) : nullptr;
    g_shadowBound = shadow != nullptr;
    if (shadow) {
        g_shadowScale = shadow->scale;
        g_shadowDirectional = shadow->originalWidth >= kDirectionalShadowSize;
        tq::probe::count(tq::probe::CounterShadowBind);
    } else {
        g_shadowScale = 1;
    }
    g_omSetRenderTargets(context, count, rtvs, dsv);
    // Transitions are what matter, and there are two kinds: shadow/not-shadow,
    // and one shadow map straight onto another. The second changes the scale
    // and the GPU phase without flipping g_shadowBound -- a directional pass
    // followed immediately by a point pass would otherwise keep the wrong
    // viewport scale until the game's next RSSetViewports, and leave the
    // directional region open so both shadow columns read blank.
    if (was != g_shadowBound) {
        applyViewports(context);
        applyScissors(context);
        if (g_shadowBound)
            tq::probe::gpuBegin(context, g_shadowDirectional
                ? tq::probe::GpuShadowDirectional : tq::probe::GpuShadowPoint);
        else
            tq::probe::gpuEnd(context, wasDirectional
                ? tq::probe::GpuShadowDirectional : tq::probe::GpuShadowPoint);
    } else if (g_shadowBound
               && (wasDirectional != g_shadowDirectional
                   || wasScale != g_shadowScale)) {
        applyViewports(context);
        applyScissors(context);
        tq::probe::gpuEnd(context, wasDirectional
            ? tq::probe::GpuShadowDirectional : tq::probe::GpuShadowPoint);
        tq::probe::gpuBegin(context, g_shadowDirectional
            ? tq::probe::GpuShadowDirectional : tq::probe::GpuShadowPoint);
    }
}

void WINAPI hookRSSetViewports(ID3D11DeviceContext* context, UINT count,
                               const D3D11_VIEWPORT* viewports) {
    g_gameViewportCount = count > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE
                        ? D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE : count;
    if (g_gameViewportCount && viewports)
        memcpy(g_gameViewports, viewports, g_gameViewportCount * sizeof(*viewports));
    applyViewports(context);
}

void WINAPI hookRSSetScissors(ID3D11DeviceContext* context, UINT count,
                              const D3D11_RECT* rects) {
    g_gameScissorCount = count > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE
                       ? D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE : count;
    if (g_gameScissorCount && rects)
        memcpy(g_gameScissors, rects, g_gameScissorCount * sizeof(*rects));
    applyScissors(context);
}

const char* kColorGradingSource =
"Texture2D SceneColor:register(t0);Texture2D ColorLut:register(t1);"
"SamplerState SceneSampler:register(s0);SamplerState LutSampler:register(s1);"
"float3 grade(float3 c){"
"float z=c.b*15.0;float slice=floor(z);float f=z-slice;"
"float2 p=c.rg*15.0+0.5;float2 uv=float2(p.x/256.0,p.y/16.0);"
"float3 a=ColorLut.SampleLevel(LutSampler,uv+float2(slice/16.0,0),0).rgb;"
"float3 b=ColorLut.SampleLevel(LutSampler,uv+float2((slice+1.0)/16.0,0),0).rgb;"
"return lerp(a,b,f);"
"}"
"float4 main(float4 p:SV_POSITION,float2 u:TEXCOORD0):SV_Target{"
"float3 raw=max(SceneColor.SampleLevel(SceneSampler,u,0).rgb,0);"
"float intensity=max(1.0,max(raw.r,max(raw.g,raw.b)));"
"return float4(grade(saturate(raw/intensity))*intensity,1);}";

const char* kToneSource =
"Texture2D screenSampler:register(t0);Texture2D gammaSampler:register(t1);"
"SamplerState screenState:register(s0);SamplerState gammaState:register(s1);"
"float3 gammaGrade(float3 c){"
"float intensity=max(1.0,max(c.r,max(c.g,c.b)));float3 n=saturate(c/intensity);"
"float3 g=float3(gammaSampler.SampleLevel(gammaState,float2(n.r,.5),0).r,"
"gammaSampler.SampleLevel(gammaState,float2(n.g,.5),0).r,"
"gammaSampler.SampleLevel(gammaState,float2(n.b,.5),0).r);return g*intensity;}"
"float3 toLinear(float3 c){float3 lo=c/12.92,hi=pow((c+.055)/1.055,2.4);"
"return lerp(lo,hi,step(.04045,c));}"
"float3 toSrgb(float3 c){c=max(c,0);float3 lo=c*12.92;"
"float3 hi=1.055*pow(c,1.0/2.4)-.055;return lerp(lo,hi,step(.0031308,c));}"
"float3 linearizeExtended(float3 c){float intensity=max(1.0,max(c.r,max(c.g,c.b)));"
"return toLinear(saturate(c/intensity))*intensity;}"
"float agxContrast(float x){float x2=x*x,x4=x2*x2;return saturate("
"15.5*x4*x2-40.14*x4*x+31.96*x4-6.868*x2*x+.4298*x2+.1191*x-.00232);}"
"float agxCurve(float x){float v=saturate((log2(max(x,1e-10))+12.47393)/16.5);"
"return pow(agxContrast(v),2.376);}"
"float frostbiteCurve(float x){x=max(x,0);float knee=TQ_PEAK*.75;"
"if(x<=knee)return x;float range=max(TQ_PEAK-knee,1e-4);"
"return min(knee+range*(1-exp(-(x-knee)/range)),TQ_PEAK);}"
"float displayCurve(float x){return agxCurve(x);}"
"float mapLuma(float l){if(TQ_FROSTBITE)return frostbiteCurve(l);"
"float white=displayCurve(1);float low=displayCurve(min(l,1));"
"float range=max(TQ_PEAK-white,0);float high=white+range*(1-exp(-max(l-1,0)/max(range,1)));"
"return l<=1?low:min(high,TQ_PEAK);}"
"float3 mapColor(float3 c){if(TQ_FROSTBITE){float p=max(max(c.r,c.g),c.b);"
"p=max(p,1e-6);float m=mapLuma(p);float3 scaled=c*(m/p);"
"float shoulder=saturate((p-TQ_PEAK*.75)/p);"
"return max(lerp(scaled,float3(m,m,m),shoulder*shoulder),0);}"
"float l=max(dot(c,float3(.2126,.7152,.0722)),1e-6);"
"return max(c*(mapLuma(l)/l),0);}"
"float4 main(float4 p:SV_POSITION,float2 u:TEXCOORD0):SV_Target{"
"float3 encoded=max(gammaGrade(screenSampler.SampleLevel(screenState,u,0).rgb),0);"
"return float4(saturate(toSrgb(mapColor(linearizeExtended(encoded)))),1);}";

const char* kGammaSource =
"Texture2D screenSampler:register(t0);Texture2D gammaSampler:register(t1);"
"SamplerState screenState:register(s0);SamplerState gammaState:register(s1);"
"float3 gammaGrade(float3 c){"
"float intensity=max(1.0,max(c.r,max(c.g,c.b)));float3 n=saturate(c/intensity);"
"float3 g=float3(gammaSampler.SampleLevel(gammaState,float2(n.r,.5),0).r,"
"gammaSampler.SampleLevel(gammaState,float2(n.g,.5),0).r,"
"gammaSampler.SampleLevel(gammaState,float2(n.b,.5),0).r);return g*intensity;}"
"float4 main(float4 p:SV_POSITION,float2 u:TEXCOORD0):SV_Target{"
"float3 c=max(screenSampler.SampleLevel(screenState,u,0).rgb,0);"
"return float4(max(gammaGrade(c),0),1);}";

const char* kPresentVertexSource =
"float4 main(uint id:SV_VertexID,out float2 uv:TEXCOORD0):SV_POSITION{"
"uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),0,1);}";

const char* kPresentPixelSource =
"Texture2D frameTex:register(t0);Texture2D bloomTex:register(t1);"
"SamplerState frameSampler:register(s0);"
"float3 toLinear(float3 c){float3 lo=c/12.92,hi=pow((c+.055)/1.055,2.4);"
"return lerp(lo,hi,step(.04045,c));}"
"float3 linearizeExtended(float3 c){float intensity=max(1.0,max(c.r,max(c.g,c.b)));"
"return toLinear(saturate(c/intensity))*intensity;}"
"float agxContrast(float x){float x2=x*x,x4=x2*x2;return saturate("
"15.5*x4*x2-40.14*x4*x+31.96*x4-6.868*x2*x+.4298*x2+.1191*x-.00232);}"
"float agxCurve(float x){float v=saturate((log2(max(x,1e-10))+12.47393)/16.5);"
"return pow(agxContrast(v),2.376);}"
"float frostbiteCurve(float x){x=max(x,0);float knee=TQ_PEAK*.75;"
"if(x<=knee)return x;float range=max(TQ_PEAK-knee,1e-4);"
"return min(knee+range*(1-exp(-(x-knee)/range)),TQ_PEAK);}"
"float displayCurve(float x){return agxCurve(x);}"
"float mapLuma(float l){if(TQ_FROSTBITE)return frostbiteCurve(l);"
"float white=displayCurve(1);float low=displayCurve(min(l,1));"
"float range=max(TQ_PEAK-white,0);float high=white+range*(1-exp(-max(l-1,0)/max(range,1)));"
"return l<=1?low:min(high,TQ_PEAK);}"
"float3 mapColor(float3 c){if(TQ_FROSTBITE){float p=max(max(c.r,c.g),c.b);"
"p=max(p,1e-6);float m=mapLuma(p);float3 scaled=c*(m/p);"
"float shoulder=saturate((p-TQ_PEAK*.75)/p);"
"return max(lerp(scaled,float3(m,m,m),shoulder*shoulder),0);}"
"float l=max(dot(c,float3(.2126,.7152,.0722)),1e-6);"
"return max(c*(mapLuma(l)/l),0);}"
"float4 main(float4 p:SV_POSITION,float2 u:TEXCOORD0):SV_Target{"
"float3 c=max(frameTex.SampleLevel(frameSampler,u,0).rgb,0);"
"c+=max(bloomTex.SampleLevel(frameSampler,u,0).rgb,0)*TQ_BLOOM_STRENGTH;"
"c=linearizeExtended(c);"
"float l=max(dot(c,float3(.2126,.7152,.0722)),1e-6);"
"float3 outc=mapColor(c);"
"\n#if TQ_HIGHLIGHT_DEBUG\n"
"float hit=step(1.0001,l);float3 heat=l<1.5?float3(1,1,0):"
"l<2?float3(1,.35,0):l<4?float3(1,0,0):float3(1,0,1);"
"outc=lerp(outc*.28,heat,hit*.88);\n"
"#endif\n"
"outc=max(outc,0)*TQ_PAPER_SCALE;"
"return float4(min(outc,TQ_SCRGB_PEAK),1);}";

const char* kSmaaPrefix =
"#define SMAA_HLSL_4\n"
"#define SMAA_PRESET_HIGH\n"
"cbuffer SmaaMetrics:register(b0){float4 smaaMetrics;}\n"
"#define SMAA_RT_METRICS smaaMetrics\n"
"#define TQ_SMAA_PEAK 16.0\n";

const char* kEdgeWrapper =
"Texture2D tqColorTex:register(t0);\n"
"float tqLuma(float2 u){float y=max(dot(tqColorTex.SampleLevel("
"PointSampler,u,0).rgb,float3(.2126,.7152,.0722)),0);"
"return log2(1+y)/log2(1+TQ_SMAA_PEAK);}\n"
"float4 main(float4 p:SV_POSITION,float2 u:TEXCOORD0):SV_Target{"
"float L=tqLuma(u),Ll=tqLuma(u-float2(smaaMetrics.x,0)),"
"Lt=tqLuma(u-float2(0,smaaMetrics.y));float2 d=abs(L-float2(Ll,Lt));"
"float2 e=step(.05,d);if(e.x+e.y==0)discard;"
"float Lr=tqLuma(u+float2(smaaMetrics.x,0)),Lb=tqLuma(u+float2(0,smaaMetrics.y));"
"float Lll=tqLuma(u-float2(2*smaaMetrics.x,0)),Ltt=tqLuma(u-float2(0,2*smaaMetrics.y));"
"float md=max(max(max(d.x,d.y),abs(L-Lr)),max(abs(L-Lb),max(abs(Ll-Lll),abs(Lt-Ltt))));"
"e*=step(md,2*d);return float4(e,0,0);}\n";

const char* kWeightWrapper =
"Texture2D tqEdgesTex:register(t0);Texture2D tqAreaTex:register(t1);"
"Texture2D tqSearchTex:register(t2);\n"
"float4 main(float4 p:SV_POSITION,float2 u:TEXCOORD0):SV_Target{"
"float2 q;float4 o[3];SMAABlendingWeightCalculationVS(u,q,o);"
"return SMAABlendingWeightCalculationPS(u,q,o,tqEdgesTex,tqAreaTex,tqSearchTex,0);}\n";

const char* kBlendWrapper =
"Texture2D tqColorTex:register(t0);Texture2D tqBlendTex:register(t1);\n"
"float4 main(float4 p:SV_POSITION,float2 u:TEXCOORD0):SV_Target{"
"float4 o;SMAANeighborhoodBlendingVS(u,o);"
"return SMAANeighborhoodBlendingPS(u,o,tqColorTex,tqBlendTex);}\n";

// This crossfade originally rendered through UNORM, which saturated alpha.
// Preserve that blend contract without clamping the FP16 RGB highlights.
const char* kAlphaClampCopySource =
"Texture2D baseSamplerTex:register(t0);"
"SamplerState baseSampler:register(s0);"
"float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0,"
"float4 modulation:TEXCOORD1):SV_Target{"
"float4 value=baseSamplerTex.Sample(baseSampler,uv)*modulation;"
"value.a=saturate(value.a);return value;}";

#include "bloom_shaders.inc"

typedef HRESULT(WINAPI* D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                      ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
                                      ID3DBlob**, ID3DBlob**);

bool compileShader(const std::string& source, const char* target, ID3DBlob** result) {
    if (!g_compiler) g_compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!g_compiler) g_compiler = LoadLibraryW(L"d3dcompiler_43.dll");
    if (!g_compiler) return false;
    D3DCompileFn compile = (D3DCompileFn)(void*)GetProcAddress(g_compiler, "D3DCompile");
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile ? compile(source.data(), source.size(), "tqflicker", nullptr, nullptr,
                                   "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                   result, &errors) : E_FAIL;
    if (FAILED(hr) && errors && errors->GetBufferPointer())
        tq::hdr::log("Shader compile failed (%s): %.*s\r\n", target,
                     (int)errors->GetBufferSize(),
                     (const char*)errors->GetBufferPointer());
    release(errors);
    return SUCCEEDED(hr) && result && *result;
}

// The overlay compiles through the same loaded compiler, and creates its own
// resources through the original device entry points so its texture, sampler
// and pixel shader never reach the classification hooks above.
bool compileOverlayShader(const char* source, const char* target, ID3DBlob** result) {
    return compileShader(std::string(source), target, result);
}

tq::frameoverlay::DeviceCalls overlayDeviceCalls() {
    tq::frameoverlay::DeviceCalls calls = {};
    calls.createTexture2D = g_createTexture2D;
    calls.createShaderResourceView = g_createShaderResourceView;
    calls.createSamplerState = g_createSamplerState;
    calls.createPixelShader = g_createPixelShader;
    calls.compile = &compileOverlayShader;
    return calls;
}

void releaseSizeResources() {
    release(g_smaa.edgeSRV); release(g_smaa.edgeRTV); release(g_smaa.edgeTex);
    release(g_smaa.weightSRV); release(g_smaa.weightRTV); release(g_smaa.weightTex);
    g_smaa.width = g_smaa.height = 0;
}

void releaseSmaa() {
    releaseSizeResources();
    release(g_smaa.edgePS); release(g_smaa.weightPS); release(g_smaa.blendPS);
    release(g_smaa.areaSRV); release(g_smaa.areaTex);
    release(g_smaa.searchSRV); release(g_smaa.searchTex);
    release(g_smaa.metrics); release(g_smaa.linearSampler); release(g_smaa.pointSampler);
    release(g_smaa.blend);
    release(g_smaa.depth); release(g_smaa.raster);
}

void releaseBloomSizeResources() {
    for (unsigned i = 0; i < kBloomMaxLevels; ++i) {
        release(g_bloom.downSRV[i]); release(g_bloom.downRTV[i]);
        release(g_bloom.upSRV[i]); release(g_bloom.upRTV[i]);
    }
    release(g_bloom.downTexture); release(g_bloom.upTexture);
    g_bloom.width = g_bloom.height = g_bloom.levels = 0;
    g_bloom.format = DXGI_FORMAT_UNKNOWN;
    g_bloom.rejectedWidth = g_bloom.rejectedHeight = 0;
    g_bloom.rejectionLogged = false;
}

void releaseBloom() {
    releaseBloomSizeResources();
    release(g_bloom.extractPS); release(g_bloom.downsamplePS);
    release(g_bloom.upsamplePS); release(g_bloom.compositePS);
    release(g_bloom.constants); release(g_bloom.sampler);
    release(g_bloom.opaqueBlend); release(g_bloom.additiveBlend);
    release(g_bloom.depth); release(g_bloom.raster);
    for (unsigned i = 0; i < kBloomTimingSlots; ++i) {
        release(g_bloom.timing[i].disjoint);
        release(g_bloom.timing[i].begin);
        release(g_bloom.timing[i].end);
        g_bloom.timing[i].pending = false;
    }
    g_bloom.timingCursor = g_bloom.timingSamples = 0;
    g_bloom.timingTotalMs = g_bloom.timingMaxMs = 0.0;
    g_bloom.calls = 0;
    g_bloom.freshForPresent = false;
}

void releaseHdrSizeResources() {
    releaseBloomSizeResources();
    release(g_hdr.presentCopySRV); release(g_hdr.presentCopy);
    release(g_hdr.backBufferSRV);
    release(g_hdr.backBufferRTV);
    g_hdr.width = g_hdr.height = 0;
}

void releaseHdr() {
    releaseHdrSizeResources();
    releaseBloom();
    release(g_hdr.colorGradingPS); release(g_hdr.gammaPS);
    release(g_hdr.tonePS); release(g_hdr.presentPS); release(g_hdr.alphaClampPS);
    release(g_hdr.fullscreenVS); release(g_hdr.sampler); release(g_hdr.blend);
    release(g_hdr.depth); release(g_hdr.raster);
}

bool createLookupTexture(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format,
                         UINT pitch, const void* bytes, ID3D11Texture2D** texture,
                         ID3D11ShaderResourceView** srv) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = width; d.Height = height; d.MipLevels = d.ArraySize = 1;
    d.Format = format; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial = {bytes, pitch, 0};
    return SUCCEEDED(g_createTexture2D(device, &d, &initial, texture))
        && SUCCEEDED(device->CreateShaderResourceView(*texture, nullptr, srv));
}

bool createSmaaProgramResources(ID3D11Device* device) {
    if (g_smaa.edgePS) return true;
    std::string canonical((const char*)third_party_smaa_SMAA_hlsl,
                          third_party_smaa_SMAA_hlsl_len);
    ID3DBlob *p0 = nullptr, *p1 = nullptr, *p2 = nullptr;
    bool ok = compileShader(std::string(kSmaaPrefix) + canonical + kEdgeWrapper, "ps_5_0", &p0);
    if (ok) ok = compileShader(std::string(kSmaaPrefix) + canonical + kWeightWrapper, "ps_5_0", &p1);
    if (ok) ok = compileShader(std::string(kSmaaPrefix) + canonical + kBlendWrapper, "ps_5_0", &p2);
    if (ok) ok = SUCCEEDED(g_createPixelShader(device, p0->GetBufferPointer(), p0->GetBufferSize(),
                                                nullptr, &g_smaa.edgePS));
    if (ok) ok = SUCCEEDED(g_createPixelShader(device, p1->GetBufferPointer(), p1->GetBufferSize(),
                                                nullptr, &g_smaa.weightPS));
    if (ok) ok = SUCCEEDED(g_createPixelShader(device, p2->GetBufferPointer(), p2->GetBufferSize(),
                                                nullptr, &g_smaa.blendPS));
    release(p0); release(p1); release(p2);
    if (!ok) { releaseSmaa(); return false; }

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC linear = {};
    linear.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    linear.AddressU = linear.AddressV = linear.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    linear.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_SAMPLER_DESC point = linear;
    point.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = FALSE; depth.StencilEnable = FALSE;
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE; raster.ScissorEnable = FALSE;
    ok = SUCCEEDED(device->CreateBuffer(&bd, nullptr, &g_smaa.metrics))
      && SUCCEEDED(g_createSamplerState(device, &linear, &g_smaa.linearSampler))
      && SUCCEEDED(g_createSamplerState(device, &point, &g_smaa.pointSampler))
      && SUCCEEDED(device->CreateBlendState(&blend, &g_smaa.blend))
      && SUCCEEDED(device->CreateDepthStencilState(&depth, &g_smaa.depth))
      && SUCCEEDED(device->CreateRasterizerState(&raster, &g_smaa.raster))
      && createLookupTexture(device, AREATEX_WIDTH, AREATEX_HEIGHT, DXGI_FORMAT_R8G8_UNORM,
                             AREATEX_PITCH, areaTexBytes, &g_smaa.areaTex, &g_smaa.areaSRV)
      && createLookupTexture(device, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM,
                             SEARCHTEX_PITCH, searchTexBytes, &g_smaa.searchTex, &g_smaa.searchSRV);
    if (!ok) releaseSmaa();
    return ok;
}

std::string defineNumber(const char* name, float value) {
    char number[64];
    _snprintf(number, sizeof(number), "#define %s %.8ff\n", name, value);
    return std::string(number);
}

bool createHdrProgramResources(ID3D11Device* device) {
    const tq::hdr::Runtime& runtime = tq::hdr::runtime();
    if (runtime.settings.toneMap == tq::hdr::ToneOriginal) return true;
    if (g_hdr.colorGradingPS && g_hdr.gammaPS && g_hdr.tonePS
        && (!runtime.fp16Active || (g_hdr.presentPS && g_hdr.fullscreenVS
                                    && g_hdr.alphaClampPS))) return true;

    float peakRelative = runtime.active
        ? runtime.peakNits / runtime.settings.paperWhiteNits : 1.0f;
    if (peakRelative < 1.0f) peakRelative = 1.0f;
    float paperScale = runtime.displayHdr
        ? runtime.settings.paperWhiteNits / 80.0f : 1.0f;
    float scrgbPeak = runtime.active ? runtime.peakNits / 80.0f : paperScale;
    std::string tone = std::string("#define TQ_FROSTBITE ")
        + (runtime.settings.toneMap == tq::hdr::ToneFrostbite ? "1\n" : "0\n")
        + defineNumber("TQ_PEAK", peakRelative) + kToneSource;
    std::string present = std::string("#define TQ_FROSTBITE ")
        + (runtime.settings.toneMap == tq::hdr::ToneFrostbite ? "1\n" : "0\n")
        + std::string("#define TQ_HIGHLIGHT_DEBUG ")
        + (runtime.settings.debug ? "1\n" : "0\n")
        + defineNumber("TQ_PEAK", peakRelative)
        + defineNumber("TQ_PAPER_SCALE", paperScale)
        + defineNumber("TQ_SCRGB_PEAK", scrgbPeak)
        + defineNumber("TQ_BLOOM_STRENGTH", g_options.bloomStrength)
        + kPresentPixelSource;
    ID3DBlob *color = nullptr, *gamma = nullptr, *toneBlob = nullptr;
    ID3DBlob *presentBlob = nullptr, *vertex = nullptr, *alphaClamp = nullptr;
    bool ok = compileShader(kColorGradingSource, "ps_5_0", &color)
           && compileShader(kGammaSource, "ps_5_0", &gamma)
           && compileShader(tone, "ps_5_0", &toneBlob);
    if (ok && runtime.fp16Active)
        ok = compileShader(present, "ps_5_0", &presentBlob)
          && compileShader(kPresentVertexSource, "vs_5_0", &vertex)
          && compileShader(kAlphaClampCopySource, "ps_5_0", &alphaClamp);
    if (ok) ok = SUCCEEDED(g_createPixelShader(
        device, color->GetBufferPointer(), color->GetBufferSize(), nullptr,
        &g_hdr.colorGradingPS));
    if (ok) ok = SUCCEEDED(g_createPixelShader(
        device, gamma->GetBufferPointer(), gamma->GetBufferSize(), nullptr,
        &g_hdr.gammaPS));
    if (ok) ok = SUCCEEDED(g_createPixelShader(
        device, toneBlob->GetBufferPointer(), toneBlob->GetBufferSize(), nullptr,
        &g_hdr.tonePS));
    if (ok && runtime.fp16Active) ok = SUCCEEDED(g_createPixelShader(
        device, presentBlob->GetBufferPointer(), presentBlob->GetBufferSize(), nullptr,
        &g_hdr.presentPS));
    if (ok && runtime.fp16Active) ok = SUCCEEDED(device->CreateVertexShader(
        vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr,
        &g_hdr.fullscreenVS));
    if (ok && runtime.fp16Active) ok = SUCCEEDED(g_createPixelShader(
        device, alphaClamp->GetBufferPointer(), alphaClamp->GetBufferSize(), nullptr,
        &g_hdr.alphaClampPS));
    release(color); release(gamma); release(toneBlob); release(presentBlob); release(vertex);
    release(alphaClamp);
    if (!ok) { releaseHdr(); return false; }

    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_DEPTH_STENCIL_DESC depth = {};
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    ok = SUCCEEDED(g_createSamplerState(device, &sampler, &g_hdr.sampler))
      && SUCCEEDED(device->CreateBlendState(&blend, &g_hdr.blend))
      && SUCCEEDED(device->CreateDepthStencilState(&depth, &g_hdr.depth))
      && SUCCEEDED(device->CreateRasterizerState(&raster, &g_hdr.raster));
    if (!ok) releaseHdr();
    tq::hdr::log("Post-process program: tone=%u fp16=%u hdr=%u peakRelative=%.3f ready=%u\r\n",
        (unsigned)runtime.settings.toneMap, runtime.fp16Active ? 1u : 0u,
        runtime.active ? 1u : 0u,
        peakRelative, ok ? 1u : 0u);
    return ok;
}

bool createBloomProgramResources(ID3D11Device* device) {
    if (g_options.bloom != BloomEnhanced
        || !tq::hdr::runtime().fp16Active) return true;
    if (g_bloom.extractPS && g_bloom.downsamplePS && g_bloom.upsamplePS
        && g_bloom.compositePS && g_bloom.constants) return true;

    ID3DBlob *extract = nullptr, *downsample = nullptr;
    ID3DBlob *upsample = nullptr, *composite = nullptr;
    bool ok = compileShader(kBloomExtractSource, "ps_5_0", &extract)
           && compileShader(kBloomDownsampleSource, "ps_5_0", &downsample)
           && compileShader(kBloomUpsampleSource, "ps_5_0", &upsample)
           && compileShader(kBloomCompositeSource, "ps_5_0", &composite);
    if (ok) ok = SUCCEEDED(g_createPixelShader(
        device, extract->GetBufferPointer(), extract->GetBufferSize(), nullptr,
        &g_bloom.extractPS));
    if (ok) ok = SUCCEEDED(g_createPixelShader(
        device, downsample->GetBufferPointer(), downsample->GetBufferSize(), nullptr,
        &g_bloom.downsamplePS));
    if (ok) ok = SUCCEEDED(g_createPixelShader(
        device, upsample->GetBufferPointer(), upsample->GetBufferSize(), nullptr,
        &g_bloom.upsamplePS));
    if (ok) ok = SUCCEEDED(g_createPixelShader(
        device, composite->GetBufferPointer(), composite->GetBufferSize(), nullptr,
        &g_bloom.compositePS));
    release(extract); release(downsample); release(upsample); release(composite);

    D3D11_BUFFER_DESC constants = {};
    constants.ByteWidth = 32;
    constants.Usage = D3D11_USAGE_DEFAULT;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BLEND_DESC opaque = {};
    opaque.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_BLEND_DESC additive = {};
    additive.RenderTarget[0].BlendEnable = TRUE;
    additive.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    additive.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    additive.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    additive.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    additive.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    additive.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    additive.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN
      | D3D11_COLOR_WRITE_ENABLE_BLUE;
    D3D11_DEPTH_STENCIL_DESC depth = {};
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    if (ok) ok = SUCCEEDED(device->CreateBuffer(&constants, nullptr, &g_bloom.constants))
              && SUCCEEDED(g_createSamplerState(device, &sampler, &g_bloom.sampler))
              && SUCCEEDED(device->CreateBlendState(&opaque, &g_bloom.opaqueBlend))
              && SUCCEEDED(device->CreateBlendState(&additive, &g_bloom.additiveBlend))
              && SUCCEEDED(device->CreateDepthStencilState(&depth, &g_bloom.depth))
              && SUCCEEDED(device->CreateRasterizerState(&raster, &g_bloom.raster));
    if (!ok) {
        releaseBloom();
        tq::hdr::log("Enhanced bloom program unavailable; original bloom retained\r\n");
        return false;
    }

    if (tq::hdr::runtime().settings.debug) {
        bool timingReady = true;
        for (unsigned i = 0; i < kBloomTimingSlots && timingReady; ++i) {
            D3D11_QUERY_DESC query = {};
            query.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            timingReady = SUCCEEDED(device->CreateQuery(&query,
                                                        &g_bloom.timing[i].disjoint));
            query.Query = D3D11_QUERY_TIMESTAMP;
            if (timingReady) timingReady = SUCCEEDED(device->CreateQuery(
                &query, &g_bloom.timing[i].begin));
            if (timingReady) timingReady = SUCCEEDED(device->CreateQuery(
                &query, &g_bloom.timing[i].end));
        }
        if (!timingReady) {
            for (unsigned i = 0; i < kBloomTimingSlots; ++i) {
                release(g_bloom.timing[i].disjoint);
                release(g_bloom.timing[i].begin);
                release(g_bloom.timing[i].end);
            }
            tq::hdr::log("Enhanced bloom GPU timing unavailable\r\n");
        }
    }
    tq::hdr::log("Enhanced bloom program ready\r\n");
    return true;
}

bool createProgramResources(ID3D11Device* device) {
    // Timestamp queries are device objects like any other, so they are built
    // here rather than from inside a hooked call.
    if (tq::probe::enabled()) tq::probe::createResources(device);
    bool ok = true;
    if (g_options.smaa) ok = createSmaaProgramResources(device);
    if (ok) ok = createHdrProgramResources(device);
    if (ok && g_options.bloom == BloomEnhanced
        && tq::hdr::runtime().fp16Active)
        createBloomProgramResources(device);
    // Failing to build the overlay never fails the frame the game asked for.
    if (ok && tq::frameoverlay::enabled())
        tq::hdr::log("Frame overlay resources: ready=%u\r\n",
                     tq::frameoverlay::createResources(device, overlayDeviceCalls())
                         ? 1u : 0u);
    return ok;
}

bool createTarget(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format,
                  ID3D11Texture2D** texture, ID3D11RenderTargetView** rtv,
                  ID3D11ShaderResourceView** srv) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = width; d.Height = height; d.MipLevels = d.ArraySize = 1;
    d.Format = format; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(g_createTexture2D(device, &d, nullptr, texture))
        && SUCCEEDED(device->CreateRenderTargetView(*texture, nullptr, rtv))
        && SUCCEEDED(device->CreateShaderResourceView(*texture, nullptr, srv));
}

bool prepareSmaa(ID3D11Device* device, UINT width, UINT height) {
    if (InterlockedCompareExchange(&g_programState, 2, 2) != 2) return false;
    if (g_smaa.width == width && g_smaa.height == height) return true;
    releaseSizeResources();
    if (!createTarget(device, width, height, DXGI_FORMAT_R8G8_UNORM,
                      &g_smaa.edgeTex, &g_smaa.edgeRTV, &g_smaa.edgeSRV)
        || !createTarget(device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                         &g_smaa.weightTex, &g_smaa.weightRTV, &g_smaa.weightSRV)) {
        releaseSizeResources(); return false;
    }
    g_smaa.width = width; g_smaa.height = height;
    return true;
}

struct SavedState {
    ID3D11InputLayout* layout; D3D11_PRIMITIVE_TOPOLOGY topology;
    ID3D11VertexShader* vs; ID3D11PixelShader* ps; ID3D11GeometryShader* gs;
    ID3D11HullShader* hs; ID3D11DomainShader* ds;
    ID3D11Buffer* psCB; ID3D11ShaderResourceView* srvs[3]; ID3D11SamplerState* samplers[2];
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView* dsv; ID3D11BlendState* blend; FLOAT blendFactor[4]; UINT sampleMask;
    ID3D11DepthStencilState* depth; UINT stencilRef; ID3D11RasterizerState* raster;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]; UINT viewportCount;
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]; UINT scissorCount;
};

void saveState(ID3D11DeviceContext* c, SavedState& s) {
    memset(&s, 0, sizeof(s));
    c->IAGetInputLayout(&s.layout); c->IAGetPrimitiveTopology(&s.topology);
    c->VSGetShader(&s.vs, nullptr, nullptr); c->PSGetShader(&s.ps, nullptr, nullptr);
    c->GSGetShader(&s.gs, nullptr, nullptr); c->HSGetShader(&s.hs, nullptr, nullptr);
    c->DSGetShader(&s.ds, nullptr, nullptr); c->PSGetConstantBuffers(0, 1, &s.psCB);
    c->PSGetShaderResources(0, 3, s.srvs); c->PSGetSamplers(0, 2, s.samplers);
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtvs, &s.dsv);
    c->OMGetBlendState(&s.blend, s.blendFactor, &s.sampleMask);
    c->OMGetDepthStencilState(&s.depth, &s.stencilRef); c->RSGetState(&s.raster);
    s.viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    c->RSGetViewports(&s.viewportCount, s.viewports);
    s.scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    c->RSGetScissorRects(&s.scissorCount, s.scissors);
}

void restoreState(ID3D11DeviceContext* c, SavedState& s) {
    c->IASetInputLayout(s.layout); c->IASetPrimitiveTopology(s.topology);
    c->VSSetShader(s.vs, nullptr, 0); c->PSSetShader(s.ps, nullptr, 0);
    c->GSSetShader(s.gs, nullptr, 0); c->HSSetShader(s.hs, nullptr, 0); c->DSSetShader(s.ds, nullptr, 0);
    c->PSSetConstantBuffers(0, 1, &s.psCB); c->PSSetShaderResources(0, 3, s.srvs);
    c->PSSetSamplers(0, 2, s.samplers);
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtvs, s.dsv);
    c->OMSetBlendState(s.blend, s.blendFactor, s.sampleMask);
    c->OMSetDepthStencilState(s.depth, s.stencilRef); c->RSSetState(s.raster);
    c->RSSetViewports(s.viewportCount, s.viewports); c->RSSetScissorRects(s.scissorCount, s.scissors);
    release(s.layout); release(s.vs); release(s.ps); release(s.gs); release(s.hs); release(s.ds);
    release(s.psCB);
    for (UINT i = 0; i < 3; ++i) release(s.srvs[i]);
    for (UINT i = 0; i < 2; ++i) release(s.samplers[i]);
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) release(s.rtvs[i]);
    release(s.dsv); release(s.blend); release(s.depth); release(s.raster);
}

struct BloomConstants {
    float sourceTexel[2];
    float threshold;
    float knee;
    float invRange;
    float saturation;
    float strength;
    float scatter;
};

UINT bloomMipSize(UINT base, UINT level) {
    UINT value = base >> level;
    return value ? value : 1;
}

bool bloomFormatSupported(ID3D11Device* device, DXGI_FORMAT format) {
    UINT support = 0;
    const UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D
                        | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE
                        | D3D11_FORMAT_SUPPORT_RENDER_TARGET;
    return device && SUCCEEDED(device->CheckFormatSupport(format, &support))
        && (support & required) == required;
}

bool createBloomTexture(ID3D11Device* device, UINT width, UINT height, UINT levels,
                        DXGI_FORMAT format, ID3D11Texture2D** texture,
                        ID3D11ShaderResourceView** srvs,
                        ID3D11RenderTargetView** rtvs) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width; desc.Height = height;
    desc.MipLevels = levels; desc.ArraySize = 1;
    desc.Format = format; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_createTexture2D(device, &desc, nullptr, texture)) || !*texture)
        return false;
    for (UINT level = 0; level < levels; ++level) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = format;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MostDetailedMip = level;
        srv.Texture2D.MipLevels = 1;
        D3D11_RENDER_TARGET_VIEW_DESC rtv = {};
        rtv.Format = format;
        rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtv.Texture2D.MipSlice = level;
        if (FAILED(g_createShaderResourceView(device, *texture, &srv, &srvs[level]))
            || FAILED(g_createRenderTargetView(device, *texture, &rtv, &rtvs[level])))
            return false;
    }
    return true;
}

bool prepareBloom(ID3D11Device* device, UINT fullWidth, UINT fullHeight) {
    if (!device || !fullWidth || !fullHeight || !g_bloom.extractPS
        || !g_hdr.fullscreenVS) return false;
    UINT width = (fullWidth + 3) / 4;
    UINT height = (fullHeight + 3) / 4;
    UINT levels = 1;
    while (levels < kBloomMaxLevels) {
        UINT nextWidth = bloomMipSize(width, levels);
        UINT nextHeight = bloomMipSize(height, levels);
        if (nextWidth < 16 || nextHeight < 16) break;
        ++levels;
    }
    if (g_bloom.downTexture && g_bloom.width == width && g_bloom.height == height
        && g_bloom.levels == levels) return true;
    if (g_bloom.rejectedWidth == fullWidth && g_bloom.rejectedHeight == fullHeight)
        return false;
    releaseBloomSizeResources();
    DXGI_FORMAT format = bloomFormatSupported(device, DXGI_FORMAT_R11G11B10_FLOAT)
                       ? DXGI_FORMAT_R11G11B10_FLOAT
                       : DXGI_FORMAT_R16G16B16A16_FLOAT;
    bool ok = bloomFormatSupported(device, format)
           && createBloomTexture(device, width, height, levels, format,
                                 &g_bloom.downTexture, g_bloom.downSRV,
                                 g_bloom.downRTV)
           && createBloomTexture(device, width, height, levels, format,
                                 &g_bloom.upTexture, g_bloom.upSRV,
                                 g_bloom.upRTV);
    if (!ok) {
        releaseBloomSizeResources();
        g_bloom.rejectedWidth = fullWidth;
        g_bloom.rejectedHeight = fullHeight;
        tq::hdr::log("Enhanced bloom targets unavailable at %ux%u; original bloom retained\r\n",
                     fullWidth, fullHeight);
        return false;
    }
    g_bloom.width = width; g_bloom.height = height;
    g_bloom.levels = levels; g_bloom.format = format;
    uint64_t pixels = 0;
    for (UINT i = 0; i < levels; ++i)
        pixels += (uint64_t)bloomMipSize(width, i) * bloomMipSize(height, i);
    unsigned bytesPerPixel = format == DXGI_FORMAT_R11G11B10_FLOAT ? 4u : 8u;
    tq::hdr::log("Enhanced bloom targets: source=%ux%u base=%ux%u levels=%u "
                 "format=%u memory=%.2fMiB\r\n",
                 fullWidth, fullHeight, width, height, levels, (unsigned)format,
                 (double)(pixels * bytesPerPixel * 2) / (1024.0 * 1024.0));
    return true;
}

void pollBloomTimings(ID3D11DeviceContext* context) {
    if (!tq::hdr::runtime().settings.debug || !context) return;
    for (unsigned i = 0; i < kBloomTimingSlots; ++i) {
        BloomTiming& timing = g_bloom.timing[i];
        if (!timing.pending || !timing.disjoint || !timing.begin || !timing.end)
            continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
        UINT64 begin = 0, end = 0;
        if (context->GetData(timing.disjoint, &disjoint, sizeof(disjoint),
                             D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
            || context->GetData(timing.begin, &begin, sizeof(begin),
                                D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
            || context->GetData(timing.end, &end, sizeof(end),
                                D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        timing.pending = false;
        if (disjoint.Disjoint || !disjoint.Frequency || end < begin) continue;
        double milliseconds = (double)(end - begin) * 1000.0
                            / (double)disjoint.Frequency;
        g_bloom.timingTotalMs += milliseconds;
        if (milliseconds > g_bloom.timingMaxMs) g_bloom.timingMaxMs = milliseconds;
        if (++g_bloom.timingSamples >= 120) {
            tq::hdr::log("Enhanced bloom GPU: mean=%.3fms max=%.3fms samples=%u\r\n",
                         g_bloom.timingTotalMs / g_bloom.timingSamples,
                         g_bloom.timingMaxMs, g_bloom.timingSamples);
            g_bloom.timingSamples = 0;
            g_bloom.timingTotalMs = g_bloom.timingMaxMs = 0.0;
        }
    }
}

BloomTiming* beginBloomTiming(ID3D11DeviceContext* context) {
    pollBloomTimings(context);
    if (!tq::hdr::runtime().settings.debug) return nullptr;
    BloomTiming& timing = g_bloom.timing[g_bloom.timingCursor++ % kBloomTimingSlots];
    if (timing.pending || !timing.disjoint || !timing.begin || !timing.end)
        return nullptr;
    context->Begin(timing.disjoint);
    context->End(timing.begin);
    return &timing;
}

void endBloomTiming(ID3D11DeviceContext* context, BloomTiming* timing) {
    if (!context || !timing) return;
    context->End(timing->end);
    context->End(timing->disjoint);
    timing->pending = true;
}

void updateBloomConstants(ID3D11DeviceContext* context, BloomConstants& constants,
                          UINT sourceWidth, UINT sourceHeight) {
    constants.sourceTexel[0] = 1.0f / sourceWidth;
    constants.sourceTexel[1] = 1.0f / sourceHeight;
    context->UpdateSubresource(g_bloom.constants, 0, nullptr, &constants, 0, 0);
}

void setBloomViewport(ID3D11DeviceContext* context, UINT width, UINT height) {
    D3D11_VIEWPORT viewport = {0, 0, (FLOAT)width, (FLOAT)height, 0, 1};
    context->RSSetViewports(1, &viewport);
}

bool enhancedBloomSelected() {
    if (!g_globalBloomEnabled) return false;
    if (!g_options.bloomToggle) return true;
    bool chord = (GetAsyncKeyState(VK_CONTROL) & 0x8000)
              && (GetAsyncKeyState(VK_SHIFT) & 0x8000)
              && (GetAsyncKeyState('B') & 0x8000);
    if (chord && !g_bloomToggleKeyDown) {
        g_bloomEnhancedRuntime = !g_bloomEnhancedRuntime;
        // Restore native bloom immediately when comparing it.  Enabling the
        // replacement suppresses native bloom only after the global pass has
        // actually rendered successfully, preserving failure fallback.
        if (!g_bloomEnhancedRuntime) tq::bloomhook::setSuppression(false);
        tq::hdr::log("Bloom toggle: path=%s\r\n",
                     g_bloomEnhancedRuntime ? "enhanced" : "original");
    }
    g_bloomToggleKeyDown = chord;
    return g_bloomEnhancedRuntime;
}

bool renderEnhancedBloom() {
    tq::probe::Scope bloomTiming(tq::probe::PhaseBloom);
    const tq::hdr::Runtime& runtime = tq::hdr::runtime();
    if (!enhancedBloomSelected() || !runtime.fp16Active) {
        tq::bloomhook::setSuppression(false);
        return false;
    }
    // Treat a configured zero as a true no-op without spending GPU time, while
    // still suppressing the native pass exactly as bloom=off would.
    if (g_options.bloomStrength == 0.0f) {
        g_bloom.freshForPresent = true;
        tq::bloomhook::setSuppression(true);
        return true;
    }
    if (InterlockedCompareExchange(&g_programState, 2, 2) != 2
        || !g_context || !g_device || !g_hdr.fullscreenVS
        || !g_bloom.extractPS || !g_bloom.downsamplePS
        || !g_bloom.upsamplePS || !g_bloom.compositePS) {
        tq::bloomhook::setSuppression(false);
        return false;
    }

    SavedState old;
    saveState(g_context, old);
    ID3D11Resource* activeOutput = nullptr;
    if (old.rtvs[0]) old.rtvs[0]->GetResource(&activeOutput);
    bool outputMatches = activeOutput
                      && identityOf(activeOutput) == g_backBufferIdentity;
    release(activeOutput);
    bool fullViewport = old.viewportCount == 1 && old.viewports[0].TopLeftX == 0
                     && old.viewports[0].TopLeftY == 0
                     && old.viewports[0].Width == (FLOAT)g_hdr.width
                     && old.viewports[0].Height == (FLOAT)g_hdr.height;
    bool valid = g_hdr.backBufferSRV && g_hdr.backBufferRTV && outputMatches
              && g_hdr.width == g_backBufferWidth
              && g_hdr.height == g_backBufferHeight
              && g_hdr.width && g_hdr.height
              && fullViewport
              && prepareBloom(g_device, g_hdr.width, g_hdr.height);
    if (!valid) {
        if (runtime.settings.debug && !g_bloom.rejectionLogged) {
            tq::hdr::log("Enhanced bloom rejected source: view=%u size=%ux%u "
                         "viewport=%u output=%u\r\n",
                         g_hdr.backBufferSRV ? 1u : 0u,
                         g_hdr.width, g_hdr.height, fullViewport ? 1u : 0u,
                         outputMatches ? 1u : 0u);
            g_bloom.rejectionLogged = true;
        }
        restoreState(g_context, old);
        tq::bloomhook::setSuppression(false);
        return false;
    }

    // Fixed display-independent profile.  It is intentionally not derived
    // from Titan Quest's sparsely used regional bloom records: the enhanced
    // pass runs globally and the selected display mapper owns the final look.
    const float threshold = 1.0f;
    const float knee = 0.25f;
    const float strength = g_options.bloomStrength;
    const float saturation = 1.0f;
    BloomConstants constants = {};
    constants.threshold = threshold;
    constants.knee = knee;
    constants.invRange = 1.0f;
    constants.saturation = saturation;
    constants.strength = strength;
    constants.scatter = 0.65f;

    bool wasInside = g_inside;
    g_inside = true;
    BloomTiming* timing = beginBloomTiming(g_context);
    tq::probe::gpuBegin(g_context, tq::probe::GpuBloom);
    ID3D11ShaderResourceView* nullViews[3] = {};
    g_psSetShaderResources(g_context, 0, 3, nullViews);
    g_omSetRenderTargets(g_context, 0, nullptr, nullptr);
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_hdr.fullscreenVS, nullptr, 0);
    g_context->GSSetShader(nullptr, nullptr, 0);
    g_context->HSSetShader(nullptr, nullptr, 0);
    g_context->DSSetShader(nullptr, nullptr, 0);
    g_context->PSSetConstantBuffers(0, 1, &g_bloom.constants);
    g_context->PSSetSamplers(0, 1, &g_bloom.sampler);
    g_context->OMSetDepthStencilState(g_bloom.depth, 0);
    g_context->RSSetState(g_bloom.raster);
    g_context->OMSetBlendState(g_bloom.opaqueBlend, nullptr, 0xffffffff);

    updateBloomConstants(g_context, constants, g_hdr.width, g_hdr.height);
    setBloomViewport(g_context, g_bloom.width, g_bloom.height);
    g_omSetRenderTargets(g_context, 1, &g_bloom.downRTV[0], nullptr);
    g_psSetShaderResources(g_context, 0, 1, &g_hdr.backBufferSRV);
    g_psSetShader(g_context, g_bloom.extractPS, nullptr, 0);
    g_draw(g_context, 3, 0);

    for (UINT level = 1; level < g_bloom.levels; ++level) {
        g_psSetShaderResources(g_context, 0, 3, nullViews);
        UINT sourceWidth = bloomMipSize(g_bloom.width, level - 1);
        UINT sourceHeight = bloomMipSize(g_bloom.height, level - 1);
        updateBloomConstants(g_context, constants, sourceWidth, sourceHeight);
        setBloomViewport(g_context, bloomMipSize(g_bloom.width, level),
                         bloomMipSize(g_bloom.height, level));
        g_omSetRenderTargets(g_context, 1, &g_bloom.downRTV[level], nullptr);
        g_psSetShaderResources(g_context, 0, 1, &g_bloom.downSRV[level - 1]);
        g_psSetShader(g_context, g_bloom.downsamplePS, nullptr, 0);
        g_draw(g_context, 3, 0);
    }

    for (int level = (int)g_bloom.levels - 2; level >= 0; --level) {
        g_psSetShaderResources(g_context, 0, 3, nullViews);
        UINT lowerWidth = bloomMipSize(g_bloom.width, level + 1);
        UINT lowerHeight = bloomMipSize(g_bloom.height, level + 1);
        updateBloomConstants(g_context, constants, lowerWidth, lowerHeight);
        setBloomViewport(g_context, bloomMipSize(g_bloom.width, level),
                         bloomMipSize(g_bloom.height, level));
        g_omSetRenderTargets(g_context, 1, &g_bloom.upRTV[level], nullptr);
        ID3D11ShaderResourceView* inputs[2] = {
            level == (int)g_bloom.levels - 2
                ? g_bloom.downSRV[level + 1] : g_bloom.upSRV[level + 1],
            g_bloom.downSRV[level]
        };
        g_psSetShaderResources(g_context, 0, 2, inputs);
        g_psSetShader(g_context, g_bloom.upsamplePS, nullptr, 0);
        g_draw(g_context, 3, 0);
    }

    // Keep Titan Quest's game-space back buffer pristine. Some menu/loading
    // transitions deliberately reuse it; writing bloom here would make the
    // next extraction bloom an already-bloomed image. The final presentation
    // shader samples this completed pyramid once instead.
    g_psSetShaderResources(g_context, 0, 3, nullViews);

    endBloomTiming(g_context, timing);
    tq::probe::gpuEnd(g_context, tq::probe::GpuBloom);
    restoreState(g_context, old);
    g_inside = wasInside;

    ++g_bloom.calls;
    g_bloom.freshForPresent = true;
    tq::bloomhook::setSuppression(true);
    if (runtime.settings.debug
        && (g_bloom.calls <= 3 || !(g_bloom.calls % 300)))
        tq::hdr::log("Enhanced bloom draw: call=%u target=%u threshold=%.3f "
                     "knee=%.3f strength=%.3f levels=%u\r\n",
                     g_bloom.calls, 1000u, threshold, knee, strength,
                     g_bloom.levels);
    return true;
}

void releaseFlipOutputTargets() {
    g_flipBackBufferSlots = 0;
}

bool viewTargetsTexture(ID3D11RenderTargetView* view, ID3D11Texture2D* texture) {
    if (!view || !texture) return false;
    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    bool matches = resource && identityOf(resource) == identityOf(texture);
    release(resource);
    return matches;
}

void rememberFlipOutputTargets(const SavedState& state,
                               ID3D11Texture2D* backBuffer) {
    UINT slots = 0;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        if (viewTargetsTexture(state.rtvs[i], backBuffer)) slots |= 1u << i;
    g_flipBackBufferSlots = slots;
}

bool restoreFlipOutputTargets() {
    if (!g_flipBackBufferSlots || !g_context || !g_omSetRenderTargets
        || !g_hdr.backBufferRTV) return false;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                  rtvs, &dsv);
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        if (!(g_flipBackBufferSlots & (1u << i)) || rtvs[i]) continue;
        rtvs[i] = g_hdr.backBufferRTV;
        rtvs[i]->AddRef();
    }
    g_inside = true;
    g_omSetRenderTargets(g_context, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                         rtvs, dsv);
    g_inside = false;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        release(rtvs[i]);
    release(dsv);
    if (!InterlockedCompareExchange(&g_firstFlipOutputRestoreLogged, 1, 0))
        tq::hdr::log("Flip-model output binding restored after Present\r\n");
    return true;
}

void issueDraw(ID3D11DeviceContext* c, bool indexed, UINT count, UINT start, INT base) {
    if (indexed) g_drawIndexed(c, count, start, base);
    else g_draw(c, count, start);
}

bool runSmaa(ID3D11DeviceContext* c, bool indexed, UINT count, UINT start, INT base) {
    tq::probe::Scope timing(tq::probe::PhaseSmaa);
    tq::probe::GpuScope gpuTiming(c, tq::probe::GpuSmaa);
    SavedState old;
    saveState(c, old);
    ID3D11ShaderResourceView* source = old.srvs[0];
    ID3D11RenderTargetView* output = old.rtvs[0];
    if (!source || !output) { restoreState(c, old); return false; }
    ID3D11Resource *sourceResource = nullptr, *outputResource = nullptr;
    source->GetResource(&sourceResource); output->GetResource(&outputResource);
    ID3D11Texture2D *sourceTex = nullptr, *outputTex = nullptr;
    if (sourceResource) sourceResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&sourceTex);
    if (outputResource) outputResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&outputTex);
    D3D11_TEXTURE2D_DESC sd = {}, od = {};
    if (sourceTex) sourceTex->GetDesc(&sd);
    if (outputTex) outputTex->GetDesc(&od);
    bool valid = sourceTex && outputTex && sd.Width == od.Width && sd.Height == od.Height
              && sd.SampleDesc.Count == 1 && od.SampleDesc.Count == 1
              && prepareSmaa(g_device, sd.Width, sd.Height);
    release(sourceTex); release(outputTex); release(sourceResource); release(outputResource);
    if (!valid) { restoreState(c, old); return false; }

    float metrics[4] = {1.0f / sd.Width, 1.0f / sd.Height, (float)sd.Width, (float)sd.Height};
    c->UpdateSubresource(g_smaa.metrics, 0, nullptr, metrics, 0, 0);
    c->PSSetConstantBuffers(0, 1, &g_smaa.metrics);
    c->OMSetBlendState(g_smaa.blend, nullptr, 0xffffffff); c->OMSetDepthStencilState(g_smaa.depth, 0);
    c->RSSetState(g_smaa.raster);
    D3D11_VIEWPORT viewport = {0, 0, (FLOAT)sd.Width, (FLOAT)sd.Height, 0, 1};
    c->RSSetViewports(1, &viewport);
    const FLOAT clear[4] = {0, 0, 0, 0};
    c->ClearRenderTargetView(g_smaa.edgeRTV, clear);
    c->OMSetRenderTargets(1, &g_smaa.edgeRTV, nullptr);
    c->PSSetSamplers(0, 1, &g_smaa.pointSampler);
    c->PSSetShaderResources(0, 1, &source); c->PSSetShader(g_smaa.edgePS, nullptr, 0);
    issueDraw(c, indexed, count, start, base);
    ID3D11ShaderResourceView* nulls[3] = {nullptr, nullptr, nullptr};
    c->PSSetShaderResources(0, 3, nulls);
    c->ClearRenderTargetView(g_smaa.weightRTV, clear);
    c->OMSetRenderTargets(1, &g_smaa.weightRTV, nullptr);
    c->PSSetSamplers(0, 1, &g_smaa.linearSampler);
    ID3D11ShaderResourceView* weightInputs[3] = {g_smaa.edgeSRV, g_smaa.areaSRV, g_smaa.searchSRV};
    c->PSSetShaderResources(0, 3, weightInputs); c->PSSetShader(g_smaa.weightPS, nullptr, 0);
    issueDraw(c, indexed, count, start, base);
    c->PSSetShaderResources(0, 3, nulls); c->OMSetRenderTargets(1, &output, nullptr);
    ID3D11ShaderResourceView* finalInputs[2] = {source, g_smaa.weightSRV};
    c->PSSetShaderResources(0, 2, finalInputs); c->PSSetShader(g_smaa.blendPS, nullptr, 0);
    issueDraw(c, indexed, count, start, base);
    c->PSSetShaderResources(0, 3, nulls);
    restoreState(c, old);
    return true;
}

bool prepareHdrPresentation(IDXGISwapChain* swapChain) {
    if (!swapChain || !g_device || !tq::hdr::runtime().fp16Active
        || !g_hdr.presentPS || !g_hdr.fullscreenVS) return false;
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    (void**)&backBuffer)) || !backBuffer)
        return false;
    D3D11_TEXTURE2D_DESC desc = {};
    backBuffer->GetDesc(&desc);
    g_backBufferIdentity = identityOf(backBuffer);
    g_backBufferWidth = desc.Width; g_backBufferHeight = desc.Height;
    if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
        backBuffer->Release(); return false;
    }
    if (g_hdr.presentCopy && g_hdr.width == desc.Width && g_hdr.height == desc.Height) {
        backBuffer->Release(); return true;
    }
    releaseHdrSizeResources();
    D3D11_TEXTURE2D_DESC copy = desc;
    copy.Usage = D3D11_USAGE_DEFAULT;
    copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copy.CPUAccessFlags = 0; copy.MiscFlags = 0;
    bool ok = SUCCEEDED(g_createTexture2D(g_device, &copy, nullptr, &g_hdr.presentCopy))
           && SUCCEEDED(g_createShaderResourceView(
                g_device, g_hdr.presentCopy, nullptr, &g_hdr.presentCopySRV))
           && SUCCEEDED(g_createRenderTargetView(
                g_device, backBuffer, nullptr, &g_hdr.backBufferRTV));
    if (ok && g_options.bloom == BloomEnhanced) {
        HRESULT bloomSource = g_createShaderResourceView(
            g_device, backBuffer, nullptr, &g_hdr.backBufferSRV);
        if (FAILED(bloomSource)) {
            release(g_hdr.backBufferSRV);
            tq::hdr::log("Enhanced bloom back-buffer view unavailable: hr=0x%08x\r\n",
                         (unsigned)bloomSource);
        }
    }
    if (ok) { g_hdr.width = desc.Width; g_hdr.height = desc.Height; }
    else releaseHdrSizeResources();
    tq::hdr::log("FP16 presentation resources: %ux%u format=%u ready=%u\r\n",
                 desc.Width, desc.Height, (unsigned)desc.Format, ok ? 1u : 0u);
    backBuffer->Release();
    return ok;
}


// The overlay owns its rasterisation and its draw; the pipeline-state
// discipline every other enhancement here follows stays on this side, along
// with suppressing our own draw hooks for the two triangles it submits.
//
// In FP16 mode it goes on the back buffer itself rather than on whatever the
// game happened to leave bound, so the display mapper that runs next maps the
// overlay with the frame instead of leaving it in a colour space it does not
// know.
void drawFrameOverlay(IDXGISwapChain* swapChain) {
    if (!tq::frameoverlay::enabled() || !g_device || !g_context) return;
    if (InterlockedCompareExchange(&g_programState, 2, 2) != 2) return;
    tq::probe::Scope timing(tq::probe::PhaseOverlayDraw);
    SavedState old;
    saveState(g_context, old);
    ID3D11RenderTargetView* target = old.rtvs[0];
    if (tq::hdr::runtime().fp16Active && prepareHdrPresentation(swapChain)
        && g_hdr.backBufferRTV)
        target = g_hdr.backBufferRTV;
    if (target) {
        UINT fallback = g_backBufferWidth ? g_backBufferWidth
                      : old.viewportCount ? (UINT)old.viewports[0].Width : 0;
        // The target may still be bound as a post-process input; leaving it
        // there would make the overlay draw a read/write hazard.
        ID3D11ShaderResourceView* noSrvs[3] = {};
        bool wasInside = g_inside;
        g_inside = true;
        g_context->PSSetShaderResources(0, 3, noSrvs);
        tq::frameoverlay::draw(g_device, g_context, target, fallback);
        g_inside = wasInside;
    }
    restoreState(g_context, old);
}

bool presentHdrFrame(IDXGISwapChain* swapChain) {
    if (InterlockedCompareExchange(&g_programState, 2, 2) != 2
        || !prepareHdrPresentation(swapChain)) return false;
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    (void**)&backBuffer)) || !backBuffer)
        return false;
    SavedState old;
    saveState(g_context, old);
    const bool bloomReady = g_bloom.freshForPresent
                         && enhancedBloomSelected()
                         && g_bloom.levels > 0;
    rememberFlipOutputTargets(old, backBuffer);
    ID3D11RenderTargetView* noTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11ShaderResourceView* noSrvs[3] = {};
    g_context->PSSetShaderResources(0, 3, noSrvs);
    g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                  noTargets, nullptr);
    g_inside = true;
    g_context->CopyResource(g_hdr.presentCopy, backBuffer);
    g_context->OMSetRenderTargets(1, &g_hdr.backBufferRTV, nullptr);
    g_context->OMSetBlendState(g_hdr.blend, nullptr, 0xffffffff);
    g_context->OMSetDepthStencilState(g_hdr.depth, 0);
    g_context->RSSetState(g_hdr.raster);
    D3D11_VIEWPORT viewport = {0, 0, (FLOAT)g_hdr.width, (FLOAT)g_hdr.height, 0, 1};
    g_context->RSSetViewports(1, &viewport);
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_hdr.fullscreenVS, nullptr, 0);
    g_context->PSSetShader(g_hdr.presentPS, nullptr, 0);
    g_context->PSSetSamplers(0, 1, &g_hdr.sampler);
    ID3D11ShaderResourceView* presentInputs[2] = {
        g_hdr.presentCopySRV,
        bloomReady ? (g_bloom.levels > 1 ? g_bloom.upSRV[0]
                                         : g_bloom.downSRV[0])
                   : nullptr
    };
    g_context->PSSetShaderResources(0, 2, presentInputs);
    g_draw(g_context, 3, 0);
    g_context->PSSetShaderResources(0, 3, noSrvs);
    g_bloom.freshForPresent = false;
    g_inside = false;
    restoreState(g_context, old);
    backBuffer->Release();
    return true;
}

bool viewTargetsCurrentBackBuffer(ID3D11RenderTargetView* view) {
    if (!view || !g_swapChain) return false;
    ID3D11Resource* viewed = nullptr;
    ID3D11Texture2D* current = nullptr;
    view->GetResource(&viewed);
    HRESULT hr = g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                        (void**)&current);
    bool matches = viewed && SUCCEEDED(hr) && current
                && identityOf(viewed) == identityOf(current);
    release(viewed);
    release(current);
    return matches;
}

bool shouldClampRegionalCompositeAlpha(ID3D11DeviceContext* context) {
    if (!context || context != g_context || !tq::hdr::runtime().fp16Active
        || !g_hdr.alphaClampPS || !g_gamePixelShader) return false;
    bool knownCopy = false;
    for (unsigned i = 0; i < g_alphaClampCopyCount; ++i)
        if (g_gamePixelShader == g_alphaClampCopies[i]) knownCopy = true;
    if (!knownCopy) return false;

    ID3D11ShaderResourceView* sourceView = nullptr;
    context->PSGetShaderResources(0, 1, &sourceView);
    ID3D11Resource* source = nullptr;
    if (sourceView) sourceView->GetResource(&source);
    bool sourceMatches = screenTargetId(identityOf(source)) == 5;
    release(source);
    release(sourceView);
    if (!sourceMatches) return false;

    ID3D11RenderTargetView* destination = nullptr;
    context->OMGetRenderTargets(1, &destination, nullptr);
    bool destinationMatches = viewTargetsCurrentBackBuffer(destination);
    release(destination);
    if (!destinationMatches) return false;

    ID3D11BlendState* blend = nullptr;
    FLOAT factor[4] = {};
    UINT sampleMask = 0;
    context->OMGetBlendState(&blend, factor, &sampleMask);
    if (!blend) return false;
    D3D11_BLEND_DESC desc = {};
    blend->GetDesc(&desc);
    release(blend);
    const D3D11_RENDER_TARGET_BLEND_DESC& target = desc.RenderTarget[0];
    return target.BlendEnable
        && target.SrcBlend == D3D11_BLEND_INV_SRC_ALPHA
        && target.DestBlend == D3D11_BLEND_SRC_ALPHA
        && target.BlendOp == D3D11_BLEND_OP_ADD
        && target.RenderTargetWriteMask == (D3D11_COLOR_WRITE_ENABLE_RED
                                           | D3D11_COLOR_WRITE_ENABLE_GREEN
                                           | D3D11_COLOR_WRITE_ENABLE_BLUE);
}

void bindRegionalCompositeShader(ID3D11DeviceContext* context, bool clamp) {
    if (clamp) g_psSetShader(context, g_hdr.alphaClampPS, nullptr, 0);
}

void restoreRegionalCompositeShader(ID3D11DeviceContext* context, bool clamp) {
    if (clamp) g_psSetShader(context, g_gamePixelShader, nullptr, 0);
}

bool restoreGameSpaceBackBuffer() {
    if (!g_backBufferNeedsRestore || !g_context || !g_swapChain
        || !g_hdr.presentCopy) return false;
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                      (void**)&backBuffer)) || !backBuffer)
        return false;
    D3D11_TEXTURE2D_DESC source = {}, destination = {};
    g_hdr.presentCopy->GetDesc(&source);
    backBuffer->GetDesc(&destination);
    bool compatible = source.Width == destination.Width
                   && source.Height == destination.Height
                   && source.Format == destination.Format
                   && source.SampleDesc.Count == destination.SampleDesc.Count
                   && source.SampleDesc.Quality == destination.SampleDesc.Quality;
    if (compatible) {
        ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* dsv = nullptr;
        g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                      rtvs, &dsv);
        ID3D11RenderTargetView* noTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        g_omSetRenderTargets(g_context, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                             noTargets, nullptr);
        g_context->CopyResource(backBuffer, g_hdr.presentCopy);
        g_omSetRenderTargets(g_context, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                             rtvs, dsv);
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            release(rtvs[i]);
        release(dsv);
        g_backBufferNeedsRestore = false;
    }
    backBuffer->Release();
    return compatible;
}

void restoreBeforeBackBufferDraw(ID3D11DeviceContext* context) {
    if (!g_backBufferNeedsRestore || context != g_context) return;
    ID3D11RenderTargetView* target = nullptr;
    context->OMGetRenderTargets(1, &target, nullptr);
    bool backBuffer = viewTargetsCurrentBackBuffer(target);
    release(target);
    if (backBuffer) restoreGameSpaceBackBuffer();
}

void WINAPI hookClearRenderTargetView(ID3D11DeviceContext* context,
                                      ID3D11RenderTargetView* view,
                                      const FLOAT color[4]) {
    // A full clear replaces every pixel, so this frame does not depend on the
    // prior flip-buffer contents and needs no preservation copy.
    bool backBuffer = !g_inside && context == g_context
                   && viewTargetsCurrentBackBuffer(view);
    if (backBuffer && g_backBufferNeedsRestore)
        g_backBufferNeedsRestore = false;
    g_clearRenderTargetView(context, view, color);
}


// Grass vertex streams are dynamic and refilled as terrain blocks stream in,
// so the only place their contents exist on the CPU is between the game's own
// Map and Unmap. Creation is watched to know which buffers those are.
HRESULT WINAPI hookCreateBuffer(ID3D11Device* device, const D3D11_BUFFER_DESC* desc,
                                const D3D11_SUBRESOURCE_DATA* initial,
                                ID3D11Buffer** buffer) {
    const int64_t started = tq::probe::enabled() ? tq::probe::now() : 0;
    tq::probe::count(tq::probe::CounterBufferCreate);
    HRESULT result = g_createBuffer(device, desc, initial, buffer);
    const uint32_t elapsed = started && tq::probe::isRenderThread()
        ? tq::probe::finishPhase(tq::probe::PhaseBufferCreate, started)
        : started ? tq::probe::microsecondsSince(started) : 0;
    if (tq::probe::enabled() && SUCCEEDED(result) && buffer && *buffer && desc)
        tq::engineprobe::noteDeferredBufferCreated(
            *buffer, elapsed, desc->ByteWidth, desc->BindFlags,
            (unsigned)desc->Usage, desc->CPUAccessFlags, desc->MiscFlags);
    if (SUCCEEDED(result) && buffer && *buffer)
        tq::grass::noteBufferCreated(*buffer, desc);
    return result;
}

HRESULT WINAPI hookMap(ID3D11DeviceContext* context, ID3D11Resource* resource,
                       UINT subresource, D3D11_MAP type, UINT flags,
                       D3D11_MAPPED_SUBRESOURCE* mapped) {
    tq::probe::count(tq::probe::CounterMap);
    // The driver call only. grass::noteMap below is ours and is already
    // accounted elsewhere; including it here would charge the game for it.
    const int64_t mapStart =
        tq::probe::drawTimingEnabled() ? tq::probe::now() : 0;
    HRESULT result = g_map(context, resource, subresource, type, flags, mapped);
    if (mapStart) tq::probe::addPhase(tq::probe::PhaseMapResource, mapStart);
    // Grass has one scratch buffer and upload queue. Other contexts must not
    // publish mapped pointers into that immediate-context state.
    if (context == g_context && SUCCEEDED(result) && mapped)
        tq::grass::noteMap(resource, subresource, mapped);
    return result;
}

void WINAPI hookUnmap(ID3D11DeviceContext* context, ID3D11Resource* resource,
                      UINT subresource) {
    tq::probe::count(tq::probe::CounterUnmap);
    // Counted, never timed: this runs ~2,400 times a frame and a clock pair on
    // each would cost more than the lookups it measured. The rewrite itself is
    // timed inside grass.cpp, on the rare path that actually does the work.
    // Before the commit, so the driver receives the edited vertices rather
    // than a second write to memory it has already taken.
    if (context == g_context) tq::grass::noteUnmap(resource, subresource);
    g_unmap(context, resource, subresource);
    // And after it, so the rotated copy is uploaded by an ordinary device call
    // of ours instead of from inside the driver's own unmap.
    if (context == g_context) tq::grass::afterUnmap(context);
}

// The crossing card. Same index range, same everything, over a stream holding
// the block's cards turned a quarter turn about their own centres -- so every
// blade keeps its place and gains a second card through it. Priced with the
// probe by drawing it on alternating frames only: +0.04 ms of GPU a frame
// against the blades' own 2.7 ms, so it carries no switch of its own.
void drawGrassCross(ID3D11DeviceContext* context, UINT count, UINT start, INT base) {
    ID3D11Buffer* bound = nullptr;
    UINT stride = 0;
    UINT offset = 0;
    context->IAGetVertexBuffers(0, 1, &bound, &stride, &offset);
    ID3D11Buffer* crossed = tq::grass::crossedBuffer(bound);
    // No twin yet: this stream was adopted after the game filled it, so the
    // contents have to be read back rather than waited for.
    if (!crossed) tq::grass::seedFromDraw(context, bound);
    if (crossed) {
        tq::probe::count(tq::probe::CounterGrassCross);
        context->IASetVertexBuffers(0, 1, &crossed, &stride, &offset);
        // The original entry point: the hook has already run for this draw.
        g_drawIndexed(context, count, start, base);
        context->IASetVertexBuffers(0, 1, &bound, &stride, &offset);
    }
    release(bound);
}

void WINAPI hookDraw(ID3D11DeviceContext* context, UINT count, UINT start) {
    if (!g_inside && tq::secondaryadmission::secondaryAdmissionDrawSuppressed()) {
        tq::secondaryadmission::noteSecondaryAdmissionDrawSkipped();
        return;
    }
    if (!g_inside) tq::probe::count(tq::probe::CounterDraw);
    if (!g_inside) tracePostProcessBinding(context);
    if (!g_inside) restoreBeforeBackBufferDraw(context);
    if (!g_inside && g_options.smaa && g_fxaaBound) {
        g_inside = true; bool done = runSmaa(context, false, count, start, 0); g_inside = false;
        if (done) return;
    }
    bool bloomAfterDraw = !g_inside && context == g_context && g_gammaBound
                       && g_globalBloomEnabled && !g_bloom.freshForPresent;
    bool clampRegionalAlpha = !g_inside
        && shouldClampRegionalCompositeAlpha(context);
    bindRegionalCompositeShader(context, clampRegionalAlpha);
    const bool gpuChunkDraw = !g_inside
        && tq::engineprobe::gpuChunkDrawActive();
    if (gpuChunkDraw) tq::engineprobe::beginGpuChunkDraw(context);
    {
        const int64_t started =
            tq::probe::drawTimingEnabled() ? tq::probe::now() : 0;
        g_draw(context, count, start);
        if (started) {
            const uint32_t elapsed = tq::probe::finishPhase(
                tq::probe::PhaseDrawSubmit, started);
            tq::engineprobe::countDeferredDraw(
                elapsed, false, count, start, 0, &g_deferredBindings);
        }
    }
    if (gpuChunkDraw)
        tq::engineprobe::finishGpuChunkDraw(
            false, count, &g_deferredBindings);
    restoreRegionalCompositeShader(context, clampRegionalAlpha);
    if (bloomAfterDraw) renderEnhancedBloom();
}

void WINAPI hookDrawIndexed(ID3D11DeviceContext* context, UINT count, UINT start, INT base) {
    if (!g_inside && tq::secondaryadmission::secondaryAdmissionDrawSuppressed()) {
        tq::secondaryadmission::noteSecondaryAdmissionDrawSkipped();
        return;
    }
    if (!g_inside) tq::probe::count(tq::probe::CounterDrawIndexed);
    if (!g_inside) tracePostProcessBinding(context);
    if (!g_inside) restoreBeforeBackBufferDraw(context);
    if (!g_inside && g_options.smaa && g_fxaaBound) {
        g_inside = true; bool done = runSmaa(context, true, count, start, base); g_inside = false;
        if (done) return;
    }
    bool bloomAfterDraw = !g_inside && context == g_context && g_gammaBound
                       && g_globalBloomEnabled && !g_bloom.freshForPresent;
    bool clampRegionalAlpha = !g_inside
        && shouldClampRegionalCompositeAlpha(context);
    bindRegionalCompositeShader(context, clampRegionalAlpha);
    const bool grassDraw = !g_inside && context == g_context
        && g_grassEnhanced && tq::grass::rendering();
    // Opened before the game's own grass draw and closed after ours, so the
    // region covers the blades and the crossing together.
    if (grassDraw) {
        tq::probe::count(tq::probe::CounterGrassDraw);
        tq::probe::gpuBegin(context, tq::probe::GpuGrass);
    }
    const bool gpuChunkDraw = !g_inside
        && tq::engineprobe::gpuChunkDrawActive();
    if (gpuChunkDraw) tq::engineprobe::beginGpuChunkDraw(context);
    {
        // Brackets the game's own call and nothing else. Our grass cross draw
        // below has its own phase, and SMAA and bloom take theirs; a bracket
        // around the whole hook body would swallow all three and make the
        // game's submission look like whatever the mod did around it.
        const int64_t started =
            tq::probe::drawTimingEnabled() ? tq::probe::now() : 0;
        g_drawIndexed(context, count, start, base);
        if (started) {
            const uint32_t elapsed = tq::probe::finishPhase(
                tq::probe::PhaseDrawSubmit, started);
            tq::engineprobe::countDeferredDraw(
                elapsed, true, count, start, base, &g_deferredBindings);
        }
    }
    if (grassDraw) {
        {
            tq::probe::Scope timing(tq::probe::PhaseGrassCross);
            drawGrassCross(context, count, start, base);
        }
        tq::probe::gpuEnd(context, tq::probe::GpuGrass);
    }
    if (gpuChunkDraw)
        tq::engineprobe::finishGpuChunkDraw(
            true, count, &g_deferredBindings);
    restoreRegionalCompositeShader(context, clampRegionalAlpha);
    if (bloomAfterDraw) renderEnhancedBloom();
}

}  // namespace

void install(ID3D11Device* device, ID3D11DeviceContext* context,
             IDXGISwapChain* swapChain) {
    if (!device) {
        tq::hdr::log("Visual install skipped: no device\r\n");
        return;
    }
    if (InterlockedCompareExchange(&g_installed, 1, 0)) {
        tq::hdr::log("Visual install skipped: already installed\r\n");
        return;
    }
    readOptions();
    g_deferredBindingTracing =
        tq::engineprobe::deferredDrawTraceRequested();
    const bool secondaryAdmissionDrawHooks =
        tq::secondaryadmission::secondaryPassAdmissionRequested();
    memset(&g_deferredBindings, 0, sizeof(g_deferredBindings));
    g_bloomToggleKeyDown = false;
    g_bloomEnhancedRuntime = true;
    const char* bloomName = g_options.bloom == BloomOriginal ? "original"
                          : g_options.bloom == BloomOff ? "off" : "enhanced";
    tq::hdr::log("Visual options: smaa=%u shadows=%u streaming=%u bloom=%s "
                 "bloomStrength=%.3f bloomToggle=%u "
                 "anisotropy=%u\r\n",
                 g_options.smaa ? 1u : 0u, g_options.shadows ? 1u : 0u,
                 g_options.streaming ? 1u : 0u, bloomName,
                 g_options.bloomStrength,
                 g_options.bloomToggle ? 1u : 0u,
                 g_options.anisotropy);
    tq::frameoverlay::setStreamingMode(g_options.streaming);
    if (tq::frameoverlay::enabled())
        tq::hdr::log("Frame overlay enabled\r\n");
    if (!context) device->GetImmediateContext(&context); else context->AddRef();
    if (!context) {
        tq::hdr::log("Visual install failed: no immediate context\r\n");
        InterlockedExchange(&g_installed, 0);
        return;
    }
    const tq::hdr::Runtime& hdrRuntime = tq::hdr::runtime();
    bool toneEnabled = hdrRuntime.settings.toneMap != tq::hdr::ToneOriginal;
    bool enhancedBloomCapable = g_options.bloom == BloomEnhanced
                             && toneEnabled && hdrRuntime.fp16Active;
    bool nativeBloomControl = g_options.bloom == BloomOff
                           || enhancedBloomCapable;
    // Prepares the buffer table before the hooks that feed it are patched in.
    tq::grass::installBuffers();
    bool grassBufferHooks = tq::grass::enabled();
    g_grassEnhanced = tq::grass::enabled();
    g_globalBloomEnabled = enhancedBloomCapable;
    tq::hdr::log("Visual install: tone=%u hdrRequested=%u fp16=%u hdr=%u\r\n",
                 (unsigned)hdrRuntime.settings.toneMap,
                 hdrRuntime.settings.requestHdr ? 1u : 0u,
                 hdrRuntime.fp16Active ? 1u : 0u,
                 hdrRuntime.active ? 1u : 0u);
    tq::streaming::setPresentCallback(&onPresent);
    tq::streaming::setPostPresentCallback(&onPostPresent);
    // The shadow path needs the pre-resize callback too, not just FP16: the
    // renderer rebuilds its shadow targets across a resize and the identity
    // table has to be dropped with them.
    if (hdrRuntime.fp16Active || g_options.shadows)
        tq::streaming::setPreResizeCallback(&onBeforeResize);
    if (hdrRuntime.fp16Active)
        tq::streaming::setResizeCallback(&onResize);
    g_device = device;
    g_context = context;
    if (swapChain) {
        g_swapChain = swapChain;
        g_swapChain->AddRef();
        DXGI_SWAP_CHAIN_DESC swapDesc = {};
        if (SUCCEEDED(swapChain->GetDesc(&swapDesc))) {
            g_backBufferWidth = swapDesc.BufferDesc.Width;
            g_backBufferHeight = swapDesc.BufferDesc.Height;
            tq::hdr::log("Created swap chain: %ux%u format=%u buffers=%u effect=%u fp16=%u hdr=%u\r\n",
                g_backBufferWidth, g_backBufferHeight,
                (unsigned)swapDesc.BufferDesc.Format, swapDesc.BufferCount,
                (unsigned)swapDesc.SwapEffect, hdrRuntime.fp16Active ? 1u : 0u,
                hdrRuntime.active ? 1u : 0u);
        }
        ID3D11Texture2D* backBuffer = nullptr;
        if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                           (void**)&backBuffer)) && backBuffer) {
            D3D11_TEXTURE2D_DESC d = {}; backBuffer->GetDesc(&d);
            g_backBufferIdentity = identityOf(backBuffer);
            if (!g_backBufferWidth) g_backBufferWidth = d.Width;
            if (!g_backBufferHeight) g_backBufferHeight = d.Height;
            backBuffer->Release();
        }
    }
    void** dv = *(void***)device;
    void** cv = *(void***)context;
    bool ok = true;
    if (g_options.anisotropy > 1)
        ok &= patchSlot(&dv[23], (void*)&hookCreateSamplerState, (void**)&g_createSamplerState);
    else g_createSamplerState = (CreateSamplerStateFn)dv[23];
    if (grassBufferHooks || g_deferredBindingTracing
        || tq::engineprobe::reflectionAdmissionBufferTrackingRequested()) {
        ok &= patchSlot(&dv[3], (void*)&hookCreateBuffer, (void**)&g_createBuffer);
    } else {
        g_createBuffer = (CreateBufferFn)dv[3];
    }
    if (grassBufferHooks) {
        ok &= patchSlot(&cv[14], (void*)&hookMap, (void**)&g_map);
        ok &= patchSlot(&cv[15], (void*)&hookUnmap, (void**)&g_unmap);
    }
    if (g_options.shadows || g_options.streaming || toneEnabled
        || g_deferredBindingTracing)
        ok &= patchSlot(&dv[5], (void*)&hookCreateTexture2D, (void**)&g_createTexture2D);
    else g_createTexture2D = (CreateTexture2DFn)dv[5];
    if (g_options.streaming || hdrRuntime.fp16Active)
        ok &= patchSlot(&dv[7], (void*)&hookCreateShaderResourceView,
                        (void**)&g_createShaderResourceView);
    else g_createShaderResourceView = (CreateShaderResourceViewFn)dv[7];
    if (g_options.smaa || toneEnabled)
        ok &= patchSlot(&dv[15], (void*)&hookCreatePixelShader, (void**)&g_createPixelShader);
    else g_createPixelShader = (CreatePixelShaderFn)dv[15];
    if (hdrRuntime.fp16Active)
        ok &= patchSlot(&dv[9], (void*)&hookCreateRenderTargetView,
                        (void**)&g_createRenderTargetView);
    else g_createRenderTargetView = (CreateRenderTargetViewFn)dv[9];
    if (g_options.smaa || toneEnabled || g_deferredBindingTracing)
        ok &= patchSlot(&cv[9], (void*)&hookPSSetShader, (void**)&g_psSetShader);
    else g_psSetShader = (PSSetShaderFn)cv[9];
    if (g_deferredBindingTracing) {
        ok &= patchSlot(&cv[11], (void*)&hookVSSetShader,
                        (void**)&g_vsSetShader);
        ok &= patchSlot(&cv[18], (void*)&hookIASetVertexBuffers,
                        (void**)&g_iaSetVertexBuffers);
        ok &= patchSlot(&cv[19], (void*)&hookIASetIndexBuffer,
                        (void**)&g_iaSetIndexBuffer);
    } else {
        g_vsSetShader = (VSSetShaderFn)cv[11];
        g_iaSetVertexBuffers = (IASetVertexBuffersFn)cv[18];
        g_iaSetIndexBuffer = (IASetIndexBufferFn)cv[19];
    }
    bool secondaryAdmissionDrawHooksReady = false;
    // Enhanced grass needs the indexed-draw companion even when every other
    // visual feature, secondary admission and performance tracing are off.
    if (g_options.smaa || toneEnabled || nativeBloomControl
        || g_deferredBindingTracing || secondaryAdmissionDrawHooks
        || grassBufferHooks) {
        const bool indexedOk = patchSlot(
            &cv[12], (void*)&hookDrawIndexed, (void**)&g_drawIndexed);
        const bool drawOk = patchSlot(
            &cv[13], (void*)&hookDraw, (void**)&g_draw);
        secondaryAdmissionDrawHooksReady = indexedOk && drawOk;
        ok &= secondaryAdmissionDrawHooksReady;
    } else {
        g_drawIndexed = (DrawIndexedFn)cv[12];
        g_draw = (DrawFn)cv[13];
    }
    tq::secondaryadmission::setSecondaryAdmissionDrawHooksReady(
        secondaryAdmissionDrawHooks && secondaryAdmissionDrawHooksReady);
    if (g_options.streaming || g_deferredBindingTracing)
        ok &= patchSlot(&cv[8], (void*)&hookPSSetShaderResources,
                        (void**)&g_psSetShaderResources);
    else g_psSetShaderResources = (PSSetShaderResourcesFn)cv[8];
    if (g_options.shadows)
        ok &= patchSlot(&cv[33], (void*)&hookOMSetRenderTargets, (void**)&g_omSetRenderTargets);
    else g_omSetRenderTargets = (OMSetRenderTargetsFn)cv[33];
    if (hdrRuntime.fp16Active)
        ok &= patchSlot(&cv[50], (void*)&hookClearRenderTargetView,
                        (void**)&g_clearRenderTargetView);
    else g_clearRenderTargetView = (ClearRenderTargetViewFn)cv[50];
    if (g_options.shadows) {
        ok &= patchSlot(&cv[44], (void*)&hookRSSetViewports, (void**)&g_rsSetViewports);
        ok &= patchSlot(&cv[45], (void*)&hookRSSetScissors, (void**)&g_rsSetScissors);
    }
    g_updateSubresource = g_options.streaming ? (UpdateSubresourceFn)cv[48] : nullptr;
    // After patching, not before it: the module's dependencies are the
    // originals the patches hand back. The window this opens is a few slot
    // writes wide and fails open -- upload::ready() is false, so a texture
    // created inside it takes the engine's own synchronous path, which is
    // exactly what happens when streaming is off.
    if (g_options.streaming && !tq::upload::install(uploadCalls())) {
        tq::hdr::log("Progressive upload disabled: missing device entry point\r\n");
    }
    if (g_options.streaming) startUnmapWorker();
    if (g_options.looseTextureMax) installFileSourceGate();
    // Last of the Engine.dll work, and the only part that writes into .text.
    // Its trace groups require the probe, while accepted performance fixes
    // enter independently and install only their audited sites.
    tq::engine::install(GetModuleHandleW(L"Engine.dll"));
    tq::hdr::log("Visual slot patching returned: ok=%u patches=%d\r\n",
                 ok ? 1u : 0u, g_patchCount);
    if (ok && nativeBloomControl) {
        HMODULE engine = GetModuleHandleW(L"Engine.dll");
        tq::bloomhook::HotBlurFn original = engine
            ? (tq::bloomhook::HotBlurFn)(void*)GetProcAddress(
                engine, "?HotBlurFrameBuffer@GraphicsCanvas@GAME@@QAEXIIMMM@Z")
            : nullptr;
        bool bloomHook = tq::bloomhook::install(engine, original);
        bool suppress = false;
        if (bloomHook) {
            // Enhanced mode proves that its GPU path can render before it
            // suppresses native bloom.  bloom=off is the only immediate no-op.
            suppress = g_options.bloom == BloomOff;
            tq::bloomhook::setSuppression(suppress);
        } else {
            g_globalBloomEnabled = false;
        }
        tq::hdr::log("Native bloom control: ready=%u engine=%p original=%p "
                     "suppressed=%u global=%u\r\n",
                     bloomHook ? 1u : 0u, engine, (void*)original,
                     suppress ? 1u : 0u, g_globalBloomEnabled ? 1u : 0u);
    }
    if (ok && (g_options.smaa || toneEnabled || tq::frameoverlay::enabled())) {
        startProgramBuild(device);
        tq::hdr::log("Shader program build requested\r\n");
    }
    if (!ok) {
        g_deferredBindingTracing = false;
        memset(&g_deferredBindings, 0, sizeof(g_deferredBindings));
        tq::bloomhook::shutdown();
        g_globalBloomEnabled = false;
        restoreSlots();
        g_context->Release();
        g_context = nullptr;
        stopUnmapWorker();
        tq::upload::shutdown();
        tq::streaming::setPresentCallback(nullptr);
        tq::streaming::setPostPresentCallback(nullptr);
        tq::streaming::setPreResizeCallback(nullptr);
        tq::streaming::setResizeCallback(nullptr);
        InterlockedExchange(&g_installed, 0);
        tq::hdr::log("Visual install rolled back after patch failure\r\n");
    } else {
        tq::hdr::log("Visual install completed\r\n");
    }
}

void onPresent(IDXGISwapChain* swapChain) {
    // First, so the interval it closes is exactly the span the phases below
    // were accumulated over: our own post-work here, the game's Present, and
    // the game's next rendered frame.
    tq::frameoverlay::recordFrame();
    tq::secondaryadmission::secondaryAdmissionFrameBoundary();
    tq::probe::beginFrame(g_context);
    // The probe and the overlay both need device objects, and the worker that
    // builds them is otherwise only started by a matched shader -- which never
    // arrives when every visual enhancement is switched off.
    if ((tq::probe::enabled() || tq::frameoverlay::enabled()) && g_device)
        startProgramBuild(g_device);
    tq::probe::Scope presentTiming(tq::probe::PhasePresent);
    { tq::probe::Scope grassTiming(tq::probe::PhaseGrassPresent);
      tq::grass::onPresent(g_context); }
    if (!InterlockedCompareExchange(&g_firstPresentLogged, 1, 0))
        tq::hdr::log("First Present reached: swapChain=%p fp16=%u hdr=%u\r\n",
                     swapChain, tq::hdr::runtime().fp16Active ? 1u : 0u,
                     tq::hdr::runtime().active ? 1u : 0u);
    { tq::probe::Scope streamTiming(tq::probe::PhaseStreamStep);
      advanceTextureUploadsInternal(); }
    if (tq::hdr::runtime().fp16Active) {
        // If the game submits another Present without touching the new flip
        // buffer, restore before capturing it as the next game-space frame.
        if (g_backBufferNeedsRestore) restoreGameSpaceBackBuffer();
        // After that restore, which would otherwise copy the overlay away.
        drawFrameOverlay(swapChain);
        presentHdrFrame(swapChain);
    } else {
        drawFrameOverlay(swapChain);
    }
}

void onPostPresent(IDXGISwapChain* swapChain) {
    (void)swapChain;
    // Defer the copy until the next frame actually reuses old pixels. A normal
    // full clear cancels it in hookClearRenderTargetView at zero copy cost.
    if (tq::hdr::runtime().fp16Active && g_hdr.presentCopy) {
        g_backBufferNeedsRestore = true;
        restoreFlipOutputTargets();
    }
}

void onBeforeResize(IDXGISwapChain* swapChain) {
    (void)swapChain;
    // DXGI requires every reference to a swap-chain buffer to be released
    // before ResizeBuffers. Also restart ordinal target matching because the
    // game recreates its resolution-dependent post-process chain afterward.
    releaseHdrSizeResources();
    releaseFlipOutputTargets();
    g_backBufferIdentity = nullptr;
    g_backBufferWidth = g_backBufferHeight = 0;
    g_backBufferNeedsRestore = false;
    memset(g_screenTargets, 0, sizeof(g_screenTargets));
    g_screenTargetCount = 0;
    memset(g_postProcessBindings, 0, sizeof(g_postProcessBindings));
    g_postProcessBindingCount = 0;
    // The renderer rebuilds its shadow targets here too, and these are raw
    // identity pointers with no release hook: a freed allocation's address can
    // be handed back for something else entirely, and a depth-only pass that
    // matched it would have its viewport multiplied.
    memset(g_shadowTextures, 0, sizeof(g_shadowTextures));
    g_shadowTextureCount = 0;
    g_shadowBound = false;
    g_shadowScale = 1;
    InterlockedExchange(&g_shadowTableFullLogged, 0);
    tq::shadow::resetShadowMapSizes();
}

void onResize(IDXGISwapChain* swapChain) {
    tq::hdr::reapplyColorSpace(swapChain);
    ID3D11Texture2D* backBuffer = nullptr;
    if (swapChain && SUCCEEDED(swapChain->GetBuffer(
        0, __uuidof(ID3D11Texture2D), (void**)&backBuffer)) && backBuffer) {
        D3D11_TEXTURE2D_DESC d = {}; backBuffer->GetDesc(&d);
        g_backBufferIdentity = identityOf(backBuffer);
        g_backBufferWidth = d.Width; g_backBufferHeight = d.Height;
        backBuffer->Release();
    }
}

void shutdown() {
    // First: these are writes into Engine.dll's .text, and every one of them
    // has to be back before the probe they report into goes away.
    tq::engine::shutdown();
    g_deferredBindingTracing = false;
    memset(&g_deferredBindings, 0, sizeof(g_deferredBindings));
    tq::streaming::setPresentCallback(nullptr);
    tq::streaming::setPostPresentCallback(nullptr);
    tq::streaming::setPreResizeCallback(nullptr);
    tq::streaming::setResizeCallback(nullptr);
    tq::bloomhook::shutdown();
    g_globalBloomEnabled = false;
    g_bloomToggleKeyDown = false;
    g_bloomEnhancedRuntime = true;
    restoreSlots();
    // Jobs first, so nothing is still holding a lease when the leases go, and
    // the worker before that, so no view is unmapped from under it.
    tq::upload::shutdown();
    stopUnmapWorker();
    if (g_looseRedirects)
        tq::hdr::log("Loose texture cap: %ld redirected to the archive, "
                     "largest %ld px\r\n", g_looseRedirects, g_looseBiggest);
    for (unsigned i = 0; i < kMaxMappingLeases; ++i) {
        if (g_mappingLeases[i].used && g_mappingLeases[i].mappedBase)
            UnmapViewOfFile(g_mappingLeases[i].mappedBase);
        memset(&g_mappingLeases[i], 0, sizeof(g_mappingLeases[i]));
    }
    InterlockedExchange(&g_leasedBytes, 0);
    bool workerStopped = true;
    if (g_programThread) {
        workerStopped = WaitForSingleObject(g_programThread, 2000) == WAIT_OBJECT_0;
        CloseHandle(g_programThread);
        g_programThread = nullptr;
    }
    // On an abnormal explicit unload, leaking process-local graphics objects is
    // safer than freeing them beneath a worker that DXMT has not returned.
    if (workerStopped) {
        releaseSmaa();
        releaseHdr();
        tq::frameoverlay::releaseResources();
        if (g_compiler) { FreeLibrary(g_compiler); g_compiler = nullptr; }
    }
    memset(g_fxaa, 0, sizeof(g_fxaa)); g_fxaaCount = 0;
    memset(g_colorGrading, 0, sizeof(g_colorGrading)); g_colorGradingCount = 0;
    memset(g_gamma, 0, sizeof(g_gamma)); g_gammaCount = 0;
    memset(g_alphaClampCopies, 0, sizeof(g_alphaClampCopies));
    g_alphaClampCopyCount = 0;
    g_gamePixelShader = nullptr;
    memset(g_screenTargets, 0, sizeof(g_screenTargets)); g_screenTargetCount = 0;
    memset(g_postProcessBindings, 0, sizeof(g_postProcessBindings));
    g_postProcessBindingCount = 0;
    memset(g_shadowTextures, 0, sizeof(g_shadowTextures)); g_shadowTextureCount = 0;
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    g_device = nullptr; g_fxaaBound = g_shadowBound = false;
    g_colorGradingBound = g_gammaBound = false;
    g_backBufferNeedsRestore = false;
    releaseFlipOutputTargets();
    InterlockedExchange(&g_firstFlipOutputRestoreLogged, 0);
    g_backBufferIdentity = nullptr; g_backBufferWidth = g_backBufferHeight = 0;
    g_createTexture2D = nullptr;
    g_createBuffer = nullptr;
    g_createPixelShader = nullptr;
    g_createShaderResourceView = nullptr;
    g_createRenderTargetView = nullptr;
    g_clearRenderTargetView = nullptr;
    g_psSetShaderResources = nullptr;
    g_psSetShader = nullptr;
    g_vsSetShader = nullptr;
    g_iaSetVertexBuffers = nullptr;
    g_iaSetIndexBuffer = nullptr;
    g_updateSubresource = nullptr;
    g_archiveUnmap = nullptr;
    g_directoryOpenFile = nullptr;
    g_archiveOpenFile = nullptr;
    g_looseTraceLines = g_looseRedirects = g_looseBiggest = 0;
    if (workerStopped) tq::probe::releaseResources();
    tq::probe::shutdown();
    tq::frameoverlay::reset();
    InterlockedExchange(&g_archiveVtablePatched, 0);
    InterlockedExchange(&g_programState, 0);
    InterlockedExchange(&g_installed, 0);
}

}  // namespace visual
}  // namespace tq
