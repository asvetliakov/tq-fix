#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "dxbc_patch.h"
#include "bloom_hook.h"
#include "frame_overlay.h"
#include "frustum_fix.h"
#include "grass.h"
#include "hdr.h"
#include "probe.h"
#include "shadow_fix.h"
#include "streaming.h"
#include "visual.h"
#include "bloom_shaders.inc"

namespace {

// Default shadow map scale; the tests exercise the shipped default. Measured
// at 3.73 ms a frame against 2.31 ms at scale 2, and kept at 4 because the
// smaller map visibly softens the shadows; the README documents the trade.
const UINT kShadowScale = 4;
// Point and spot maps scale separately; the split does not touch them.
const UINT kPointShadowScale = 2;

FILE* g_report;
int   g_failures;
int   g_presentOrder;
bool  g_presentOrderValid;
IDXGISwapChain* g_presentSwapChain;

void check(bool passed, const char* description) {
    fprintf(g_report, "%s  %s\n", passed ? "ok  " : "FAIL", description);
    if (!passed) ++g_failures;
}

void onTestPrePresent(IDXGISwapChain* swapChain) {
    g_presentOrderValid &= g_presentOrder == 0;
    g_presentOrder = 1;
    g_presentSwapChain = swapChain;
}

void onTestPostPresent(IDXGISwapChain* swapChain) {
    g_presentOrderValid &= g_presentOrder == 2 && swapChain == g_presentSwapChain;
    g_presentOrder = 3;
}

HRESULT WINAPI testOriginalPresent(IDXGISwapChain* swapChain, UINT interval,
                                   UINT flags) {
    g_presentOrderValid &= g_presentOrder == 1 && swapChain == g_presentSwapChain
                        && interval == 0 && flags == 0;
    g_presentOrder = 2;
    return S_OK;
}

void testRendererPresentHook() {
    const SIZE_T imageSize = 0x192000;
    BYTE* image = (BYTE*)VirtualAlloc(nullptr, imageSize,
                                     MEM_RESERVE | MEM_COMMIT,
                                     PAGE_EXECUTE_READWRITE);
    if (!image) {
        check(false, "allocate a synthetic Titan Quest renderer image");
        return;
    }
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)image;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x100;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(image + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    // Timestamp is deliberately arbitrary: identical renderer code repackaged
    // with different linker metadata must still be accepted.
    nt->FileHeader.TimeDateStamp = 0xdeadbeefu;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    nt->OptionalHeader.SizeOfImage = (DWORD)imageSize;
    static const BYTE presentCode[] = {
        0x8b, 0x51, 0x34, 0x33, 0xc0, 0x38, 0x81, 0xdb,
        0x05, 0x00, 0x00, 0x56, 0x8b, 0x32, 0x0f, 0x95,
        0xc0, 0x6a, 0x00, 0x50, 0x52, 0xff, 0x56, 0x20,
        0x5e, 0xc2, 0x04, 0x00
    };
    memcpy(image + 0x61190, presentCode, sizeof(presentCode));
    void** rendererPresentSlot = (void**)(image + 0x8625c);
    *rendererPresentSlot = image + 0x61190;

    void* swapVtable[14] = {};
    swapVtable[8] = (void*)&testOriginalPresent;
    struct FakeSwapChain { void** vtable; } swapChain = {swapVtable};
    BYTE renderer[0x600] = {};
    *(IDXGISwapChain**)(renderer + 0x34) = (IDXGISwapChain*)&swapChain;

    tq::streaming::setPresentCallback(&onTestPrePresent);
    tq::streaming::setPostPresentCallback(&onTestPostPresent);
    bool installed = tq::streaming::installRenderer((HMODULE)image);
    typedef HRESULT (__thiscall* RendererPresentFn)(void*, void*);
    RendererPresentFn present = (RendererPresentFn)*rendererPresentSlot;
    g_presentOrder = 0;
    g_presentOrderValid = true;
    g_presentSwapChain = (IDXGISwapChain*)&swapChain;
    HRESULT result = installed ? present(renderer, nullptr) : E_FAIL;
    check(installed && tq::streaming::presentHookInstalled(),
          "install the signature-gated renderer-level Present hook");
    check(SUCCEEDED(result) && g_presentOrderValid && g_presentOrder == 3,
          "run pre-Present, Steam-safe original Present, and post-Present in order");
    check(swapVtable[8] == (void*)&testOriginalPresent,
          "leave the shared IDXGISwapChain Present slot untouched");

    tq::streaming::shutdown();
    check(*rendererPresentSlot == image + 0x61190
          && !tq::streaming::presentHookInstalled(),
          "restore the renderer hook cleanly during shutdown");
    image[0x61190] ^= 1;
    check(!tq::streaming::installRenderer((HMODULE)image)
          && *rendererPresentSlot == image + 0x61190,
          "reject a near-match renderer without changing its vtable");
    tq::streaming::shutdown();
    VirtualFree(image, 0, MEM_RELEASE);
}

void testBloomHook() {
    const SIZE_T imageSize = 0x300000;
    BYTE* image = (BYTE*)VirtualAlloc(nullptr, imageSize,
                                     MEM_RESERVE | MEM_COMMIT,
                                     PAGE_EXECUTE_READWRITE);
    if (!image) {
        check(false, "allocate a synthetic Titan Quest Engine image");
        return;
    }
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)image;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x100;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(image + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER);
    nt->OptionalHeader.SizeOfImage = (DWORD)imageSize;
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    memcpy(section->Name, ".text", 5);
    section->VirtualAddress = 0x1000;
    section->Misc.VirtualSize = 0x2aa69c;

    static const BYTE body[] = {
        0x55,0x8b,0xec,0x83,0xe4,0xf8, // validated prologue
        0x8b,0xe5,0x5d,0xc2,0x14,0x00  // synthetic epilogue
    };
    BYTE* original = image + 0x15d7f0;
    memcpy(original, body, sizeof(body));
    bool hooked = tq::bloomhook::install(
        (HMODULE)image, (tq::bloomhook::HotBlurFn)(void*)original);
    check(hooked && tq::bloomhook::installed()
          && original[0] == 0x68 && original[5] == 0xc3,
          "detour the exact Engine bloom export with one absolute branch");
    tq::bloomhook::shutdown();
    check(!tq::bloomhook::installed()
          && !memcmp(original, body, sizeof(body)),
          "restore the Engine bloom function entry during shutdown");
    original[0] ^= 1;
    check(!tq::bloomhook::install(
              (HMODULE)image, (tq::bloomhook::HotBlurFn)(void*)original)
          && !tq::bloomhook::installed(),
          "reject a near-match bloom prologue without patching it");
    tq::bloomhook::shutdown();
    VirtualFree(image, 0, MEM_RELEASE);
}

// The grass probe writes into Engine.dll's own code, so the parts worth
// testing without the game are the ones that can corrupt it: that the exact
// prologue is required, that the trampoline still reaches the body, that
// suppression is the only thing that stops it, and that every byte comes back.
LONG g_grassRenderBody;

