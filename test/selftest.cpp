#include <windows.h>
#include <d3d11.h>

#include <stdio.h>
#include <stdlib.h>

#include "dxbc_patch.h"
#include "frustum_fix.h"
#include "hdr.h"
#include "streaming.h"
#include "visual.h"

namespace {

FILE* g_report;
int   g_failures;

void check(bool passed, const char* description) {
    fprintf(g_report, "%s  %s\n", passed ? "ok  " : "FAIL", description);
    if (!passed) ++g_failures;
}

void* readFile(const char* path, long* size) {
    *size = 0;
    FILE* file = fopen(path, "rb");
    if (!file) return nullptr;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    void* bytes = length > 0 ? malloc((size_t)length) : nullptr;
    if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        bytes = nullptr;
    } else {
        *size = length;
    }
    fclose(file);
    return bytes;
}

}  // namespace

int main(int argc, char** argv) {
    const char* dll = argc > 1 ? argv[1] : "winmm.dll";
    const char* report = argc > 2 ? argv[2] : "C:\\tqflicker-selftest.txt";
    g_report = fopen(report, "w");
    if (!g_report) return 99;

    check(tq::streaming::optimizationEnabled(nullptr),
          "streaming optimization defaults on when the setting is absent");
    check(tq::streaming::optimizationEnabled(L"optimized"),
          "streaming=optimized enables progressive uploads");
    check(!tq::streaming::optimizationEnabled(L"original"),
          "streaming=original restores synchronous uploads");

    tq::hdr::Settings defaultHdr = tq::hdr::readSettings();
    check(defaultHdr.requestHdr && defaultHdr.toneMap == tq::hdr::ToneFrostbite
          && defaultHdr.paperWhiteNits == 203.0f
          && defaultHdr.peakNitsOverride == 0.0f && !defaultHdr.debug,
          "HDR defaults to auto/Frostbite/203 nits with diagnostics disabled");

    const tq::hdr::ToneMap outputModes[] = {
        tq::hdr::ToneAgx, tq::hdr::ToneFrostbite
    };
    bool sdrCurvesValid = true;
    bool hdrCurvesValid = true;
    for (unsigned mode = 0; mode < sizeof(outputModes) / sizeof(outputModes[0]); ++mode) {
        float previousSdr = -1.0f;
        float previousHdr = -1.0f;
        for (unsigned i = 0; i <= 1024; ++i) {
            float input = i * (32.0f / 1024.0f);
            float sdr = tq::hdr::toneMapLuminance(outputModes[mode], input, 1.0f);
            float hdr = tq::hdr::toneMapLuminance(outputModes[mode], input, 4.926108f);
            sdrCurvesValid &= sdr == sdr && sdr >= 0.0f && sdr <= 1.00001f
                           && sdr + 0.00001f >= previousSdr;
            hdrCurvesValid &= hdr == hdr && hdr >= 0.0f && hdr <= 4.92612f
                           && hdr + 0.00001f >= previousHdr;
            previousSdr = sdr;
            previousHdr = hdr;
        }
    }
    float agxWhite = tq::hdr::toneMapLuminance(tq::hdr::ToneAgx, 1.0f, 1.0f);
    float agxHighlight = tq::hdr::toneMapLuminance(tq::hdr::ToneAgx, 4.0f, 1.0f);
    float frostbiteMid = tq::hdr::toneMapLuminance(
        tq::hdr::ToneFrostbite, 0.5f, 1.0f);
    float frostbiteWhite = tq::hdr::toneMapLuminance(
        tq::hdr::ToneFrostbite, 1.0f, 1.0f);
    float frostbiteHighlight = tq::hdr::toneMapLuminance(
        tq::hdr::ToneFrostbite, 4.0f, 1.0f);
    check(sdrCurvesValid && agxWhite > 0.4f && agxWhite < 0.9f
          && frostbiteMid == 0.5f
          && frostbiteWhite > 0.90f && frostbiteWhite < 0.92f
          && agxHighlight > agxWhite && agxHighlight < 1.0f
          && frostbiteHighlight > frostbiteWhite && frostbiteHighlight < 1.0f,
          "all output curves monotonically roll extended highlights into SDR");
    float agxHdr = tq::hdr::toneMapLuminance(tq::hdr::ToneAgx, 4.0f, 4.926108f);
    float frostbiteHdrWhite = tq::hdr::toneMapLuminance(
        tq::hdr::ToneFrostbite, 1.0f, 4.926108f);
    float frostbiteHdr = tq::hdr::toneMapLuminance(
        tq::hdr::ToneFrostbite, 4.0f, 4.926108f);
    check(hdrCurvesValid && agxHdr > 1.0f && agxHdr < 4.926108f
          && frostbiteHdrWhite == 1.0f
          && frostbiteHdr > 3.9f && frostbiteHdr < 4.0f,
          "all output curves preserve extended luminance for HDR output");
    check(frostbiteWhite != agxWhite && frostbiteHighlight != agxHighlight,
          "AgX and Frostbite select different curves");

    unsigned char colorGrade[1288] = {};
    const unsigned char colorChecksum[16] = {
        0x15,0x07,0x85,0xe4,0xfb,0xb5,0xca,0x43,
        0x79,0xfc,0x92,0xf9,0x64,0x2c,0x0c,0x9b
    };
    memcpy(colorGrade, "DXBC", 4);
    memcpy(colorGrade + 4, colorChecksum, sizeof(colorChecksum));
    *(uint32_t*)(colorGrade + 24) = sizeof(colorGrade);
    memcpy(colorGrade + 64, "SceneColor", 11);
    memcpy(colorGrade + 96, "ColorLut", 9);
    check(tq::hdr::isColorGradingShader(colorGrade, sizeof(colorGrade)),
          "recognize the exact Titan Quest color-grading shader signature");
    colorGrade[4] ^= 1;
    check(!tq::hdr::isColorGradingShader(colorGrade, sizeof(colorGrade)),
          "reject a near-match color-grading shader signature");

    unsigned char gamma[1108] = {};
    const unsigned char gammaChecksum[16] = {
        0xa2,0x0f,0xf7,0xb0,0xe5,0x78,0x2f,0x87,
        0x20,0x5c,0x22,0x36,0xb1,0xf7,0xe2,0x05
    };
    memcpy(gamma, "DXBC", 4);
    memcpy(gamma + 4, gammaChecksum, sizeof(gammaChecksum));
    *(uint32_t*)(gamma + 24) = sizeof(gamma);
    memcpy(gamma + 64, "screenSampler", 14);
    memcpy(gamma + 96, "gammaSampler", 13);
    check(tq::hdr::isGammaShader(gamma, sizeof(gamma)),
          "recognize the exact Titan Quest gamma shader signature");
    gamma[24] ^= 1;
    check(!tq::hdr::isGammaShader(gamma, sizeof(gamma)),
          "reject a malformed gamma shader container");

    const uintptr_t viewportSlot = 0x12345678u;
    const uintptr_t frustumSlot = 0x23456789u;
    BYTE updateSignature[] = {
        0x68, 0x00, 0x03, 0x00, 0x00,
        0x68, 0x00, 0x04, 0x00, 0x00,
        0x6a, 0x00, 0x6a, 0x00,
        0x8d, 0x4c, 0x24, 0x18, 0xff, 0x15,
        0, 0, 0, 0,
        0x8d, 0x44, 0x24, 0x08, 0x50,
        0x8d, 0x84, 0x24, 0x5c, 0x06, 0x00, 0x00, 0x50,
        0x8d, 0x4c, 0x24, 0x20, 0xff, 0x15,
        0, 0, 0, 0,
        0xb9, 0x02, 0x01, 0x00, 0x00, 0x8b, 0xf0, 0xf3, 0xa5
    };
    memcpy(updateSignature + 20, &viewportSlot, sizeof(uint32_t));
    memcpy(updateSignature + 43, &frustumSlot, sizeof(uint32_t));
    BYTE signatureBuffer[160] = {};
    memcpy(signatureBuffer + 16, updateSignature, sizeof(updateSignature));
    unsigned matches = 0;
    const BYTE* callSite = tq::frustum::findUpdateViewportCall(
        signatureBuffer, sizeof(signatureBuffer), viewportSlot, frustumSlot, &matches);
    check(matches == 1 && callSite == signatureBuffer + 16 + 24,
          "find the unique fixed 4:3 entity-update frustum");
    signatureBuffer[16 + 55] ^= 1;
    callSite = tq::frustum::findUpdateViewportCall(
        signatureBuffer, sizeof(signatureBuffer), viewportSlot, frustumSlot, &matches);
    check(!callSite && matches == 0, "reject a near-match update-frustum signature");
    signatureBuffer[16 + 55] ^= 1;
    memcpy(signatureBuffer + 88, updateSignature, sizeof(updateSignature));
    callSite = tq::frustum::findUpdateViewportCall(
        signatureBuffer, sizeof(signatureBuffer), viewportSlot, frustumSlot, &matches);
    check(!callSite && matches == 2, "reject ambiguous update-frustum signatures");

    int selectedWidth = 0, selectedHeight = 0;
    bool expanded169 = tq::frustum::selectViewportSize(
        true, true, 1024, 768, 1920, 1080, &selectedWidth, &selectedHeight);
    check(expanded169 && selectedWidth == 1920 && selectedHeight == 1080,
          "expand entity updates to a 16:9 viewport");
    bool expanded219 = tq::frustum::selectViewportSize(
        true, true, 1024, 768, 3440, 1440, &selectedWidth, &selectedHeight);
    check(expanded219 && selectedWidth == 3440 && selectedHeight == 1440,
          "expand entity updates to a 21:9 viewport");
    bool expanded329 = tq::frustum::selectViewportSize(
        true, true, 1024, 768, 5120, 1440, &selectedWidth, &selectedHeight);
    check(expanded329 && selectedWidth == 5120 && selectedHeight == 1440,
          "replace the centered 4:3 update aspect with the full 32:9 aspect");
    bool expanded43 = tq::frustum::selectViewportSize(
        true, true, 1024, 768, 1600, 1200, &selectedWidth, &selectedHeight);
    check(!expanded43 && selectedWidth == 1024 && selectedHeight == 768,
          "retain the original update frustum at 4:3");
    bool wrongCaller = tq::frustum::selectViewportSize(
        true, false, 1024, 768, 3440, 1440, &selectedWidth, &selectedHeight);
    check(!wrongCaller && selectedWidth == 1024 && selectedHeight == 768,
          "leave identical viewport construction from other callers untouched");
    bool disabled = tq::frustum::selectViewportSize(
        false, true, 1024, 768, 3440, 1440, &selectedWidth, &selectedHeight);
    check(!disabled && selectedWidth == 1024 && selectedHeight == 768,
          "restore the original frustum when edge updates are disabled");
    bool invalid = tq::frustum::selectViewportSize(
        true, true, 1024, 768, 20000, 1440, &selectedWidth, &selectedHeight);
    check(!invalid && selectedWidth == 1024 && selectedHeight == 768,
          "reject invalid live display dimensions");

    check(tq::visual::isFp16SceneTargetOrdinal(5)
          && tq::visual::isFp16SceneTargetOrdinal(7)
          && tq::visual::isFp16SceneTargetOrdinal(9)
          && tq::visual::isFp16SceneTargetOrdinal(11)
          && tq::visual::isFp16SceneTargetOrdinal(12)
          && tq::visual::isFp16SceneTargetOrdinal(13)
          && !tq::visual::isFp16SceneTargetOrdinal(4)
          && !tq::visual::isFp16SceneTargetOrdinal(6)
          && !tq::visual::isFp16SceneTargetOrdinal(10)
          && !tq::visual::isFp16SceneTargetOrdinal(14),
          "keep every confirmed scene/post target, including the alternate gamma snapshot, in FP16");

    HMODULE proxy = LoadLibraryA(dll);
    check(proxy != nullptr, "load the winmm proxy");
    if (proxy) {
        static const char* const names[] = {
#define TQ_WINMM_NAME(name, required) name,
#include "winmm_names.inc"
#undef TQ_WINMM_NAME
        };
        bool complete = true;
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
            if (!GetProcAddress(proxy, names[i])) complete = false;
        check(complete, "all winmm exports are present");

        FARPROC address = GetProcAddress(proxy, "timeGetTime");
        typedef DWORD(WINAPI* TimeGetTimeFn)();
        DWORD before = address ? ((TimeGetTimeFn)address)() : 0;
        Sleep(30);
        DWORD after = address ? ((TimeGetTimeFn)address)() : 0;
        check(address && before && after >= before, "timeGetTime forwards to the real winmm");
    }

    HMODULE host = LoadLibraryA("C:\\tqflicker-selftest\\Direct3D11.dll");
    check(host != nullptr, "load the production-path Direct3D11 host");
    typedef HRESULT (*MakeDeviceFn)(ID3D11Device**, ID3D11DeviceContext**);
    MakeDeviceFn makeDevice = host
        ? (MakeDeviceFn)(void*)GetProcAddress(host, "make_device") : nullptr;
    check(makeDevice != nullptr, "find the host's device-creation entry point");

    // The production DLL polls every 10 ms because the game's renderer is
    // loaded after winmm. Give the same path ample time in this off-game test.
    Sleep(250);
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT result = makeDevice ? makeDevice(&device, &context) : E_FAIL;
    check(SUCCEEDED(result) && device && context,
          "create a 32-bit DXMT D3D11 device through the hooked host");
    if (device && proxy) {
        void* createVertexShader = (*(void***)device)[12];
        void* createTexture2D = (*(void***)device)[5];
        void* createPixelShader = (*(void***)device)[15];
        void* createSamplerState = (*(void***)device)[23];
        MEMORY_BASIC_INFORMATION info = {};
        bool queried = VirtualQuery(createVertexShader, &info, sizeof(info)) != 0;
        check(queried && info.AllocationBase == proxy,
              "CreateVertexShader is redirected into the minimal proxy");
        queried = VirtualQuery(createTexture2D, &info, sizeof(info)) != 0;
        check(queried && info.AllocationBase == proxy,
              "CreateTexture2D is redirected into the visual proxy");
        queried = VirtualQuery(createPixelShader, &info, sizeof(info)) != 0;
        check(queried && info.AllocationBase == proxy,
              "CreatePixelShader is redirected into the visual proxy");
        queried = VirtualQuery(createSamplerState, &info, sizeof(info)) != 0;
        check(queried && info.AllocationBase == proxy,
              "CreateSamplerState is redirected into the visual proxy");
        void* draw = (*(void***)context)[13];
        queried = VirtualQuery(draw, &info, sizeof(info)) != 0;
        check(queried && info.AllocationBase == proxy,
              "Draw is redirected into the visual proxy");
    }

    if (device) {
        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW
                             = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11SamplerState* sampler = nullptr;
        D3D11_SAMPLER_DESC observedSampler = {};
        HRESULT samplerResult = device->CreateSamplerState(&samplerDesc, &sampler);
        if (sampler) sampler->GetDesc(&observedSampler);
        check(SUCCEEDED(samplerResult) && sampler
              && observedSampler.Filter == D3D11_FILTER_ANISOTROPIC
              && observedSampler.MaxAnisotropy == 16,
              "trilinear wrap sampling is upgraded to 16x anisotropy");
        if (sampler) sampler->Release();

        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler = nullptr;
        memset(&observedSampler, 0, sizeof(observedSampler));
        samplerResult = device->CreateSamplerState(&samplerDesc, &sampler);
        if (sampler) sampler->GetDesc(&observedSampler);
        check(SUCCEEDED(samplerResult) && sampler
              && observedSampler.Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR,
              "clamped post-process sampling retains its original filter");
        if (sampler) sampler->Release();

        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler = nullptr;
        memset(&observedSampler, 0, sizeof(observedSampler));
        samplerResult = device->CreateSamplerState(&samplerDesc, &sampler);
        if (sampler) sampler->GetDesc(&observedSampler);
        check(SUCCEEDED(samplerResult) && sampler
              && observedSampler.Filter == D3D11_FILTER_MIN_MAG_MIP_POINT,
              "point sampling retains its original filter");
        if (sampler) sampler->Release();

        D3D11_TEXTURE2D_DESC shadow = {};
        shadow.Width = shadow.Height = 512;
        shadow.MipLevels = shadow.ArraySize = 1;
        shadow.Format = DXGI_FORMAT_R32_TYPELESS;
        shadow.SampleDesc.Count = 1;
        shadow.Usage = D3D11_USAGE_DEFAULT;
        shadow.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* texture = nullptr;
        HRESULT textureResult = E_FAIL;
        D3D11_TEXTURE2D_DESC actual = {};
        const UINT shadowSizes[] = {512, 1024, 2048};
        bool allShadowSizes = true;
        for (UINT i = 0; i < sizeof(shadowSizes) / sizeof(shadowSizes[0]); ++i) {
            shadow.Width = shadow.Height = shadowSizes[i];
            texture = nullptr;
            textureResult = device->CreateTexture2D(&shadow, nullptr, &texture);
            memset(&actual, 0, sizeof(actual));
            if (texture) texture->GetDesc(&actual);
            allShadowSizes &= SUCCEEDED(textureResult) && texture
                           && actual.Width == shadowSizes[i] * 2
                           && actual.Height == shadowSizes[i] * 2;
            if (texture) texture->Release();
        }
        check(allShadowSizes, "enhanced shadows double Low/Medium/High map dimensions");

        shadow.Width = shadow.Height = 512;
        shadow.Format = DXGI_FORMAT_R24G8_TYPELESS;
        texture = nullptr;
        textureResult = device->CreateTexture2D(&shadow, nullptr, &texture);
        if (texture) texture->GetDesc(&actual);
        check(SUCCEEDED(textureResult) && texture && actual.Width == 512 && actual.Height == 512,
              "other square depth/SRV targets retain their requested dimensions");
        if (texture) texture->Release();

        shadow.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        shadow.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texture = nullptr;
        textureResult = device->CreateTexture2D(&shadow, nullptr, &texture);
        if (texture) texture->GetDesc(&actual);
        check(SUCCEEDED(textureResult) && texture && actual.Width == 512 && actual.Height == 512,
              "non-shadow square targets retain their requested dimensions");
        if (texture) texture->Release();

        // A water-reflection pass has both a color target and a square depth
        // target. Even if that depth texture resembles a shadow map, its
        // viewport must remain at the reflection target's dimensions.
        shadow.Format = DXGI_FORMAT_R32_TYPELESS;
        shadow.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* passDepth = nullptr;
        ID3D11DepthStencilView* passDSV = nullptr;
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        bool passTargets = SUCCEEDED(device->CreateTexture2D(&shadow, nullptr, &passDepth));
        if (passTargets) passTargets = SUCCEEDED(device->CreateDepthStencilView(
            passDepth, &dsvDesc, &passDSV));
        D3D11_TEXTURE2D_DESC passColorDesc = {};
        passColorDesc.Width = passColorDesc.Height = 512;
        passColorDesc.MipLevels = passColorDesc.ArraySize = 1;
        passColorDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        passColorDesc.SampleDesc.Count = 1;
        passColorDesc.Usage = D3D11_USAGE_DEFAULT;
        passColorDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D* passColor = nullptr;
        ID3D11RenderTargetView* passRTV = nullptr;
        if (passTargets) passTargets = SUCCEEDED(device->CreateTexture2D(
            &passColorDesc, nullptr, &passColor));
        if (passTargets) passTargets = SUCCEEDED(device->CreateRenderTargetView(
            passColor, nullptr, &passRTV));
        D3D11_VIEWPORT passViewport = {0, 0, 512, 512, 0, 1};
        if (passTargets && context) {
            context->RSSetViewports(1, &passViewport);
            context->OMSetRenderTargets(1, &passRTV, passDSV);
            UINT viewportCount = 1;
            D3D11_VIEWPORT observed = {};
            context->RSGetViewports(&viewportCount, &observed);
            check(viewportCount == 1 && observed.Width == 512 && observed.Height == 512,
                  "reflection color/depth passes keep their original viewport");
            context->OMSetRenderTargets(0, nullptr, passDSV);
            viewportCount = 1;
            context->RSGetViewports(&viewportCount, &observed);
            check(viewportCount == 1 && observed.Width == 1024 && observed.Height == 1024,
                  "depth-only shadow passes receive the scaled viewport");
            context->OMSetRenderTargets(0, nullptr, nullptr);
        } else {
            check(false, "create reflection/shadow viewport test targets");
            check(false, "run the depth-only shadow viewport test");
        }
        if (passRTV) passRTV->Release();
        if (passColor) passColor->Release();
        if (passDSV) passDSV->Release();
        if (passDepth) passDepth->Release();

        long fxaaSize = 0;
        void* fxaaBytes = readFile("C:\\tqflicker-selftest\\tq-dxbc-PS-fxaa.dxbc", &fxaaSize);
        ID3D11PixelShader* fxaa = nullptr;
        HRESULT fxaaResult = fxaaBytes ? device->CreatePixelShader(
            fxaaBytes, (SIZE_T)fxaaSize, nullptr, &fxaa) : E_FAIL;
        check(SUCCEEDED(fxaaResult) && fxaa,
              "the captured Titan Quest FXAA shader is accepted through the visual hook");
        if (fxaa && context) {
            Sleep(1000);
            context->PSSetShader(fxaa, nullptr, 0);
            ID3D11PixelShader* rebound = nullptr;
            context->PSGetShader(&rebound, nullptr, nullptr);
            check(rebound == fxaa, "the FXAA marker shader remains bindable before draw replacement");
            if (rebound) rebound->Release();

            UINT pixels[64];
            for (UINT i = 0; i < 64; ++i) pixels[i] = ((i + i / 8) & 1) ? 0xffffffffu : 0xff000000u;
            D3D11_TEXTURE2D_DESC color = {};
            color.Width = color.Height = 8; color.MipLevels = color.ArraySize = 1;
            color.Format = DXGI_FORMAT_R8G8B8A8_UNORM; color.SampleDesc.Count = 1;
            color.Usage = D3D11_USAGE_DEFAULT; color.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init = {pixels, 8 * sizeof(UINT), 0};
            ID3D11Texture2D *input = nullptr, *output = nullptr;
            ID3D11ShaderResourceView* inputView = nullptr;
            ID3D11RenderTargetView* outputView = nullptr;
            ID3D11VertexShader* fullscreenVS = nullptr;
            ID3D11InputLayout* fullscreenLayout = nullptr;
            ID3D11Buffer* fullscreenVB = nullptr;
            long vsSize = 0;
            void* vsBytes = readFile("C:\\tqflicker-selftest\\tq-dxbc-VS-fxaa.dxbc", &vsSize);
            bool rendered = SUCCEEDED(device->CreateTexture2D(&color, &init, &input));
            if (rendered) rendered = SUCCEEDED(device->CreateShaderResourceView(input, nullptr, &inputView));
            color.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (rendered) rendered = SUCCEEDED(device->CreateTexture2D(&color, nullptr, &output));
            if (rendered) rendered = SUCCEEDED(device->CreateRenderTargetView(output, nullptr, &outputView));
            if (rendered) rendered = vsBytes && SUCCEEDED(device->CreateVertexShader(
                vsBytes, (SIZE_T)vsSize, nullptr, &fullscreenVS));
            D3D11_INPUT_ELEMENT_DESC elements[2] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                 D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12,
                 D3D11_INPUT_PER_VERTEX_DATA, 0}
            };
            if (rendered) rendered = SUCCEEDED(device->CreateInputLayout(
                elements, 2, vsBytes, (SIZE_T)vsSize, &fullscreenLayout));
            struct Vertex { float x, y, z, u, v; } vertices[3] = {
                {-1, -1, 0, 0, 1}, {-1, 3, 0, 0, -1}, {3, -1, 0, 2, 1}
            };
            D3D11_BUFFER_DESC vbDesc = {};
            vbDesc.ByteWidth = sizeof(vertices); vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
            vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA vbData = {vertices, 0, 0};
            if (rendered) rendered = SUCCEEDED(device->CreateBuffer(&vbDesc, &vbData, &fullscreenVB));
            if (rendered) {
                FLOAT magenta[4] = {1, 0, 1, 1};
                D3D11_VIEWPORT vp = {0, 0, 8, 8, 0, 1};
                context->ClearRenderTargetView(outputView, magenta);
                context->OMSetRenderTargets(1, &outputView, nullptr);
                context->RSSetViewports(1, &vp);
                context->PSSetShaderResources(0, 1, &inputView);
                UINT stride = sizeof(Vertex), offset = 0;
                context->IASetInputLayout(fullscreenLayout);
                context->IASetVertexBuffers(0, 1, &fullscreenVB, &stride, &offset);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->VSSetShader(fullscreenVS, nullptr, 0);
                context->PSSetShader(fxaa, nullptr, 0);
                context->Draw(3, 0);
                ID3D11PixelShader* restoredPS = nullptr;
                ID3D11ShaderResourceView* restoredSRV = nullptr;
                ID3D11RenderTargetView* restoredRTV = nullptr;
                UINT restoredViewportCount = 1;
                D3D11_VIEWPORT restoredViewport = {};
                context->PSGetShader(&restoredPS, nullptr, nullptr);
                context->PSGetShaderResources(0, 1, &restoredSRV);
                context->OMGetRenderTargets(1, &restoredRTV, nullptr);
                context->RSGetViewports(&restoredViewportCount, &restoredViewport);
                check(restoredPS == fxaa && restoredSRV == inputView && restoredRTV == outputView
                      && restoredViewportCount == 1 && restoredViewport.Width == 8,
                      "the AA replacement restores the game's pipeline state");
                if (restoredPS) restoredPS->Release();
                if (restoredSRV) restoredSRV->Release();
                if (restoredRTV) restoredRTV->Release();
                ID3D11ShaderResourceView* noView = nullptr;
                ID3D11RenderTargetView* noTarget = nullptr;
                context->PSSetShaderResources(0, 1, &noView);
                context->OMSetRenderTargets(1, &noTarget, nullptr);
                check(device->GetDeviceRemovedReason() == S_OK,
                      "the captured FXAA draw executes the three-pass AA pipeline");
            } else {
                check(false, "create the off-game AA render targets");
            }
            if (outputView) outputView->Release();
            if (inputView) inputView->Release();
            if (fullscreenVB) fullscreenVB->Release();
            if (fullscreenLayout) fullscreenLayout->Release();
            if (fullscreenVS) fullscreenVS->Release();
            if (output) output->Release();
            if (input) input->Release();
            free(vsBytes);
        }
        if (fxaa) fxaa->Release();

        long gradeSize = 0, gammaSize = 0;
        void* gradeBytes = readFile(
            "C:\\tqflicker-selftest\\tq-dxbc-PS-colorgrading.dxbc", &gradeSize);
        void* gammaBytes = readFile(
            "C:\\tqflicker-selftest\\tq-dxbc-PS-gamma.dxbc", &gammaSize);
        ID3D11PixelShader *gradeShader = nullptr, *gammaShader = nullptr;
        bool postShaders = gradeBytes && gammaBytes
            && SUCCEEDED(device->CreatePixelShader(gradeBytes, gradeSize, nullptr, &gradeShader))
            && SUCCEEDED(device->CreatePixelShader(gammaBytes, gammaSize, nullptr, &gammaShader));
        check(postShaders && gradeShader && gammaShader,
              "the exact color-grading and gamma shaders pass validation");
        if (postShaders && context) {
            Sleep(3000);
            context->PSSetShader(gradeShader, nullptr, 0);
            ID3D11PixelShader* reboundGrade = nullptr;
            context->PSGetShader(&reboundGrade, nullptr, nullptr);
            check(reboundGrade && reboundGrade != gradeShader,
                  "the HDR-safe shader replaces only the exact color-grading pass");
            if (reboundGrade) reboundGrade->Release();
            context->PSSetShader(gammaShader, nullptr, 0);
            ID3D11PixelShader* reboundGamma = nullptr;
            context->PSGetShader(&reboundGamma, nullptr, nullptr);
            check(reboundGamma && reboundGamma != gammaShader,
                  "the selected output transform replaces the exact gamma pass");
            if (reboundGamma) reboundGamma->Release();
        } else {
            check(false, "replace the exact color-grading pass");
            check(false, "replace the exact gamma pass");
        }
        if (gradeShader) gradeShader->Release();
        if (gammaShader) gammaShader->Release();
        free(gradeBytes); free(gammaBytes);

        tq::dxbc::PatchResult notShadow = {};
        check(!tq::dxbc::enhanceShadowPcf(fxaaBytes, (SIZE_T)fxaaSize, &notShadow),
              "the shadow transformer rejects the FXAA shader");
        tq::dxbc::release(&notShadow);
        free(fxaaBytes);

        long shadowSize = 0;
        void* shadowBytes = readFile("C:\\tqflicker-selftest\\tq-dxbc-PS-shadow.dxbc", &shadowSize);
        tq::dxbc::PatchResult nearShadow = {};
        bool rejectedNearShadow = false;
        if (shadowBytes && shadowSize > 0) {
            unsigned char* nearBytes = (unsigned char*)malloc((size_t)shadowSize);
            memcpy(nearBytes, shadowBytes, (size_t)shadowSize);
            const char marker[] = "shadowBluriness";
            for (long i = 0; nearBytes && i + (long)sizeof(marker) <= shadowSize; ++i) {
                if (!memcmp(nearBytes + i, marker, sizeof(marker) - 1)) {
                    nearBytes[i] ^= 1;
                    break;
                }
            }
            rejectedNearShadow = !tq::dxbc::enhanceShadowPcf(
                nearBytes, (SIZE_T)shadowSize, &nearShadow);
            free(nearBytes);
        }
        check(rejectedNearShadow, "the shadow transformer rejects a near-match shader");
        tq::dxbc::release(&nearShadow);
        tq::dxbc::PatchResult shadowPatch = {};
        bool shadowChanged = shadowBytes && tq::dxbc::enhanceShadowPcf(
            shadowBytes, (SIZE_T)shadowSize, &shadowPatch);
        check(shadowChanged && shadowPatch.size == (SIZE_T)shadowSize,
              "transform one captured Titan Quest shadow receiver shader");
        if (shadowChanged) {
            ID3D11PixelShader* receiver = nullptr;
            HRESULT receiverResult = device->CreatePixelShader(
                shadowPatch.data, shadowPatch.size, nullptr, &receiver);
            check(SUCCEEDED(receiverResult) && receiver,
                  "DXMT accepts the enhanced shadow receiver shader");
            if (receiver) receiver->Release();
        }
        tq::dxbc::release(&shadowPatch);
        free(shadowBytes);
    }

    int transformed = 0;
    if (device) {
        for (int i = 3; i < argc; ++i) {
            long size;
            void* original = readFile(argv[i], &size);
            tq::dxbc::PatchResult patch = {};
            bool changed = original && tq::dxbc::clampBoneIndices(original, (SIZE_T)size, &patch);
            check(changed && patch.size == (SIZE_T)size + 40,
                  "transform one captured Titan Quest skinning shader");
            if (changed) {
                ID3D11VertexShader* shader = nullptr;
                HRESULT shaderResult = device->CreateVertexShader(
                    patch.data, patch.size, nullptr, &shader);
                check(SUCCEEDED(shaderResult) && shader,
                      "DXMT accepts the transformed shader");
                if (shader) shader->Release();
                ++transformed;
            }
            tq::dxbc::release(&patch);
            free(original);
        }
    }
    if (argc > 3)
        check(transformed == argc - 3, "all captured shader variants were transformed");

    if (context) context->Release();
    if (device) device->Release();
    // The proxy restores its IAT and vtable slots on an explicit unload, so the
    // host must remain mapped until after this call.
    if (proxy) FreeLibrary(proxy);
    if (host) FreeLibrary(host);

    check(GetFileAttributesA("tqflicker-hdr.log") == INVALID_FILE_ATTRIBUTES,
          "HDR logging creates no file when hdr_debug is absent");
    fprintf(g_report, "\nRESULT: %d failure(s)\n", g_failures);
    fclose(g_report);
    return g_failures ? 1 : 0;
}
