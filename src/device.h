// Stage 3: reach the device.
//
// Hold a valid `ID3D11Device*` and `ID3D11DeviceContext*`, log them, and change
// nothing else. No vtable is touched this stage — that is Stage 4, and doing it
// early would mean a flicker measurement taken with an untested patch in the
// frame path.

#pragma once

#include <windows.h>

namespace tq {
namespace device {

/**
 * Wait for `Direct3D11.dll`, then redirect its import of `D3D11CreateDevice*`.
 *
 * Called from our watcher thread, never from `DllMain`: it sleeps. `cancel` is
 * signalled on an orderly detach and cuts every wait short — without it a
 * `FreeLibrary` would unmap this code while the thread was still parked in it.
 *
 * Returns true if at least one entry point was hooked.
 */
bool install(HANDLE cancel);

/** One block saying what was seen. Safe to call when nothing ever fired. */
void report(const char* when);

}  // namespace device
}  // namespace tq
