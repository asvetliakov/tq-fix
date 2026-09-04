#include "frame_overlay.h"

#include "probe.h"

#include <algorithm>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace tq {
namespace frameoverlay {
namespace {

// 4096 frames is about 68 seconds at 60 FPS, which covers one A/B pass over the
// same stretch of world without the window rolling out from under it.
const unsigned kSampleCount = 4096;

const UINT kTextWidth = 660;   // 81 glyphs at 8 px plus the 12 px left margin
const UINT kTextHeight = 80;
const UINT kGraphHeight = 110;
const UINT kGraphMinWidth = 264;
const UINT kGraphMaxWidth = kSampleCount + 8;
const UINT kGraphMargin = 20;  // screen pixels either side of the graph
const int kGraphInset = 4;     // border and padding inside the graph texture

// The full plotted range. Above it a frame is clamped to the top of the graph
// and is already unmistakable from its colour.
const float kGraphMaximumMs = 66.7f;
const float kHitchMs = 25.0f;

struct FrameSample {
    LONGLONG ticks;
    float milliseconds;
};

struct Resources {
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11Texture2D* textTexture;
    ID3D11ShaderResourceView* textSrv;
    ID3D11Texture2D* graphTexture;
    ID3D11ShaderResourceView* graphSrv;
    ID3D11SamplerState* sampler;
    ID3D11BlendState* blend;
    ID3D11DepthStencilState* depth;
    ID3D11RasterizerState* raster;
    UINT graphWidth;
} g_res;

DeviceCalls g_calls;
bool g_enabled;
bool g_streamingOptimized = true;

uint32_t* g_textPixels;
uint32_t* g_graphPixels;
// What the graph texture currently holds, so only the columns that actually
// changed have to be uploaded.
uint32_t* g_graphUploaded;
FrameSample* g_samples;
float* g_sorted;
unsigned g_write;
unsigned g_count;
LONGLONG g_lastRefresh;
LARGE_INTEGER g_frequency;
LARGE_INTEGER g_lastFrame;

template <typename T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

template <typename T> void freeBuffer(T*& p) { free(p); p = nullptr; }

uint32_t rgba(unsigned r, unsigned g, unsigned b, unsigned a) {
    return (a << 24) | (b << 16) | (g << 8) | r;
}

void fillRect(uint32_t* pixels, UINT canvasWidth, UINT canvasHeight,
              int x, int y, int width, int height, uint32_t color) {
    if (!pixels) return;
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > (int)canvasWidth) width = (int)canvasWidth - x;
    if (y + height > (int)canvasHeight) height = (int)canvasHeight - y;
    if (width <= 0 || height <= 0) return;
    for (int row = 0; row < height; ++row)
        std::fill(pixels + (size_t)(y + row) * canvasWidth + x,
                  pixels + (size_t)(y + row) * canvasWidth + x + width,
                  color);
}

void drawBorder(uint32_t* pixels, UINT width, UINT height, uint32_t color) {
    fillRect(pixels, width, height, 0, 0, (int)width, 2, color);
    fillRect(pixels, width, height, 0, (int)height - 2, (int)width, 2, color);
    fillRect(pixels, width, height, 0, 0, 2, (int)height, color);
    fillRect(pixels, width, height, (int)width - 2, 0, 2, (int)height, color);
}

// A 3x5 bitmap font, one row of three pixels per nibble triple. Only the
// characters the overlay actually prints exist.
#define TQ_GLYPH(a, b, c, d, e) \
    ((uint16_t)((a) << 12 | (b) << 9 | (c) << 6 | (d) << 3 | (e)))

uint16_t glyphBits(char character) {
    static const uint16_t digits[10] = {
        TQ_GLYPH(7,5,5,5,7), TQ_GLYPH(2,6,2,2,7),
        TQ_GLYPH(7,1,7,4,7), TQ_GLYPH(7,1,7,1,7),
        TQ_GLYPH(5,5,7,1,1), TQ_GLYPH(7,4,7,1,7),
        TQ_GLYPH(7,4,7,5,7), TQ_GLYPH(7,1,2,2,2),
        TQ_GLYPH(7,5,7,5,7), TQ_GLYPH(7,5,7,1,7)
    };
    static const uint16_t letters[26] = {
        TQ_GLYPH(2,5,7,5,5), TQ_GLYPH(6,5,6,5,6),
        TQ_GLYPH(7,4,4,4,7), TQ_GLYPH(6,5,5,5,6),
        TQ_GLYPH(7,4,6,4,7), TQ_GLYPH(7,4,6,4,4),
        TQ_GLYPH(7,4,5,5,7), TQ_GLYPH(5,5,7,5,5),
        TQ_GLYPH(7,2,2,2,7), TQ_GLYPH(1,1,1,5,7),
        TQ_GLYPH(5,5,6,5,5), TQ_GLYPH(4,4,4,4,7),
        TQ_GLYPH(5,7,7,5,5), TQ_GLYPH(5,7,7,7,5),
        TQ_GLYPH(7,5,5,5,7), TQ_GLYPH(7,5,7,4,4),
        TQ_GLYPH(7,5,5,7,1), TQ_GLYPH(6,5,6,5,5),
        TQ_GLYPH(7,4,7,1,7), TQ_GLYPH(7,2,2,2,2),
        TQ_GLYPH(5,5,5,5,7), TQ_GLYPH(5,5,5,5,2),
        TQ_GLYPH(5,5,7,7,5), TQ_GLYPH(5,5,2,5,5),
        TQ_GLYPH(5,5,2,2,2), TQ_GLYPH(7,1,2,4,7)
    };
    if (character >= 'a' && character <= 'z') character = (char)(character - 32);
    if (character >= '0' && character <= '9') return digits[character - '0'];
    if (character >= 'A' && character <= 'Z') return letters[character - 'A'];
    switch (character) {
        case ':': return TQ_GLYPH(0,2,0,2,0);
        case '.': return TQ_GLYPH(0,0,0,0,2);
        case '>': return TQ_GLYPH(4,2,1,2,4);
        case '/': return TQ_GLYPH(1,1,2,4,4);
        case '-': return TQ_GLYPH(0,0,7,0,0);
        default: return 0;
    }
}

#undef TQ_GLYPH

void drawText(int x, int y, const char* text, uint32_t color) {
    const int scale = 2;
    if (!text) return;
    for (; *text; ++text, x += 4 * scale) {
        uint16_t bits = glyphBits(*text);
        for (int row = 0; row < 5; ++row) {
            unsigned bitRow = (bits >> ((4 - row) * 3)) & 7;
            for (int column = 0; column < 3; ++column)
                if (bitRow & (1u << (2 - column)))
                    fillRect(g_textPixels, kTextWidth, kTextHeight,
                             x + column * scale, y + row * scale,
                             scale, scale, color);
        }
    }
}

float sampleMilliseconds(unsigned index) {
    return g_samples[index % kSampleCount].milliseconds;
}

void renderTextPanel(uint32_t mode, unsigned visible, unsigned oldest) {
    const uint32_t panel = rgba(9, 14, 20, 188);
    const uint32_t white = rgba(195, 210, 222, 232);
    const uint32_t muted = rgba(128, 148, 165, 218);
    std::fill(g_textPixels, g_textPixels + (size_t)kTextWidth * kTextHeight, panel);
    drawBorder(g_textPixels, kTextWidth, kTextHeight, mode);

    double sum = 0.0;
    float maximum = 0.0f;
    unsigned hitches = 0;
    for (unsigned i = 0; i < visible; ++i) {
        float value = sampleMilliseconds(oldest + i);
        g_sorted[i] = value;
        sum += value;
        if (value > maximum) maximum = value;
        if (value > kHitchMs) ++hitches;
    }
    if (visible) std::sort(g_sorted, g_sorted + visible);
    float latest = visible ? sampleMilliseconds(g_write + kSampleCount - 1) : 0.0f;
    float average = visible ? (float)(sum / visible) : 0.0f;
    // Nearest rank, and always below `visible` for any non-empty window.
    float p99 = visible ? g_sorted[(size_t)(((uint64_t)visible * 99) / 100)] : 0.0f;
    float fps = latest > 0.01f ? 1000.0f / latest : 0.0f;
    float historySeconds = visible && g_frequency.QuadPart
        ? (float)(g_lastFrame.QuadPart - g_samples[oldest % kSampleCount].ticks)
          / (float)g_frequency.QuadPart : 0.0f;

    char line[96];
    snprintf(line, sizeof(line), "STREAMING: %s",
             g_streamingOptimized ? "OPTIMIZED" : "ORIGINAL");
    drawText(12, 9, line, mode);
    snprintf(line, sizeof(line), "FRAME: %.1f MS  FPS: %.1f", latest, fps);
    drawText(12, 23, line, white);
    snprintf(line, sizeof(line), "AVG: %.1f  P99: %.1f  MAX: %.1f",
             average, p99, maximum);
    drawText(12, 37, line, white);
    snprintf(line, sizeof(line), "HITCHES >%u MS: %u / %.1f S",
             (unsigned)kHitchMs, hitches, historySeconds);
    drawText(12, 51, line, muted);
    // Where the time went, when the probe is on to answer it.
    char attribution[80] = {};
    if (tq::probe::enabled())
        tq::probe::summarize(attribution, sizeof(attribution));
    if (attribution[0]) drawText(12, 65, attribution, muted);
}

void renderGraphPanel(uint32_t mode, unsigned visible, unsigned oldest) {
    const UINT width = g_res.graphWidth;
    const uint32_t graphPanel = rgba(4, 7, 11, 198);
    std::fill(g_graphPixels, g_graphPixels + (size_t)width * kGraphHeight, graphPanel);
    drawBorder(g_graphPixels, width, kGraphHeight, mode);

    const int graphLeft = kGraphInset;
    const int graphTop = kGraphInset;
    const int graphWidth = (int)width - 2 * kGraphInset;
    const int graphHeight = (int)kGraphHeight - 2 * kGraphInset;
    if (graphWidth <= 0 || graphHeight <= 1) return;

    const float thresholds[3] = {16.7f, 33.3f, 50.0f};
    const uint32_t gridColors[3] = {
        rgba(38, 83, 61, 175), rgba(101, 88, 37, 175), rgba(105, 47, 45, 175)
    };
    const uint32_t divisionGrid = rgba(54, 65, 76, 105);
    for (int division = 1; division < 8; ++division)
        fillRect(g_graphPixels, width, kGraphHeight,
                 graphLeft + division * graphWidth / 8, graphTop,
                 1, graphHeight, divisionGrid);
    for (unsigned i = 0; i < 3; ++i)
        fillRect(g_graphPixels, width, kGraphHeight, graphLeft,
                 graphTop + graphHeight - 1
                 - (int)(thresholds[i] / kGraphMaximumMs * (graphHeight - 1)),
                 graphWidth, 1, gridColors[i]);

    // One column per screen pixel, filled from the right so a history shorter
    // than the graph stays anchored to now instead of stretching. Once there
    // are more frames than columns a column reports the worst frame it covers:
    // averaging or dropping would hide exactly the spikes being looked for.
    unsigned columns = visible < (unsigned)graphWidth ? visible : (unsigned)graphWidth;
    int columnOffset = graphWidth - (int)columns;
    int previousY = graphTop + graphHeight - 1;
    for (unsigned column = 0; column < columns; ++column) {
        unsigned begin = (unsigned)(((uint64_t)column * visible) / columns);
        unsigned end = (unsigned)(((uint64_t)(column + 1) * visible) / columns);
        if (end <= begin) end = begin + 1;
        if (end > visible) end = visible;
        float worst = 0.0f;
        for (unsigned i = begin; i < end; ++i) {
            float value = sampleMilliseconds(oldest + i);
            if (value > worst) worst = value;
        }
        float clipped = worst < kGraphMaximumMs ? worst : kGraphMaximumMs;
        int y = graphTop + graphHeight - 1
              - (int)(clipped / kGraphMaximumMs * (graphHeight - 1));
        uint32_t color = worst > 33.3f ? rgba(235, 88, 78, 238)
                       : worst > 20.0f ? rgba(225, 187, 70, 232)
                                       : rgba(80, 190, 125, 225);
        int top = column ? (y < previousY ? y : previousY) : y;
        int height = column ? (y < previousY ? previousY - y + 1 : y - previousY + 1) : 2;
        fillRect(g_graphPixels, width, kGraphHeight,
                 graphLeft + columnOffset + (int)column, top, 1, height, color);
        previousY = y;
    }
}

void renderOverlayPixels() {
    const uint32_t mode = g_streamingOptimized ? rgba(55, 190, 218, 238)
                                               : rgba(232, 146, 58, 238);
    unsigned visible = g_count < kSampleCount ? g_count : kSampleCount;
    unsigned oldest = (g_write + kSampleCount - visible) % kSampleCount;
    renderTextPanel(mode, visible, oldest);
    renderGraphPanel(mode, visible, oldest);
}

const char* kOverlayVertexShader =
"struct Output{float4 position:SV_POSITION;float2 uv:TEXCOORD0;};"
"Output main(uint id:SV_VertexID){Output o;"
"float2 uv=float2((id<<1)&2,id&2);o.uv=uv;"
"o.position=float4(uv*float2(2,-2)+float2(-1,1),0,1);return o;}";

const char* kOverlayPixelShader =
"Texture2D image:register(t0);SamplerState pointSampler:register(s0);"
"float4 main(float4 position:SV_POSITION,float2 uv:TEXCOORD0):SV_TARGET{"
"return image.Sample(pointSampler,uv);}";

bool createPanelTexture(ID3D11Device* device, UINT width, UINT height,
                        ID3D11Texture2D** texture, ID3D11ShaderResourceView** srv) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(g_calls.createTexture2D(device, &desc, nullptr, texture))
        && SUCCEEDED(g_calls.createShaderResourceView(device, *texture, nullptr, srv));
}

