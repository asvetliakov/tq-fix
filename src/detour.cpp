#include "detour.h"

#include <string.h>

namespace tq {
namespace detour {

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
        // Only the start is probed, deliberately. Every write here runs
        // VirtualProtect over a handful of bytes and back, and that splits the
        // section's memory region: asking whether the whole of .text is one
        // readable range succeeds for the first hook and then fails for every
        // one after it, which is a false negative that silently costs the
        // instrument. What the bytes actually are is still checked -- matches()
        // and writeBytes() probe exactly the range they touch.
        if (!length || !readable(address, 1)
            || address + length < address) return false;
        *begin = address;
        *size = length;
        return true;
    }
    return false;
}

void absoluteBranch(BYTE* code, const void* destination) {
    code[0] = 0x68;
    uint32_t address = (uint32_t)(uintptr_t)destination;
    memcpy(code + 1, &address, sizeof(address));
    code[5] = 0xc3;
}

bool writeBytes(BYTE* address, const BYTE* expected, const BYTE* replacement,
                SIZE_T bytes) {
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

bool matches(HMODULE module, const void* address, const Signature& signature) {
    if (!module || !address || !signature.bytes || !signature.length
        || signature.length > kMaxSignature
        || signature.relocationCount > kMaxRelocations
        || (signature.relocationCount && !signature.relocations)
        || !readable(address, signature.length))
        return false;
    const BYTE* at = (const BYTE*)address;
    // Resolve the relocated dwords first and mark the bytes they cover, so the
    // literal compare below can stay a straight walk.
    bool relocated[kMaxSignature] = {};
    for (unsigned i = 0; i < signature.relocationCount; ++i) {
        const Relocation& relocation = signature.relocations[i];
        if (relocation.offset + sizeof(uint32_t) > signature.length) return false;
        uint32_t value = 0;
        memcpy(&value, at + relocation.offset, sizeof(value));
        if (value != (uint32_t)(uintptr_t)((const BYTE*)module + relocation.rva))
            return false;
        for (unsigned b = 0; b < sizeof(uint32_t); ++b)
            relocated[relocation.offset + b] = true;
    }
    for (SIZE_T i = 0; i < signature.length; ++i)
        if (!relocated[i] && at[i] != signature.bytes[i]) return false;
    return true;
}

namespace {

// The bytes we are about to overwrite have to lie inside the module's own
// .text, and so does everything the signature reaches over: a target resolved
// from a stale export table could otherwise put the write anywhere.
bool withinText(HMODULE module, const void* address, SIZE_T bytes) {
    BYTE* text = nullptr;
    SIZE_T textSize = 0;
    const BYTE* at = (const BYTE*)address;
    return moduleText(module, &text, &textSize) && at >= text
        && at + bytes >= at && at + bytes <= text + textSize;
}

// Shared by attach and replace: verify, then write the branch over `stolen`
// bytes with nop for whatever the six-byte branch does not cover.
bool overwriteEntry(Detour& detour, BYTE* entry, SIZE_T stolen,
                    const void* replacement) {
    memcpy(detour.original, entry, stolen);
    memset(detour.patched, 0x90, kMaxStolen);
    absoluteBranch(detour.patched, replacement);
    return writeBytes(entry, detour.original, detour.patched, stolen);
}

}  // namespace

bool attach(Detour& detour, HMODULE module, void* target,
            const Signature& verify, SIZE_T stolen, const void* replacement,
            void** trampoline) {
    if (detour.installed || !module || !target || !replacement || !trampoline
        || stolen < kBranchSize || stolen > kMaxStolen
        || verify.length < stolen)
        return false;
    BYTE* entry = (BYTE*)target;
    if (!withinText(module, entry, verify.length)
        || !matches(module, entry, verify))
        return false;

    BYTE* code = (BYTE*)VirtualAlloc(nullptr, stolen + kBranchSize,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!code) return false;
    memcpy(code, entry, stolen);
    absoluteBranch(code + stolen, entry + stolen);
    DWORD oldProtection = 0;
    if (!VirtualProtect(code, stolen + kBranchSize, PAGE_EXECUTE_READ,
                        &oldProtection)) {
        VirtualFree(code, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), code, stolen + kBranchSize);

    // Published before the entry is overwritten, not after. The caller passes
    // the variable its hook actually calls through, so the first call to
    // arrive -- possibly on another thread, between these two statements --
    // finds a trampoline rather than a null pointer and a dropped call.
    *trampoline = code;
    if (!overwriteEntry(detour, entry, stolen, replacement)) {
        *trampoline = nullptr;
        VirtualFree(code, 0, MEM_RELEASE);
        memset(&detour, 0, sizeof(detour));
        return false;
    }
    detour.entry = entry;
    detour.trampoline = code;
    detour.stolen = stolen;
    detour.installed = true;
    return true;
}

bool replace(Detour& detour, HMODULE module, void* target,
             const Signature& verify, SIZE_T length, const void* replacement) {
    if (detour.installed || !module || !target || !replacement
        || length < kBranchSize || length > kMaxStolen
        || verify.length < length)
        return false;
    BYTE* entry = (BYTE*)target;
    if (!withinText(module, entry, verify.length)
        || !matches(module, entry, verify))
        return false;
    if (!overwriteEntry(detour, entry, length, replacement)) {
        memset(&detour, 0, sizeof(detour));
        return false;
    }
    detour.entry = entry;
    detour.trampoline = nullptr;
    detour.stolen = length;
    detour.installed = true;
    return true;
}

void detach(Detour& detour) {
    if (detour.installed && detour.entry && readable(detour.entry, detour.stolen)
        && !memcmp(detour.entry, detour.patched, detour.stolen))
        writeBytes(detour.entry, detour.patched, detour.original, detour.stolen);
    if (detour.trampoline) VirtualFree(detour.trampoline, 0, MEM_RELEASE);
    memset(&detour, 0, sizeof(detour));
}

bool patchCall(CallPatch& patch, HMODULE module, void* window,
               const Signature& signature, unsigned callOffset,
               const void* expectedTarget, const void* replacement) {
    if (patch.installed || !module || !window || !replacement
        || !expectedTarget || callOffset + 5 > signature.length
        || !withinText(module, window, signature.length)
        || !matches(module, window, signature))
        return false;

    BYTE* call = (BYTE*)window + callOffset;
    uint32_t original = 0, updated = 0;
    BYTE* operand = nullptr;
    if (call[0] == 0xe8) {
        memcpy(&original, call + 1, sizeof(original));
        if ((const BYTE*)call + 5 + (int32_t)original != expectedTarget)
            return false;
        const int32_t rel =
            (int32_t)((uintptr_t)replacement - ((uintptr_t)call + 5));
        memcpy(&updated, &rel, sizeof(updated));
        operand = call + 1;
    } else if (call[0] == 0xff && call[1] == 0x15) {
        if (callOffset + 6 > signature.length) return false;
        memcpy(&original, call + 2, sizeof(original));
        void* const* slot = (void* const*)(uintptr_t)original;
        if (!readable(slot, sizeof(*slot)) || *slot != expectedTarget)
            return false;
        // Published before the operand points at it, so the site can never
        // read a cell that has not been filled in yet.
        patch.target = (void*)replacement;
        updated = (uint32_t)(uintptr_t)&patch.target;
        operand = call + 2;
    } else {
        return false;
    }

    if (!writeBytes(operand, (const BYTE*)&original, (const BYTE*)&updated,
                    sizeof(original))) {
        patch.target = nullptr;
        return false;
    }
    patch.operand = operand;
    patch.original = original;
    patch.replacement = updated;
    patch.installed = true;
    return true;
}

namespace {

bool sameName(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return !*a && !*b;
}

const IMAGE_NT_HEADERS* headers(HMODULE module) {
    if (!module || !readable(module, sizeof(IMAGE_DOS_HEADER))) return nullptr;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)module;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
    const IMAGE_NT_HEADERS* nt =
        (const IMAGE_NT_HEADERS*)((const BYTE*)module + dos->e_lfanew);
    if (!readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;
    return nt;
}

// The slot in `module`'s import address table for `dll`!`name`, or null.
// Imports by ordinal have no name to match and are skipped.
void** importSlot(HMODULE module, const char* dll, const char* name) {
    const IMAGE_NT_HEADERS* nt = headers(module);
    if (!nt) return nullptr;
    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size) return nullptr;
    BYTE* base = (BYTE*)module;
    const IMAGE_IMPORT_DESCRIPTOR* descriptor =
        (const IMAGE_IMPORT_DESCRIPTOR*)(base + directory.VirtualAddress);
    for (; readable(descriptor, sizeof(*descriptor))
           && (descriptor->OriginalFirstThunk || descriptor->FirstThunk);
         ++descriptor) {
        const char* moduleName = (const char*)(base + descriptor->Name);
        if (!readable(moduleName, 1) || !sameName(moduleName, dll)) continue;
        // The name table may be absent in a bound image; there is then no way
        // to identify the slot, so refuse rather than guess at an index.
        if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) continue;
        const IMAGE_THUNK_DATA* named =
            (const IMAGE_THUNK_DATA*)(base + descriptor->OriginalFirstThunk);
        IMAGE_THUNK_DATA* bound =
            (IMAGE_THUNK_DATA*)(base + descriptor->FirstThunk);
        for (; readable(named, sizeof(*named)) && named->u1.AddressOfData;
             ++named, ++bound) {
            if (named->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            const IMAGE_IMPORT_BY_NAME* entry =
                (const IMAGE_IMPORT_BY_NAME*)(base + named->u1.AddressOfData);
            if (!readable(entry, sizeof(*entry) + 1)) continue;
            if (!sameName((const char*)entry->Name, name)) continue;
            if (!readable(bound, sizeof(*bound))) return nullptr;
            return (void**)&bound->u1.Function;
        }
    }
    return nullptr;
}

}  // namespace

bool patchImport(CallPatch& patch, HMODULE module, const char* dll,
                 const char* name, const void* expectedTarget,
                 const void* replacement) {
    if (patch.installed || !module || !dll || !name || !expectedTarget
        || !replacement)
        return false;
    void** slot = importSlot(module, dll, name);
    if (!slot || !readable(slot, sizeof(*slot)) || *slot != expectedTarget)
        return false;
    const uint32_t original = (uint32_t)(uintptr_t)*slot;
    const uint32_t updated = (uint32_t)(uintptr_t)replacement;
    if (!writeBytes((BYTE*)slot, (const BYTE*)&original,
                    (const BYTE*)&updated, sizeof(original)))
        return false;
    patch.operand = (BYTE*)slot;
    patch.original = original;
    patch.replacement = updated;
    patch.installed = true;
    return true;
}

void restoreCall(CallPatch& patch) {
    if (patch.installed && patch.operand)
        writeBytes(patch.operand, (const BYTE*)&patch.replacement,
                   (const BYTE*)&patch.original, sizeof(patch.original));
    memset(&patch, 0, sizeof(patch));
}

}  // namespace detour
}  // namespace tq
