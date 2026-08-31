#include "bloom_hook.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

namespace tq {
namespace bloomhook {
namespace {

const BYTE kExpectedPrologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
const SIZE_T kEntrySize = sizeof(kExpectedPrologue);
const SIZE_T kTrampolineSize = kEntrySize + 6;

BYTE* g_entry;
BYTE* g_trampoline;
BYTE g_originalEntry[kEntrySize];
BYTE g_hookEntry[kEntrySize];
HotBlurFn g_original;
LONG g_installed;
LONG g_suppressionEnabled;

void __fastcall hookHotBlurFrameBuffer(void* canvas, void*, unsigned width,
                                       unsigned height, float extraction,
                                       float strength, float saturation);

bool readable(const void* address, SIZE_T bytes) {
    MEMORY_BASIC_INFORMATION info = {};
    if (!address || !bytes || !VirtualQuery(address, &info, sizeof(info))) return false;
    DWORD protection = info.Protect & 0xff;
    const BYTE* end = (const BYTE*)address + bytes;
    const BYTE* regionEnd = (const BYTE*)info.BaseAddress + info.RegionSize;
    return info.State == MEM_COMMIT && !(info.Protect & PAGE_GUARD)
        && protection != PAGE_NOACCESS && end >= (const BYTE*)address
        && end <= regionEnd;
}

bool moduleText(HMODULE module, BYTE** begin, SIZE_T* size) {
    if (!module || !begin || !size || !readable(module, sizeof(IMAGE_DOS_HEADER)))
        return false;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)module;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const BYTE* ntAddress = (const BYTE*)module + dos->e_lfanew;
    if (!readable(ntAddress, sizeof(IMAGE_NT_HEADERS))) return false;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)ntAddress;
    if (nt->Signature != IMAGE_NT_SIGNATURE
        || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) return false;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    if (!readable(section, nt->FileHeader.NumberOfSections * sizeof(*section)))
        return false;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(section[i].Name, ".text", 5)) continue;
        SIZE_T length = section[i].Misc.VirtualSize;
        BYTE* address = (BYTE*)module + section[i].VirtualAddress;
        if (!length || !readable(address, length)) return false;
        *begin = address;
        *size = length;
        return true;
    }
    return false;
}

void absoluteBranch(BYTE* code, const void* destination) {
    // push imm32; ret is an absolute six-byte branch on x86. It avoids any
    // assumption about the relative placement of Engine.dll and winmm.dll.
    code[0] = 0x68;
    uint32_t address = (uint32_t)(uintptr_t)destination;
    memcpy(code + 1, &address, sizeof(address));
    code[5] = 0xc3;
}

bool writeEntry(const BYTE* expected, const BYTE* replacement) {
    if (!g_entry || !expected || !replacement || !readable(g_entry, kEntrySize)
        || memcmp(g_entry, expected, kEntrySize)) return false;
    DWORD oldProtection = 0;
    if (!VirtualProtect(g_entry, kEntrySize, PAGE_EXECUTE_READWRITE,
                        &oldProtection)) return false;
    memcpy(g_entry, replacement, kEntrySize);
    FlushInstructionCache(GetCurrentProcess(), g_entry, kEntrySize);
    DWORD ignored = 0;
    VirtualProtect(g_entry, kEntrySize, oldProtection, &ignored);
    return true;
}

void restoreEntry() {
    if (g_entry && readable(g_entry, kEntrySize)
        && !memcmp(g_entry, g_hookEntry, kEntrySize))
        writeEntry(g_hookEntry, g_originalEntry);
    g_entry = nullptr;
    if (g_trampoline) VirtualFree(g_trampoline, 0, MEM_RELEASE);
    g_trampoline = nullptr;
    memset(g_originalEntry, 0, sizeof(g_originalEntry));
    memset(g_hookEntry, 0, sizeof(g_hookEntry));
}

void __fastcall hookHotBlurFrameBuffer(void* canvas, void*, unsigned width,
                                       unsigned height, float extraction,
                                       float strength, float saturation) {
    // HotBlurFrameBuffer is the complete native bloom operation. Its temporary
    // surfaces and all target/viewport changes are local and restored before
    // return, so enhanced/off mode can skip the operation as a whole. Original
    // mode and enhanced-bloom failure fallback continue through the trampoline.
    if (InterlockedCompareExchange(&g_suppressionEnabled, 0, 0)) return;
    if (g_original)
        g_original(canvas, width, height, extraction, strength, saturation);
}

}  // namespace

bool install(HMODULE engine, HotBlurFn original) {
    if (!engine || !original
        || InterlockedCompareExchange(&g_installed, 1, 0)) return false;
    MEMORY_BASIC_INFORMATION owner = {};
    if (!VirtualQuery((const void*)original, &owner, sizeof(owner))
        || owner.AllocationBase != engine) {
        InterlockedExchange(&g_installed, 0);
        return false;
    }
    BYTE* text = nullptr;
    SIZE_T textSize = 0;
    if (!moduleText(engine, &text, &textSize)
        || (BYTE*)original < text
        || (BYTE*)original + kEntrySize < (BYTE*)original
        || (BYTE*)original + kEntrySize > text + textSize
        || memcmp((const void*)original, kExpectedPrologue, kEntrySize)) {
        InterlockedExchange(&g_installed, 0);
        return false;
    }

    g_entry = (BYTE*)original;
    memcpy(g_originalEntry, g_entry, kEntrySize);
    absoluteBranch(g_hookEntry, (const void*)&hookHotBlurFrameBuffer);
    g_trampoline = (BYTE*)VirtualAlloc(nullptr, kTrampolineSize,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_trampoline) {
        restoreEntry();
        InterlockedExchange(&g_installed, 0);
        return false;
    }
    memcpy(g_trampoline, g_originalEntry, kEntrySize);
    absoluteBranch(g_trampoline + kEntrySize, g_entry + kEntrySize);
    DWORD oldProtection = 0;
    if (!VirtualProtect(g_trampoline, kTrampolineSize, PAGE_EXECUTE_READ,
                        &oldProtection)) {
        restoreEntry();
        InterlockedExchange(&g_installed, 0);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), g_trampoline, kTrampolineSize);
    g_original = (HotBlurFn)(void*)g_trampoline;
    if (!writeEntry(g_originalEntry, g_hookEntry)) {
        g_original = nullptr;
        restoreEntry();
        InterlockedExchange(&g_installed, 0);
        return false;
    }
    return true;
}

void setSuppression(bool enabled) {
    InterlockedExchange(&g_suppressionEnabled, enabled ? 1 : 0);
}

void shutdown() {
    g_original = nullptr;
    restoreEntry();
    InterlockedExchange(&g_suppressionEnabled, 0);
    InterlockedExchange(&g_installed, 0);
}

bool installed() {
    return InterlockedCompareExchange(&g_installed, 1, 1) != 0;
}

float extractBrightness(float brightness, float threshold, float knee) {
    if (!(brightness > 0.0f) || !(knee > 0.0f)
        || !isfinite(brightness) || !isfinite(threshold)
        || !isfinite(knee)) return 0.0f;
    float soft = brightness - threshold + knee;
    float twoKnee = 2.0f * knee;
    if (soft < 0.0f) soft = 0.0f;
    if (soft > twoKnee) soft = twoKnee;
    soft = soft * soft / (4.0f * knee + 1e-6f);
    float excess = brightness - threshold;
    if (excess < 0.0f) excess = 0.0f;
    return soft > excess ? soft : excess;
}

}  // namespace bloomhook
}  // namespace tq