// The graph is blitted 1:1, so its texture is as wide as the space it occupies
// on screen. Point sampling a wider texture down to the screen would drop the
// columns in between, which is where the hitches are.
bool ensureGraphTexture(ID3D11Device* device, UINT screenWidth) {
    UINT width = screenWidth > 2 * kGraphMargin ? screenWidth - 2 * kGraphMargin : 0;
    if (width < kGraphMinWidth) width = kGraphMinWidth;
    if (width > kGraphMaxWidth) width = kGraphMaxWidth;
    if (g_res.graphTexture && g_res.graphWidth == width) return true;
    release(g_res.graphSrv);
    release(g_res.graphTexture);
    freeBuffer(g_graphPixels);
    freeBuffer(g_graphUploaded);
    g_res.graphWidth = 0;
    g_graphPixels = (uint32_t*)calloc((size_t)width * kGraphHeight, sizeof(uint32_t));
    g_graphUploaded = (uint32_t*)calloc((size_t)width * kGraphHeight, sizeof(uint32_t));
    if (!g_graphPixels) return false;
    if (!createPanelTexture(device, width, kGraphHeight,
                            &g_res.graphTexture, &g_res.graphSrv)) {
        release(g_res.graphSrv);
        release(g_res.graphTexture);
        freeBuffer(g_graphPixels);
        freeBuffer(g_graphUploaded);
        return false;
    }
    g_res.graphWidth = width;
    return true;
}

