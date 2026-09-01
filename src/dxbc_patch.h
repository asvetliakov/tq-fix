#pragma once

#include <windows.h>

namespace tq {
namespace dxbc {

struct PatchResult {
    void*  data;
    SIZE_T size;
};

// Inserts one vector UMIN before TQ's bone-index IMUL and makes the IMUL use
// the clamped value. Returns false for every shader that is not the exact
// dynamic-indexed TQ skinning shape; callers then pass the original blob on.
bool clampBoneIndices(const void* bytecode, SIZE_T bytecodeSize, PatchResult* out);

// Moves TQ's four bilinear shadow taps from a cross to the four corners of a
// 3x3 PCF footprint. The comparison-linear sampler supplies the interpolated
// sub-taps, so this widens coverage without adding five texture instructions.
bool enhanceShadowPcf(const void* bytecode, SIZE_T bytecodeSize, PatchResult* out);

// Retunes the four PCF taps of the deferred screen-space shadow receiver, and
// only that shader. Titan Quest applies directional shadows in one such pass
// rather than per material; the per-material receivers above and the
// point-light receiver share the same tap shape but must not be touched.
//
// `factor` scales the tap offsets. An offset is a UV distance, so the blur it
// produces measures 0.5 * bluriness * world coverage -- widening the shadow
// projection softens edges in world space regardless of map resolution. Pass
// the inverse coverage ratio to hold softness constant.
//
// `biasScale` scales the receiver's depth bias, which is normalised to the
// fitted depth range and so grows in world units as the split widens,
// detaching shadows from their casters.
//
// `corners` moves the taps from the native axis cross onto the corners of a
// 3x3 footprint, covering an area rather than a cross for the same four
// texture instructions.
bool tuneDeferredShadowFilter(const void* bytecode, SIZE_T bytecodeSize,
                              float factor, float biasScale, bool corners,
                              PatchResult* out);
void release(PatchResult* result);

}  // namespace dxbc
}  // namespace tq
