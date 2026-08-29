#pragma once

#include <d3d11.h>

namespace tq {
namespace visual {

// Installs the optional visual-enhancement hooks on the game's one immediate
// device/context. The original rendering path remains callable on every
// failure.
void install(ID3D11Device* device, ID3D11DeviceContext* context);
void shutdown();

}  // namespace visual
}  // namespace tq