// The graph is the expensive upload -- at a 3000 px display it is well over a
// megabyte through the immediate context, inside Present. Most refreshes change
// a narrow band of it, so compare against what was uploaded last time and send
// only the columns that moved.
void uploadGraphColumns(ID3D11DeviceContext* context) {
    const UINT width = g_res.graphWidth;
    if (!width || !g_graphPixels) return;
    const UINT pitch = width * (UINT)sizeof(uint32_t);
    // Once the sample window is full the whole waveform scrolls every refresh,
    // so the diff below would scan everything only to find everything changed;
    // upload it whole and skip both the compare and the mirror copy.
    if (g_count >= kSampleCount) {
        context->UpdateSubresource(g_res.graphTexture, 0, nullptr,
                                   g_graphPixels, pitch, 0);
        return;
    }
    if (!g_graphUploaded) {
        context->UpdateSubresource(g_res.graphTexture, 0, nullptr,
                                   g_graphPixels, pitch, 0);
        return;
    }
    UINT first = width, last = 0;
    for (UINT column = 0; column < width; ++column) {
        bool differs = false;
        for (UINT row = 0; row < kGraphHeight && !differs; ++row)
            differs = g_graphPixels[(size_t)row * width + column]
                   != g_graphUploaded[(size_t)row * width + column];
        if (!differs) continue;
        if (column < first) first = column;
        last = column;
    }
    if (first > last) return;   // nothing moved
    D3D11_BOX box = {first, 0, 0, last + 1, kGraphHeight, 1};
    context->UpdateSubresource(g_res.graphTexture, 0, &box,
                               g_graphPixels + first, pitch, 0);
    for (UINT row = 0; row < kGraphHeight; ++row)
        memcpy(g_graphUploaded + (size_t)row * width + first,
               g_graphPixels + (size_t)row * width + first,
               (size_t)(last - first + 1) * sizeof(uint32_t));
}

