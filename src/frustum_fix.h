#pragma once

#include <windows.h>
#include <stdint.h>

namespace tq {
namespace frustum {

// Finds the one GetFrustumForPlayer viewport-construction sequence. The
// returned address is the instruction immediately after the Viewport
// constructor call, which lets the runtime hook reject every other caller.
const BYTE* findUpdateViewportCall(const BYTE* code, SIZE_T size,
                                   uintptr_t viewportCtorSlot,
                                   uintptr_t worldFrustumSlot,
                                   unsigned* matchCount = nullptr);

// Applies the runtime hook's argument policy without touching the caller's
// left/top values. Returns true only when width/height were expanded.
bool selectViewportSize(bool enabled, bool targetCall,
                        int requestedWidth, int requestedHeight,
                        int liveWidth, int liveHeight,
                        int* selectedWidth, int* selectedHeight);

// Installs the optional, signature-gated Game.dll hook. Failure is deliberately
// silent and leaves the game's original update frustum active.
void install(HMODULE gameModule);
void shutdown();

}  // namespace frustum
}  // namespace tq
