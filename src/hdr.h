#pragma once

#include <d3d11.h>
#include <dxgi1_6.h>

namespace tq {
namespace hdr {

enum ToneMap {
    ToneOriginal,
    ToneAgx,
    ToneFrostbite
};

struct Settings {
    bool requestHdr;
    ToneMap toneMap;
    float paperWhiteNits;
    float peakNitsOverride;
    bool debug;
    bool trace;
};

struct Runtime {
    Settings settings;
    bool displayHdr;
    bool fp16Active;
    bool active;
    float peakNits;
};

Settings readSettings();
const Runtime& runtime();

bool supportsTearing(IDXGIFactory* factory);
DXGI_SWAP_CHAIN_DESC fp16SwapChainDescription(
    const DXGI_SWAP_CHAIN_DESC& original, bool allowTearing);

// Returns true when enhanced output is selected and candidate contains a complete
// FP16 flip-model description. The same linear scRGB path is used for SDR and
// HDR; only the final display mapping differs.
bool makeSwapChainCandidate(const DXGI_SWAP_CHAIN_DESC& original,
                            DXGI_SWAP_CHAIN_DESC* candidate);

// Validates FP16/color-space support after creation. The caller must discard
// the candidate device and retry the original description when this fails.
bool activateSwapChain(IDXGISwapChain* swapChain);
void reapplyColorSpace(IDXGISwapChain* swapChain);

bool isColorGradingShader(const void* bytecode, SIZE_T size);
bool isGammaShader(const void* bytecode, SIZE_T size);

// CPU reference for the luminance curve embedded in the output shaders.
float toneMapLuminance(ToneMap toneMap, float luminance, float peakRelative);

// Diagnostics are buffered in memory and flushed by a worker. With both trace
// and hdr_debug disabled, log calls return before locking or starting it.
void log(const char* format, ...);
void shutdown();

}  // namespace hdr
}  // namespace tq