bool resourcesReady() {
    return g_res.vs && g_res.ps && g_res.textTexture && g_res.textSrv
        && g_res.graphTexture && g_res.graphSrv && g_res.sampler && g_res.blend
        && g_res.depth && g_res.raster && g_textPixels && g_graphPixels
        && g_samples && g_sorted;
}

UINT targetWidth(ID3D11RenderTargetView* target, UINT fallbackWidth) {
    ID3D11Resource* resource = nullptr;
    ID3D11Texture2D* texture = nullptr;
    target->GetResource(&resource);
    if (resource)
        resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture);
    UINT width = 0;
    if (texture) {
        D3D11_TEXTURE2D_DESC desc = {};
        texture->GetDesc(&desc);
        width = desc.Width;
    }
    release(texture);
    release(resource);
    if (!width) width = fallbackWidth;
    return width >= 200 ? width : 1920;
}

}  // namespace

void readOptions(const wchar_t* iniPath) {
    // The key lives under [debug] now; a value in its old [performance] home
    // is still honoured so an existing INI keeps working. -1 as the missing
    // marker: 0 is a real answer.
    int mode = iniPath
             ? GetPrivateProfileIntW(L"debug", L"frame_overlay", -1, iniPath)
             : 0;
    if (mode == -1)
        mode = GetPrivateProfileIntW(L"performance", L"frame_overlay", 0, iniPath);
    g_enabled = mode > 0;
    if (!g_enabled) return;
    // Measurement starts at install rather than when the shaders finish
    // building, so the first seconds of a session are not a hole in the graph.
    if (!g_samples)
        g_samples = (FrameSample*)calloc(kSampleCount, sizeof(FrameSample));
    if (!g_sorted) g_sorted = (float*)calloc(kSampleCount, sizeof(float));
    if (!g_samples || !g_sorted) {
        reset();
        g_enabled = false;
    }
}

