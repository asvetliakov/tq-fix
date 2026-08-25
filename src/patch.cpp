#include "patch.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "log.h"

namespace tq {
namespace patch {

namespace {

enum class Kind { Iat, Vtable };

struct Entry {
    Kind        kind;
    void**      slot;
    void*       original;
    void*       ours;
    const char* where;   // module name, or the vtable's name
    const char* what;    // import name, or "slot 42"
    char        slotText[16];
};

// Fixed, because a failed allocation on the way in would leave a patch installed
// with no record of how to remove it. The arithmetic this project needs is
// small: one IAT entry in Stage 3, and five vtable slots in Stage 4 — `Present`
// plus the four `Draw*`. Sixty-four is room for an order of magnitude more than
// that, and a patch that does not fit is refused a record, which the log says
// loudly rather than leaving it unremovable in silence.
Entry g_entries[64];
int   g_count;

Entry* findEntry(void** slot) {
    for (int i = 0; i < g_count; i++)
        if (g_entries[i].slot == slot) return &g_entries[i];
    return nullptr;
}

/** The write itself, with the page opened and closed around it. */
bool storePointer(void** slot, void* value, void** previous) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    if (previous) *previous = *slot;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

}  // namespace

// Two holes in the sibling's first version of this, both found the hard way when
// a misread pointer walked off the end of a struct. Kept guarded here because a
// primitive that guards dereferences has to be right about the strange cases —
// the strange cases are the only ones that reach it:
//
//   * `at + bytes` can **overflow** for a wild pointer, making `end` smaller
//     than `at`, so the loop never runs and the function returns *true* for an
//     address that is not mapped at all. The caller then dereferences it.
//   * `BaseAddress + RegionSize` is not guaranteed to advance past `at` for
//     every possible query result, and a loop that does not advance is a hang
//     inside a hook — which looks exactly like the game freezing.
//
// The sibling also caches the last few regions that answered yes. That cache was
// a *measured* optimisation for a pass over 50,269 game objects, and this
// project has no such pass: `readable` is called a handful of times per device
// creation. It is left out rather than carried over untested.
bool readable(const void* p, size_t bytes) {
    if (!p || !bytes) return false;

    uintptr_t start = (uintptr_t)p;
    if (bytes > (uintptr_t)-1 - start) return false;      // the range wraps: not real memory
    uintptr_t end = start + bytes;

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t at = start;
    for (int guard = 0; at < end; guard++) {
        if (guard > 4096) return false;                   // cannot need this many regions
        if (!VirtualQuery((const void*)at, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        DWORD prot = mbi.Protect & 0xff;
        if (prot == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return false;
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= at) return false;                     // no progress: refuse rather than spin
        at = next;
    }
    return true;
}

int findSlot(void* const* vt, int maxSlots, const void* fn) {
    if (!vt || !fn) return -1;
    for (int i = 0; i < maxSlots; i++) {
        if (!readable(&vt[i], sizeof(void*))) return -1;
        if (vt[i] == fn) return i;
    }
    return -1;
}

void** iatSlot(HMODULE mod, const char* impDll, const char* impFunc) {
    if (!mod) return nullptr;
    char* base = (char*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    IMAGE_DATA_DIRECTORY* dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress || !dir->Size) return nullptr;

    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress);
    for (; imp->Name; imp++) {
        const char* dll = base + imp->Name;
        if (_stricmp(dll, impDll) != 0) continue;

        // The INT keeps the names; the IAT is what we rewrite. Some binaries have
        // no INT, in which case there is nothing to match a name against.
        // Direct3D11.dll has one (checked with objdump before this was written).
        if (!imp->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA* name = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* addr = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);

        for (; name->u1.AddressOfData; name++, addr++) {
            if (IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal)) continue;   // imported by ordinal
            IMAGE_IMPORT_BY_NAME* n = (IMAGE_IMPORT_BY_NAME*)(base + name->u1.AddressOfData);
            if (strcmp((const char*)n->Name, impFunc) != 0) continue;
            return (void**)&addr->u1.Function;
        }
    }
    return nullptr;
}

void* iat(HMODULE mod, const char* modName,
          const char* impDll, const char* impFunc, void* replacement) {
    void** slot = iatSlot(mod, impDll, impFunc);
    if (!slot) return nullptr;

    if (Entry* already = findEntry(slot)) {
        tqlog("  IAT %s imports %s.%s - already patched; reusing original %p",
              modName, impDll, impFunc, already->original);
        return already->original;
    }

    void* original = nullptr;
    if (!storePointer(slot, replacement, &original)) {
        tqlog("!! IAT %s!%s.%s - VirtualProtect failed (%u)",
              modName, impDll, impFunc, (unsigned)GetLastError());
        return nullptr;
    }

    if (g_count < (int)(sizeof(g_entries) / sizeof(g_entries[0]))) {
        Entry& p = g_entries[g_count++];
        p.kind = Kind::Iat; p.slot = slot; p.original = original;
        p.ours = replacement; p.where = modName; p.what = impFunc;
        p.slotText[0] = 0;
    } else {
        tqlog("!! patch table full - %s.%s is installed but will NOT be removed",
              impDll, impFunc);
    }
    tqlog("  IAT %s imports %s.%s at %p -> was %p, now %p",
          modName, impDll, impFunc, (void*)slot, original, replacement);
    return original;
}

void* vtableSlot(void** vt, int slot, const char* what, void* replacement) {
    if (!vt || slot < 0 || !replacement) return nullptr;
    void** p = &vt[slot];
    if (!readable(p, sizeof(void*))) {
        tqlog("!! VT %s slot %d at %p - not readable, refusing", what, slot, (void*)p);
        return nullptr;
    }

    if (Entry* already = findEntry(p)) {
        tqlog("  VT %s slot %d - already patched; reusing original %p",
              what, slot, already->original);
        return already->original;
    }

    void* original = nullptr;
    if (!storePointer(p, replacement, &original)) {
        tqlog("!! VT %s slot %d at %p - VirtualProtect failed (%u)",
              what, slot, (void*)p, (unsigned)GetLastError());
        return nullptr;
    }

    if (g_count < (int)(sizeof(g_entries) / sizeof(g_entries[0]))) {
        Entry& e = g_entries[g_count++];
        e.kind = Kind::Vtable; e.slot = p; e.original = original;
        e.ours = replacement; e.where = what;
        _snprintf(e.slotText, sizeof(e.slotText), "slot %d", slot);
        e.slotText[sizeof(e.slotText) - 1] = 0;
        e.what = e.slotText;
    } else {
        tqlog("!! patch table full - %s slot %d is installed but will NOT be removed", what, slot);
    }
    tqlog("  VT %-28s slot %3d at %p -> was %p, now %p",
          what, slot, (void*)p, original, replacement);
    return original;
}

int installed() { return g_count; }

// ------------------------------------------------------------- the self-test

namespace {

int g_realCalls, g_hookCalls;

int testReal(int x) { g_realCalls++; return x + 1; }
int testOther(int)  { return 0; }

typedef int (*TestFn)(int);
TestFn g_testOriginal;

int testHook(int x) {
    g_hookCalls++;
    return g_testOriginal ? g_testOriginal(x) * 10 : -1;
}

}  // namespace

// The table is a page from VirtualAlloc, filled and then turned read-only —
// which is what a real vtable's page is, and what makes the VirtualProtect path
// real rather than decorative.
//
// The sibling's first attempt used a `static void* const[3]`, and it quietly did
// not test anything: the array is const, so the compiler folded every read back
// to the initialiser and the call went to the unpatched function while the
// patched slot sat there unread. Writing through a const object is undefined
// behaviour and the compiler was within its rights. A page whose contents the
// compiler cannot know has no such loophole. Do not "simplify" this back.
bool selfTest() {
    g_realCalls = g_hookCalls = 0;

    void** table = (void**)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!table) {
        tqlog("!! patch self-test: VirtualAlloc failed (%u)", (unsigned)GetLastError());
        return false;
    }
    table[0] = (void*)&testOther;
    table[1] = (void*)&testReal;
    table[2] = (void*)&testOther;

