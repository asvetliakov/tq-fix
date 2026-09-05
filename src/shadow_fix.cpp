#include "shadow_fix.h"

#include "probe.h"

#include "hdr.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
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
// Both fitted from the same two measured projections (0.325 and 1.05). The
// box does not grow uniformly: horizontally it scales as split^1.90 while its
// depth range scales as split^1.12. Two points is a fit, not a law -- both
// factors are overridable.
const float kCoverageExponent = 1.90f;
const float kDepthExponent = 1.12f;

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

// ----------------------------------------------------------- fit stabilization

// RenderDirectional refits the directional projection from scratch every frame
// around the camera-frustum corners, with no texel snapping. The fitted box
// therefore lands at a different sub-texel phase each frame, and every shadow
// edge crawls for as long as the camera keeps moving.
//
// Snapping the centre is necessary but not sufficient. A 240-frame in-game
// capture across a zoom showed the extents varying by under 1% while the light
// basis rotated by up to 0.88 degrees per frame -- see the basis pin below,
// which is the dominant term. Snapping without it stabilises nothing.
//
// The fit ends by building one stack-resident orthographic camera and passing
// it to a Camera setup routine. Both the caster projection and the receiver's
// world-to-shadow matrix are derived from that struct afterwards, so adjusting
// it once, before the setup call, keeps what is rasterised and what is sampled
// in agreement. The mod retargets that single call to a thunk that adjusts the
// camera and then tail-jumps to the original routine with the stack untouched.
const DWORD kFitCameraCallRva = 0x0018ec69u;
const DWORD kFitCameraSetupRva = 0x00123e30u;
const BYTE kFitCameraCall[5] = {0xe8, 0xc2, 0x51, 0xf9, 0xff};

// The fit builds its light-space basis with a look-at helper: the light
// direction is normalised into row 2, then crossed with a caller-supplied
// reference vector to produce rows 0 and 1. That reference tracks the camera,
// and Titan Quest pitches the camera as it zooms, so the whole texel grid
// rotates -- measured at up to 0.88 degrees per frame, which slides the grid
// tens of texels at the edge of the box no matter how large the map is.
// Neither snapping nor resolution can survive that, so the reference is
// replaced with a fixed world axis and the basis becomes a function of the
// light alone. The box is then looser than the camera-aligned fit, which is
// the standard price of a stable shadow map.
const DWORD kBasisCallRva = 0x0018e7fau;
const DWORD kBasisBuilderRva = 0x00283df0u;
const BYTE kBasisCall[5] = {0xe8, 0xf1, 0x55, 0x0f, 0x00};

struct FitCamera {
    int32_t type;       // +0x00  1 = orthographic
    float basis[9];     // +0x04  three orthonormal world-space rows
    float position[3];  // +0x28  box centre pushed back along the light
    float reserved;     // +0x34
    float extentRow0;   // +0x38  full width measured along basis row 0
    float extentRow1;   // +0x3c  full width measured along basis row 1
    float nearDepth;    // +0x40
    float farDepth;     // +0x44
};

static_assert(offsetof(FitCamera, position) == 0x28, "camera position offset");
static_assert(offsetof(FitCamera, extentRow0) == 0x38, "camera extent offset");
static_assert(offsetof(FitCamera, farDepth) == 0x44, "camera far offset");

struct CallPatch {
    BYTE* displacement;
    uint32_t original;
    uint32_t replacement;
    bool installed;
};

CallPatch g_fitCall;
CallPatch g_basisCall;
// One page holding both thunks, allocated once and never released: a shutdown
// that freed it could unmap the page while another thread was still executing
// inside it.
BYTE* g_thunks;
const unsigned kFitThunkOffset = 0;
const unsigned kBasisThunkOffset = 64;
float g_referenceUp[3];
bool g_stabilizeBasis = true;
LONG g_basisLogged;
// The directional map's texel count, which is the grid the fit is snapped
// onto, and the smallest map seen as a fallback for the lowest shadow quality
// where no request is large enough to be classified directional.
unsigned g_directionalTexels;
unsigned g_smallestTexels;

