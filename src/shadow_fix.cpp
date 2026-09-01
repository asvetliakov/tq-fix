#include "shadow_fix.h"

#include "hdr.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace tq {
namespace shadow {
namespace {

const DWORD kEngineImageSize = 0x0044b000u;
const DWORD kNativeCropRva = 0x002f9550u;

// The native ray parameter, and the split that suits a wide display once the
// shadow map is enlarged to match. Coverage scales as split^1.90.
const float kNativeSplit = 0.325f;
const float kDefaultSplit = 0.45f;
const float kCoverageExponent = 1.90f;

struct CropReference {
    DWORD rva;
    BYTE prefix[4];
};

// Every read of Engine.dll's 0.325 directional crop inside
// GraphicsShadowMapDx11::RenderDirectional. The operand is redirected rather
// than the shared constant, so point shadows and unrelated engine code keep
// reading the original value.
const CropReference kCropReferences[] = {
    {0x0018e40du, {0xf3, 0x0f, 0x59, 0x15}},
    {0x0018e42eu, {0xf3, 0x0f, 0x59, 0x0d}},
    {0x0018e446u, {0xf3, 0x0f, 0x59, 0x05}},
    {0x0018e503u, {0xf3, 0x0f, 0x59, 0x15}},
    {0x0018e51bu, {0xf3, 0x0f, 0x59, 0x0d}},
    {0x0018e533u, {0xf3, 0x0f, 0x59, 0x05}},
    {0x0018e5ddu, {0xf3, 0x0f, 0x59, 0x15}},
    {0x0018e609u, {0xf3, 0x0f, 0x59, 0x0d}},
    {0x0018e618u, {0xf3, 0x0f, 0x59, 0x05}},
    {0x0018e6fcu, {0xf3, 0x0f, 0x10, 0x05}},
    {0x0018f556u, {0xf3, 0x0f, 0x10, 0x0d}},
};

const unsigned kCropCount = sizeof(kCropReferences) / sizeof(kCropReferences[0]);

struct CropPatch {
    BYTE* operand;
    uint32_t original;
    uint32_t replacement;
    bool installed;
};

CropPatch g_cropPatches[kCropCount];
LONG g_installAttempted;
float g_split = kDefaultSplit;

bool readable(const void* address, SIZE_T bytes) {
    MEMORY_BASIC_INFORMATION info = {};
    if (!address || !bytes || !VirtualQuery(address, &info, sizeof(info)))
        return false;
    const DWORD protection = info.Protect & 0xff;
    const BYTE* begin = (const BYTE*)address;
    const BYTE* end = begin + bytes;
    const BYTE* regionEnd = (const BYTE*)info.BaseAddress + info.RegionSize;
    return info.State == MEM_COMMIT && !(info.Protect & PAGE_GUARD)
        && protection != PAGE_NOACCESS && end >= begin && end <= regionEnd;
}

bool belongsTo(HMODULE module, const void* address) {
    MEMORY_BASIC_INFORMATION info = {};
    return module && address && VirtualQuery(address, &info, sizeof(info))
        && info.AllocationBase == module;
}

void iniPath(wchar_t path[MAX_PATH]) {
    path[0] = 0;
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) { path[0] = 0; return; }
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) { path[0] = 0; return; }
    lstrcpyW(slash + 1, L"tqflicker.ini");
}

float readFloat(const wchar_t* key, float fallback, float low, float high) {
    wchar_t path[MAX_PATH];
    iniPath(path);
    if (!path[0]) return fallback;
    wchar_t value[32];
    if (!GetPrivateProfileStringW(L"graphics", key, L"", value, 32, path)
        || !value[0])
        return fallback;
    float parsed = (float)_wtof(value);
    if (!_finite(parsed) || parsed < low || parsed > high) return fallback;
    return parsed;
}

float configuredSplit() {
    return readFloat(L"shadow_split", kDefaultSplit, 0.15f, 0.95f);
}

