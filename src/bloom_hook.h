#pragma once

#include <windows.h>

namespace tq {
namespace bloomhook {

typedef void (__thiscall* HotBlurFn)(void* canvas, unsigned width, unsigned height,
                                     float extraction, float strength,
                                     float saturation);

// Detours the exported Engine HotBlurFrameBuffer entry after validating its
// exact six-byte x86 prologue. Suppression skips the complete native bloom
// operation; original mode and failure fallback use an executable trampoline.
// Every patched byte is restored by shutdown().
bool install(HMODULE engine, HotBlurFn original);
void setSuppression(bool enabled);
void shutdown();
bool installed();

// Pure reference used by off-game tests and by the bloom shader contract.
float extractBrightness(float brightness, float threshold, float knee);

}  // namespace bloomhook
}  // namespace tq
