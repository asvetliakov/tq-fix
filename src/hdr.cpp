#include "hdr.h"

#include <windows.h>

#include <stdarg.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace tq {
namespace hdr {
namespace {

Runtime g_runtime = {{true, ToneAgx, 203.0f, 0.0f, false},
                     false, false, false, 1000.0f};
bool g_settingsRead;
char g_log[64 * 1024];
volatile LONG g_logBytes;
SRWLOCK g_logLock = SRWLOCK_INIT;
volatile LONG g_loggerState;
HANDLE g_logEvent;
HANDLE g_logStop;
HANDLE g_logThread;

bool contains(const BYTE* bytes, SIZE_T size, const char* text) {
    SIZE_T count = strlen(text);
    if (!bytes || !count || count > size) return false;
    for (SIZE_T i = 0; i <= size - count; ++i)
        if (!memcmp(bytes + i, text, count)) return true;
    return false;
}

bool exactShader(const void* bytecode, SIZE_T size, SIZE_T expectedSize,
                 const BYTE checksum[16], const char* first, const char* second) {
    if (!bytecode || size != expectedSize || size < 32
        || memcmp(bytecode, "DXBC", 4)
        || memcmp((const BYTE*)bytecode + 4, checksum, 16)
        || *(const uint32_t*)((const BYTE*)bytecode + 24) != size) return false;
    return contains((const BYTE*)bytecode, size, first)
        && contains((const BYTE*)bytecode, size, second);
}

void iniPath(wchar_t path[MAX_PATH]) {
    path[0] = 0;
    DWORD count = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!count || count >= MAX_PATH) { path[0] = 0; return; }
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) { path[0] = 0; return; }
    lstrcpyW(slash + 1, L"tqflicker.ini");
}

float readFloat(const wchar_t* path, const wchar_t* key, float fallback) {
    wchar_t value[32];
    GetPrivateProfileStringW(L"graphics", key, L"", value, 32, path);
    if (!value[0]) return fallback;
    wchar_t* end = nullptr;
    double parsed = wcstod(value, &end);
    return end != value && *end == 0 ? (float)parsed : fallback;
}

bool outputDescription(IDXGIOutput* output, DXGI_OUTPUT_DESC1* result) {
    if (!output || !result) return false;
    IDXGIOutput6* output6 = nullptr;
    HRESULT hr = output->QueryInterface(__uuidof(IDXGIOutput6), (void**)&output6);
    if (FAILED(hr) || !output6) return false;
    hr = output6->GetDesc1(result);
    output6->Release();
    return SUCCEEDED(hr);
}

bool inspectAdapter(IDXGIAdapter* adapter, HMONITOR monitor,
                    DXGI_OUTPUT_DESC1* selected) {
    if (!adapter || !selected) return false;
    for (UINT i = 0;; ++i) {
        IDXGIOutput* output = nullptr;
        if (adapter->EnumOutputs(i, &output) == DXGI_ERROR_NOT_FOUND) break;
        if (!output) continue;
        DXGI_OUTPUT_DESC base = {};
        DXGI_OUTPUT_DESC1 desc = {};
        bool matches = SUCCEEDED(output->GetDesc(&base))
                    && (!monitor || base.Monitor == monitor);
        bool ok = matches && outputDescription(output, &desc);
        output->Release();
        if (ok) { *selected = desc; return true; }
    }
    return false;
}

bool detectOutput(const DXGI_SWAP_CHAIN_DESC& swap, DXGI_OUTPUT_DESC1* selected) {
    HMONITOR monitor = swap.OutputWindow
        ? MonitorFromWindow(swap.OutputWindow, MONITOR_DEFAULTTONEAREST) : nullptr;
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) return false;
    typedef HRESULT(WINAPI* CreateFactoryFn)(REFIID, void**);
    CreateFactoryFn createFactory = (CreateFactoryFn)(void*)GetProcAddress(
        dxgi, "CreateDXGIFactory1");
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = createFactory
        ? createFactory(__uuidof(IDXGIFactory1), (void**)&factory) : E_FAIL;
    bool found = false;
    if (SUCCEEDED(hr) && factory) {
        for (UINT i = 0; !found; ++i) {
            IDXGIAdapter1* adapter = nullptr;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            if (!adapter) continue;
            found = inspectAdapter(adapter, monitor, selected);
            adapter->Release();
        }
        factory->Release();
    }
    FreeLibrary(dxgi);
    return found;
}

