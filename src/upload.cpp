#include "upload.h"

#include <string.h>

#include "probe.h"

namespace tq {
namespace upload {

namespace {

const UINT kUploadChunkBytes = 2 * 1024 * 1024;

// What one progressive upload chunk is allowed to cost. At 60 FPS a frame is
// 16.7 ms, and a background upload that takes a fifth of it is already
// visible in the pacing graph.
const double kUploadTargetMs = 3.0;
const UINT kUploadFloorBytes = 256 * 1024;

const unsigned kMaxUploadJobs = 256;

// Milliseconds per KiB, smoothed over recent chunks. The source is a mapped
// archive view, so a chunk's cost depends on whether its pages are resident
// and varies far too much to predict from size alone -- but the recent rate is
// a much better opening guess than the ceiling.
double g_uploadMsPerKib = 0.002;

struct UploadJob {
    LONG state;  // 0 free, 1 reserved, 2 uploading
    ID3D11Texture2D* texture;
    ID3D11ShaderResourceView* fullView;
    ID3D11ShaderResourceView* lowView;
    void* token;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_SUBRESOURCE_DATA source[kMaxTextureMips];
    UINT lowMip;
    UINT mip;
    UINT blockRow;
    UINT chunkBytes;
    UINT maxChunkBytes;
};

UploadJob g_uploadJobs[kMaxUploadJobs];
CRITICAL_SECTION g_uploadLock;
bool g_uploadLockReady;
Calls g_calls;

UploadJob* reserveUploadJob() {
    for (unsigned i = 0; i < kMaxUploadJobs; ++i)
        if (InterlockedCompareExchange(&g_uploadJobs[i].state, 1, 0) == 0)
            return &g_uploadJobs[i];
    return nullptr;
}

}  // namespace

UINT chunkBytesForTargetMs() {
    if (!(g_uploadMsPerKib > 0.0)) return kUploadFloorBytes;
    double kib = kUploadTargetMs / g_uploadMsPerKib;
    if (kib < 256.0) kib = 256.0;
    if (kib > (double)kUploadChunkBytes / 1024.0)
        kib = (double)kUploadChunkBytes / 1024.0;
    return (UINT)(kib * 1024.0);
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

bool install(const Calls& calls) {
    if (!calls.createTexture2D || !calls.createShaderResourceView
        || !calls.updateSubresource || !calls.now || !calls.millisecondsSince
        || !calls.retain || !calls.release) return false;
    if (g_uploadLockReady) return true;
    g_calls = calls;
    InitializeCriticalSection(&g_uploadLock);
    g_uploadLockReady = true;
    return true;
}

bool ready() { return g_uploadLockReady; }

void lock() { if (g_uploadLockReady) EnterCriticalSection(&g_uploadLock); }
void unlock() { if (g_uploadLockReady) LeaveCriticalSection(&g_uploadLock); }

void shutdown() {
    if (!g_uploadLockReady) return;
    EnterCriticalSection(&g_uploadLock);
    for (unsigned i = 0; i < kMaxUploadJobs; ++i) {
        UploadJob& job = g_uploadJobs[i];
        if (job.lowView) job.lowView->Release();
        if (job.texture) job.texture->Release();
        memset(&job, 0, sizeof(job));
    }
    LeaveCriticalSection(&g_uploadLock);
    g_uploadLockReady = false;
    DeleteCriticalSection(&g_uploadLock);
    memset(&g_calls, 0, sizeof(g_calls));
}

HRESULT create(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc,
               const D3D11_SUBRESOURCE_DATA* initial,
               ID3D11Texture2D** texture, uint64_t topBytes, void* owner,
               bool* handled) {
    *handled = false;
    if (!g_uploadLockReady || !desc || !initial || !texture) return E_FAIL;
    UploadJob* job = reserveUploadJob();
    if (!job) {
        tq::probe::engineCount(tq::probe::CounterUploadRejectPool);
        tq::probe::engineCount(tq::probe::CounterUploadRejected);
        return E_FAIL;
    }

    UINT lowMip = lowMipFor(desc);
    D3D11_SUBRESOURCE_DATA staged[kMaxTextureMips] = {};
    for (UINT mip = lowMip; mip < desc->MipLevels; ++mip)
        staged[mip] = initial[mip];
    HRESULT hr = g_calls.createTexture2D(device, desc, staged, texture);
    if (FAILED(hr) || !*texture) {
        InterlockedExchange(&job->state, 0);
        return E_FAIL;
    }

    EnterCriticalSection(&g_uploadLock);
    void* token = nullptr;
    if (!g_calls.retain(owner, &token)) {
        LeaveCriticalSection(&g_uploadLock);
        (*texture)->Release();
        *texture = nullptr;
        InterlockedExchange(&job->state, 0);
        // The retention resource could not be acquired: the lease table is
        // full, or the mapping pointer moved under the swap. Stage 2 replaces
        // leases with a copy, and this becomes the allocation failure proper.
        tq::probe::engineCount(tq::probe::CounterUploadRejectAlloc);
        tq::probe::engineCount(tq::probe::CounterUploadRejected);
        return E_FAIL;
    }

    memset((BYTE*)job + sizeof(job->state), 0, sizeof(*job) - sizeof(job->state));
    job->texture = *texture;
    job->texture->AddRef();
    job->token = token;
    job->desc = *desc;
    memcpy(job->source, initial, desc->MipLevels * sizeof(*initial));
    job->lowMip = lowMip;
    job->mip = 0;
    job->blockRow = 0;
    job->maxChunkBytes = topBytes <= 4ull * 1024ull * 1024ull
                       ? 1024 * 1024 : kUploadChunkBytes;
    // Start from what the last chunks actually cost rather than from the
    // ceiling. Opening at the maximum is what produced the 34 ms outlier: the
    // controller could only correct after a frame had already paid for it.
    job->chunkBytes = chunkBytesForTargetMs();
    // The loader thread's, so the engine channel; this counter read zero for
    // every run that ever recorded it.
    tq::probe::engineCount(tq::probe::CounterUploadJobsStarted);
    InterlockedExchange(&job->state, 2);
    LeaveCriticalSection(&g_uploadLock);

    *handled = true;
    return hr;
}

void noteShaderResourceView(ID3D11Device* device, ID3D11Resource* resource,
                            const D3D11_SHADER_RESOURCE_VIEW_DESC* description,
                            ID3D11ShaderResourceView* view) {
    if (!g_uploadLockReady || !view) return;
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
            if (SUCCEEDED(g_calls.createShaderResourceView(device, resource,
                                                           &low, &lowView))) {
                job.fullView = view;
                job.lowView = lowView;
            }
        }
        break;
    }
    LeaveCriticalSection(&g_uploadLock);
}

void substituteLocked(UINT count, ID3D11ShaderResourceView** slots) {
    if (!g_uploadLockReady || !slots) return;
    for (UINT slot = 0; slot < count; ++slot) {
        if (!slots[slot]) continue;
        for (unsigned i = 0; i < kMaxUploadJobs; ++i) {
            UploadJob& job = g_uploadJobs[i];
            if (job.state == 2 && job.fullView == slots[slot] && job.lowView) {
                slots[slot] = job.lowView;
                break;
            }
        }
    }
}

void advance(ID3D11DeviceContext* context) {
    if (!g_uploadLockReady || !context) return;
    ID3D11Texture2D* releaseTexture = nullptr;
    ID3D11ShaderResourceView* releaseLowView = nullptr;
    void* releaseToken = nullptr;
    bool retired = false;

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
        // QueryPerformanceCounter, not GetTickCount. The tick counter's
        // resolution is about 15.6 ms, so a chunk that took four milliseconds
        // measured zero and the budget ratcheted to its ceiling and stayed
        // there, while a chunk unlucky enough to straddle a tick measured
        // fifteen and was halved for no reason. A measured run showed the
        // controller pinned at the ceiling while chunks really cost 3.2 ms per
        // MiB -- up to 19 ms in a single frame.
        //
        // `source` points into the memory-mapped archive, so this call can also
        // fault pages in off disk. That cost is real, is part of what the frame
        // paid, and is inside the interval measured here.
        int64_t before = g_calls.now();
        g_calls.updateSubresource(context, job.texture, job.mip, &box,
                                  source, pitch, 0);
        double stepMs = g_calls.millisecondsSince(before);
        // Budget in time rather than in bytes: what matters to frame pacing is
        // the milliseconds this stole, and how many bytes that bought varies
        // with whether the source was resident.
        UINT sentKib = (UINT)(((uint64_t)rows * pitch) / 1024u);
        if (sentKib && stepMs > 0.0) {
            // Weighted towards recent history, and hard against the worst
            // case: a chunk that cost far more than expected must move the
            // estimate immediately, not over the next twenty chunks.
            double observed = stepMs / (double)sentKib;
            double weight = observed > g_uploadMsPerKib ? 0.5 : 0.1;
            g_uploadMsPerKib += (observed - g_uploadMsPerKib) * weight;
        }
        UINT predicted = chunkBytesForTargetMs();
        if (stepMs > kUploadTargetMs && job.chunkBytes > kUploadFloorBytes)
            job.chunkBytes = job.chunkBytes / 2 < predicted
                           ? job.chunkBytes / 2 : predicted;
        else
            job.chunkBytes = predicted;
        if (job.chunkBytes < kUploadFloorBytes)
            job.chunkBytes = kUploadFloorBytes;
        if (job.chunkBytes > job.maxChunkBytes)
            job.chunkBytes = job.maxChunkBytes;
        tq::probe::count(tq::probe::CounterUploadSteps);
        tq::probe::count(tq::probe::CounterUploadKiB,
                         (uint32_t)(((uint64_t)rows * pitch) / 1024u));
        job.blockRow += rows;
        if (job.blockRow >= totalBlockRows) {
            ++job.mip;
            job.blockRow = 0;
        }
    }

    if (job.mip >= job.lowMip) {
        tq::probe::count(tq::probe::CounterUploadJobsDone);
        releaseTexture = job.texture;
        releaseLowView = job.lowView;
        releaseToken = job.token;
        retired = true;
        memset((BYTE*)&job + sizeof(job.state), 0,
               sizeof(job) - sizeof(job.state));
        InterlockedExchange(&job.state, 0);
    }
    LeaveCriticalSection(&g_uploadLock);

    if (releaseLowView) releaseLowView->Release();
    if (releaseTexture) releaseTexture->Release();
    // Outside the lock, so an implementation is free to make a syscall here --
    // the shipped one unmaps a view. It may take the lock back if it needs to;
    // a CRITICAL_SECTION is recursive, and this thread no longer holds it.
    if (retired) g_calls.release(releaseToken);
}

#ifdef TQ_SELFTEST
unsigned runningJobsForTest() {
    unsigned n = 0;
    for (unsigned i = 0; i < kMaxUploadJobs; ++i)
        if (g_uploadJobs[i].state == 2) ++n;
    return n;
}

double msPerKibForTest() { return g_uploadMsPerKib; }

void resetRateForTest() { g_uploadMsPerKib = 0.002; }
#endif

}  // namespace upload
}  // namespace tq