void emitAbsoluteIncrement(BYTE* code, const void* counter) {
    code[0] = 0xff;  // inc dword ptr [imm32]
    code[1] = 0x05;
    uint32_t address = (uint32_t)(uintptr_t)counter;
    memcpy(code + 2, &address, sizeof(address));
}

void testGrassProbe() {
    const SIZE_T imageSize = 0x400000;
    BYTE* image = (BYTE*)VirtualAlloc(nullptr, imageSize,
                                      MEM_RESERVE | MEM_COMMIT,
                                      PAGE_EXECUTE_READWRITE);
    if (!image) {
        check(false, "allocate a synthetic Titan Quest Engine image");
        return;
    }
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)image;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x100;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(image + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER);
    nt->OptionalHeader.SizeOfImage = (DWORD)imageSize;
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    memcpy(section->Name, ".text", 5);
    section->VirtualAddress = 0x1000;
    section->Misc.VirtualSize = 0x2aa69c;

    // void __thiscall RenderGrass(Name&, Canvas&, SceneRenderer&, Pass&)
    BYTE* render = image + 0x23afc0;
    BYTE renderBody[] = {
        0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,  // validated prologue
        0xff, 0x05, 0, 0, 0, 0,              // inc [g_grassRenderBody]
        0x8b, 0xe5, 0x5d, 0xc2, 0x10, 0x00   // mov esp,ebp; pop ebp; ret 0x10
    };
    emitAbsoluteIncrement(renderBody + 6, &g_grassRenderBody);
    memcpy(render, renderBody, sizeof(renderBody));

    tq::grass::Exports exports = {};
    exports.renderGrassRT = render;
    bool hooked = tq::grass::install((HMODULE)image, exports);
    check(hooked && tq::grass::installed()
          && render[0] == 0x68 && render[5] == 0xc3,
          "detour the exported grass render entry with one absolute branch");

    typedef void (__fastcall* RenderFn)(void*, void*, const void*, void*,
                                        const void*, const void*);
    RenderFn callRender = (RenderFn)(void*)render;

    // The detour exists to report that a grass draw is on the stack, and it
    // must still reach the body: this is the game's own rendering, not ours.
    g_grassRenderBody = 0;
    check(!tq::grass::rendering(), "report no grass draw outside RenderGrass");
    callRender(image, nullptr, image, image, image, image);
    check(g_grassRenderBody == 1,
          "reach the grass render body through the trampoline");
    check(!tq::grass::rendering(), "stop reporting once RenderGrass returns");

    tq::grass::shutdown();
    check(!tq::grass::installed()
          && !memcmp(render, renderBody, sizeof(renderBody)),
          "restore the grass render entry during shutdown");

    render[0] ^= 1;
    tq::grass::Exports broken = {};
    broken.renderGrassRT = render;
    check(!tq::grass::install((HMODULE)image, broken) && !tq::grass::installed(),
          "reject a near-match grass prologue without patching it");
    tq::grass::shutdown();
    VirtualFree(image, 0, MEM_RELEASE);
}

// The captured first grass plane, verbatim from the bound stream during a
// live draw: position, normal, uv per vertex, wound top-left, top-right,
// bottom-right, bottom-left. Using the real bytes means the fingerprint is
// tested against what the game actually writes rather than against an idea of
// it.
const uint32_t kCapturedPlane[32] = {
    0x43139465, 0x41c0d49a, 0x41fa311f, 0xbe3b9187, 0x3f7b44d5, 0x3d62e2d8,
        0x00000000, 0x00000000,
    0x431489d5, 0x41c0d49a, 0x42000629, 0xbe3b9187, 0x3f7b44d5, 0x3d62e2d8,
        0x3f000000, 0x00000000,
    0x431489d5, 0x41b7315d, 0x42000629, 0xbe3b9187, 0x3f7b44d5, 0x3d62e2d8,
        0x3f000000, 0x3f800000,
    0x43139465, 0x41b7315d, 0x41fa311f, 0xbe3b9187, 0x3f7b44d5, 0x3d62e2d8,
        0x00000000, 0x3f800000
};

void loadPlane(float plane[32]) {
    memcpy(plane, kCapturedPlane, sizeof(kCapturedPlane));
}

float kCapturedPlaneFloat(unsigned index) {
    float value;
    memcpy(&value, kCapturedPlane + index, sizeof(value));
    return value;
}

void testGrassCrossed() {
    float plane[32];
    loadPlane(plane);

    // The fingerprint, against bytes captured from the running game rather
    // than against an idea of what a card looks like. Everything else here
    // depends on this recognising a real card and nothing else.
    check(tq::grass::isGrassPlane(plane),
          "recognise a captured grass card by its exact shape");
    check(!memcmp(plane + 3, plane + 11, 3 * sizeof(float))
          && !memcmp(plane + 3, plane + 19, 3 * sizeof(float))
          && !memcmp(plane + 3, plane + 27, 3 * sizeof(float)),
          "confirm the captured card shares one normal across four vertices");

    const float* v0 = plane;
    const float* v1 = plane + 8;
    const float* v2 = plane + 16;
    const float* v3 = plane + 24;

    const double cx = ((double)v0[0] + v1[0]) * 0.5;
    const double cz = ((double)v0[2] + v1[2]) * 0.5;
    const double wx = (double)v1[0] - v0[0];
    const double wz = (double)v1[2] - v0[2];
    const double width = sqrt(wx * wx + wz * wz);
    const double top = v0[1];
    const double bottom = v2[1];

    check(tq::grass::rotatePlane(plane), "turn a captured card a quarter turn");

    // A crossing card is only a crossing card if it still stands where the
    // original stood: same centre, same size, same height.
    const double cx2 = ((double)v0[0] + v1[0]) * 0.5;
    const double cz2 = ((double)v0[2] + v1[2]) * 0.5;
    check(fabs(cx2 - cx) < 1e-4 && fabs(cz2 - cz) < 1e-4,
          "keep the turned card on the original card's centre");

    const double wx2 = (double)v1[0] - v0[0];
    const double wz2 = (double)v1[2] - v0[2];
    const double width2 = sqrt(wx2 * wx2 + wz2 * wz2);
    check(fabs(width2 - width) < 1e-3, "keep the turned card's width");
    check(v0[1] == top && v1[1] == top && v2[1] == bottom && v3[1] == bottom,
          "keep the turned card's height untouched");

    // Perpendicular is the whole point: a card turned by anything less would
    // still vanish edge-on at the same angle the original does.
    const double along = (wx * wx2 + wz * wz2) / (width * width2);
    check(fabs(along) < 1e-4, "turn the card perpendicular to the original");

    check(tq::grass::isGrassPlane(plane),
          "leave the turned card recognisable as a grass card");

    // Untouched apart from position: the copy shares the original's texture
    // column and its normal, so only the geometry differs.
    check(!memcmp(plane + 3, kCapturedPlane + 3, 3 * sizeof(float))
          && !memcmp(plane + 6, kCapturedPlane + 6, 2 * sizeof(float))
          && !memcmp(plane + 30, kCapturedPlane + 30, 2 * sizeof(float)),
          "leave the turned card's normal and uv exactly as they were");

    // Two quarter turns are a half turn, which is the same card seen from the
    // other side -- so the operation cannot drift the geometry.
    check(tq::grass::rotatePlane(plane), "turn a card that has already turned");
    check(fabs((double)v0[0] - kCapturedPlaneFloat(8)) < 1e-3
          && fabs((double)v1[0] - kCapturedPlaneFloat(0)) < 1e-3,
          "return a twice-turned card to the original corners, swapped");

    float zero[32];
    memset(zero, 0, sizeof(zero));
    check(!tq::grass::rotatePlane(zero), "leave an unwritten buffer slot unturned");
}