bool hdrColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace) {
    return colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
        || colorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
}

void logPath(wchar_t path[MAX_PATH]) {
    iniPath(path);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) lstrcpyW(slash + 1, L"tqflicker-hdr.log");
}

void flushLog(HANDLE file) {
    if (!file || file == INVALID_HANDLE_VALUE) return;
    char snapshot[sizeof(g_log)];
    AcquireSRWLockShared(&g_logLock);
    LONG bytes = g_logBytes;
    if (bytes > 0) memcpy(snapshot, g_log, (SIZE_T)bytes);
    ReleaseSRWLockShared(&g_logLock);
    SetFilePointer(file, 0, nullptr, FILE_BEGIN);
    DWORD written = 0;
    if (bytes > 0) WriteFile(file, snapshot, (DWORD)bytes, &written, nullptr);
    SetEndOfFile(file);
    FlushFileBuffers(file);
}

DWORD WINAPI loggerThread(void*) {
    wchar_t path[MAX_PATH];
    logPath(path);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE events[2] = {g_logStop, g_logEvent};
    for (;;) {
        DWORD wait = WaitForMultipleObjects(2, events, FALSE, 1000);
        flushLog(file);
        if (wait == WAIT_OBJECT_0) break;
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return 0;
}

void ensureLogger() {
    if (InterlockedCompareExchange(&g_loggerState, 1, 0) != 0) return;
    g_logEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_logStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_logEvent && g_logStop)
        g_logThread = CreateThread(nullptr, 0, loggerThread, nullptr, 0, nullptr);
    InterlockedExchange(&g_loggerState, g_logThread ? 2 : 3);
}

}  // namespace

Settings readSettings() {
    if (g_settingsRead) return g_runtime.settings;
    g_settingsRead = true;
    wchar_t path[MAX_PATH];
    iniPath(path);
    wchar_t value[32];
    GetPrivateProfileStringW(L"graphics", L"hdr", L"auto", value, 32, path);
    g_runtime.settings.requestHdr = _wcsicmp(value, L"off") != 0;
    GetPrivateProfileStringW(L"graphics", L"tonemap", L"frostbite", value, 32, path);
    g_runtime.settings.toneMap = !_wcsicmp(value, L"original") ? ToneOriginal
                                       : !_wcsicmp(value, L"agx") ? ToneAgx
                                       : ToneFrostbite;
    float paper = readFloat(path, L"paper_white_nits", 203.0f);
    g_runtime.settings.paperWhiteNits = paper >= 80.0f && paper <= 500.0f
                                      ? paper : 203.0f;
    GetPrivateProfileStringW(L"graphics", L"peak_nits", L"auto", value, 32, path);
    if (_wcsicmp(value, L"auto")) {
        wchar_t* end = nullptr;
        double peak = wcstod(value, &end);
        if (end != value && *end == 0 && peak >= g_runtime.settings.paperWhiteNits
            && peak <= 10000.0)
            g_runtime.settings.peakNitsOverride = (float)peak;
    }
    GetPrivateProfileStringW(L"debug", L"hdr_debug", L"0", value, 32, path);
    g_runtime.settings.debug = !_wcsicmp(value, L"1")
                            || !_wcsicmp(value, L"on")
                            || !_wcsicmp(value, L"true");
    return g_runtime.settings;
}

const Runtime& runtime() {
    readSettings();
    return g_runtime;
}

