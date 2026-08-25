#include "winmm_proxy.h"

#include <string.h>

#include "log.h"

// The generated half. Both files come out of one run of
// scripts/gen-winmm-proxy.sh, so slot i and name i cannot drift apart.
extern "C" void* tq_winmm_targets[];

static const char* const kNames[] = {
#define TQ_WINMM_NAME(n) n,
#include "winmm_names.inc"
#undef TQ_WINMM_NAME
};
static const int kCount = (int)(sizeof(kNames) / sizeof(kNames[0]));

namespace tq {
namespace winmm {

namespace {

char        g_from[MAX_PATH] = "nothing - the real winmm was never found";
int         g_resolved;
int         g_missing;
const char* g_firstMissing;
volatile LONG g_unresolvedCalls;

/** Is `p` inside `module`? A stub that resolved to us is a jump to itself. */
bool insideModule(HMODULE module, const void* p) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    return (HMODULE)mbi.AllocationBase == module;
}

/**
 * Load one candidate and refuse it if it is us.
 *
 * The refusal is the point, and it is the sibling repo's, kept verbatim in
 * spirit. We are `winmm.dll`; if the loader ever answers a request for a real
 * winmm with our own module, filling the slots from it would point all 186 stubs
 * at themselves. The failure would be an infinite jump on the game's first call
 * to `timeGetTime`: a hang, before a frame, with a log that said nothing wrong.
 */
HMODULE loadReal(const wchar_t* path, HINSTANCE self) {
    HMODULE m = LoadLibraryW(path);
    if (!m) return NULL;
    if (m == (HMODULE)self) {
        tqlog("!! winmm: %S loaded back as OUR OWN module - refusing it, or every", path);
        tqlog("!! winmm: stub would jump to itself and hang the game on its first call.");
        FreeLibrary(m);
        return NULL;
    }
    return m;
}

}  // namespace

/**
 * The slot every export starts out pointing at, and the one it keeps if the real
 * winmm turns out not to have that name.
 *
 * ## The i386 hazard, which the 64-bit sibling does not have
 *
 * Next door this function is safe: x86-64 Windows has **one** calling
 * convention, so a single fallback can stand in for any export and the caller
 * cleans up. **We are i386, where these are `__stdcall` and the *callee* cleans
 * the stack** — and the byte count differs per function. `timeGetTime` ends in
 * `ret` and `waveOutWrite` in `ret 12`. This function compiles to a bare `ret`
 * (verified by disassembly during Stage 2), so if it is ever actually reached
 * through a slot belonging to an export that takes arguments, **it returns with
 * the caller's arguments still on the stack and corrupts it.**
 *
 * There is no general fix: the export table carries no argument counts, and the
 * 186 names are undecorated, so nothing tells us what each `ret N` should be.
 *
 * **So this is made unreachable by construction instead.** The name list is
 * generated from the very DLL that `resolve()` then loads at runtime — the
 * system's own winmm — so every name resolves. If that ever stops being true,
 * `report()` says so with `!!` at attach, which is *before* anything could call
 * one, and that line is the warning to act on. Zero is returned rather than
 * jumping to a null slot, so the counter below stays the way anyone finds out.
 */
extern "C" unsigned long tq_winmm_unresolved(void) {
    InterlockedIncrement(&g_unresolvedCalls);
    return 0;
}

void resolve(HINSTANCE self) {
    wchar_t path[MAX_PATH];
    HMODULE real = NULL;

    // The system's own, and only the system's own.
    //
    // `GetSystemDirectoryW` is the right call here *because* we are a 32-bit
    // process: under WOW64 it answers with `syswow64`, which is where the i386
    // winmm actually lives. Stage 2 confirmed that matters on this machine — the
    // bottle is ARM64, so `system32\winmm.dll` is a **PE32+ Aarch64** binary and
    // the i386 one is in `syswow64`. Hard-coding either path would be wrong on
    // some other machine; asking is right on all of them.
    wchar_t sys[MAX_PATH];
    UINT s = GetSystemDirectoryW(sys, MAX_PATH);
    if (s > 0 && s < MAX_PATH - 16) {
        _snwprintf(path, MAX_PATH, L"%s\\winmm.dll", sys);
        path[MAX_PATH - 1] = 0;
        real = loadReal(path, self);
    }

    if (!real) {
        g_missing = kCount;
        tqlog("!! winmm: no real winmm could be loaded. Every winmm call in this process");
        tqlog("!! winmm: now returns 0, which will break the game's timing and its sound.");
        return;
    }

    _snprintf(g_from, sizeof(g_from), "%S", path);
    g_from[sizeof(g_from) - 1] = 0;

    for (int i = 0; i < kCount; i++) {
        FARPROC p = GetProcAddress(real, kNames[i]);
        // The same refusal as above, one slot at a time: a forwarder inside the
        // real winmm that resolved back to us would be a jump to itself just as
        // surely as the whole module being us.
        if (p && insideModule((HMODULE)self, (const void*)p)) {
            tqlog("!! winmm: %s in %S resolves back into our own module - left unresolved.",
                  kNames[i], path);
            p = NULL;
        }
        if (!p) {
            if (!g_firstMissing) g_firstMissing = kNames[i];
            g_missing++;
            continue;
        }
        tq_winmm_targets[i] = (void*)p;
        g_resolved++;
    }
}

void report(const char* when) {
    bool attach = strcmp(when, "attach") == 0;
    if (attach) {
        tqlog("winmm:    %d of %d exports forwarded to %s", g_resolved, kCount, g_from);
        if (g_missing) {
            tqlog("!! winmm: %d name(s) the real winmm does not have, first \"%s\".", g_missing,
                  g_firstMissing ? g_firstMissing : "?");
            tqlog("!! winmm: on i386 calling one of those corrupts the stack - see the note on");
            tqlog("!! winmm: tq_winmm_unresolved. Do not ship this build; find out why it missed.");
        }
        return;
    }
    LONG called = g_unresolvedCalls;
    tqlog("winmm (%s): %d of %d exports forwarded, %ld call(s) to one we could not forward%s",
          when, g_resolved, kCount, called,
          called ? " - SOMETHING GOT A ZERO INSTEAD OF AN ANSWER" : "");
}

}  // namespace winmm
}  // namespace tq