// Counted but not printed: a batch check would otherwise report sixty-four
// identical lines.
void check_quiet(bool passed) {
    if (!passed) ++g_failures;
}

void testGrassPointerIndex() {
    static tq::grass::PointerIndex index;
    tq::grass::indexReset(index);

    void* a = (void*)0x10004000;
    void* b = (void*)0x10008000;
    unsigned value = 0;
    check(!tq::grass::indexLookup(index, a, &value), "miss on an empty index");
    check(tq::grass::indexInsert(index, a, 7)
          && tq::grass::indexLookup(index, a, &value) && value == 7,
          "find a key that was inserted");
    check(!tq::grass::indexLookup(index, b, &value), "miss a key never inserted");
    check(tq::grass::indexInsert(index, a, 9)
          && tq::grass::indexLookup(index, a, &value) && value == 9,
          "replace the value of an existing key");
    check(tq::grass::indexRemove(index, a) && !tq::grass::indexLookup(index, a, &value),
          "miss a key after it is removed");

    // Removal leaves a tombstone precisely so a key that probed past the
    // removed one is still reachable. Buffers are recycled constantly, so this
    // is the ordinary case, not an edge case.
    tq::grass::indexReset(index);
    void* keys[64];
    for (unsigned i = 0; i < 64; ++i) {
        keys[i] = (void*)(uintptr_t)(0x20000000 + i * 0x100);
        check_quiet(tq::grass::indexInsert(index, keys[i], i));
    }
    bool all = true;
    for (unsigned i = 0; i < 64; ++i)
        all = all && tq::grass::indexLookup(index, keys[i], &value) && value == i;
    check(all, "find all of a batch of realistic buffer addresses");

    for (unsigned i = 0; i < 64; i += 2) tq::grass::indexRemove(index, keys[i]);
    bool survivors = true;
    for (unsigned i = 1; i < 64; i += 2)
        survivors = survivors && tq::grass::indexLookup(index, keys[i], &value)
                 && value == i;
    check(survivors, "keep the rest reachable after removing every other key");
    bool gone = true;
    for (unsigned i = 0; i < 64; i += 2)
        gone = gone && !tq::grass::indexLookup(index, keys[i], &value);
    check(gone, "leave no removed key findable");

    // A tombstone has to be reusable or a long session of streaming blocks
    // would fill the table with them and stop tracking anything.
    check(tq::grass::indexInsert(index, keys[0], 111)
          && tq::grass::indexLookup(index, keys[0], &value) && value == 111,
          "reuse a tombstoned slot for a later key");

    check(!tq::grass::indexInsert(index, nullptr, 1)
          && !tq::grass::indexLookup(index, nullptr, &value),
          "refuse a null key rather than storing one");

    // Insertion is allowed to fail, and the caller treats that as untracked.
    // What it must never do is report success without storing the key.
    tq::grass::indexReset(index);
    unsigned stored = 0;
    for (unsigned i = 0; i < 4096; ++i) {
        void* key = (void*)(uintptr_t)(0x30000000 + i * 0x40);
        if (tq::grass::indexInsert(index, key, i)) ++stored;
    }
    bool honest = true;
    unsigned found = 0;
    for (unsigned i = 0; i < 4096; ++i) {
        void* key = (void*)(uintptr_t)(0x30000000 + i * 0x40);
        if (tq::grass::indexLookup(index, key, &value)) {
            ++found;
            honest = honest && value == i;
        }
    }
    check(found == stored && honest,
          "store exactly the keys whose insertion was reported successful");
}

// Crossing and bending are independent settings that can both be on, so the
// order they compose in matters: a bent card must still be turnable, and a
// turned card must still be bendable.
void testBloomExtraction() {
    bool monotonic = true;
    float previous = -1.0f;
    for (unsigned i = 0; i <= 2048; ++i) {
        float input = i * (8.0f / 2048.0f);
        float output = tq::bloomhook::extractBrightness(input, 1.0f, 0.25f);
        monotonic &= output == output && output >= 0.0f
                  && output + 0.00001f >= previous;
        previous = output;
    }
    float dark = tq::bloomhook::extractBrightness(0.5f, 1.0f, 0.25f);
    float shoulder = tq::bloomhook::extractBrightness(0.9f, 1.0f, 0.25f);
    float white = tq::bloomhook::extractBrightness(1.0f, 1.0f, 0.25f);
    float highlight = tq::bloomhook::extractBrightness(2.0f, 1.0f, 0.25f);
    check(monotonic && dark == 0.0f && shoulder > 0.0f && shoulder < 0.1f
          && white > shoulder && white < 0.1f
          && highlight > 0.999f && highlight < 1.001f,
          "global HDR bloom extraction is soft, monotonic, and unclipped");
}

bool overlayCompile(const char* source, const char* target, ID3DBlob** result) {
    static HMODULE compiler;
    if (!compiler) compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!compiler) compiler = LoadLibraryW(L"d3dcompiler_43.dll");
    typedef HRESULT(WINAPI* CompileFn)(
        LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
        LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    CompileFn compile = compiler
        ? (CompileFn)(void*)GetProcAddress(compiler, "D3DCompile") : nullptr;
    if (!compile) return false;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile(source, strlen(source), "overlay-selftest", nullptr,
                         nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3,
                         0, result, &errors);
    if (errors) errors->Release();
    return SUCCEEDED(hr) && result && *result;
}

HRESULT WINAPI overlayCreateTexture2D(ID3D11Device* device,
                                      const D3D11_TEXTURE2D_DESC* desc,
                                      const D3D11_SUBRESOURCE_DATA* initial,
                                      ID3D11Texture2D** out) {
    return device->CreateTexture2D(desc, initial, out);
}

HRESULT WINAPI overlayCreateShaderResourceView(
    ID3D11Device* device, ID3D11Resource* resource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, ID3D11ShaderResourceView** out) {
    return device->CreateShaderResourceView(resource, desc, out);
}

HRESULT WINAPI overlayCreateSamplerState(ID3D11Device* device,
                                         const D3D11_SAMPLER_DESC* desc,
                                         ID3D11SamplerState** out) {
    return device->CreateSamplerState(desc, out);
}