bool makeSwapChainCandidate(const DXGI_SWAP_CHAIN_DESC& original,
                            DXGI_SWAP_CHAIN_DESC* candidate) {
    Settings settings = readSettings();
    g_runtime.displayHdr = false;
    g_runtime.fp16Active = false;
    g_runtime.active = false;
    if (!candidate || settings.toneMap == ToneOriginal)
        return false;
    DXGI_OUTPUT_DESC1 output = {};
    bool detected = detectOutput(original, &output);
    if (detected) {
        g_runtime.displayHdr = hdrColorSpace(output.ColorSpace);
        log("Output: colorSpace=%u min=%.3f max=%.1f fullFrame=%.1f hdr=%u\r\n",
            (unsigned)output.ColorSpace, output.MinLuminance, output.MaxLuminance,
            output.MaxFullFrameLuminance, g_runtime.displayHdr ? 1u : 0u);
    } else {
        log("Output detection unavailable; attempting SDR FP16 presentation\r\n");
    }
    float reportedPeak = detected ? output.MaxLuminance : 0.0f;
    if (!(reportedPeak >= settings.paperWhiteNits && reportedPeak <= 10000.0f))
        reportedPeak = 1000.0f;
    g_runtime.peakNits = settings.peakNitsOverride > 0.0f
                       ? settings.peakNitsOverride : reportedPeak;
    *candidate = original;
    candidate->BufferDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    candidate->BufferCount = candidate->BufferCount < 2 ? 2 : candidate->BufferCount;
    candidate->SampleDesc.Count = 1;
    candidate->SampleDesc.Quality = 0;
    candidate->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    log("FP16 candidate: %ux%u format=%u buffers=%u effect=%u hdrRequested=%u paper=%.1f peak=%.1f\r\n",
        candidate->BufferDesc.Width, candidate->BufferDesc.Height,
        (unsigned)candidate->BufferDesc.Format, candidate->BufferCount,
        (unsigned)candidate->SwapEffect, settings.requestHdr ? 1u : 0u,
        settings.paperWhiteNits, g_runtime.peakNits);
    return true;
}

bool activateSwapChain(IDXGISwapChain* swapChain) {
    if (!swapChain) return false;
    IDXGISwapChain3* swap3 = nullptr;
    HRESULT hr = swapChain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&swap3);
    UINT support = 0;
    if (SUCCEEDED(hr) && swap3)
        hr = swap3->CheckColorSpaceSupport(
            DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support);
    if (SUCCEEDED(hr) && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
        hr = swap3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
    else if (SUCCEEDED(hr)) hr = DXGI_ERROR_UNSUPPORTED;
    if (swap3) swap3->Release();
    g_runtime.fp16Active = SUCCEEDED(hr);
    g_runtime.active = g_runtime.fp16Active && g_runtime.displayHdr
                    && g_runtime.settings.requestHdr;
    log("scRGB color-space activation: hr=0x%08lx support=0x%x fp16=%u hdr=%u\r\n",
        (unsigned long)hr, support, g_runtime.fp16Active ? 1u : 0u,
        g_runtime.active ? 1u : 0u);
    return g_runtime.fp16Active;
}

void reapplyColorSpace(IDXGISwapChain* swapChain) {
    if (!g_runtime.fp16Active || !swapChain) return;
    IDXGISwapChain3* swap3 = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&swap3))
        && swap3) {
        HRESULT hr = swap3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
        log("scRGB color space reapplied: hr=0x%08lx\r\n", (unsigned long)hr);
        swap3->Release();
    }
}

bool isColorGradingShader(const void* bytecode, SIZE_T size) {
    static const BYTE checksum[16] = {
        0x15,0x07,0x85,0xe4,0xfb,0xb5,0xca,0x43,
        0x79,0xfc,0x92,0xf9,0x64,0x2c,0x0c,0x9b
    };
    return exactShader(bytecode, size, 1288, checksum, "SceneColor", "ColorLut");
}

