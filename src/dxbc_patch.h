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
void release(PatchResult* result);

}  // namespace dxbc
}  // namespace tq