HRESULT WINAPI overlayCreatePixelShader(ID3D11Device* device, const void* bytes,
                                        SIZE_T size, ID3D11ClassLinkage* linkage,
                                        ID3D11PixelShader** out) {
    return device->CreatePixelShader(bytes, size, linkage, out);
}

// The overlay is a debug instrument, so what matters most is that a shipped
// configuration never reaches it: with the setting absent or 0 it must not
// measure, allocate, build, or draw. The rest of the test proves it still
// works when it is asked for.
// Reads a whole small file, so the probe's CSV can be asserted on rather than
// merely assumed to exist.
char* readTextFile(const wchar_t* path, long* size) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    DWORD bytes = GetFileSize(file, nullptr);
    char* text = (char*)calloc(bytes + 1, 1);
    DWORD read = 0;
    if (text) ReadFile(file, text, bytes, &read, nullptr);
    CloseHandle(file);
    if (size) *size = (long)read;
    return text;
}

// Runs on a thread that is deliberately not the render thread, so the two
// counting channels can be told apart by where their writes end up.
DWORD WINAPI engineChannelWorker(void*) {
    tq::probe::count(tq::probe::CounterDrawIndexed, 5);
    tq::probe::engineCount(tq::probe::CounterEngineTexCreateOff, 3);
    tq::probe::engineCount(tq::probe::CounterEngineTexCreateOffUs, 900);
    return 0;
}

// Fields in a CSV line, counting the separators rather than parsing: the only
// field that can contain a space is `unusual`, and none contains a comma.
unsigned csvFieldCount(const char* line) {
    unsigned fields = 1;
    for (const char* p = line; *p && *p != '\r' && *p != '\n'; ++p)
        if (*p == ',') ++fields;
    return fields;
}

// Whether the line's last field is exactly `name`.
bool csvLastFieldIs(const char* line, const char* name) {
    const char* end = line;
    while (*end && *end != '\r' && *end != '\n') ++end;
    const char* last = line;
    for (const char* p = line; p < end; ++p) if (*p == ',') last = p + 1;
    size_t length = strlen(name);
    return (size_t)(end - last) == length && strncmp(last, name, length) == 0;
}

// Whether this device retires timestamp queries at all, and under which of the
// three ways of asking. Two in-game runs of eight thousand frames each resolved
// not one, so the capability has to be established here rather than assumed:
// the probe's GPU columns are only worth having if the answer is yes.
void testTimestampCapability(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context) return;
    D3D11_QUERY_DESC disjointDesc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC stampDesc = {D3D11_QUERY_TIMESTAMP, 0};
    ID3D11Query *disjoint = nullptr, *begin = nullptr, *end = nullptr;
    bool created = SUCCEEDED(device->CreateQuery(&disjointDesc, &disjoint))
                && SUCCEEDED(device->CreateQuery(&stampDesc, &begin))
                && SUCCEEDED(device->CreateQuery(&stampDesc, &end));
    check(created, "the device creates timestamp and disjoint queries");
    if (!created) {
        if (disjoint) disjoint->Release();
        if (begin) begin->Release();
        if (end) end->Release();
        return;
    }

    // Something for the GPU to actually do between the two timestamps.
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = desc.Height = 512;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *source = nullptr, *destination = nullptr;
    bool ready = SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &source))
              && SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &destination));

    context->Begin(disjoint);
    context->End(begin);
    if (ready) for (unsigned i = 0; i < 32; ++i)
        context->CopyResource(destination, source);
    context->End(end);
    context->End(disjoint);

    // First the way the render path asks: never flush, never wait.
    bool quiet = false;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data = {};
    for (unsigned i = 0; i < 200 && !quiet; ++i) {
        quiet = context->GetData(disjoint, &data, sizeof(data),
                                 D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (!quiet) Sleep(1);
    }
    // Then the way that permits the runtime to flush first.
    context->Flush();
    bool flushed = quiet;
    for (unsigned i = 0; i < 200 && !flushed; ++i) {
        flushed = context->GetData(disjoint, &data, sizeof(data), 0) == S_OK;
        if (!flushed) Sleep(1);
    }
    UINT64 first = 0, last = 0;
    bool stamps = flushed
               && context->GetData(begin, &first, sizeof(first), 0) == S_OK
               && context->GetData(end, &last, sizeof(last), 0) == S_OK;

    char detail[192];
    snprintf(detail, sizeof(detail),
             "timestamp queries retire on this device "
             "(donotflush=%u afterflush=%u stamps=%u disjoint=%u freq=%llu)",
             quiet ? 1u : 0u, flushed ? 1u : 0u, stamps ? 1u : 0u,
             data.Disjoint ? 1u : 0u, (unsigned long long)data.Frequency);
    check(flushed && stamps && data.Frequency != 0, detail);

    if (source) source->Release();
    if (destination) destination->Release();
    disjoint->Release();
    begin->Release();
    end->Release();
}