bool enabled() { return g_enabled; }

void setStreamingMode(bool optimized) { g_streamingOptimized = optimized; }

bool createResources(ID3D11Device* device, const DeviceCalls& calls) {
    if (!g_enabled || !device || !calls.compile || !calls.createTexture2D
        || !calls.createShaderResourceView || !calls.createSamplerState
        || !calls.createPixelShader) return false;
    g_calls = calls;
    if (g_res.vs && g_res.ps && g_res.textTexture) return true;
    if (!g_textPixels)
        g_textPixels = (uint32_t*)calloc((size_t)kTextWidth * kTextHeight,
                                         sizeof(uint32_t));

    ID3DBlob *vertex = nullptr, *pixel = nullptr;
    bool ok = g_textPixels != nullptr
           && calls.compile(kOverlayVertexShader, "vs_5_0", &vertex)
           && calls.compile(kOverlayPixelShader, "ps_5_0", &pixel);
    if (ok) ok = SUCCEEDED(device->CreateVertexShader(
        vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &g_res.vs));
    if (ok) ok = SUCCEEDED(calls.createPixelShader(
        device, pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &g_res.ps));
    release(vertex);
    release(pixel);

    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = FALSE;
    depth.StencilEnable = FALSE;
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    raster.ScissorEnable = FALSE;
    if (ok) ok = createPanelTexture(device, kTextWidth, kTextHeight,
                                    &g_res.textTexture, &g_res.textSrv);
    if (ok) ok = SUCCEEDED(calls.createSamplerState(device, &sampler, &g_res.sampler));
    if (ok) ok = SUCCEEDED(device->CreateBlendState(&blend, &g_res.blend));
    if (ok) ok = SUCCEEDED(device->CreateDepthStencilState(&depth, &g_res.depth));
    if (ok) ok = SUCCEEDED(device->CreateRasterizerState(&raster, &g_res.raster));
    if (!ok) releaseResources();
    return ok;
}

