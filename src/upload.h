#pragma once

#include <windows.h>
#include <d3d11.h>
#include <stdint.h>

namespace tq {
namespace upload {

// Progressive texture upload.
//
// Titan Quest creates a streamed texture with every mip's bytes already in
// hand and hands the whole thing to the driver in one CreateTexture2D. For a
// 2048x2048 BC1 that is a multi-megabyte synchronous upload on whichever
// thread happened to touch the resource, and it lands as a frame hitch.
//
// This module creates the texture with only its small mips populated, hands
// the game a view of those, and feeds the large mips to the driver in bounded
// chunks over the following frames -- substituting the low-mip view wherever
// the game binds the full one, until the upload has caught up. Terrain goes
// soft for a few frames instead of the frame going long.
//
// It knows nothing about Engine.dll, about hooks, or about where the source
// bytes came from. Everything it needs is injected, which is also what makes
// it testable off-game: the chunk controller is a feedback loop over measured
// time, and it cannot be tested at all against a clock that only ever tells
// the truth.

const unsigned kMaxTextureMips = 16;

struct Calls {
    HRESULT (WINAPI* createTexture2D)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                      const D3D11_SUBRESOURCE_DATA*,
                                      ID3D11Texture2D**);
    HRESULT (WINAPI* createShaderResourceView)(ID3D11Device*, ID3D11Resource*,
                                               const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                               ID3D11ShaderResourceView**);
    void (WINAPI* updateSubresource)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                     const D3D11_BOX*, const void*, UINT, UINT);
    // The clock, injected in two halves so a test can advance it by a chosen
    // amount rather than by sleeping.
    int64_t (*now)();
    double (*millisecondsSince)(int64_t start);
    // Whatever hold the caller needs on the source bytes for as long as the
    // job runs. Called under this module's lock once the texture exists;
    // returning false declines the job. The token is stored in the job and
    // handed back to `release` when it retires -- after the lock is dropped,
    // so an implementation that needs the lock must take it, which is safe
    // because it is a recursive CRITICAL_SECTION.
    bool (*retain)(void* owner, void** token);
    void (*release)(void* token);
};

// Brings the module up with its dependencies. Allocates the lock; everything
// else is static. Returns false if `calls` is missing an entry point.
bool install(const Calls& calls);
void shutdown();
bool ready();

// The module's lock, exposed because the substitution path is called with it
// already held -- see substituteLocked.
void lock();
void unlock();

// Starts a progressive job for a texture the caller has already decided is a
// candidate, creating it through Calls::createTexture2D with only the mips at
// or below `lowMipFor(desc)` populated. `*handled` is set only when a job was
// actually started and `*texture` is the caller's to return; on every other
// path it stays false and the caller must fall back to the ordinary creation
// it would have done anyway. `owner` is opaque and reaches Calls::retain.
HRESULT create(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc,
               const D3D11_SUBRESOURCE_DATA* initial,
               ID3D11Texture2D** texture, uint64_t topBytes, void* owner,
               bool* handled);

// Called after the real CreateShaderResourceView, with the lock not held: if
// the view belongs to a running job, records it and builds the matching
// low-mip view to stand in for it.
void noteShaderResourceView(ID3D11Device* device, ID3D11Resource* resource,
                            const D3D11_SHADER_RESOURCE_VIEW_DESC* description,
                            ID3D11ShaderResourceView* view);

// Replaces any full view in `slots` that belongs to a running job with that
// job's low-mip view. **Called with the lock already held**, so the caller can
// keep it across its own PSSetShaderResources -- which is what the shipped
// code does, and what Stage 2.6 of the mitigation plan changes.
void substituteLocked(UINT count, ID3D11ShaderResourceView** slots);

// One chunk of one job, if there is one to do. Called from the pre-Present
// callback on the render thread.
void advance(ID3D11DeviceContext* context);

// Recovers a texture container's base-level dimensions from its first bytes,
// for both shapes this game ships: a "TEX" container, whose payload begins
// with a DDS-style header twelve bytes in, and a bare DDS. Returns false for
// anything that is not one, which is what lets a caller run this over every
// file a source opens without knowing which are textures.
//
// Here rather than beside the hook that uses it because it is pure, and
// scripts/selftest-offgame.sh links this translation unit.
bool textureDimensions(const void* header, size_t bytes, UINT* width,
                       UINT* height);

// The largest mip the low-detail view starts at: the first mip at or below
// 512 on both axes, or the last one if none is.
UINT lowMipFor(const D3D11_TEXTURE2D_DESC* desc);

// How many bytes one chunk should be, from the smoothed cost of recent
// chunks. Exposed for the test, which is the only way to observe the
// controller without a device.
UINT chunkBytesForTargetMs();

#ifdef TQ_SELFTEST
unsigned runningJobsForTest();
double msPerKibForTest();
void resetRateForTest();
#endif

}  // namespace upload
}  // namespace tq