void testProbe(ID3D11Device* device, ID3D11DeviceContext* context) {
    wchar_t ini[MAX_PATH], csv[MAX_PATH];
    if (!GetFullPathNameW(L"tqflicker-probe-selftest.ini", MAX_PATH, ini, nullptr)
        || !GetFullPathNameW(L"tqflicker-probe-selftest.csv", MAX_PATH, csv, nullptr))
        return;
    DeleteFileW(ini);
    DeleteFileW(csv);

    // Off by default, and silent: the probe must never write a file the user
    // did not ask for, which is the same invariant the two logs hold.
    tq::probe::readOptions(ini);
    check(!tq::probe::enabled(), "probe stays off when the INI has no setting");
    tq::probe::count(tq::probe::CounterDraw);
    tq::probe::engineCount(tq::probe::CounterEngineTexCreateOff, 11);
    tq::probe::endFrame(33.0f);
    check(tq::probe::frameCountForTest() == 0,
          "probe records nothing while it is off");
    check(tq::probe::microsecondsSince(tq::probe::now()) == 0,
          "microsecondsSince reports nothing while the probe is off");
    check(!tq::probe::createResources(device),
          "probe creates no device objects while it is off");

    WritePrivateProfileStringW(L"debug", L"performance_trace", L"0", ini);
    tq::probe::readOptions(ini);
    check(!tq::probe::enabled(), "performance_trace=0 leaves the probe off");

    WritePrivateProfileStringW(L"debug", L"performance_trace", L"1", ini);
    tq::probe::readOptions(ini);
    check(tq::probe::enabled() && !tq::probe::logsEveryFrame(),
          "performance_trace=1 records hitching frames only");
    tq::probe::resetForTest();

    WritePrivateProfileStringW(L"debug", L"performance_trace", L"full", ini);
    tq::probe::readOptions(ini);
    tq::probe::setOutputPath(csv);
    check(tq::probe::enabled() && tq::probe::logsEveryFrame(),
          "performance_trace=full turns the probe on for every frame");

    // A phase interval the clock can actually resolve, and a counter beside it.
    int64_t start = tq::probe::now();
    Sleep(4);
    tq::probe::addPhase(tq::probe::PhaseGrassPresent, start);
    tq::probe::count(tq::probe::CounterGrassSeedQueued);
    tq::probe::count(tq::probe::CounterDraw, 7);
    tq::probe::endFrame(16.7f);
    float measured = tq::probe::phaseMillisecondsForTest(0, tq::probe::PhaseGrassPresent);
    check(measured >= 2.0f && measured < 500.0f,
          "probe times a phase with the high-resolution clock");
    check(tq::probe::counterForTest(0, tq::probe::CounterDraw) == 7
          && tq::probe::counterForTest(0, tq::probe::CounterGrassSeedQueued) == 1,
          "probe counts what the frame was asked to do");
    check(tq::probe::phaseMillisecondsForTest(0, tq::probe::PhaseBloom) == 0.0f,
          "a phase that did not run stays at zero");

    // The engine channel. The frame above taught the probe which thread is the
    // render thread, so from here a write from anywhere else is refused by
    // count() and accepted by engineCount() -- which is the entire reason the
    // second channel exists, and the invariant the first one must not lose.
    HANDLE worker = CreateThread(nullptr, 0, &engineChannelWorker, nullptr, 0,
                                 nullptr);
    if (worker) { WaitForSingleObject(worker, 5000); CloseHandle(worker); }
    tq::probe::endFrame(16.7f);
    check(worker != nullptr
          && tq::probe::counterForTest(0, tq::probe::CounterEngineTexCreateOff) == 3
          && tq::probe::counterForTest(0, tq::probe::CounterEngineTexCreateOffUs) == 900,
          "engineCount from the game's own thread lands in the frame that closed");
    check(tq::probe::counterForTest(0, tq::probe::CounterDrawIndexed) == 0,
          "count() from a thread that is not the render thread still records nothing");
    tq::probe::endFrame(16.7f);
    check(tq::probe::counterForTest(0, tq::probe::CounterEngineTexCreateOff) == 0,
          "the engine channel drains, so a count is charged to one frame only");

    // A steady baseline, then one frame that spikes in a single phase. The row
    // for that frame has to name the phase, not merely report the frame time.
    for (unsigned i = 0; i < 90; ++i) tq::probe::endFrame(16.7f);
    int64_t hitchStart = tq::probe::now();
    Sleep(30);
    tq::probe::addPhase(tq::probe::PhaseGrassPresent, hitchStart);
    tq::probe::endFrame(48.0f);
    for (unsigned i = 0; i < 16; ++i) tq::probe::endFrame(16.7f);

    char summary[80] = {};
    tq::probe::summarize(summary, sizeof(summary));
    check(strstr(summary, "GRASS-PRES") != nullptr,
          "the overlay summary names the phase that dominated the hitch");

    if (device && context) {
        check(tq::probe::createResources(device),
              "probe builds its timestamp queries on the live device");

        // Drive frames with real GPU work in them until a timestamp comes
        // back. Two eight-thousand-frame runs reported no GPU timing at all
        // because the frame's disjoint query was begun and never ended, and
        // every timestamp is gated on that disjoint result. Asserting only
        // that issuing the queries does not crash could not see it; this
        // asserts that a number actually arrives.
        D3D11_TEXTURE2D_DESC surface = {};
        surface.Width = surface.Height = 512;
        surface.MipLevels = surface.ArraySize = 1;
        surface.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        surface.SampleDesc.Count = 1;
        surface.Usage = D3D11_USAGE_DEFAULT;
        surface.BindFlags = D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D *from = nullptr, *to = nullptr;
        bool copies = SUCCEEDED(device->CreateTexture2D(&surface, nullptr, &from))
                   && SUCCEEDED(device->CreateTexture2D(&surface, nullptr, &to));
        unsigned before = tq::probe::frameCountForTest();
        for (unsigned i = 0; i < 40; ++i) {
            tq::probe::beginFrame(context);
            tq::probe::gpuBegin(context, tq::probe::GpuSmaa);
            if (copies) for (unsigned c = 0; c < 16; ++c)
                context->CopyResource(to, from);
            tq::probe::gpuEnd(context, tq::probe::GpuSmaa);
            tq::probe::endFrame(16.7f);
            context->Flush();
            Sleep(2);
        }
        bool resolved = false;
        for (unsigned back = 0;
             back < tq::probe::frameCountForTest() - before && !resolved; ++back)
            resolved = tq::probe::gpuResolvedForTest(back);
        check(copies && resolved,
              "probe reads back a GPU timestamp for a frame it timed");
        if (from) from->Release();
        if (to) to->Release();
        tq::probe::releaseResources();
    }

    tq::probe::shutdown();
    long size = 0;
    char* csvText = readTextFile(csv, &size);
    bool header = csvText
               && strstr(csvText, "# performance_trace=full") == csvText
               && strstr(csvText, "frame,ms") != nullptr
               && strstr(csvText, "grass_present_ms") != nullptr
               && strstr(csvText, "gpu_shadow_dir_ms") != nullptr
               && strstr(csvText, ",unusual") != nullptr;
    check(header, "the probe writes its mode and a header naming every column");
    check(csvText && strstr(csvText, "engine_tex_create_off_us") != nullptr,
          "the header carries the engine channel's columns");
    // The permanent regression test for the header buffer. snprintf truncation
    // is silent -- `n += snprintf(...)` returns the length it wanted, so an
    // overrun writes a short, unterminated header and nothing reports it. A
    // header with fewer fields than its rows is the shape that failure takes.
    bool widths = false;
    if (csvText) {
        const char* headerLine = strchr(csvText, '\n');
        headerLine = headerLine ? headerLine + 1 : nullptr;
        const char* firstRow = headerLine ? strchr(headerLine, '\n') : nullptr;
        firstRow = firstRow ? firstRow + 1 : nullptr;
        widths = headerLine && firstRow && *firstRow
              && csvFieldCount(headerLine) == csvFieldCount(firstRow)
              && csvLastFieldIs(headerLine, "unusual");
    }
    check(widths, "the CSV header has exactly as many fields as a row, ending in unusual");
    free(csvText);

    DeleteFileW(csv);
    tq::probe::resetForTest();
    tq::probe::readOptions(nullptr);
    check(!tq::probe::enabled(), "probe stays off without an INI path");
    DeleteFileW(ini);
}