// The smallest shadow map the texture hook has created. A snap grid coarser
// than a texel still aligns the texel grid exactly, because every size the
// game requests and every scale it is given are powers of two; a grid *finer*
// than a texel would snap to half-texel positions and stabilise nothing. So
// the minimum is the safe choice, and the default is the smallest map the game
// ever asks for, in case a fit runs before any map exists.
unsigned g_mapTexels;   // 0 until a shadow map has actually been created
unsigned g_stabilizeSteps = 8;
LONG g_stabilizeLogged;
LONG g_noMapLogged;
// The quantised extent last fitted, so a step across a threshold can be counted.
double g_lastExtent0, g_lastExtent1;

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

// The one dial that is not a [graphics] setting: a debug key, kept beside the
// rest of the contact configuration rather than in a second reader.
bool readDebugFlag(const wchar_t* key) {
    wchar_t path[MAX_PATH];
    iniPath(path);
    if (!path[0]) return false;
    return GetPrivateProfileIntW(L"debug", key, 0, path) != 0;
}

bool readSwitch(const wchar_t* key, const wchar_t* fallback, const wchar_t* off) {
    wchar_t path[MAX_PATH];
    iniPath(path);
    if (!path[0]) return _wcsicmp(fallback, off) != 0;
    wchar_t value[32];
    GetPrivateProfileStringW(L"graphics", key, fallback, value, 32, path);
    return _wcsicmp(value, off) != 0;
}

// Read once and kept. The compensations below are queried for every pixel
// shader the game creates, which is hundreds of times across a session, and
// each query used to reopen the file several times over. Caching also means a
// mid-session edit cannot leave shaders patched with two different settings.
struct Config {
    bool shadows;
    float split;
    bool corners;
    bool stabilize;
    bool stabilizeBasis;
    unsigned steps;
    float blurScale;
    float biasScale;
    ContactSettings contact;
};

Config g_config;
bool g_configLoaded;

float clampScale(float value) {
    if (!_finite(value) || value <= 0.0f) return 1.0f;
    return value > 1.0f ? 1.0f : value;
}

const Config& config() {
    if (g_configLoaded) return g_config;
    g_configLoaded = true;
    g_config.shadows = readSwitch(L"shadows", L"enhanced", L"original");
    g_config.corners = readSwitch(L"shadow_filter", L"corners", L"cross");
    g_config.split = g_config.shadows
        ? readFloat(L"shadow_split", kDefaultSplit, 0.15f, 0.95f)
        : kNativeSplit;
    g_config.stabilize =
        g_config.shadows && readSwitch(L"shadow_stabilize", L"on", L"off");
    g_config.stabilizeBasis =
        readSwitch(L"shadow_stabilize_basis", L"on", L"off");
    g_config.steps =
        (unsigned)readFloat(L"shadow_stabilize_steps", 8.0f, 1.0f, 64.0f);
    if (!g_config.steps) g_config.steps = 8;

    // Both compensations default to whatever holds the native look at the
    // configured split, and both are overridable.
    float blur = powf(kNativeSplit / g_config.split, kCoverageExponent);
    // Corner taps sit at (+/-r, +/-r), so they reach sqrt(2) further from the
    // centre than the native cross at the same offset. Pull them in to keep
    // the filter footprint the same size whichever placement is used.
    if (g_config.corners) blur /= 1.41421356f;
    const float bias = powf(kNativeSplit / g_config.split, kDepthExponent);
    g_config.blurScale =
        clampScale(readFloat(L"shadow_blur_scale", blur, 0.05f, 1.0f));
    g_config.biasScale =
        clampScale(readFloat(L"shadow_bias_scale", bias, 0.05f, 1.0f));
    if (!g_config.shadows) {
        g_config.blurScale = 1.0f;
        g_config.biasScale = 1.0f;
    }

    // Contact shadows are independent of the split: they add detail the map
    // cannot resolve at any coverage, so they are not disabled by
    // shadows=original. Enabled with the gameplay-approved normal-zoom profile.
    g_config.contact.enabled = readSwitch(L"shadow_contact", L"on", L"off");
    g_config.contact.steps =
        (unsigned)readFloat(L"shadow_contact_steps", 12.0f, 4.0f, 16.0f);
    // Gameplay-approved reach for the fixed, elevated camera. Twelve taps keep
    // samples close as the ray reaches farther around vegetation and props.
    // Hidden geometry still cannot contribute to a screen-space ray.
    g_config.contact.length =
        readFloat(L"shadow_contact_length", 0.35f, 0.02f, 8.0f);
    // Both in world units, not NDC. A constant NDC bias is unusable here: the
    // same 0.0005 is 0.046 world units ten units from the camera and 0.80 at
    // forty, which is wider than the whole march, so the effect would die with
    // distance. The shader linearises the depth instead.
    g_config.contact.bias =
        readFloat(L"shadow_contact_bias", 0.012f, 0.0f, 1.0f);
    g_config.contact.thickness =
        readFloat(L"shadow_contact_thickness", 0.30f, 0.01f, 8.0f);
    g_config.contact.strength =
        readFloat(L"shadow_contact_strength", 0.70f, 0.0f, 1.0f);
    g_config.contact.upright =
        readFloat(L"shadow_contact_upright", 0.0f, -1.0f, 1.0f);
    g_config.contact.toggle = readDebugFlag(L"shadow_contact_toggle");
    g_config.contact.timing = readDebugFlag(L"shadow_contact_timing");
    return g_config;
}

