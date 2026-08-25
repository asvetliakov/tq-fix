// Stage 4: count the draws, frame by frame.
//
// Vtable data-writes on the three objects Stage 3 captured. `Present` gives the
// frame counter; the `Draw*` family gives a per-frame count; and while the
// device vtable is open, `CreateBuffer`, the two shader creators and
// `CreateSamplerState` are logged for H-B1 and for the Stage 6 bug report.
//
// Still an observer: every hook calls through with the arguments untouched and
// returns what the real function returned. The one thing this stage changes
// about the process is that a handful of vtable slots now point at us.

#pragma once

#include <windows.h>
#include <d3d11.h>

namespace tq {
namespace frames {

/**
 * Patch the vtables. Called from inside the `D3D11CreateDeviceAndSwapChain`
 * hook, on the game's thread, **before the game has been handed the device** —
 * so no other thread can be inside a slot we are rewriting (Risk 4).
 *
 * Asks Risk 3's question of `ID3D11DeviceContext1` before touching the context,
 * and patches whichever vtable(s) the answer requires.
 */
bool install(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGISwapChain* sc);

/** Totals so far. Safe when nothing was installed. */
void report(const char* when);

}  // namespace frames
}  // namespace tq