bool validateEngineImage(HMODULE module) {
    BYTE* base = (BYTE*)module;
    if (!readable(base, sizeof(IMAGE_DOS_HEADER))) return false;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const IMAGE_NT_HEADERS* nt =
        (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (!readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE
        || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC
        || nt->OptionalHeader.SizeOfImage != kEngineImageSize)
        return false;

    const uint32_t native = (uint32_t)(uintptr_t)(base + kNativeCropRva);
    for (unsigned i = 0; i < kCropCount; ++i) {
        const BYTE* instruction = base + kCropReferences[i].rva;
        uint32_t operand = 0;
        if (!belongsTo(module, instruction) || !readable(instruction, 8)
            || memcmp(instruction, kCropReferences[i].prefix, 4))
            return false;
        memcpy(&operand, instruction + 4, sizeof(operand));
        if (operand != native) return false;
    }
    return true;
}

bool writeProtected(void* address, const void* expected,
                    const void* replacement, SIZE_T bytes) {
    if (!address || !expected || !replacement || !readable(address, bytes)
        || memcmp(address, expected, bytes))
        return false;
    DWORD oldProtection = 0;
    if (!VirtualProtect(address, bytes, PAGE_EXECUTE_READWRITE, &oldProtection))
        return false;
    memcpy(address, replacement, bytes);
    FlushInstructionCache(GetCurrentProcess(), address, bytes);
    DWORD ignored = 0;
    VirtualProtect(address, bytes, oldProtection, &ignored);
    return true;
}

bool installCropPatches(HMODULE module) {
    const uint32_t native = (uint32_t)(uintptr_t)((BYTE*)module + kNativeCropRva);
    const uint32_t redirected = (uint32_t)(uintptr_t)&g_split;
    for (unsigned i = 0; i < kCropCount; ++i) {
        CropPatch& patch = g_cropPatches[i];
        patch.operand = (BYTE*)module + kCropReferences[i].rva + 4;
        patch.original = native;
        patch.replacement = redirected;
        if (!writeProtected(patch.operand, &patch.original,
                            &patch.replacement, sizeof(uint32_t)))
            return false;
        patch.installed = true;
    }
    return true;
}

void restoreCropPatches() {
    for (unsigned i = 0; i < kCropCount; ++i) {
        CropPatch& patch = g_cropPatches[i];
        if (patch.installed && patch.operand)
            writeProtected(patch.operand, &patch.replacement,
                           &patch.original, sizeof(uint32_t));
        memset(&patch, 0, sizeof(patch));
    }
}

}  // namespace

void install(HMODULE engineModule) {
    if (!engineModule || InterlockedCompareExchange(&g_installAttempted, 1, 0))
        return;
    float split = configuredSplit();
    if (fabsf(split - kNativeSplit) < 1.0e-4f) {
        tq::hdr::log("Directional shadow split left at the native %.3f\r\n",
                     kNativeSplit);
        return;
    }
    if (!validateEngineImage(engineModule)) {
        tq::hdr::log("Directional shadow split skipped: unsupported Engine.dll\r\n");
        return;
    }
    g_split = split;
    if (!installCropPatches(engineModule)) {
        restoreCropPatches();
        tq::hdr::log("Directional shadow split skipped: operand redirect failed\r\n");
        return;
    }
    tq::hdr::log("Directional shadow split: %.3f (native %.3f, about %.2fx the"
                 " world coverage)\r\n",
                 split, kNativeSplit,
                 powf(split / kNativeSplit, kCoverageExponent));
}

float blurCompensation() {
    float split = configuredSplit();
    float automatic = powf(kNativeSplit / split, kCoverageExponent);
    float configured = readFloat(L"shadow_blur_scale", automatic, 0.05f, 1.0f);
    if (!_finite(configured) || configured <= 0.0f) return 1.0f;
    return configured > 1.0f ? 1.0f : configured;
}

bool cornerFilterEnabled() {
    wchar_t path[MAX_PATH];
    iniPath(path);
    if (!path[0]) return true;
    wchar_t value[32];
    GetPrivateProfileStringW(L"graphics", L"shadow_filter", L"corners",
                             value, 32, path);
    return _wcsicmp(value, L"cross") != 0;
}

void shutdown() {
    restoreCropPatches();
    g_split = kDefaultSplit;
    InterlockedExchange(&g_installAttempted, 0);
}

#ifdef TQ_SELFTEST
bool validateSupportedImageForTest(HMODULE engineModule) {
    return validateEngineImage(engineModule);
}

bool redirectCropRoundTripForTest(HMODULE engineModule) {
    if (!installCropPatches(engineModule)) {
        restoreCropPatches();
        return false;
    }
    bool redirected = true;
    const uint32_t expected = (uint32_t)(uintptr_t)&g_split;
    for (unsigned i = 0; i < kCropCount; ++i) {
        uint32_t actual = 0;
        memcpy(&actual, g_cropPatches[i].operand, sizeof(actual));
        redirected = redirected && actual == expected;
    }
    restoreCropPatches();
    return redirected && validateEngineImage(engineModule);
}
#endif

}  // namespace shadow
}  // namespace tq