bool shadowsEnabled() { return config().shadows; }

float configuredSplit() { return config().split; }

bool stabilizeEnabled() { return config().stabilize; }

bool validatePeImage(HMODULE module) {
    BYTE* base = (BYTE*)module;
    if (!readable(base, sizeof(IMAGE_DOS_HEADER))) return false;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const IMAGE_NT_HEADERS* nt =
        (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    return readable(nt, sizeof(*nt)) && nt->Signature == IMAGE_NT_SIGNATURE
        && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386
        && nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC
        && nt->OptionalHeader.SizeOfImage == kEngineImageSize;
}

bool validateCropSites(HMODULE module) {
    BYTE* base = (BYTE*)module;
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

// The calls are relative, so their five bytes are the same whatever the image
// is loaded at and can be compared literally.
bool validateCall(HMODULE module, DWORD rva, const BYTE expected[5]) {
    const BYTE* site = (const BYTE*)module + rva;
    return belongsTo(module, site) && readable(site, 5)
        && !memcmp(site, expected, 5);
}

bool validateFitCameraCall(HMODULE module) {
    return validateCall(module, kFitCameraCallRva, kFitCameraCall);
}

bool validateBasisCall(HMODULE module) {
    return validateCall(module, kBasisCallRva, kBasisCall);
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

double dot3(const float* a, const float* b) {
    return (double)a[0] * b[0] + (double)a[1] * b[1] + (double)a[2] * b[2];
}

// Rounds up to the next 1/steps of an octave. Snapping the centre onto a grid
// whose spacing changed every frame would stabilise nothing, so the extent has
// to be piecewise constant: quantising it this way costs at most 9% coverage
// at the default eight steps and turns a continuous crawl into one brief pop
// each time a zoom crosses a threshold.
double quantizeExtent(double extent, unsigned steps) {
    const double octave = log(extent) / 0.69314718055994531;
    return pow(2.0, ceil(octave * steps) / (double)steps);
}

void __stdcall stabilizeFit(FitCamera* camera) {
    if (!camera || camera->type != 1) return;
    const double e0 = camera->extentRow0;
    const double e1 = camera->extentRow1;
    if (!_finite(e0) || !_finite(e1) || !(e0 > 0.0) || !(e1 > 0.0)) return;

    // The projection reads these rows as the light-space axes. A scaled or
    // skewed basis would put the snap grid somewhere other than where the
    // texels are, so leave such a fit alone rather than move it wrongly.
    const float* r0 = camera->basis;
    const float* r1 = camera->basis + 3;
    if (fabs(dot3(r0, r0) - 1.0) > 1.0e-3 || fabs(dot3(r1, r1) - 1.0) > 1.0e-3
        || fabs(dot3(r0, r1)) > 1.0e-3)
        return;

    // A snapped centre moves by up to one texel, so the box has to carry that
    // much slack or it would stop covering what the tight fit enclosed.
    const double slack = 1.01;
    const double q0 = quantizeExtent(e0, g_stabilizeSteps) * slack;
    const double q1 = quantizeExtent(e1, g_stabilizeSteps) * slack;
    if (!_finite(q0) || !_finite(q1) || q0 < e0 || q1 < e1) return;
    // No map reported yet: leave the engine's own fit alone rather than snap
    // to a guess. Shadow maps are created before the first frame is drawn, so
    // this is a startup window of at most a frame or two -- unless the size
    // hook never installed at all, which the one-shot line below makes
    // visible instead of letting the shimmer return silently.
    if (!g_mapTexels) {
        if (InterlockedCompareExchange(&g_noMapLogged, 1, 0) == 0)
            tq::hdr::log("Shadow fit left native: no shadow map size"
                         " reported\r\n");
        return;
    }
    const double t0 = q0 / (double)g_mapTexels;
    const double t1 = q1 / (double)g_mapTexels;
    if (!(t0 > 0.0) || !(t1 > 0.0)) return;

    // Done in double: world coordinates are large enough that a float quotient
    // would lose a useful fraction of a texel before it was floored.
    const double p0 = dot3(camera->position, r0);
    const double p1 = dot3(camera->position, r1);
    const double d0 = floor(p0 / t0) * t0 - p0;
    const double d1 = floor(p1 / t1) * t1 - p1;
    if (!_finite(d0) || !_finite(d1)) return;

    for (int i = 0; i < 3; ++i)
        camera->position[i] =
            (float)((double)camera->position[i] + r0[i] * d0 + r1[i] * d1);
    // A step across a quantisation threshold re-fits the whole map against a
    // different caster set, so the frame it happens in is worth marking.
    if (q0 != g_lastExtent0 || q1 != g_lastExtent1) {
        g_lastExtent0 = q0;
        g_lastExtent1 = q1;
        tq::probe::count(tq::probe::CounterShadowFitChange);
    }
    camera->extentRow0 = (float)q0;
    camera->extentRow1 = (float)q1;

    if (InterlockedCompareExchange(&g_stabilizeLogged, 1, 0) == 0)
        tq::hdr::log("Shadow fit stabilised: extents %.2f x %.2f -> %.2f x %.2f,"
                     " texel %.4f over %u\r\n",
                     e0, e1, q0, q1, t0, g_mapTexels);
}

// Returns a world axis to cross the light direction with, choosing the one the
// light is least aligned with so the cross product never degenerates. It is a
// function of the light alone, so for a static directional light the basis it
// produces is the same every frame. `fallback` is the engine's own reference,
// returned unchanged whenever the direction cannot be trusted.
const float* __stdcall chooseReferenceUp(const float* direction,
                                         const float* fallback) {
    if (!direction) return fallback;
    const double x = direction[0], y = direction[1], z = direction[2];
    if (!_finite(x) || !_finite(y) || !_finite(z)) return fallback;
    const double length = sqrt(x * x + y * y + z * z);
    if (!(length > 1.0e-6) || !_finite(length)) return fallback;
    const double ax = fabs(x) / length, ay = fabs(y) / length,
                 az = fabs(z) / length;
    const unsigned axis = ax <= ay && ax <= az ? 0u : (ay <= az ? 1u : 2u);
    g_referenceUp[0] = g_referenceUp[1] = g_referenceUp[2] = 0.0f;
    g_referenceUp[axis] = 1.0f;
    if (InterlockedCompareExchange(&g_basisLogged, 1, 0) == 0)
        tq::hdr::log("Shadow basis pinned: light %.4f,%.4f,%.4f -> world axis"
                     " %u\r\n", x / length, y / length, z / length, axis);
    return g_referenceUp;
}

BYTE* buildThunk(HMODULE module) {
    if (!g_thunks)
        g_thunks = (BYTE*)VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE);
    if (!g_thunks) return nullptr;
    memset(g_thunks, 0xcc, 128);

    // Fit thunk. ecx holds the camera pointer at the retargeted call and the
    // routine's five stack arguments are already in place, so preserve ecx,
    // pass a copy to the stabiliser, and tail-jump with the stack untouched.
    BYTE* fit = g_thunks + kFitThunkOffset;
    const uintptr_t stabilizer = (uintptr_t)&stabilizeFit;
    const uintptr_t setup = (uintptr_t)((BYTE*)module + kFitCameraSetupRva);
    int32_t rel = (int32_t)(stabilizer - ((uintptr_t)fit + 7));
    fit[0] = 0x51;  // push ecx
    fit[1] = 0x51;  // push ecx
    fit[2] = 0xe8;  // call stabilizeFit (__stdcall: it pops the argument)
    memcpy(fit + 3, &rel, sizeof(rel));
    fit[7] = 0x59;  // pop ecx
    fit[8] = 0xe9;  // jmp Camera setup
    rel = (int32_t)(setup - ((uintptr_t)fit + 13));
    memcpy(fit + 9, &rel, sizeof(rel));

    // Basis thunk. The look-at builder is cdecl with the light direction and
    // the reference vector on the stack; ecx and edx carry the output basis
    // and are preserved. Only the reference argument is rewritten, in place,
    // so the engine still derives the light axis exactly as it always did.
    BYTE* basis = g_thunks + kBasisThunkOffset;
    const uintptr_t chooser = (uintptr_t)&chooseReferenceUp;
    const uintptr_t builder = (uintptr_t)((BYTE*)module + kBasisBuilderRva);
    const BYTE prologue[] = {
        0x50,                          // push eax
        0x51,                          // push ecx
        0x52,                          // push edx
        0xff, 0x74, 0x24, 0x14,        // push [esp+0x14]  reference (fallback)
        0xff, 0x74, 0x24, 0x14,        // push [esp+0x14]  light direction
    };
    memcpy(basis, prologue, sizeof(prologue));
    unsigned at = sizeof(prologue);
    basis[at] = 0xe8;                  // call chooseReferenceUp (__stdcall)
    rel = (int32_t)(chooser - ((uintptr_t)basis + at + 5));
    memcpy(basis + at + 1, &rel, sizeof(rel));
    at += 5;
    const BYTE epilogue[] = {
        0x89, 0x44, 0x24, 0x14,        // mov [esp+0x14],eax  replace reference
        0x5a,                          // pop edx
        0x59,                          // pop ecx
        0x58,                          // pop eax
    };
    memcpy(basis + at, epilogue, sizeof(epilogue));
    at += sizeof(epilogue);
    basis[at] = 0xe9;                  // jmp the look-at builder
    rel = (int32_t)(builder - ((uintptr_t)basis + at + 5));
    memcpy(basis + at + 1, &rel, sizeof(rel));

    FlushInstructionCache(GetCurrentProcess(), g_thunks, 128);
    return g_thunks;
}

bool retargetCall(HMODULE module, DWORD rva, BYTE* thunk, CallPatch* patch) {
    if (!thunk) return false;
    BYTE* site = (BYTE*)module + rva;
    uint32_t original = 0, replacement = 0;
    memcpy(&original, site + 1, sizeof(original));
    const int32_t rel = (int32_t)((uintptr_t)thunk - ((uintptr_t)site + 5));
    memcpy(&replacement, &rel, sizeof(replacement));
    if (!writeProtected(site + 1, &original, &replacement, sizeof(uint32_t)))
        return false;
    patch->displacement = site + 1;
    patch->original = original;
    patch->replacement = replacement;
    patch->installed = true;
    return true;
}

void restoreCall(CallPatch* patch) {
    if (patch->installed && patch->displacement)
        writeProtected(patch->displacement, &patch->replacement,
                       &patch->original, sizeof(uint32_t));
    memset(patch, 0, sizeof(*patch));
}

// Both take the already-built page rather than building it themselves:
// buildThunk clears the page before emitting, and clearing it while the other
// thunk is already the target of a live call would leave int3 in its path.
bool installStabilizer(HMODULE module, BYTE* thunks) {
    return retargetCall(module, kFitCameraCallRva, thunks + kFitThunkOffset,
                        &g_fitCall);
}

bool installBasisPin(HMODULE module, BYTE* thunks) {
    return retargetCall(module, kBasisCallRva, thunks + kBasisThunkOffset,
                        &g_basisCall);
}

void restoreStabilizer() {
    restoreCall(&g_basisCall);
    restoreCall(&g_fitCall);
}

void installSplit(HMODULE module) {
    const float split = configuredSplit();
    if (fabsf(split - kNativeSplit) < 1.0e-4f) {
        tq::hdr::log("Directional shadow split left at the native %.3f\r\n",
                     kNativeSplit);
        return;
    }
    if (!validateCropSites(module)) {
        tq::hdr::log("Directional shadow split skipped: unsupported Engine.dll\r\n");
        return;
    }
    g_split = split;
    if (!installCropPatches(module)) {
        restoreCropPatches();
        tq::hdr::log("Directional shadow split skipped: operand redirect failed\r\n");
        return;
    }
    tq::hdr::log("Directional shadow split: %.3f (native %.3f, about %.2fx the"
                 " world coverage)\r\n",
                 split, kNativeSplit,
                 powf(split / kNativeSplit, kCoverageExponent));
}

void installStabilization(HMODULE module) {
    if (!stabilizeEnabled()) {
        tq::hdr::log("Shadow fit stabilisation disabled by configuration\r\n");
        return;
    }
    if (!validateFitCameraCall(module)) {
        tq::hdr::log("Shadow fit stabilisation skipped: unsupported Engine.dll\r\n");
        return;
    }
    g_stabilizeSteps = config().steps;
    g_stabilizeBasis = config().stabilizeBasis;
    BYTE* thunks = buildThunk(module);
    if (!thunks || !installStabilizer(module, thunks)) {
        restoreStabilizer();
        tq::hdr::log("Shadow fit stabilisation skipped: call retarget failed\r\n");
        return;
    }
    // Pinning the basis is what makes snapping worth anything, but it is the
    // more invasive half, so it fails independently and says so.
    bool pinned = false;
    if (g_stabilizeBasis) {
        if (!validateBasisCall(module)) {
            tq::hdr::log("Shadow basis pinning skipped: unsupported Engine.dll\r\n");
        } else if (!installBasisPin(module, thunks)) {
            restoreCall(&g_basisCall);
            tq::hdr::log("Shadow basis pinning skipped: call retarget failed\r\n");
        } else {
            pinned = true;
        }
    }
    tq::hdr::log("Shadow fit stabilisation: on, %u extent steps per octave,"
                 " basis %s\r\n",
                 g_stabilizeSteps, pinned ? "pinned" : "camera-aligned");
}

}  // namespace

void install(HMODULE engineModule) {
    if (!engineModule || InterlockedCompareExchange(&g_installAttempted, 1, 0))
        return;
    if (!shadowsEnabled()) {
        tq::hdr::log("Directional shadows left original by configuration\r\n");
        return;
    }
    if (!validatePeImage(engineModule)) {
        tq::hdr::log("Directional shadow work skipped: unsupported Engine.dll\r\n");
        return;
    }
    installSplit(engineModule);
    installStabilization(engineModule);
}

void noteShadowMapSize(unsigned texels, bool directional) {
    if (texels < 256 || texels > 16384) return;
    // The grid to snap on is the directional map's own, because it is the
    // directional projection being fitted. Taking the smallest map instead --
    // which is what an initial value of 512 that could only ever shrink
    // amounted to -- snapped an 8192-texel map onto a 512-texel grid, sixteen
    // times coarser than the texels it was supposed to align with, so the
    // centre moved in sixteen-texel jumps and stabilised far less than it
    // looked like it did. A run reported "texel 0.0487 over 512" with the
    // directional map at 8192.
    //
    // The last directional map reported wins, not the largest: an in-session
    // shadow-quality change recreates the map at a new size, and holding on to
    // the old one would snap onto a grid finer than the live map's texels --
    // half-texel positions, which stabilise nothing.
    if (directional) {
        g_directionalTexels = texels;
    } else if (!g_smallestTexels || texels < g_smallestTexels) {
        g_smallestTexels = texels;
    }
    // Only fall back to the smallest map when no directional one was ever
    // classified, which happens at the lowest shadow quality.
    g_mapTexels = g_directionalTexels ? g_directionalTexels : g_smallestTexels;
}

void resetShadowMapSizes() {
    // The renderer is about to rebuild its shadow targets, so everything known
    // about their sizes is about to be stale.
    g_directionalTexels = g_smallestTexels = g_mapTexels = 0;
}

float blurCompensation() { return config().blurScale; }

// The receiver's depth bias is normalised to the fitted depth range, so a
// wider split scales it up in world units and shadows detach from their
// casters. The depth axis grows more slowly than the horizontal one --
// measured at split^1.12 against split^1.90 -- so it needs its own factor.
float biasCompensation() { return config().biasScale; }

bool cornerFilterEnabled() { return config().corners; }

const ContactSettings& contactSettings() { return config().contact; }

void shutdown() {
    restoreStabilizer();
    restoreCropPatches();
    g_split = kDefaultSplit;
    memset(g_referenceUp, 0, sizeof(g_referenceUp));
    g_configLoaded = false;
    InterlockedExchange(&g_stabilizeLogged, 0);
    InterlockedExchange(&g_basisLogged, 0);
    InterlockedExchange(&g_installAttempted, 0);
}

#ifdef TQ_SELFTEST
bool validateSupportedImageForTest(HMODULE engineModule) {
    return (validatePeImage(engineModule) && validateCropSites(engineModule));
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
    return redirected && (validatePeImage(engineModule) && validateCropSites(engineModule));
}

bool validateFitCameraCallForTest(HMODULE engineModule) {
    return validateFitCameraCall(engineModule);
}

bool validateBasisCallForTest(HMODULE engineModule) {
    return validateBasisCall(engineModule);
}

bool retargetFitCameraCallRoundTripForTest(HMODULE engineModule) {
    BYTE* thunks = buildThunk(engineModule);
    if (!thunks || !installStabilizer(engineModule, thunks)
        || !installBasisPin(engineModule, thunks)) {
        restoreStabilizer();
        return false;
    }
    const BYTE* fitSite = (const BYTE*)engineModule + kFitCameraCallRva;
    const BYTE* basisSite = (const BYTE*)engineModule + kBasisCallRva;
    int32_t fitRel = 0, basisRel = 0;
    memcpy(&fitRel, fitSite + 1, sizeof(fitRel));
    memcpy(&basisRel, basisSite + 1, sizeof(basisRel));
    const bool retargeted =
        fitSite[0] == 0xe8 && basisSite[0] == 0xe8
        && (const BYTE*)((uintptr_t)fitSite + 5 + fitRel)
               == g_thunks + kFitThunkOffset
        && (const BYTE*)((uintptr_t)basisSite + 5 + basisRel)
               == g_thunks + kBasisThunkOffset;
    restoreStabilizer();
    return retargeted && validateFitCameraCall(engineModule)
        && validateBasisCall(engineModule);
}

const float* chooseReferenceUpForTest(const float* direction,
                                      const float* fallback) {
    return chooseReferenceUp(direction, fallback);
}

void resetShadowMapSizesForTest() { resetShadowMapSizes(); }

void stabilizeFitForTest(void* camera, unsigned texels, unsigned steps) {
    const unsigned savedTexels = g_mapTexels;
    const unsigned savedSteps = g_stabilizeSteps;
    g_mapTexels = texels;
    g_stabilizeSteps = steps;
    stabilizeFit((FitCamera*)camera);
    g_mapTexels = savedTexels;
    g_stabilizeSteps = savedSteps;
}
#endif

}  // namespace shadow
}  // namespace tq
