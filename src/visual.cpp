#include "visual.h"
#include "dxbc_patch.h"
#include "hdr.h"
#include "streaming.h"

#include <d3dcompiler.h>
#include <stdint.h>
#include <string>
#include <string.h>

#include "AreaTex.h"
#include "SearchTex.h"
#include "smaa_source.h"

namespace tq {
namespace visual {
namespace {

struct Options { bool smaa, shadows, streaming; UINT anisotropy; };
Options g_options = {true, true, true, 16};

struct Patch { void** slot; void* original; void* replacement; };
Patch g_patches[24];
int g_patchCount;
LONG g_installed;

typedef HRESULT(WINAPI* CreateTexture2DFn)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                           const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
typedef HRESULT(WINAPI* CreatePixelShaderFn)(ID3D11Device*, const void*, SIZE_T,
                                             ID3D11ClassLinkage*, ID3D11PixelShader**);
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
typedef void(WINAPI* PSSetShaderResourcesFn)(ID3D11DeviceContext*, UINT, UINT,
                                             ID3D11ShaderResourceView* const*);
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
CreateShaderResourceViewFn g_createShaderResourceView;
CreateRenderTargetViewFn g_createRenderTargetView;
PSSetShaderFn g_psSetShader;
PSSetShaderResourcesFn g_psSetShaderResources;
DrawFn g_draw;
DrawIndexedFn g_drawIndexed;
ClearRenderTargetViewFn g_clearRenderTargetView;
OMSetRenderTargetsFn g_omSetRenderTargets;
RSSetViewportsFn g_rsSetViewports;
RSSetScissorsFn g_rsSetScissors;
UpdateSubresourceFn g_updateSubresource;

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
bool g_fxaaBound;
bool g_colorGradingBound;
bool g_gammaBound;
bool g_backBufferNeedsRestore;

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

const UINT kUploadChunkBytes = 2 * 1024 * 1024;
const unsigned kMaxUploadJobs = 256;
const unsigned kMaxMappingLeases = 128;
const unsigned kMaxTextureMips = 16;

typedef void (__thiscall* ArchiveUnmapFn)(void*);
ArchiveUnmapFn g_archiveUnmap;
LONG g_archiveVtablePatched;
CRITICAL_SECTION g_uploadLock;
bool g_uploadLockReady;

struct MappingLease {
    bool used;
    bool sealed;
    void* source;
    void* mappedBase;
    LONG jobs;
};

struct UploadJob {
    LONG state;  // 0 free, 1 reserved, 2 uploading
    ID3D11Texture2D* texture;
    ID3D11ShaderResourceView* fullView;
    ID3D11ShaderResourceView* lowView;
    MappingLease* lease;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_SUBRESOURCE_DATA source[kMaxTextureMips];
    UINT lowMip;
    UINT mip;
    UINT blockRow;
    UINT chunkBytes;
    UINT maxChunkBytes;
};

MappingLease g_mappingLeases[kMaxMappingLeases];
UploadJob g_uploadJobs[kMaxUploadJobs];

bool upgradedIdentity(void* identity);

struct ShadowTexture {
    void* identity;
    UINT originalWidth, originalHeight;
};
ShadowTexture g_shadowTextures[16];
unsigned g_shadowTextureCount;
bool g_shadowBound;
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
    ID3D11VertexShader* fullscreenVS;
    ID3D11Texture2D* presentCopy;
    ID3D11ShaderResourceView* presentCopySRV;
    ID3D11RenderTargetView* backBufferRTV;
    ID3D11SamplerState* sampler;
    ID3D11BlendState* blend;
    ID3D11DepthStencilState* depth;
    ID3D11RasterizerState* raster;
    UINT width, height;
} g_hdr;

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
    GetPrivateProfileStringW(L"performance", L"streaming", L"optimized",
                             value, 32, path);
    g_options.streaming = tq::streaming::optimizationEnabled(value);
    int anisotropy = GetPrivateProfileIntW(L"graphics", L"anisotropy", 16, path);
    g_options.anisotropy = anisotropy == 1 ? 1
                         : anisotropy >= 2 && anisotropy <= 16 ? (UINT)anisotropy : 16;
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

bool isShadowIdentity(void* identity) {
    for (unsigned i = 0; i < g_shadowTextureCount; ++i)
        if (g_shadowTextures[i].identity == identity) return true;
    return false;
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
    if (g_uploadLockReady) {
        EnterCriticalSection(&g_uploadLock);
        MappingLease* lease = findLease(source);
        if (lease && *(void**)((BYTE*)source + 0x34) == nullptr) {
            lease->sealed = true;
            retained = true;
            if (!lease->jobs) {
                unmap = lease->mappedBase;
                memset(lease, 0, sizeof(*lease));
            }
        }
        LeaveCriticalSection(&g_uploadLock);
    }
    if (unmap) UnmapViewOfFile(unmap);
    if (!retained && g_archiveUnmap) g_archiveUnmap(source);
}

bool ensureArchiveUnmapHook(void** vtable) {
    if (!vtable || !g_uploadLockReady) return false;
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

UploadJob* reserveUploadJob() {
    for (unsigned i = 0; i < kMaxUploadJobs; ++i)
        if (InterlockedCompareExchange(&g_uploadJobs[i].state, 1, 0) == 0)
            return &g_uploadJobs[i];
    return nullptr;
}

UINT lowMipFor(const D3D11_TEXTURE2D_DESC* desc) {
    UINT width = desc->Width, height = desc->Height, mip = 0;
    while (mip + 1 < desc->MipLevels && (width > 512 || height > 512)) {
        if (width > 1) width >>= 1;
        if (height > 1) height >>= 1;
        ++mip;
    }
    return mip;
}

bool progressiveTextureCandidate(const D3D11_TEXTURE2D_DESC* desc,
                                 const D3D11_SUBRESOURCE_DATA* initial,
                                 const void* caller, uint64_t* topBytes) {
    if (!g_options.streaming || !desc || !initial || !initial[0].pSysMem
        || desc->Usage != D3D11_USAGE_DEFAULT || desc->ArraySize != 1
        || !desc->MipLevels || desc->MipLevels > kMaxTextureMips
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
        || !g_uploadLockReady) return E_FAIL;
    TextureOwner owner = {};
    const BYTE* dds = (const BYTE*)initial[0].pSysMem - 0x80;
    if (!findTextureOwner(dds, &owner)) return E_FAIL;
    UploadJob* job = reserveUploadJob();
    if (!job) return E_FAIL;

    UINT lowMip = lowMipFor(desc);
    D3D11_SUBRESOURCE_DATA staged[kMaxTextureMips] = {};
    for (UINT mip = lowMip; mip < desc->MipLevels; ++mip)
        staged[mip] = initial[mip];
    HRESULT hr = g_createTexture2D(device, desc, staged, texture);
    if (FAILED(hr) || !texture || !*texture) {
        InterlockedExchange(&job->state, 0);
        return E_FAIL;
    }

    EnterCriticalSection(&g_uploadLock);
    MappingLease* lease = findLease(owner.source);
    if (owner.mappedBase
        && (!lease || lease->sealed || lease->mappedBase != owner.mappedBase))
        lease = createLease(owner.source, owner.mappedBase);
    bool hooked = lease && ensureArchiveUnmapHook(owner.vtable);
    if (hooked && owner.mappedBase) {
        void** field = (void**)((BYTE*)owner.source + 0x34);
        void* prior = InterlockedCompareExchangePointer(field, nullptr, owner.mappedBase);
        hooked = prior == owner.mappedBase;
    }
    if (hooked && !owner.mappedBase)
        hooked = lease->mappedBase != nullptr;
    if (!hooked) {
        if (lease && !lease->jobs) memset(lease, 0, sizeof(*lease));
        LeaveCriticalSection(&g_uploadLock);
        (*texture)->Release();
        *texture = nullptr;
        InterlockedExchange(&job->state, 0);
        return E_FAIL;
    }

    memset((BYTE*)job + sizeof(job->state), 0, sizeof(*job) - sizeof(job->state));
    job->texture = *texture;
    job->texture->AddRef();
    job->lease = lease;
    job->desc = *desc;
    memcpy(job->source, initial, desc->MipLevels * sizeof(*initial));
    job->lowMip = lowMip;
    job->mip = 0;
    job->blockRow = 0;
    job->maxChunkBytes = topBytes <= 4ull * 1024ull * 1024ull
                       ? 1024 * 1024 : kUploadChunkBytes;
    job->chunkBytes = job->maxChunkBytes;
    ++lease->jobs;
    InterlockedExchange(&job->state, 2);
    LeaveCriticalSection(&g_uploadLock);

    *handled = true;
    return hr;
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
    if (FAILED(hr) || !view || !*view || !g_uploadLockReady) return hr;

    EnterCriticalSection(&g_uploadLock);
    for (unsigned i = 0; i < kMaxUploadJobs; ++i) {
        UploadJob& job = g_uploadJobs[i];
        if (job.state != 2 || job.texture != resource || job.fullView) continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC low = {};
        if (description) low = *description;
        else {
            low.Format = job.desc.Format;
            low.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            low.Texture2D.MostDetailedMip = 0;
            low.Texture2D.MipLevels = job.desc.MipLevels;
        }
        if (low.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
            low.Texture2D.MostDetailedMip = job.lowMip;
            low.Texture2D.MipLevels = job.desc.MipLevels - job.lowMip;
            ID3D11ShaderResourceView* lowView = nullptr;
            if (SUCCEEDED(g_createShaderResourceView(device, resource, &low, &lowView))) {
                job.fullView = *view;
                job.lowView = lowView;
            }
        }
        break;
    }
    LeaveCriticalSection(&g_uploadLock);
    return hr;
}

void WINAPI hookPSSetShaderResources(ID3D11DeviceContext* context, UINT start,
                                     UINT count,
                                     ID3D11ShaderResourceView* const* views) {
    if (!views || !count || count > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
        || !g_uploadLockReady) {
        g_psSetShaderResources(context, start, count, views);
        return;
    }
    ID3D11ShaderResourceView* substituted[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
    memcpy(substituted, views, count * sizeof(*views));
    EnterCriticalSection(&g_uploadLock);
    for (UINT slot = 0; slot < count; ++slot) {
        if (!substituted[slot]) continue;
        for (unsigned i = 0; i < kMaxUploadJobs; ++i) {
            UploadJob& job = g_uploadJobs[i];
            if (job.state == 2 && job.fullView == substituted[slot] && job.lowView) {
                substituted[slot] = job.lowView;
                break;
            }
        }
    }
    g_psSetShaderResources(context, start, count, substituted);
    LeaveCriticalSection(&g_uploadLock);
}

void advanceTextureUploadsInternal() {
    if (!g_uploadLockReady || !g_context || !g_updateSubresource) return;
    ID3D11Texture2D* releaseTexture = nullptr;
    ID3D11ShaderResourceView* releaseLowView = nullptr;
    void* unmap = nullptr;

    EnterCriticalSection(&g_uploadLock);
    UploadJob* selected = nullptr;
    for (unsigned i = 0; i < kMaxUploadJobs; ++i) {
        if (g_uploadJobs[i].state == 2) {
            selected = &g_uploadJobs[i];
            break;
        }
    }
    if (!selected) {
        LeaveCriticalSection(&g_uploadLock);
        return;
    }

    UploadJob& job = *selected;
    if (job.mip < job.lowMip) {
        UINT width = job.desc.Width >> job.mip;
        UINT height = job.desc.Height >> job.mip;
        if (!width) width = 1;
        if (!height) height = 1;
        UINT totalBlockRows = (height + 3u) / 4u;
        if (!totalBlockRows) totalBlockRows = 1;
        UINT pitch = job.source[job.mip].SysMemPitch;
        UINT rows = pitch ? job.chunkBytes / pitch : 0;
        if (!rows) rows = 1;
        if (rows > totalBlockRows - job.blockRow)
            rows = totalBlockRows - job.blockRow;
        D3D11_BOX box = {};
        box.left = 0;
        box.right = width;
        box.top = job.blockRow * 4u;
        box.bottom = (job.blockRow + rows) * 4u;
        if (box.bottom > height) box.bottom = height;
        box.front = 0;
        box.back = 1;
        const BYTE* source = (const BYTE*)job.source[job.mip].pSysMem
                           + (uint64_t)job.blockRow * pitch;
        DWORD stepStartedMs = GetTickCount();
        g_updateSubresource(g_context, job.texture, job.mip, &box,
                            source, pitch, 0);
        DWORD stepMs = GetTickCount() - stepStartedMs;
        if (stepMs >= 6 && job.chunkBytes > 512 * 1024)
            job.chunkBytes /= 2;
        else if (stepMs <= 2 && job.chunkBytes < job.maxChunkBytes) {
            job.chunkBytes += 256 * 1024;
            if (job.chunkBytes > job.maxChunkBytes)
                job.chunkBytes = job.maxChunkBytes;
        }
        job.blockRow += rows;
        if (job.blockRow >= totalBlockRows) {
            ++job.mip;
            job.blockRow = 0;
        }
    }

    if (job.mip >= job.lowMip) {
        releaseTexture = job.texture;
        releaseLowView = job.lowView;
        MappingLease* lease = job.lease;
        memset((BYTE*)&job + sizeof(job.state), 0,
               sizeof(job) - sizeof(job.state));
        InterlockedExchange(&job.state, 0);
        if (lease && lease->jobs > 0) --lease->jobs;
        if (lease && lease->sealed && !lease->jobs) {
            unmap = lease->mappedBase;
            memset(lease, 0, sizeof(*lease));
        }
    }
    LeaveCriticalSection(&g_uploadLock);

    if (releaseLowView) releaseLowView->Release();
    if (releaseTexture) releaseTexture->Release();
    if (unmap) UnmapViewOfFile(unmap);
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
    // Exact Engine-build/runtime match for the two confirmed gameplay color
    // surfaces: target 11 feeds FXAA/SMAA and target 12 feeds the gamma pass.
    // Both must remain FP16 or target 12's UNORM store clips extended scene
    // values immediately before HDR-safe gamma and presentation.
    const unsigned nextId = g_screenTargetCount + 1;
    return tq::hdr::runtime().fp16Active && (nextId == 11 || nextId == 12) && !initial
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
    if (SUCCEEDED(hr) && texture) recordScreenTarget(requested, *texture, caller);
    return hr;
}

HRESULT WINAPI hookCreateTexture2D(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc,
                                   const D3D11_SUBRESOURCE_DATA* initial,
                                   ID3D11Texture2D** texture) {
    const void* caller = __builtin_return_address(0);
    bool progressivelyHandled = false;
    HRESULT progressive = createProgressiveTexture(device, desc, initial, texture,
                                                    caller, &progressivelyHandled);
    if (progressivelyHandled) return progressive;
    if (!g_options.shadows || !shadowDepthDesc(desc) || initial)
        return createOriginalTexture(device, desc, initial, texture, caller);
    D3D11_TEXTURE2D_DESC scaled = *desc;
    if (scaled.Width <= 8192 / 2) { scaled.Width *= 2; scaled.Height *= 2; }
    HRESULT hr = g_createTexture2D(device, &scaled, initial, texture);
    if (FAILED(hr) || !texture || !*texture) {
        return createOriginalTexture(device, desc, initial, texture, caller);
    }
    if (g_shadowTextureCount < sizeof(g_shadowTextures) / sizeof(g_shadowTextures[0])) {
        ShadowTexture& s = g_shadowTextures[g_shadowTextureCount++];
        s.identity = identityOf(*texture);
        s.originalWidth = desc->Width;
        s.originalHeight = desc->Height;
    }
    return hr;
}

HRESULT WINAPI hookCreatePixelShader(ID3D11Device* device, const void* bytecode, SIZE_T size,
                                     ID3D11ClassLinkage* linkage, ID3D11PixelShader** shader) {
    tq::dxbc::PatchResult patch = {};
    bool enhanced = g_options.shadows && tq::dxbc::enhanceShadowPcf(bytecode, size, &patch);
    HRESULT hr = g_createPixelShader(device, enhanced ? patch.data : bytecode,
                                     enhanced ? patch.size : size, linkage, shader);
    if (enhanced && FAILED(hr)) hr = g_createPixelShader(device, bytecode, size, linkage, shader);
    tq::dxbc::release(&patch);
    if (SUCCEEDED(hr) && shader && *shader) {
        bool outputTransform = tq::hdr::runtime().settings.toneMap != tq::hdr::ToneOriginal;
        bool fxaa = g_options.smaa && isFxaa(bytecode, size);
        bool color = outputTransform && tq::hdr::isColorGradingShader(bytecode, size);
        bool gamma = outputTransform && tq::hdr::isGammaShader(bytecode, size);
        if (fxaa && g_fxaaCount < sizeof(g_fxaa) / sizeof(g_fxaa[0]))
            g_fxaa[g_fxaaCount++] = *shader;
        if (color && g_colorGradingCount < sizeof(g_colorGrading) / sizeof(g_colorGrading[0]))
            g_colorGrading[g_colorGradingCount++] = *shader;
        if (gamma && g_gammaCount < sizeof(g_gamma) / sizeof(g_gamma[0]))
            g_gamma[g_gammaCount++] = *shader;
        if (fxaa || color || gamma) {
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
    if (!translate)
        return g_createRenderTargetView(device, resource, description, view);
    D3D11_RENDER_TARGET_VIEW_DESC translated = *description;
    translated.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    HRESULT hr = g_createRenderTargetView(device, resource, &translated, view);
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
    g_psSetShader(context, replacement, classes, count);
}

bool dsvIsShadow(ID3D11DepthStencilView* dsv) {
    if (!dsv) return false;
    ID3D11Resource* resource = nullptr;
    dsv->GetResource(&resource);
    void* identity = identityOf(resource);
    release(resource);
    return isShadowIdentity(identity);
}

void applyViewports(ID3D11DeviceContext* context) {
    if (!g_gameViewportCount) return;
    D3D11_VIEWPORT v[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    memcpy(v, g_gameViewports, g_gameViewportCount * sizeof(*v));
    if (g_shadowBound) for (UINT i = 0; i < g_gameViewportCount; ++i) {
        v[i].TopLeftX *= 2.0f; v[i].TopLeftY *= 2.0f;
        v[i].Width *= 2.0f; v[i].Height *= 2.0f;
    }
    g_rsSetViewports(context, g_gameViewportCount, v);
}

void applyScissors(ID3D11DeviceContext* context) {
    if (!g_gameScissorCount) return;
    D3D11_RECT r[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    memcpy(r, g_gameScissors, g_gameScissorCount * sizeof(*r));
    if (g_shadowBound) for (UINT i = 0; i < g_gameScissorCount; ++i) {
        r[i].left *= 2; r[i].top *= 2; r[i].right *= 2; r[i].bottom *= 2;
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
    bool was = g_shadowBound;
    // TQ's water-reflection pass can allocate a square R32 depth/SRV texture
    // resembling the shadow map. Its DSV is paired with a color target; the
    // actual shadow-map pass is depth-only. Never scale reflection viewports.
    g_shadowBound = g_options.shadows && !hasColorTarget && dsvIsShadow(dsv);
    g_omSetRenderTargets(context, count, rtvs, dsv);
    if (was != g_shadowBound) { applyViewports(context); applyScissors(context); }
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
"Texture2D frameTex:register(t0);SamplerState frameSampler:register(s0);"
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
"float3 c=linearizeExtended(max(frameTex.SampleLevel(frameSampler,u,0).rgb,0));"
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

void releaseHdrSizeResources() {
    release(g_hdr.presentCopySRV); release(g_hdr.presentCopy);
    release(g_hdr.backBufferRTV);
    g_hdr.width = g_hdr.height = 0;
}

void releaseHdr() {
    releaseHdrSizeResources();
    release(g_hdr.colorGradingPS); release(g_hdr.gammaPS);
    release(g_hdr.tonePS); release(g_hdr.presentPS);
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
        && (!runtime.fp16Active || (g_hdr.presentPS && g_hdr.fullscreenVS))) return true;

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
        + kPresentPixelSource;
    ID3DBlob *color = nullptr, *gamma = nullptr, *toneBlob = nullptr;
    ID3DBlob *presentBlob = nullptr, *vertex = nullptr;
    bool ok = compileShader(kColorGradingSource, "ps_5_0", &color)
           && compileShader(kGammaSource, "ps_5_0", &gamma)
           && compileShader(tone, "ps_5_0", &toneBlob);
    if (ok && runtime.fp16Active)
        ok = compileShader(present, "ps_5_0", &presentBlob)
          && compileShader(kPresentVertexSource, "vs_5_0", &vertex);
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
    release(color); release(gamma); release(toneBlob); release(presentBlob); release(vertex);
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

bool createProgramResources(ID3D11Device* device) {
    bool ok = true;
    if (g_options.smaa) ok = createSmaaProgramResources(device);
    if (ok) ok = createHdrProgramResources(device);
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

void issueDraw(ID3D11DeviceContext* c, bool indexed, UINT count, UINT start, INT base) {
    if (indexed) g_drawIndexed(c, count, start, base);
    else g_draw(c, count, start);
}

bool runSmaa(ID3D11DeviceContext* c, bool indexed, UINT count, UINT start, INT base) {
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
    if (ok) { g_hdr.width = desc.Width; g_hdr.height = desc.Height; }
    else releaseHdrSizeResources();
    tq::hdr::log("FP16 presentation resources: %ux%u format=%u ready=%u\r\n",
                 desc.Width, desc.Height, (unsigned)desc.Format, ok ? 1u : 0u);
    backBuffer->Release();
    return ok;
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
    g_context->PSSetShaderResources(0, 1, &g_hdr.presentCopySRV);
    g_draw(g_context, 3, 0);
    g_inside = false;
    g_context->PSSetShaderResources(0, 3, noSrvs);
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
    if (!g_inside && g_backBufferNeedsRestore && context == g_context
        && viewTargetsCurrentBackBuffer(view))
        g_backBufferNeedsRestore = false;
    g_clearRenderTargetView(context, view, color);
}

void WINAPI hookDraw(ID3D11DeviceContext* context, UINT count, UINT start) {
    if (!g_inside) tracePostProcessBinding(context);
    if (!g_inside) restoreBeforeBackBufferDraw(context);
    if (!g_inside && g_options.smaa && g_fxaaBound) {
        g_inside = true; bool done = runSmaa(context, false, count, start, 0); g_inside = false;
        if (done) return;
    }
    g_draw(context, count, start);
}

void WINAPI hookDrawIndexed(ID3D11DeviceContext* context, UINT count, UINT start, INT base) {
    if (!g_inside) tracePostProcessBinding(context);
    if (!g_inside) restoreBeforeBackBufferDraw(context);
    if (!g_inside && g_options.smaa && g_fxaaBound) {
        g_inside = true; bool done = runSmaa(context, true, count, start, base); g_inside = false;
        if (done) return;
    }
    g_drawIndexed(context, count, start, base);
}

}  // namespace

void install(ID3D11Device* device, ID3D11DeviceContext* context,
             IDXGISwapChain* swapChain) {
    if (!device || InterlockedCompareExchange(&g_installed, 1, 0)) return;
    readOptions();
    if (!context) device->GetImmediateContext(&context); else context->AddRef();
    if (!context) { InterlockedExchange(&g_installed, 0); return; }
    const tq::hdr::Runtime& hdrRuntime = tq::hdr::runtime();
    bool toneEnabled = hdrRuntime.settings.toneMap != tq::hdr::ToneOriginal;
    tq::hdr::log("Visual install: tone=%u hdrRequested=%u fp16=%u hdr=%u\r\n",
                 (unsigned)hdrRuntime.settings.toneMap,
                 hdrRuntime.settings.requestHdr ? 1u : 0u,
                 hdrRuntime.fp16Active ? 1u : 0u,
                 hdrRuntime.active ? 1u : 0u);
    if (g_options.streaming) {
        InitializeCriticalSection(&g_uploadLock);
        g_uploadLockReady = true;
    }
    tq::streaming::setPresentCallback(&onPresent);
    tq::streaming::setPostPresentCallback(&onPostPresent);
    if (hdrRuntime.fp16Active) {
        tq::streaming::setPreResizeCallback(&onBeforeResize);
        tq::streaming::setResizeCallback(&onResize);
    }
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
    if (g_options.shadows || g_options.streaming || toneEnabled)
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
    if (g_options.smaa || toneEnabled) {
        ok &= patchSlot(&cv[9], (void*)&hookPSSetShader, (void**)&g_psSetShader);
        ok &= patchSlot(&cv[12], (void*)&hookDrawIndexed, (void**)&g_drawIndexed);
        ok &= patchSlot(&cv[13], (void*)&hookDraw, (void**)&g_draw);
    }
    if (g_options.streaming)
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
    if (ok && (g_options.smaa || toneEnabled)) startProgramBuild(device);
    if (!ok) {
        restoreSlots();
        g_context->Release();
        g_context = nullptr;
        if (g_uploadLockReady) {
            g_uploadLockReady = false;
            DeleteCriticalSection(&g_uploadLock);
        }
        tq::streaming::setPresentCallback(nullptr);
        tq::streaming::setPostPresentCallback(nullptr);
        tq::streaming::setPreResizeCallback(nullptr);
        tq::streaming::setResizeCallback(nullptr);
        InterlockedExchange(&g_installed, 0);
    }
}

void onPresent(IDXGISwapChain* swapChain) {
    advanceTextureUploadsInternal();
    if (tq::hdr::runtime().fp16Active) {
        // If the game submits another Present without touching the new flip
        // buffer, restore before capturing it as the next game-space frame.
        if (g_backBufferNeedsRestore) restoreGameSpaceBackBuffer();
        presentHdrFrame(swapChain);
    }
}

void onPostPresent(IDXGISwapChain* swapChain) {
    (void)swapChain;
    // Defer the copy until the next frame actually reuses old pixels. A normal
    // full clear cancels it in hookClearRenderTargetView at zero copy cost.
    if (tq::hdr::runtime().fp16Active && g_hdr.presentCopy)
        g_backBufferNeedsRestore = true;
}

void onBeforeResize(IDXGISwapChain* swapChain) {
    (void)swapChain;
    // DXGI requires every reference to a swap-chain buffer to be released
    // before ResizeBuffers. Also restart ordinal target matching because the
    // game recreates its resolution-dependent post-process chain afterward.
    releaseHdrSizeResources();
    g_backBufferIdentity = nullptr;
    g_backBufferWidth = g_backBufferHeight = 0;
    g_backBufferNeedsRestore = false;
    memset(g_screenTargets, 0, sizeof(g_screenTargets));
    g_screenTargetCount = 0;
    memset(g_postProcessBindings, 0, sizeof(g_postProcessBindings));
    g_postProcessBindingCount = 0;
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
    tq::streaming::setPresentCallback(nullptr);
    tq::streaming::setPostPresentCallback(nullptr);
    tq::streaming::setPreResizeCallback(nullptr);
    tq::streaming::setResizeCallback(nullptr);
    restoreSlots();
    if (g_uploadLockReady) {
        EnterCriticalSection(&g_uploadLock);
        for (unsigned i = 0; i < kMaxUploadJobs; ++i) {
            UploadJob& job = g_uploadJobs[i];
            if (job.lowView) job.lowView->Release();
            if (job.texture) job.texture->Release();
            memset(&job, 0, sizeof(job));
        }
        for (unsigned i = 0; i < kMaxMappingLeases; ++i) {
            if (g_mappingLeases[i].used && g_mappingLeases[i].mappedBase)
                UnmapViewOfFile(g_mappingLeases[i].mappedBase);
            memset(&g_mappingLeases[i], 0, sizeof(g_mappingLeases[i]));
        }
        LeaveCriticalSection(&g_uploadLock);
        g_uploadLockReady = false;
        DeleteCriticalSection(&g_uploadLock);
    }
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
        if (g_compiler) { FreeLibrary(g_compiler); g_compiler = nullptr; }
    }
    memset(g_fxaa, 0, sizeof(g_fxaa)); g_fxaaCount = 0;
    memset(g_colorGrading, 0, sizeof(g_colorGrading)); g_colorGradingCount = 0;
    memset(g_gamma, 0, sizeof(g_gamma)); g_gammaCount = 0;
    memset(g_screenTargets, 0, sizeof(g_screenTargets)); g_screenTargetCount = 0;
    memset(g_postProcessBindings, 0, sizeof(g_postProcessBindings));
    g_postProcessBindingCount = 0;
    memset(g_shadowTextures, 0, sizeof(g_shadowTextures)); g_shadowTextureCount = 0;
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    g_device = nullptr; g_fxaaBound = g_shadowBound = false;
    g_colorGradingBound = g_gammaBound = false;
    g_backBufferNeedsRestore = false;
    g_backBufferIdentity = nullptr; g_backBufferWidth = g_backBufferHeight = 0;
    g_createTexture2D = nullptr;
    g_createPixelShader = nullptr;
    g_createShaderResourceView = nullptr;
    g_createRenderTargetView = nullptr;
    g_clearRenderTargetView = nullptr;
    g_psSetShaderResources = nullptr;
    g_updateSubresource = nullptr;
    g_archiveUnmap = nullptr;
    InterlockedExchange(&g_archiveVtablePatched, 0);
    InterlockedExchange(&g_programState, 0);
    InterlockedExchange(&g_installed, 0);
}

}  // namespace visual
}  // namespace tq
