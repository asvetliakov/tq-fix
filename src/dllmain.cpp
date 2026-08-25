// Stage 2: get inside the process, forward everything, write one line.
//
// If this file does anything clever it has failed. The gate is that the game
// plays exactly as it did before — including the flicker, unchanged. We are
// adding an observer, not a variable.

#include <windows.h>

#include "log.h"
#include "winmm_proxy.h"

#ifndef TQFLICKER_BUILD
#define TQFLICKER_BUILD "unknown"
#endif

namespace {

HINSTANCE g_self;

/** The exe we were loaded into, base name only. */
void logHost() {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        tqlog("host:     (could not be determined)");
        return;
    }
    const wchar_t* base = wcsrchr(exe, L'\\');
    base = base ? base + 1 : exe;
    tqlog("host:     %S  (pid %lu)", base, (unsigned long)GetCurrentProcessId());
}

}  // namespace

/**
 * `DLL_PROCESS_ATTACH` runs under the loader lock, so the rule is: resolve the
 * slots, write a handful of lines, return. Nothing here waits on another thread,
 * loads a module it does not have to, or calls into the game.
 *
 * `LoadLibraryW` on the system winmm is the one load we do, and it is
 * unavoidable — the slots are useless without it, and every winmm call in the
 * process is broken until they are filled.
 */
extern "C" BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved) {
    (void)reserved;

    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            g_self = self;
            // We are winmm; nothing here needs thread-attach callbacks, and
            // refusing them is a few thousand no-ops saved in a game that makes
            // threads.
            DisableThreadLibraryCalls(self);

            tq::logOpen(self);
            tqlog("%s", "");
            tqlog("tqflicker %s - Stage 2, proxy only. No patches installed.", TQFLICKER_BUILD);
            logHost();
            tq::winmm::resolve(self);
            tq::winmm::report("attach");
            tqlog("log:      %S", tq::logPath());
            return TRUE;
        }

        case DLL_PROCESS_DETACH: {
            // `reserved` non-null means the process is exiting rather than the
            // library being unloaded. On that path the loader is about to tear
            // everything down anyway and other threads have been killed
            // wherever they stood; doing less is safer than doing more.
            tq::winmm::report(reserved ? "process exit" : "detach");
            tq::logClose();
            return TRUE;
        }
    }
    return TRUE;
}
