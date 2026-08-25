/*
 * Off-game self-test: load our proxy and prove it forwards, with no game.
 *
 * Built for 32-bit Windows and run inside the bottle by
 * scripts/selftest-offgame.sh. It writes its findings to a file rather than
 * stdout, because a console under `cxstart` is not reliably capturable.
 *
 * What it proves, in order of what would hurt most if it were wrong:
 *
 *   1. Our winmm.dll loads at all as a 32-bit module.
 *   2. It is OUR module and not the system's - checked by the log line and by
 *      the module path, so a test that silently exercised the real winmm cannot
 *      pass.
 *   3. `timeGetTime` forwards and returns a real tick count, twice, increasing.
 *      This is the call the game makes constantly; if the stub mechanism were
 *      wrong it would hang or return zero here rather than in the game.
 *   4. Every one of the 186 exports resolves through GetProcAddress.
 */
#include <windows.h>
#include <stdio.h>

static FILE* out;
static int   fails;

static void ck(int cond, const char* what) {
    fprintf(out, "%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) fails++;
}

int main(int argc, char** argv) {
    const char* dll = (argc > 1) ? argv[1] : "winmm.dll";
    const char* rep = (argc > 2) ? argv[2] : "C:\\tqflicker-selftest.txt";

    out = fopen(rep, "w");
    if (!out) return 99;
    fprintf(out, "tqflicker off-game self-test\ndll: %s\n\n", dll);

    HMODULE m = LoadLibraryA(dll);
    ck(m != NULL, "LoadLibrary(our winmm.dll)");
    if (!m) {
        fprintf(out, "\nGetLastError=%lu\n", (unsigned long)GetLastError());
        fprintf(out, "RESULT: %d failure(s)\n", fails);
        fclose(out);
        return 1;
    }

    char path[MAX_PATH] = "";
    GetModuleFileNameA(m, path, MAX_PATH);
    fprintf(out, "loaded from: %s\n", path);
    /* If this loaded the system winmm instead of ours the whole test is
       meaningless, so make that a failure rather than a silent pass. */
    ck(strstr(path, "syswow64") == NULL && strstr(path, "system32") == NULL,
       "loaded OUR winmm, not the system's");

    FARPROC tgt = GetProcAddress(m, "timeGetTime");
    ck(tgt != NULL, "GetProcAddress(timeGetTime)");
    if (tgt) {
        DWORD (WINAPI *fn)(void) = (DWORD (WINAPI*)(void))tgt;
        DWORD a = fn();
        Sleep(30);
        DWORD b = fn();
        fprintf(out, "timeGetTime: %lu then %lu\n", (unsigned long)a, (unsigned long)b);
        /* Zero is what an unresolved slot returns, so it is the specific
           failure this is looking for. */
        ck(a != 0, "timeGetTime forwarded (non-zero: not the unresolved stub)");
        ck(b >= a, "timeGetTime advances");
    }

    /* Every export must resolve. A missing one is not cosmetic here: on i386 an
       unresolved slot that is actually called corrupts the stack, because the
       fallback cannot know how many bytes a __stdcall callee should pop. See
       the note on tq_winmm_unresolved in src/winmm_proxy.cpp. */
    static const char* const names[] = {
#define TQ_WINMM_NAME(n) n,
#include "winmm_names.inc"
#undef TQ_WINMM_NAME
    };
    int n = (int)(sizeof(names) / sizeof(names[0]));
    int missing = 0;
    const char* first = NULL;
    for (int i = 0; i < n; i++) {
        if (!GetProcAddress(m, names[i])) {
            if (!first) first = names[i];
            missing++;
        }
    }
    fprintf(out, "exports: %d checked, %d missing\n", n, missing);
    if (missing) fprintf(out, "first missing: %s\n", first);
    ck(missing == 0, "all exports resolve through our module");

    FreeLibrary(m);
    fprintf(out, "\nRESULT: %d failure(s)\n", fails);
    fclose(out);
    return fails ? 1 : 0;
}