void recordFrame() {
    // The frame boundary is one clock, shared. The probe is configured
    // independently of the overlay, so a run can attribute hitches without
    // paying for a panel; measuring here keeps them from drifting apart.
    const bool sampling = g_enabled && g_samples;
    if (!sampling && !tq::probe::enabled()) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_frequency.QuadPart) QueryPerformanceFrequency(&g_frequency);
    if (g_lastFrame.QuadPart && g_frequency.QuadPart) {
        double milliseconds = (double)(now.QuadPart - g_lastFrame.QuadPart) * 1000.0
                            / (double)g_frequency.QuadPart;
        // Outside this range the sample is not a frame: below it a counter
        // glitch, above it a suspend, a debugger, or minutes spent alt-tabbed
        // away -- which recorded as a clamped ten-second "frame" would own the
        // worst-frame line and write a hitch row no phase can explain. Load
        // hitches are orders of magnitude shorter and pass untouched.
        if (milliseconds > 0.01 && milliseconds < 10000.0) {
            if (sampling) {
                g_samples[g_write].ticks = now.QuadPart;
                g_samples[g_write].milliseconds = (float)milliseconds;
                g_write = (g_write + 1) % kSampleCount;
                if (g_count < kSampleCount) ++g_count;
            }
            tq::probe::endFrame((float)milliseconds);
        }
    }
    g_lastFrame = now;
}

void draw(ID3D11Device* device, ID3D11DeviceContext* context,
          ID3D11RenderTargetView* target, UINT fallbackWidth) {
    if (!g_enabled || !device || !context || !target) return;
    if (!g_res.vs || !g_res.ps || !g_res.textTexture) return;
    UINT width = targetWidth(target, fallbackWidth);
    if (!ensureGraphTexture(device, width) || !resourcesReady()) return;

    // Ten refreshes a second: fast enough to read the running numbers, slow
    // enough that rasterising two panels on the CPU stays off the frame budget.
    LONGLONG refreshTicks = g_frequency.QuadPart / 10;
    if (!g_lastRefresh || !refreshTicks
        || g_lastFrame.QuadPart - g_lastRefresh >= refreshTicks) {
        tq::probe::Scope timing(tq::probe::PhaseOverlayRaster);
        renderOverlayPixels();
        context->UpdateSubresource(g_res.textTexture, 0, nullptr, g_textPixels,
                                   kTextWidth * (UINT)sizeof(uint32_t), 0);
        uploadGraphColumns(context);
        g_lastRefresh = g_lastFrame.QuadPart;
    }

    D3D11_VIEWPORT textViewport = {(FLOAT)kGraphMargin, 20.0f, (FLOAT)kTextWidth,
                                   (FLOAT)kTextHeight, 0.0f, 1.0f};
    D3D11_VIEWPORT graphViewport = {(FLOAT)kGraphMargin, 96.0f,
                                    (FLOAT)g_res.graphWidth, (FLOAT)kGraphHeight,
                                    0.0f, 1.0f};
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g_res.vs, nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(g_res.ps, nullptr, 0);
    context->PSSetSamplers(0, 1, &g_res.sampler);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(g_res.blend, nullptr, 0xffffffff);
    context->OMSetDepthStencilState(g_res.depth, 0);
    context->RSSetState(g_res.raster);
    context->PSSetShaderResources(0, 1, &g_res.textSrv);
    context->RSSetViewports(1, &textViewport);
    context->Draw(3, 0);
    context->PSSetShaderResources(0, 1, &g_res.graphSrv);
    context->RSSetViewports(1, &graphViewport);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* nullView = nullptr;
    context->PSSetShaderResources(0, 1, &nullView);
}

void releaseResources() {
    release(g_res.vs); release(g_res.ps);
    release(g_res.textSrv); release(g_res.textTexture);
    release(g_res.graphSrv); release(g_res.graphTexture);
    release(g_res.sampler); release(g_res.blend);
    release(g_res.depth); release(g_res.raster);
    g_res.graphWidth = 0;
    freeBuffer(g_textPixels);
    freeBuffer(g_graphPixels);
    freeBuffer(g_graphUploaded);
}

void reset() {
    freeBuffer(g_samples);
    freeBuffer(g_sorted);
    g_write = g_count = 0;
    g_lastRefresh = 0;
    g_frequency.QuadPart = g_lastFrame.QuadPart = 0;
}

}  // namespace frameoverlay
}  // namespace tq