void testFrameOverlay(ID3D11Device* device, ID3D11DeviceContext* context) {
    wchar_t ini[MAX_PATH];
    if (!GetFullPathNameW(L"tqflicker-overlay-selftest.ini", MAX_PATH, ini, nullptr))
        return;
    DeleteFileW(ini);

    tq::frameoverlay::DeviceCalls calls = {};
    calls.createTexture2D = nullptr;
    calls.compile = &overlayCompile;

    tq::frameoverlay::readOptions(ini);
    check(!tq::frameoverlay::enabled(),
          "frame overlay stays off when the INI has no setting");
    tq::frameoverlay::recordFrame();
    check(!tq::frameoverlay::createResources(device, calls),
          "frame overlay builds nothing while it is off");
    tq::frameoverlay::draw(device, context, nullptr, 1920);

    WritePrivateProfileStringW(L"debug", L"frame_overlay", L"0", ini);
    tq::frameoverlay::readOptions(ini);
    check(!tq::frameoverlay::enabled(), "frame_overlay=0 leaves the overlay off");

    // An INI from before the key moved out of [performance] keeps working,
    // and an explicit [debug] value wins over the legacy one.
    WritePrivateProfileStringW(L"debug", L"frame_overlay", nullptr, ini);
    WritePrivateProfileStringW(L"performance", L"frame_overlay", L"1", ini);
    tq::frameoverlay::readOptions(ini);
    check(tq::frameoverlay::enabled(),
          "the key's old [performance] home is still honoured");
    WritePrivateProfileStringW(L"debug", L"frame_overlay", L"0", ini);
    tq::frameoverlay::readOptions(ini);
    check(!tq::frameoverlay::enabled(),
          "a [debug] value overrides the legacy [performance] one");
    WritePrivateProfileStringW(L"performance", L"frame_overlay", nullptr, ini);

    WritePrivateProfileStringW(L"debug", L"frame_overlay", L"1", ini);
    tq::frameoverlay::readOptions(ini);
    check(tq::frameoverlay::enabled(), "frame_overlay=1 turns the overlay on");

    if (device && context) {
        calls.createTexture2D = &overlayCreateTexture2D;
        calls.createShaderResourceView = &overlayCreateShaderResourceView;
        calls.createSamplerState = &overlayCreateSamplerState;
        calls.createPixelShader = &overlayCreatePixelShader;
        check(tq::frameoverlay::createResources(device, calls),
              "build the overlay's shaders and panels on the live device");

        for (unsigned i = 0; i < 8; ++i) { tq::frameoverlay::recordFrame(); Sleep(16); }

        D3D11_TEXTURE2D_DESC targetDesc = {};
        targetDesc.Width = 960; targetDesc.Height = 540;
        targetDesc.MipLevels = targetDesc.ArraySize = 1;
        targetDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        targetDesc.SampleDesc.Count = 1;
        targetDesc.Usage = D3D11_USAGE_DEFAULT;
        targetDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        D3D11_TEXTURE2D_DESC stagingDesc = targetDesc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* surface = nullptr;
        ID3D11Texture2D* staging = nullptr;
        ID3D11RenderTargetView* rtv = nullptr;
        bool ready = SUCCEEDED(device->CreateTexture2D(&targetDesc, nullptr, &surface))
                  && SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))
                  && SUCCEEDED(device->CreateRenderTargetView(surface, nullptr, &rtv));
        check(ready, "allocate a scratch surface for the overlay draw");
        if (ready) {
            const FLOAT clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            context->ClearRenderTargetView(rtv, clear);
            tq::frameoverlay::draw(device, context, rtv, targetDesc.Width);
            context->CopyResource(staging, surface);
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            bool mappedOk = SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
            bool panelDrawn = false, graphDrawn = false, outsideClean = true;
            if (mappedOk) {
                const BYTE* rows = (const BYTE*)mapped.pData;
                panelDrawn = *(const uint32_t*)(rows + 40 * mapped.RowPitch + 40 * 4) != 0;
                graphDrawn = *(const uint32_t*)(rows + 150 * mapped.RowPitch + 40 * 4) != 0;
                outsideClean = *(const uint32_t*)(rows + 400 * mapped.RowPitch + 400 * 4) == 0;
                context->Unmap(staging, 0);
            }
            check(mappedOk && panelDrawn, "the overlay writes its statistics panel");
            check(mappedOk && graphDrawn, "the overlay writes its pacing graph");
            check(mappedOk && outsideClean,
                  "the overlay leaves the rest of the frame untouched");
        }
        if (rtv) rtv->Release();
        if (staging) staging->Release();
        if (surface) surface->Release();
        tq::frameoverlay::releaseResources();
    }

    tq::frameoverlay::reset();
    tq::frameoverlay::readOptions(nullptr);
    check(!tq::frameoverlay::enabled(), "frame overlay stays off without an INI path");
    DeleteFileW(ini);
}

