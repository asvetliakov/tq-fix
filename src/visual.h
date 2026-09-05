#pragma once

#include <d3d11.h>

namespace tq {
namespace visual {

// Exact full-resolution renderer-target ordinals that carry scene/post color
// and must remain FP16 through tone mapping.
inline bool isFp16SceneTargetOrdinal(unsigned id) {
    return id == 5 || id == 7 || id == 9 || id == 11 || id == 12 || id == 13;
}

// Installs the optional visual-enhancement hooks on the game's one immediate
// device/context. The original rendering path remains callable on every
// failure.
void install(ID3D11Device* device, ID3D11DeviceContext* context,
             IDXGISwapChain* swapChain = nullptr);
// Called at the renderer's device-creation boundary, with rendering stopped.
// Retires the previous device before DXGI can reuse its window. A compiler
// timeout leaves the existing state intact and rejects this creation attempt.
bool prepareDeviceRecreation();
bool isAuxiliaryWindow(HWND window);
// Advances one bounded chunk of a retained game-side texture upload. Called
// from the game's Present path so upload work is spread across frames.
void onPresent(IDXGISwapChain* swapChain);
void onPostPresent(IDXGISwapChain* swapChain);
void onBeforeResize(IDXGISwapChain* swapChain);
void onResize(IDXGISwapChain* swapChain);
void shutdown();

}  // namespace visual
}  // namespace tq
