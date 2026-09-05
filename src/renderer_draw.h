#pragma once

#include <d3d11.h>

namespace tq { namespace rendererdraw {
typedef void (WINAPI* DrawFn)(ID3D11DeviceContext*, UINT, UINT);
typedef void (WINAPI* DrawIndexedFn)(ID3D11DeviceContext*, UINT, UINT, INT);

// Installs both audited renderer submission sites, or neither. Called before
// the renderer starts using its newly created device. Native context tables
// are never changed; callbacks forward through the live context methods.
bool install(HMODULE renderer, DrawFn draw, DrawIndexedFn indexed);
bool installed();
void shutdown();
} }