void testBloomShaders() {
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!compiler) compiler = LoadLibraryW(L"d3dcompiler_43.dll");
    typedef HRESULT(WINAPI* CompileFn)(
        LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
        LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    CompileFn compile = compiler
        ? (CompileFn)(void*)GetProcAddress(compiler, "D3DCompile") : nullptr;
    const char* sources[] = {
        kBloomExtractSource, kBloomDownsampleSource,
        kBloomUpsampleSource, kBloomCompositeSource
    };
    bool accepted = compile != nullptr;
    for (unsigned i = 0; accepted && i < sizeof(sources) / sizeof(sources[0]); ++i) {
        ID3DBlob *shader = nullptr, *errors = nullptr;
        HRESULT hr = compile(sources[i], strlen(sources[i]), "bloom-selftest",
                             nullptr, nullptr, "main", "ps_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                             &shader, &errors);
        accepted &= SUCCEEDED(hr) && shader && shader->GetBufferSize() > 0;
        if (shader) shader->Release();
        if (errors) errors->Release();
    }
    check(accepted, "compile all HDR bloom shaders with the runtime compiler");
    if (compiler) FreeLibrary(compiler);
}

void testShadowSplitRedirect() {
    const SIZE_T imageSize = 0x0044b000u;
    BYTE* image = (BYTE*)VirtualAlloc(nullptr, imageSize,
                                      MEM_RESERVE | MEM_COMMIT,
                                      PAGE_EXECUTE_READWRITE);
    check(image != nullptr, "allocate a synthetic Engine image");
    if (!image) return;
    memset(image, 0, imageSize);

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)image;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(image + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    // Timestamp is deliberately arbitrary: identical code repackaged with
    // different linker metadata must still be accepted.
    nt->FileHeader.TimeDateStamp = 0xdeadbeefu;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    nt->OptionalHeader.SizeOfImage = (DWORD)imageSize;

    const struct { DWORD rva; BYTE reg, opcode; } references[] = {
        {0x0018e40du, 0x15, 0x59}, {0x0018e42eu, 0x0d, 0x59},
        {0x0018e446u, 0x05, 0x59}, {0x0018e503u, 0x15, 0x59},
        {0x0018e51bu, 0x0d, 0x59}, {0x0018e533u, 0x05, 0x59},
        {0x0018e5ddu, 0x15, 0x59}, {0x0018e609u, 0x0d, 0x59},
        {0x0018e618u, 0x05, 0x59}, {0x0018e6fcu, 0x05, 0x10},
        {0x0018f556u, 0x0d, 0x10},
    };
    const unsigned count = sizeof(references) / sizeof(references[0]);
    uint32_t cropAddress = (uint32_t)(uintptr_t)(image + 0x002f9550u);
    for (unsigned i = 0; i < count; ++i) {
        BYTE* instruction = image + references[i].rva;
        instruction[0] = 0xf3; instruction[1] = 0x0f;
        instruction[2] = references[i].opcode;
        instruction[3] = references[i].reg;
        memcpy(instruction + 4, &cropAddress, sizeof(cropAddress));
    }

    check(tq::shadow::validateSupportedImageForTest((HMODULE)image),
          "accept the audited Engine layout with a different PE timestamp");
    check(tq::shadow::redirectCropRoundTripForTest((HMODULE)image),
          "redirect and restore all eleven crop operands exactly");

    // The fit stabiliser retargets one relative call rather than rewriting an
    // immediate, so the site is identified by its five literal bytes: a
    // relative displacement does not depend on where the image is loaded.
    const DWORD fitCallRva = 0x0018ec69u;
    const BYTE fitCall[5] = {0xe8, 0xc2, 0x51, 0xf9, 0xff};
    memcpy(image + fitCallRva, fitCall, sizeof(fitCall));
    const DWORD basisCallRva = 0x0018e7fau;
    const BYTE basisCall[5] = {0xe8, 0xf1, 0x55, 0x0f, 0x00};
    memcpy(image + basisCallRva, basisCall, sizeof(basisCall));
    check(tq::shadow::validateFitCameraCallForTest((HMODULE)image),
          "accept the audited Camera setup call site");
    check(tq::shadow::validateBasisCallForTest((HMODULE)image),
          "accept the audited light-basis call site");
    check(tq::shadow::retargetFitCameraCallRoundTripForTest((HMODULE)image),
          "retarget the Camera setup call to the thunk and restore it exactly");
    image[fitCallRva + 1] ^= 1;
    check(!tq::shadow::validateFitCameraCallForTest((HMODULE)image),
          "reject a Camera setup call with an unexpected target");
    image[fitCallRva + 1] ^= 1;
    image[basisCallRva + 1] ^= 1;
    check(!tq::shadow::validateBasisCallForTest((HMODULE)image),
          "reject a light-basis call with an unexpected target");
    image[basisCallRva + 1] ^= 1;

    image[references[6].rva + 3] ^= 1;
    check(!tq::shadow::validateSupportedImageForTest((HMODULE)image),
          "reject a near-match crop instruction");
    image[references[6].rva + 3] ^= 1;

    uint32_t wrong = cropAddress + 4;
    memcpy(image + references[10].rva + 4, &wrong, sizeof(wrong));
    check(!tq::shadow::validateSupportedImageForTest((HMODULE)image),
          "reject a crop read pointing at an unexpected constant");
    memcpy(image + references[10].rva + 4, &cropAddress, sizeof(cropAddress));

    nt->OptionalHeader.SizeOfImage = (DWORD)imageSize + 0x1000;
    check(!tq::shadow::validateSupportedImageForTest((HMODULE)image),
          "reject an Engine image of unexpected size");

    VirtualFree(image, 0, MEM_RELEASE);
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

// The fitted light camera as RenderDirectional lays it out on its stack. The
// offsets are the contract between this test and shadow_fix.cpp; keeping them
// spelled out here means a layout mistake fails the build rather than the game.
struct FitCameraImage {
    int32_t type;       // +0x00
    float basis[9];     // +0x04
    float position[3];  // +0x28
    float reserved;     // +0x34
    float extentRow0;   // +0x38
    float extentRow1;   // +0x3c
    float nearDepth;    // +0x40
    float farDepth;     // +0x44
};

FitCameraImage makeFit(float x, float y, float z, float e0, float e1) {
    FitCameraImage fit = {};
    fit.type = 1;
    fit.basis[0] = 1.0f; fit.basis[4] = 1.0f; fit.basis[8] = 1.0f;
    fit.position[0] = x; fit.position[1] = y; fit.position[2] = z;
    fit.extentRow0 = e0;
    fit.extentRow1 = e1;
    fit.farDepth = 300.0f;
    return fit;
}

void testShadowBasisReference() {
    const float fallback[3] = {9.0f, 9.0f, 9.0f};

    // A light mostly along -Y must be crossed with a world axis it is least
    // aligned with, or the cross product degenerates and the basis collapses.
    const float steep[3] = {0.10f, -0.98f, 0.17f};
    const float* up = tq::shadow::chooseReferenceUpForTest(steep, fallback);
    check(up && up[0] == 1.0f && up[1] == 0.0f && up[2] == 0.0f,
          "the pinned reference picks the axis the light is least aligned with");

    const float acrossX[3] = {0.97f, -0.20f, 0.14f};
    up = tq::shadow::chooseReferenceUpForTest(acrossX, fallback);
    check(up && up[1] == 0.0f && up[2] == 1.0f && up[0] == 0.0f,
          "a light along X is crossed with a different axis");

    // Determinism is the whole point: the same light must produce the same
    // basis every frame or nothing downstream can be snapped to it.
    const float* again = tq::shadow::chooseReferenceUpForTest(steep, fallback);
    check(again && again[0] == 1.0f && again[1] == 0.0f && again[2] == 0.0f,
          "the pinned reference is a function of the light alone");

    const float zero[3] = {0.0f, 0.0f, 0.0f};
    check(tq::shadow::chooseReferenceUpForTest(zero, fallback) == fallback,
          "a degenerate light direction keeps the engine's own reference");
    check(tq::shadow::chooseReferenceUpForTest(nullptr, fallback) == fallback,
          "a missing light direction keeps the engine's own reference");
}

void testShadowFitStabilizer() {
    const unsigned texels = 1024;
    const unsigned steps = 8;

    FitCameraImage fit = makeFit(123.456f, 77.75f, 5.0f, 100.0f, 60.0f);
    const FitCameraImage original = fit;
    tq::shadow::stabilizeFitForTest(&fit, texels, steps);

    check(fit.extentRow0 >= original.extentRow0
              && fit.extentRow1 >= original.extentRow1,
          "quantising the fit never shrinks the box below the tight fit");

    const double t0 = (double)fit.extentRow0 / texels;
    const double t1 = (double)fit.extentRow1 / texels;
    const double p0 = fit.position[0] / t0;
    const double p1 = fit.position[1] / t1;
    check(fabs(p0 - floor(p0 + 0.5)) < 1.0e-3
              && fabs(p1 - floor(p1 + 0.5)) < 1.0e-3,
          "the stabilised centre lands on the shadow map texel grid");

    // Snapping moves the box, so the quantised extent has to carry at least
    // that much slack or the fit would stop covering what it enclosed.
    check(fit.extentRow0 >= original.extentRow0 + 2.0 * t0
              && fit.extentRow1 >= original.extentRow1 + 2.0 * t1,
          "the quantised box still covers the tight fit after snapping");
    check(fabs(fit.position[0] - original.position[0]) <= t0 + 1.0e-4
              && fabs(fit.position[1] - original.position[1]) <= t1 + 1.0e-4,
          "snapping moves the centre by less than one texel");
    check(fit.position[2] == original.position[2],
          "snapping leaves the depth axis alone");

    // The point of the exercise: a camera that has crept a fraction of a texel
    // must produce the same grid, which is what stops shadow edges crawling.
    FitCameraImage crept = makeFit(123.456f + (float)(t0 * 0.3),
                                   77.75f + (float)(t1 * 0.4), 5.0f,
                                   100.0f, 60.0f);
    tq::shadow::stabilizeFitForTest(&crept, texels, steps);
    check(crept.extentRow0 == fit.extentRow0 && crept.extentRow1 == fit.extentRow1,
          "a sub-texel camera creep does not change the fitted extents");
    const double shift0 = (crept.position[0] - fit.position[0]) / t0;
    const double shift1 = (crept.position[1] - fit.position[1]) / t1;
    check(fabs(shift0 - floor(shift0 + 0.5)) < 1.0e-3
              && fabs(shift1 - floor(shift1 + 0.5)) < 1.0e-3,
          "a sub-texel camera creep moves the grid by whole texels only");

    // A basis the projection would not read as light-space axes is left alone
    // rather than moved onto a grid that is not where the texels are.
    FitCameraImage skewed = makeFit(123.456f, 77.75f, 5.0f, 100.0f, 60.0f);
    skewed.basis[0] = 2.0f;
    const FitCameraImage skewedBefore = skewed;
    tq::shadow::stabilizeFitForTest(&skewed, texels, steps);
    check(!memcmp(&skewed, &skewedBefore, sizeof(skewed)),
          "a non-orthonormal light basis is left untouched");

    FitCameraImage perspective = makeFit(123.456f, 77.75f, 5.0f, 100.0f, 60.0f);
    perspective.type = 0;
    const FitCameraImage perspectiveBefore = perspective;
    tq::shadow::stabilizeFitForTest(&perspective, texels, steps);
    check(!memcmp(&perspective, &perspectiveBefore, sizeof(perspective)),
          "a camera that is not the orthographic fit is left untouched");

    FitCameraImage degenerate = makeFit(1.0f, 2.0f, 3.0f, 0.0f, 60.0f);
    const FitCameraImage degenerateBefore = degenerate;
    tq::shadow::stabilizeFitForTest(&degenerate, texels, steps);
    check(!memcmp(&degenerate, &degenerateBefore, sizeof(degenerate)),
          "a degenerate fit extent is left untouched");
}

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
    testRendererPresentHook();
    testBloomHook();
    testGrassProbe();
    testGrassPointerIndex();
    testGrassCrossed();
    testBloomExtraction();
    testBloomShaders();
    testShadowSplitRedirect();
    testShadowFitStabilizer();
    testShadowBasisReference();

    tq::hdr::Settings defaultHdr = tq::hdr::readSettings();
    check(!defaultHdr.requestHdr && defaultHdr.toneMap == tq::hdr::ToneOriginal
          && defaultHdr.paperWhiteNits == 203.0f
          && defaultHdr.peakNitsOverride == 0.0f
          && !defaultHdr.debug && !defaultHdr.trace,
          "HDR defaults to off/original/203 nits with diagnostics disabled");

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
            UINT scale = shadowSizes[i] >= 2048 ? kShadowScale : kPointShadowScale;
            UINT expected = shadowSizes[i] * scale;
            while (expected > 8192) expected /= 2;
            allShadowSizes &= SUCCEEDED(textureResult) && texture
                           && actual.Width == expected
                           && actual.Height == expected;
            if (texture) texture->Release();
        }
        check(allShadowSizes,
              "enhanced shadows scale Low/Medium/High map dimensions");

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
            check(viewportCount == 1
                      && observed.Width == 512.0f * kPointShadowScale
                      && observed.Height == 512.0f * kPointShadowScale,
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
            check(reboundGrade == gradeShader,
                  "the original color-grading pass remains active by default");
            if (reboundGrade) reboundGrade->Release();
            context->PSSetShader(gammaShader, nullptr, 0);
            ID3D11PixelShader* reboundGamma = nullptr;
            context->PSGetShader(&reboundGamma, nullptr, nullptr);
            check(reboundGamma == gammaShader,
                  "the original gamma pass remains active by default");
            if (reboundGamma) reboundGamma->Release();
        } else {
            check(false, "retain the exact color-grading pass");
            check(false, "retain the exact gamma pass");
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

        // The deferred screen-space receiver is the shader Titan Quest
        // actually uses to apply directional shadows. Its taps must be
        // retuned, and no other shader's may be: the per-material receivers
        // and the point-light one share the tap shape but were not widened.
        long deferredSize = 0;
        void* deferredBytes = readFile(
            "C:\\tqflicker-selftest\\tq-dxbc-PS-deferred-shadow.dxbc", &deferredSize);
        check(deferredBytes && deferredSize > 0,
              "read the captured deferred shadow receiver");
        tq::dxbc::PatchResult tuned = {};
        bool retuned = deferredBytes && tq::dxbc::tuneDeferredShadowFilter(
            deferredBytes, (SIZE_T)deferredSize, 0.38f, 0.695f, true, &tuned);
        check(retuned && tuned.size == (SIZE_T)deferredSize,
              "retune the deferred receiver's PCF taps in place");
        if (retuned && device) {
            ID3D11PixelShader* shader = nullptr;
            HRESULT hr = device->CreatePixelShader(tuned.data, tuned.size,
                                                   nullptr, &shader);
            check(SUCCEEDED(hr) && shader,
                  "DXMT accepts the retuned deferred receiver");
            if (shader) shader->Release();
        }
        tq::dxbc::release(&tuned);

        tq::dxbc::PatchResult widened = {};
        check(deferredBytes && !tq::dxbc::tuneDeferredShadowFilter(
                  deferredBytes, (SIZE_T)deferredSize, 1.5f, 1.0f, true, &widened),
              "refuse an offset scale that would widen the blur");
        tq::dxbc::PatchResult loosened = {};
        check(deferredBytes && !tq::dxbc::tuneDeferredShadowFilter(
                  deferredBytes, (SIZE_T)deferredSize, 0.38f, 1.5f, true, &loosened),
              "refuse a bias scale that would loosen the depth test");
        tq::dxbc::release(&loosened);
        tq::dxbc::release(&widened);

        tq::dxbc::PatchResult legacyTuned = {};
        check(shadowBytes && !tq::dxbc::tuneDeferredShadowFilter(
                  shadowBytes, (SIZE_T)shadowSize, 0.38f, 0.695f, true, &legacyTuned),
              "leave a per-material receiver's taps untouched");
        tq::dxbc::release(&legacyTuned);
        free(deferredBytes);
        free(shadowBytes);
    }

    testTimestampCapability(device, context);
    testProbe(device, context);
    testFrameOverlay(device, context);

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
    check(GetFileAttributesA("tqflicker-debug.log") == INVALID_FILE_ATTRIBUTES,
          "startup tracing creates no file when trace is absent");
    check(GetFileAttributesA("tqflicker-frames.csv") == INVALID_FILE_ATTRIBUTES,
          "the probe creates no CSV when probe is absent");
    fprintf(g_report, "\nRESULT: %d failure(s)\n", g_failures);
    fclose(g_report);
    return g_failures ? 1 : 0;
}
