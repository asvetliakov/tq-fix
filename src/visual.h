#pragma once

#include <d3d11.h>

namespace tq {
namespace visual {

// Installs the optional visual-enhancement hooks on the game's one immediate
// device/context. The original rendering path remains callable on every
// failure.
void install(ID3D11Device* device, ID3D11DeviceContext* context,
             IDXGISwapChain* swapChain = nullptr);
// Advances one bounded chunk of a retained game-side texture upload. Called
// from the game's Present path so upload work is spread across frames.
void onPresent(IDXGISwapChain* swapChain);
void onPostPresent(IDXGISwapChain* swapChain);
void onBeforeResize(IDXGISwapChain* swapChain);
void onResize(IDXGISwapChain* swapChain);
void shutdown();

}  // namespace visual
}  // namespace tq
