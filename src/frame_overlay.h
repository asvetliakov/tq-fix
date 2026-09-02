#pragma once

#include <windows.h>
#include <d3d11.h>

namespace tq {
namespace frameoverlay {

// A frame-pacing overlay for A/B tests: the running frame time and FPS, a
// rolling average, the 99th percentile, the worst frame, a hitch count, and a
// graph of the sampled history.
//
// It is off unless [debug] frame_overlay is 1 in tqflicker.ini (the key's old
// home under [performance] is still honoured). While it is off this module
// allocates nothing and draws nothing -- enabled() is the only entry point the
// render path reaches -- though recordFrame still samples the frame boundary
// whenever the probe needs it, since the two share one clock. Measuring
// without the panel is not a mode here any more: that is simply
// [debug] performance_trace with the overlay off, and it is how a measurement
// run avoids the panel's own pipeline save/restore and uploads landing inside
// the very frame times being recorded.

// The original, unhooked device entry points. Nothing the overlay creates is
// something the game asked for, so its resources must not travel through
// visual.cpp's classification hooks: a sampler here would be upgraded to
// anisotropic, a pixel shader would be checked against the FXAA and gamma
// shapes, and a texture would be offered to the progressive uploader.
struct DeviceCalls {
    HRESULT (WINAPI* createTexture2D)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                      const D3D11_SUBRESOURCE_DATA*,
                                      ID3D11Texture2D**);
    HRESULT (WINAPI* createShaderResourceView)(ID3D11Device*, ID3D11Resource*,
                                               const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                               ID3D11ShaderResourceView**);
    HRESULT (WINAPI* createSamplerState)(ID3D11Device*, const D3D11_SAMPLER_DESC*,
                                         ID3D11SamplerState**);
    HRESULT (WINAPI* createPixelShader)(ID3D11Device*, const void*, SIZE_T,
                                        ID3D11ClassLinkage*, ID3D11PixelShader**);
    bool (*compile)(const char* source, const char* target, ID3DBlob** result);
};

// Reads [performance] frame_overlay from the INI beside the executable. Called
// once from the visual install, before anything else here.
void readOptions(const wchar_t* iniPath);
bool enabled();

// Which streaming path the install settled on, which is what the overlay
// exists to compare. Names the mode in the panel and colours its border.
void setStreamingMode(bool optimized);

// Compiles the two overlay shaders and creates its fixed resources. Called on
// the shader-build worker, so the caller must publish the result before the
// render thread reaches draw(); visual.cpp does that through g_programState.
bool createResources(ID3D11Device* device, const DeviceCalls& calls);

// One sample per presented frame, taken from the game's Present wrapper. It is
// also the frame boundary the probe records against, so it runs whenever either
// of the two is on.
void recordFrame();

// Draws over `target` at its own resolution. The caller owns saving and
// restoring the pipeline state and owns suppressing its own draw hooks, the
// same way it does for SMAA and the display mapper.
void draw(ID3D11Device* device, ID3D11DeviceContext* context,
          ID3D11RenderTargetView* target, UINT fallbackWidth);

void releaseResources();

// Drops the sampled history. Separate from releaseResources() because an
// abnormal unload deliberately leaks graphics objects rather than freeing them
// beneath a worker the driver has not returned.
void reset();

}  // namespace frameoverlay
}  // namespace tq
