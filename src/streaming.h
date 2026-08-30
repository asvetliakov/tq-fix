#pragma once

#include <dxgi.h>

namespace tq {
namespace streaming {

// Only "original" disables the optimization. Missing, "optimized", and
// unknown values retain the release default.
bool optimizationEnabled(const wchar_t* value);

// Runs the progressive-upload step immediately before Present, keeping all
// D3D11 immediate-context work on the render thread.
void installSwapChain(IDXGISwapChain* swapChain);
void setPresentCallback(void (*callback)(IDXGISwapChain*));
void setPostPresentCallback(void (*callback)(IDXGISwapChain*));
void setPreResizeCallback(void (*callback)(IDXGISwapChain*));
void setResizeCallback(void (*callback)(IDXGISwapChain*));
void shutdown();

}  // namespace streaming
}  // namespace tq
