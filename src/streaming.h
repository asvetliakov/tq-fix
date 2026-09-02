#pragma once

#include <dxgi.h>

namespace tq {
namespace streaming {

// Only "original" disables the optimization. Missing, "optimized", and
// unknown values retain the release default.
bool optimizationEnabled(const wchar_t* value);

// Hooks Titan Quest's renderer-level Present wrapper. The game's wrapper then
// calls the current swap-chain vtable normally, preserving Steam/THQN/driver
// overlay ownership and ordering.
bool installRenderer(HMODULE renderer);
bool presentHookInstalled();

// Retains ResizeBuffers handling only where no Steam overlay is loaded. Present
// itself is deliberately never patched through the shared DXGI vtable.
void installSwapChain(IDXGISwapChain* swapChain);
void setPresentCallback(void (*callback)(IDXGISwapChain*));
void setPostPresentCallback(void (*callback)(IDXGISwapChain*));
void setPreResizeCallback(void (*callback)(IDXGISwapChain*));
void setResizeCallback(void (*callback)(IDXGISwapChain*));
void shutdown();

}  // namespace streaming
}  // namespace tq