bool isGammaShader(const void* bytecode, SIZE_T size) {
    static const BYTE checksum[16] = {
        0xa2,0x0f,0xf7,0xb0,0xe5,0x78,0x2f,0x87,
        0x20,0x5c,0x22,0x36,0xb1,0xf7,0xe2,0x05
    };
    return exactShader(bytecode, size, 1108, checksum, "screenSampler", "gammaSampler");
}

namespace {

float clampFloat(float value, float low, float high) {
    return value < low ? low : value > high ? high : value;
}

float agxContrast(float x) {
    float x2 = x * x;
    float x4 = x2 * x2;
    return clampFloat(15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4
                    - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x
                    - 0.00232f, 0.0f, 1.0f);
}

float agxLuminance(float x) {
    if (!(x > 0.0f)) return 0.0f;
    float encoded = clampFloat((logf(x) / logf(2.0f) + 12.47393f) / 16.5f,
                               0.0f, 1.0f);
    // A restrained medium-contrast AgX look. The neutral transform is a
    // little too flat for Titan Quest's already display-referred artwork.
    return powf(agxContrast(encoded), 2.2f * 1.08f);
}

float frostbiteLuminance(float x, float peakRelative) {
    // Frostbite treats tone mapping as a neutral display-range transform, not
    // an artistic contrast curve. Keep the lower 75% of the target range
    // linear, then use a C1-continuous exponential shoulder toward the peak.
    // On HDR displays this leaves diffuse white and ordinary highlights
    // untouched; on SDR it creates headroom without changing the midtones.
    x = x > 0.0f ? x : 0.0f;
    float knee = peakRelative * 0.75f;
    if (x <= knee) return x;
    float range = peakRelative - knee;
    return knee + range * (1.0f - expf(-(x - knee) / range));
}

}  // namespace

float toneMapLuminance(ToneMap toneMap, float luminance, float peakRelative) {
    if (!(luminance > 0.0f) || !(peakRelative >= 1.0f)) return 0.0f;
    if (toneMap == ToneOriginal)
        return clampFloat(luminance, 0.0f, peakRelative);
    if (toneMap == ToneFrostbite)
        return clampFloat(frostbiteLuminance(luminance, peakRelative),
                          0.0f, peakRelative);
    float (*curve)(float) = agxLuminance;
    float white = curve(1.0f);
    if (luminance <= 1.0f) return clampFloat(curve(luminance), 0.0f, peakRelative);
    float range = peakRelative - white;
    if (!(range > 0.0f)) return peakRelative;
    float shoulder = white + range * (1.0f - expf(-(luminance - 1.0f)
                                                   / (range > 1.0f ? range : 1.0f)));
    return clampFloat(shoulder, 0.0f, peakRelative);
}

void log(const char* format, ...) {
    if (!readSettings().debug) return;
    AcquireSRWLockExclusive(&g_logLock);
    LONG offset = g_logBytes;
    if (!format || offset < 0 || offset >= (LONG)sizeof(g_log) - 1) {
        ReleaseSRWLockExclusive(&g_logLock);
        return;
    }
    va_list args;
    va_start(args, format);
    int written = _vsnprintf(g_log + offset, sizeof(g_log) - offset - 1, format, args);
    va_end(args);
    if (written > 0) {
        LONG next = offset + written;
        if (next >= (LONG)sizeof(g_log)) next = sizeof(g_log) - 1;
        g_log[next] = 0;
        g_logBytes = next;
    }
    ReleaseSRWLockExclusive(&g_logLock);
    ensureLogger();
    if (g_loggerState == 2) SetEvent(g_logEvent);
}

void shutdown() {
    if (g_loggerState == 2) {
        SetEvent(g_logStop);
        WaitForSingleObject(g_logThread, 2000);
    }
    if (g_logThread) CloseHandle(g_logThread);
    if (g_logStop) CloseHandle(g_logStop);
    if (g_logEvent) CloseHandle(g_logEvent);
    g_logThread = g_logStop = g_logEvent = nullptr;
}

}  // namespace hdr
}  // namespace tq
