#include "visual.h"
#include "dxbc_patch.h"
#include "streaming.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <string.h>

#include "AreaTex.h"
#include "SearchTex.h"
#include "smaa_source.h"

namespace tq {
namespace visual {
namespace {

struct Options { bool smaa, shadows, streaming, frameOverlay; UINT anisotropy; };
Options g_options = {true, true, true, false, 16};

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
typedef void(WINAPI* PSSetShaderFn)(ID3D11DeviceContext*, ID3D11PixelShader*,
                                    ID3D11ClassInstance* const*, UINT);
typedef void(WINAPI* PSSetShaderResourcesFn)(ID3D11DeviceContext*, UINT, UINT,
                                             ID3D11ShaderResourceView* const*);
typedef void(WINAPI* DrawFn)(ID3D11DeviceContext*, UINT, UINT);
typedef void(WINAPI* DrawIndexedFn)(ID3D11DeviceContext*, UINT, UINT, INT);
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
PSSetShaderFn g_psSetShader;
PSSetShaderResourcesFn g_psSetShaderResources;
DrawFn g_draw;
DrawIndexedFn g_drawIndexed;
OMSetRenderTargetsFn g_omSetRenderTargets;
RSSetViewportsFn g_rsSetViewports;
RSSetScissorsFn g_rsSetScissors;
UpdateSubresourceFn g_updateSubresource;

ID3D11Device* g_device;
ID3D11DeviceContext* g_context;
ID3D11PixelShader* g_fxaa[8];
unsigned g_fxaaCount;
bool g_fxaaBound;
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

const UINT kTextWidth = 532;
const UINT kTextHeight = 66;
const UINT kGraphTextureWidth = 4104;
const UINT kGraphTextureHeight = 110;
const UINT kGraphPointCount = 4096;
const unsigned kFrameSampleCount = 16384;

struct OverlayResources {
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11Texture2D* textTexture;
    ID3D11ShaderResourceView* textSrv;
    ID3D11Texture2D* graphTexture;
    ID3D11ShaderResourceView* graphSrv;
    ID3D11SamplerState* sampler;
    ID3D11BlendState* blend;
    ID3D11DepthStencilState* depth;
    ID3D11RasterizerState* raster;
} g_overlay;

struct FrameSample {
    LONGLONG ticks;
    float milliseconds;
};

uint32_t g_textPixels[kTextWidth * kTextHeight];
uint32_t g_graphPixels[kGraphTextureWidth * kGraphTextureHeight];
FrameSample g_frameSamples[kFrameSampleCount];
float g_sortedFrameTimes[kFrameSampleCount];
unsigned g_frameWrite;
unsigned g_frameCount;
LONGLONG g_lastOverlayRefresh;
LARGE_INTEGER g_frameFrequency;
LARGE_INTEGER g_lastFrame;

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
    g_options.frameOverlay =
        GetPrivateProfileIntW(L"performance", L"frame_overlay", 0, path) == 1;
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
    HRESULT hr = g_createShaderResourceView(device, resource, description, view);
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

HRESULT WINAPI hookCreateTexture2D(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc,
                                   const D3D11_SUBRESOURCE_DATA* initial,
                                   ID3D11Texture2D** texture) {
    const void* caller = __builtin_return_address(0);
    bool progressivelyHandled = false;
    HRESULT progressive = createProgressiveTexture(device, desc, initial, texture,
                                                    caller, &progressivelyHandled);
    if (progressivelyHandled) return progressive;
    if (!g_options.shadows || !shadowDepthDesc(desc) || initial)
        return g_createTexture2D(device, desc, initial, texture);
    D3D11_TEXTURE2D_DESC scaled = *desc;
    if (scaled.Width <= 8192 / 2) { scaled.Width *= 2; scaled.Height *= 2; }
    HRESULT hr = g_createTexture2D(device, &scaled, initial, texture);
    if (FAILED(hr) || !texture || !*texture) {
        return g_createTexture2D(device, desc, initial, texture);
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
    if (SUCCEEDED(hr) && shader && *shader && g_options.smaa && isFxaa(bytecode, size)) {
        if (g_fxaaCount < sizeof(g_fxaa) / sizeof(g_fxaa[0]))
            g_fxaa[g_fxaaCount++] = *shader;
        // DXMT deadlocks if a device shader is created re-entrantly from either
        // CreatePixelShader or Draw. A one-shot device worker builds the fixed
        // program after this call returns; FXAA remains active until it is ready.
        startProgramBuild(device);
    }
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
    g_fxaaBound = false;
    for (unsigned i = 0; i < g_fxaaCount; ++i)
        if (shader == g_fxaa[i]) g_fxaaBound = true;
    g_psSetShader(context, shader, classes, count);
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
    if (rtvs) for (UINT i = 0; i < count; ++i)
        if (rtvs[i]) { hasColorTarget = true; break; }
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

const char* kSmaaPrefix =
"#define SMAA_HLSL_4\n"
"#define SMAA_PRESET_HIGH\n"
"cbuffer SmaaMetrics:register(b0){float4 smaaMetrics;}\n"
"#define SMAA_RT_METRICS smaaMetrics\n";

const char* kEdgeWrapper =
"Texture2D tqColorTex:register(t0);\n"
"float4 main(float4 p:SV_POSITION,float2 u:TEXCOORD0):SV_Target{"
"float4 o[3];SMAAEdgeDetectionVS(u,o);"
"return float4(SMAALumaEdgeDetectionPS(u,o,tqColorTex),0,0);}\n";

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

void releaseOverlay() {
    release(g_overlay.vs); release(g_overlay.ps);
    release(g_overlay.textSrv); release(g_overlay.textTexture);
    release(g_overlay.graphSrv); release(g_overlay.graphTexture);
    release(g_overlay.sampler); release(g_overlay.blend);
    release(g_overlay.depth); release(g_overlay.raster);
}

uint32_t rgba(unsigned r, unsigned g, unsigned b, unsigned a) {
    return (a << 24) | (b << 16) | (g << 8) | r;
}

void fillRect(uint32_t* pixels, UINT canvasWidth, UINT canvasHeight,
              int x, int y, int width, int height, uint32_t color) {
    if (!pixels) return;
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > (int)canvasWidth) width = (int)canvasWidth - x;
    if (y + height > (int)canvasHeight) height = (int)canvasHeight - y;
    if (width <= 0 || height <= 0) return;
    for (int row = 0; row < height; ++row)
        std::fill(pixels + (y + row) * canvasWidth + x,
                  pixels + (y + row) * canvasWidth + x + width,
                  color);
}

#define TQ_GLYPH(a, b, c, d, e) \
    ((uint16_t)((a) << 12 | (b) << 9 | (c) << 6 | (d) << 3 | (e)))

uint16_t glyphBits(char character) {
    static const uint16_t digits[10] = {
        TQ_GLYPH(7,5,5,5,7), TQ_GLYPH(2,6,2,2,7),
        TQ_GLYPH(7,1,7,4,7), TQ_GLYPH(7,1,7,1,7),
        TQ_GLYPH(5,5,7,1,1), TQ_GLYPH(7,4,7,1,7),
        TQ_GLYPH(7,4,7,5,7), TQ_GLYPH(7,1,2,2,2),
        TQ_GLYPH(7,5,7,5,7), TQ_GLYPH(7,5,7,1,7)
    };
    static const uint16_t letters[26] = {
        TQ_GLYPH(2,5,7,5,5), TQ_GLYPH(6,5,6,5,6),
        TQ_GLYPH(7,4,4,4,7), TQ_GLYPH(6,5,5,5,6),
        TQ_GLYPH(7,4,6,4,7), TQ_GLYPH(7,4,6,4,4),
        TQ_GLYPH(7,4,5,5,7), TQ_GLYPH(5,5,7,5,5),
        TQ_GLYPH(7,2,2,2,7), TQ_GLYPH(1,1,1,5,7),
        TQ_GLYPH(5,5,6,5,5), TQ_GLYPH(4,4,4,4,7),
        TQ_GLYPH(5,7,7,5,5), TQ_GLYPH(5,7,7,7,5),
        TQ_GLYPH(7,5,5,5,7), TQ_GLYPH(7,5,7,4,4),
        TQ_GLYPH(7,5,5,7,1), TQ_GLYPH(6,5,6,5,5),
        TQ_GLYPH(7,4,7,1,7), TQ_GLYPH(7,2,2,2,2),
        TQ_GLYPH(5,5,5,5,7), TQ_GLYPH(5,5,5,5,2),
        TQ_GLYPH(5,5,7,7,5), TQ_GLYPH(5,5,2,5,5),
        TQ_GLYPH(5,5,2,2,2), TQ_GLYPH(7,1,2,4,7)
    };
    if (character >= '0' && character <= '9') return digits[character - '0'];
    if (character >= 'A' && character <= 'Z') return letters[character - 'A'];
    switch (character) {
        case ':': return TQ_GLYPH(0,2,0,2,0);
        case '.': return TQ_GLYPH(0,0,0,0,2);
        case '>': return TQ_GLYPH(4,2,1,2,4);
        case '/': return TQ_GLYPH(1,1,2,4,4);
        case '-': return TQ_GLYPH(0,0,7,0,0);
        default: return 0;
    }
}

#undef TQ_GLYPH

void drawText(int x, int y, const char* text, uint32_t color) {
    const int scale = 2;
    if (!text) return;
    for (; *text; ++text, x += 4 * scale) {
        uint16_t bits = glyphBits(*text);
        for (int row = 0; row < 5; ++row) {
            unsigned pixels = (bits >> ((4 - row) * 3)) & 7;
            for (int column = 0; column < 3; ++column)
                if (pixels & (1u << (2 - column)))
                    fillRect(g_textPixels, kTextWidth, kTextHeight,
                             x + column * scale, y + row * scale,
                             scale, scale, color);
        }
    }
}

void recordFrameTime() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_frameFrequency.QuadPart) QueryPerformanceFrequency(&g_frameFrequency);
    if (g_lastFrame.QuadPart && g_frameFrequency.QuadPart) {
        double milliseconds = (double)(now.QuadPart - g_lastFrame.QuadPart) * 1000.0
                            / (double)g_frameFrequency.QuadPart;
        if (milliseconds > 0.01 && milliseconds < 10000.0) {
            g_frameSamples[g_frameWrite].ticks = now.QuadPart;
            g_frameSamples[g_frameWrite].milliseconds = (float)milliseconds;
            g_frameWrite = (g_frameWrite + 1) % kFrameSampleCount;
            if (g_frameCount < kFrameSampleCount) ++g_frameCount;
        }
    }
    g_lastFrame = now;
}

void renderOverlayPixels() {
    const uint32_t panel = rgba(9, 14, 20, 188);
    const uint32_t graphPanel = rgba(4, 7, 11, 198);
    const uint32_t white = rgba(195, 210, 222, 232);
    const uint32_t muted = rgba(128, 148, 165, 218);
    const uint32_t mode = g_options.streaming
                        ? rgba(55, 190, 218, 238) : rgba(232, 146, 58, 238);
    std::fill(g_textPixels, g_textPixels + kTextWidth * kTextHeight, panel);
    std::fill(g_graphPixels,
              g_graphPixels + kGraphTextureWidth * kGraphTextureHeight,
              graphPanel);
    fillRect(g_textPixels, kTextWidth, kTextHeight, 0, 0, kTextWidth, 2, mode);
    fillRect(g_textPixels, kTextWidth, kTextHeight,
             0, kTextHeight - 2, kTextWidth, 2, mode);
    fillRect(g_textPixels, kTextWidth, kTextHeight, 0, 0, 2, kTextHeight, mode);
    fillRect(g_textPixels, kTextWidth, kTextHeight,
             kTextWidth - 2, 0, 2, kTextHeight, mode);
    fillRect(g_graphPixels, kGraphTextureWidth, kGraphTextureHeight,
             0, 0, kGraphTextureWidth, 2, mode);
    fillRect(g_graphPixels, kGraphTextureWidth, kGraphTextureHeight,
             0, kGraphTextureHeight - 2, kGraphTextureWidth, 2, mode);
    fillRect(g_graphPixels, kGraphTextureWidth, kGraphTextureHeight,
             0, 0, 2, kGraphTextureHeight, mode);
    fillRect(g_graphPixels, kGraphTextureWidth, kGraphTextureHeight,
             kGraphTextureWidth - 2, 0, 2, kGraphTextureHeight, mode);

    double sum = 0.0;
    float latest = 0.0f, maximum = 0.0f;
    unsigned included = 0, hitches = 0;
    unsigned visible = g_frameCount < kGraphPointCount
                     ? g_frameCount : kGraphPointCount;
    unsigned oldest = (g_frameWrite + kFrameSampleCount - visible)
                    % kFrameSampleCount;
    LONGLONG firstIncluded = visible ? g_frameSamples[oldest].ticks
                                    : g_lastFrame.QuadPart;
    for (unsigned i = 0; i < visible; ++i) {
        const FrameSample& sample =
            g_frameSamples[(oldest + i) % kFrameSampleCount];
        if (!sample.ticks) continue;
        float value = sample.milliseconds;
        g_sortedFrameTimes[included++] = value;
        sum += value;
        if (value > maximum) maximum = value;
        if (value > 25.0f) ++hitches;
    }
    if (included) {
        unsigned latestIndex = (g_frameWrite + kFrameSampleCount - 1)
                             % kFrameSampleCount;
        latest = g_frameSamples[latestIndex].milliseconds;
        std::sort(g_sortedFrameTimes, g_sortedFrameTimes + included);
    }
    float average = included ? (float)(sum / included) : 0.0f;
    float p99 = included ? g_sortedFrameTimes[(included - 1) * 99 / 100] : 0.0f;
    float fps = latest > 0.01f ? 1000.0f / latest : 0.0f;
    float historySeconds = included && g_frameFrequency.QuadPart
        ? (float)(g_lastFrame.QuadPart - firstIncluded)
          / (float)g_frameFrequency.QuadPart : 0.0f;

    char line[96];
    snprintf(line, sizeof(line), "STREAMING: %s",
             g_options.streaming ? "OPTIMIZED" : "ORIGINAL");
    drawText(12, 9, line, mode);
    snprintf(line, sizeof(line), "FRAME: %.1F MS  FPS: %.1F", latest, fps);
    drawText(12, 23, line, white);
    snprintf(line, sizeof(line), "AVG: %.1F  P99: %.1F  MAX: %.1F",
             average, p99, maximum);
    drawText(12, 37, line, white);
    snprintf(line, sizeof(line), "HITCHES >25 MS: %u / %.1F S",
             hitches, historySeconds);
    drawText(12, 51, line, muted);

    const int graphLeft = 4;
    const int graphTop = 4;
    const int graphWidth = kGraphTextureWidth - 8;
    const int graphHeight = kGraphTextureHeight - 8;
    const float graphMaximum = 66.7f;
    const float thresholds[3] = {16.7f, 33.3f, 50.0f};
    const uint32_t gridColors[3] = {
        rgba(38, 83, 61, 175), rgba(101, 88, 37, 175), rgba(105, 47, 45, 175)
    };
    const uint32_t divisionGrid = rgba(54, 65, 76, 105);
    for (unsigned division = 1; division < 8; ++division) {
        int x = graphLeft + (int)(division * graphWidth / 8);
        fillRect(g_graphPixels, kGraphTextureWidth, kGraphTextureHeight,
                 x, graphTop, 1, graphHeight, divisionGrid);
    }
    for (unsigned i = 0; i < 3; ++i) {
        int y = graphTop + graphHeight - 1
              - (int)(thresholds[i] / graphMaximum * (graphHeight - 1));
        fillRect(g_graphPixels, kGraphTextureWidth, kGraphTextureHeight,
                 graphLeft, y, graphWidth, 1, gridColors[i]);
    }

    int previousY = graphTop + graphHeight - 1;
    for (unsigned i = 0; i < visible; ++i) {
        float value = g_frameSamples[(oldest + i) % kFrameSampleCount].milliseconds;
        float clipped = value < graphMaximum ? value : graphMaximum;
        int y = graphTop + graphHeight - 1
              - (int)(clipped / graphMaximum * (graphHeight - 1));
        int x = graphLeft + graphWidth - visible + i;
        uint32_t color = value > 33.3f ? rgba(235, 88, 78, 238)
                       : value > 20.0f ? rgba(225, 187, 70, 232)
                                       : rgba(80, 190, 125, 225);
        int top = i && y < previousY ? y : previousY;
        int height = i ? (y < previousY ? previousY - y + 1
                                        : y - previousY + 1) : 2;
        if (!i) top = y;
        fillRect(g_graphPixels, kGraphTextureWidth, kGraphTextureHeight,
                 x, top, 1, height, color);
        previousY = y;
    }
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

const char* kOverlayVertexShader =
"struct Output{float4 position:SV_POSITION;float2 uv:TEXCOORD0;};"
"Output main(uint id:SV_VertexID){Output o;"
"float2 uv=float2((id<<1)&2,id&2);o.uv=uv;"
"o.position=float4(uv*float2(2,-2)+float2(-1,1),0,1);return o;}";

const char* kOverlayPixelShader =
"Texture2D image:register(t0);SamplerState pointSampler:register(s0);"
"float4 main(float4 position:SV_POSITION,float2 uv:TEXCOORD0):SV_TARGET{"
"return image.Sample(pointSampler,uv);}";

bool createOverlayResources(ID3D11Device* device) {
    if (g_overlay.vs && g_overlay.ps && g_overlay.textTexture
        && g_overlay.graphTexture) return true;
    ID3DBlob *vertex = nullptr, *pixel = nullptr;
    bool ok = compileShader(kOverlayVertexShader, "vs_5_0", &vertex)
           && compileShader(kOverlayPixelShader, "ps_5_0", &pixel);
    if (ok) ok = SUCCEEDED(device->CreateVertexShader(
        vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &g_overlay.vs));
    if (ok) ok = SUCCEEDED(g_createPixelShader(
        device, pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &g_overlay.ps));
    release(vertex); release(pixel);

    D3D11_TEXTURE2D_DESC textTexture = {};
    textTexture.Width = kTextWidth;
    textTexture.Height = kTextHeight;
    textTexture.MipLevels = textTexture.ArraySize = 1;
    textTexture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textTexture.SampleDesc.Count = 1;
    textTexture.Usage = D3D11_USAGE_DEFAULT;
    textTexture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_TEXTURE2D_DESC graphTexture = textTexture;
    graphTexture.Width = kGraphTextureWidth;
    graphTexture.Height = kGraphTextureHeight;
    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = FALSE;
    depth.StencilEnable = FALSE;
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    raster.ScissorEnable = FALSE;
    if (ok) ok = SUCCEEDED(g_createTexture2D(device, &textTexture, nullptr,
                                              &g_overlay.textTexture));
    if (ok) ok = SUCCEEDED(g_createShaderResourceView(
        device, g_overlay.textTexture, nullptr, &g_overlay.textSrv));
    if (ok) ok = SUCCEEDED(g_createTexture2D(device, &graphTexture, nullptr,
                                              &g_overlay.graphTexture));
    if (ok) ok = SUCCEEDED(g_createShaderResourceView(
        device, g_overlay.graphTexture, nullptr, &g_overlay.graphSrv));
    if (ok) ok = SUCCEEDED(g_createSamplerState(device, &sampler, &g_overlay.sampler));
    if (ok) ok = SUCCEEDED(device->CreateBlendState(&blend, &g_overlay.blend));
    if (ok) ok = SUCCEEDED(device->CreateDepthStencilState(&depth, &g_overlay.depth));
    if (ok) ok = SUCCEEDED(device->CreateRasterizerState(&raster, &g_overlay.raster));
    if (!ok) releaseOverlay();
    return ok;
}

bool createProgramResources(ID3D11Device* device) {
    bool smaaOk = !g_options.smaa || createSmaaProgramResources(device);
    if (g_options.frameOverlay) createOverlayResources(device);
    return smaaOk;
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

void drawFrameOverlay(ID3D11DeviceContext* context) {
    if (!g_options.frameOverlay || !context || !g_overlay.vs || !g_overlay.ps
        || !g_overlay.textTexture || !g_overlay.textSrv
        || !g_overlay.graphTexture || !g_overlay.graphSrv || !g_overlay.sampler
        || !g_overlay.blend || !g_overlay.depth || !g_overlay.raster) return;
    LONGLONG refreshTicks = g_frameFrequency.QuadPart / 10;
    if (!g_lastOverlayRefresh || !refreshTicks
        || g_lastFrame.QuadPart - g_lastOverlayRefresh >= refreshTicks) {
        renderOverlayPixels();
        context->UpdateSubresource(g_overlay.textTexture, 0, nullptr, g_textPixels,
                                   kTextWidth * sizeof(uint32_t), 0);
        context->UpdateSubresource(g_overlay.graphTexture, 0, nullptr, g_graphPixels,
                                   kGraphTextureWidth * sizeof(uint32_t), 0);
        g_lastOverlayRefresh = g_lastFrame.QuadPart;
    }

    SavedState old;
    saveState(context, old);
    if (!old.rtvs[0]) {
        restoreState(context, old);
        return;
    }

    FLOAT renderWidth = old.viewportCount ? old.viewports[0].Width : 1920.0f;
    ID3D11Resource* outputResource = nullptr;
    ID3D11Texture2D* outputTexture = nullptr;
    old.rtvs[0]->GetResource(&outputResource);
    if (outputResource)
        outputResource->QueryInterface(__uuidof(ID3D11Texture2D),
                                       (void**)&outputTexture);
    if (outputTexture) {
        D3D11_TEXTURE2D_DESC outputDesc = {};
        outputTexture->GetDesc(&outputDesc);
        if (outputDesc.Width) renderWidth = (FLOAT)outputDesc.Width;
    }
    release(outputTexture);
    release(outputResource);
    if (renderWidth < 200.0f) renderWidth = 1920.0f;
    D3D11_VIEWPORT textViewport = {20.0f, 20.0f, (FLOAT)kTextWidth,
                                   (FLOAT)kTextHeight, 0.0f, 1.0f};
    D3D11_VIEWPORT graphViewport = {20.0f, 96.0f, renderWidth - 40.0f,
                                    (FLOAT)kGraphTextureHeight, 0.0f, 1.0f};
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g_overlay.vs, nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(g_overlay.ps, nullptr, 0);
    context->PSSetSamplers(0, 1, &g_overlay.sampler);
    context->OMSetRenderTargets(1, &old.rtvs[0], nullptr);
    context->OMSetBlendState(g_overlay.blend, nullptr, 0xffffffff);
    context->OMSetDepthStencilState(g_overlay.depth, 0);
    context->RSSetState(g_overlay.raster);
    bool previousInside = g_inside;
    g_inside = true;
    context->PSSetShaderResources(0, 1, &g_overlay.textSrv);
    context->RSSetViewports(1, &textViewport);
    context->Draw(3, 0);
    context->PSSetShaderResources(0, 1, &g_overlay.graphSrv);
    context->RSSetViewports(1, &graphViewport);
    context->Draw(3, 0);
    g_inside = previousInside;
    ID3D11ShaderResourceView* nullView = nullptr;
    context->PSSetShaderResources(0, 1, &nullView);
    restoreState(context, old);
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

void WINAPI hookDraw(ID3D11DeviceContext* context, UINT count, UINT start) {
    if (!g_inside && g_options.smaa && g_fxaaBound) {
        g_inside = true; bool done = runSmaa(context, false, count, start, 0); g_inside = false;
        if (done) return;
    }
    g_draw(context, count, start);
}

void WINAPI hookDrawIndexed(ID3D11DeviceContext* context, UINT count, UINT start, INT base) {
    if (!g_inside && g_options.smaa && g_fxaaBound) {
        g_inside = true; bool done = runSmaa(context, true, count, start, base); g_inside = false;
        if (done) return;
    }
    g_drawIndexed(context, count, start, base);
}

}  // namespace

void install(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || InterlockedCompareExchange(&g_installed, 1, 0)) return;
    readOptions();
    if (!context) device->GetImmediateContext(&context); else context->AddRef();
    if (!context) { InterlockedExchange(&g_installed, 0); return; }
    if (g_options.streaming) {
        InitializeCriticalSection(&g_uploadLock);
        g_uploadLockReady = true;
    }
    if (g_options.streaming || g_options.frameOverlay)
        tq::streaming::setPresentCallback(&onPresent);
    g_device = device;
    g_context = context;
    void** dv = *(void***)device;
    void** cv = *(void***)context;
    bool ok = true;
    if (g_options.anisotropy > 1)
        ok &= patchSlot(&dv[23], (void*)&hookCreateSamplerState, (void**)&g_createSamplerState);
    else g_createSamplerState = (CreateSamplerStateFn)dv[23];
    if (g_options.shadows || g_options.streaming)
        ok &= patchSlot(&dv[5], (void*)&hookCreateTexture2D, (void**)&g_createTexture2D);
    else g_createTexture2D = (CreateTexture2DFn)dv[5];
    if (g_options.streaming)
        ok &= patchSlot(&dv[7], (void*)&hookCreateShaderResourceView,
                        (void**)&g_createShaderResourceView);
    else g_createShaderResourceView = (CreateShaderResourceViewFn)dv[7];
    if (g_options.smaa)
        ok &= patchSlot(&dv[15], (void*)&hookCreatePixelShader, (void**)&g_createPixelShader);
    else g_createPixelShader = (CreatePixelShaderFn)dv[15];
    if (g_options.smaa) {
        ok &= patchSlot(&cv[9], (void*)&hookPSSetShader, (void**)&g_psSetShader);
        ok &= patchSlot(&cv[12], (void*)&hookDrawIndexed, (void**)&g_drawIndexed);
        ok &= patchSlot(&cv[13], (void*)&hookDraw, (void**)&g_draw);
    }
    if (g_options.streaming)
        ok &= patchSlot(&cv[8], (void*)&hookPSSetShaderResources,
                        (void**)&g_psSetShaderResources);
    else g_psSetShaderResources = (PSSetShaderResourcesFn)cv[8];
    if (g_options.shadows) {
        ok &= patchSlot(&cv[33], (void*)&hookOMSetRenderTargets, (void**)&g_omSetRenderTargets);
        ok &= patchSlot(&cv[44], (void*)&hookRSSetViewports, (void**)&g_rsSetViewports);
        ok &= patchSlot(&cv[45], (void*)&hookRSSetScissors, (void**)&g_rsSetScissors);
    }
    g_updateSubresource = g_options.streaming ? (UpdateSubresourceFn)cv[48] : nullptr;
    if (ok && g_options.frameOverlay) startProgramBuild(device);
    if (!ok) {
        restoreSlots();
        g_context->Release();
        g_context = nullptr;
        if (g_uploadLockReady) {
            g_uploadLockReady = false;
            DeleteCriticalSection(&g_uploadLock);
        }
        tq::streaming::setPresentCallback(nullptr);
        InterlockedExchange(&g_installed, 0);
    }
}

void onPresent() {
    if (g_options.frameOverlay) recordFrameTime();
    advanceTextureUploadsInternal();
    if (g_options.frameOverlay) drawFrameOverlay(g_context);
}

void shutdown() {
    tq::streaming::setPresentCallback(nullptr);
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
        releaseOverlay();
        if (g_compiler) { FreeLibrary(g_compiler); g_compiler = nullptr; }
    }
    memset(g_fxaa, 0, sizeof(g_fxaa)); g_fxaaCount = 0;
    memset(g_shadowTextures, 0, sizeof(g_shadowTextures)); g_shadowTextureCount = 0;
    if (g_context) { g_context->Release(); g_context = nullptr; }
    g_device = nullptr; g_fxaaBound = g_shadowBound = false;
    g_createTexture2D = nullptr;
    g_createPixelShader = nullptr;
    g_createShaderResourceView = nullptr;
    g_psSetShaderResources = nullptr;
    g_updateSubresource = nullptr;
    g_archiveUnmap = nullptr;
    memset(g_frameSamples, 0, sizeof(g_frameSamples));
    memset(g_sortedFrameTimes, 0, sizeof(g_sortedFrameTimes));
    g_frameWrite = g_frameCount = 0;
    g_lastOverlayRefresh = 0;
    g_frameFrequency.QuadPart = g_lastFrame.QuadPart = 0;
    InterlockedExchange(&g_archiveVtablePatched, 0);
    InterlockedExchange(&g_programState, 0);
    InterlockedExchange(&g_installed, 0);
}

}  // namespace visual
}  // namespace tq