    DWORD old;
    bool readOnly = VirtualProtect(table, 4096, PAGE_READONLY, &old) != 0;
    tqlog("patch self-test: a 3-slot table at %p, made %s - %s",
          (void*)table, readOnly ? "PAGE_READONLY" : "read-write (VirtualProtect refused)",
          readOnly ? "the same shape as a real vtable's page"
                   : "the unprotect path is NOT exercised");

    bool ok = true;
    int slot = findSlot(table, 3, (void*)&testReal);
    if (slot != 1) { tqlog("!! FAIL: findSlot returned %d, expected 1", slot); ok = false; }

    void* original = ok ? vtableSlot(table, slot, "self-test table", (void*)&testHook) : nullptr;
    if (ok && original != (void*)&testReal) {
        tqlog("!! FAIL: original was %p, expected %p", original, (void*)&testReal);
        ok = false;
    }
    g_testOriginal = (TestFn)original;

    // Installing twice must not stack: the second install returns the first
    // original, so a caller that stores it still reaches the real function.
    if (ok) {
        void* again = vtableSlot(table, slot, "self-test table", (void*)&testHook);
        if (again != original) {
            tqlog("!! FAIL: second install returned %p, not %p", again, original);
            ok = false;
        }
    }

    if (ok) {
        int result = ((TestFn)table[slot])(41);
        if (g_hookCalls != 1 || g_realCalls != 1 || result != 420) {
            tqlog("!! FAIL: hook=%d real=%d result=%d, expected 1/1/420",
                  g_hookCalls, g_realCalls, result);
            ok = false;
        } else {
            tqlog("  patched slot fired: hook -> original -> %d, and the original ran exactly once",
                  result);
        }
    }

    unpatchAll();
    if (table[slot] != (void*)&testReal) {
        tqlog("!! FAIL: slot holds %p after unpatch, not %p", table[slot], (void*)&testReal);
        ok = false;
    } else if (((TestFn)table[slot])(41) != 42 || g_hookCalls != 1) {
        tqlog("!! FAIL: the hook still ran after unpatch");
        ok = false;
    }

    // The page had better still be read-only: a primitive that leaves a vtable
    // writable would be a quiet change to the process we are guests in.
    MEMORY_BASIC_INFORMATION mbi;
    if (readOnly && VirtualQuery(table, &mbi, sizeof(mbi)) && mbi.Protect != PAGE_READONLY) {
        tqlog("!! FAIL: the page was left at protection 0x%x, not PAGE_READONLY",
              (unsigned)mbi.Protect);
        ok = false;
    }

    VirtualFree(table, 0, MEM_RELEASE);
    tqlog("patch self-test %s: vtable slot patched, fired, restored, page protection put back",
          ok ? "PASSED" : "FAILED");
    return ok;
}

// Put every slot back, newest first. Reports anything that changed under us: a
// slot that no longer holds our pointer means something else patched it too, and
// blind restoration would be the bug. The THQ overlay is in this process and
// hooks D3D11 as well (docs/rev/prior-art.md), so this is not hypothetical.
void unpatchAll() {
    int restored = 0, foreign = 0;
    for (int i = g_count - 1; i >= 0; i--) {
        Entry& e = g_entries[i];
        if (*e.slot != e.ours) {
            foreign++;
            tqlog("!! %s %s!%s - slot holds %p, not ours (%p); left alone",
                  e.kind == Kind::Iat ? "IAT" : "VT ", e.where, e.what, *e.slot, e.ours);
            continue;
        }
        if (storePointer(e.slot, e.original, nullptr)) restored++;
    }
    tqlog("patches removed: %d restored, %d left alone, of %d installed",
          restored, foreign, g_count);
    g_count = 0;
}

}  // namespace patch
}  // namespace tq
