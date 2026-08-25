// Stage 4: get inside the process, reach the device, and count the draws.
//
// Still an observer: every hook calls through with its arguments untouched and
// returns what the real function returned. What changed since Stage 3 is that a
// dozen vtable slots now point at src/frames.cpp, patched at device creation on
// the game's own thread (Risk 4). The gate is unchanged - the game plays as it
// did before, flicker included - plus a per-frame draw count in the log.

#include <windows.h>

#include "device.h"
#include "frames.h"
#include "log.h"
#include "patch.h"
#include "winmm_proxy.h"

#ifndef TQFLICKER_BUILD
#define TQFLICKER_BUILD "unknown"
#endif

namespace {

HINSTANCE g_self;
HANDLE    g_thread;
HANDLE    g_wake;        // set to ask the watcher to stop
HANDLE    g_finished;    // set by the watcher as the last thing it does

/** The exe we were loaded into, base name only. The pid is on every line. */
void logHost() {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        tqlog("host:     (could not be determined)");
        return;
    }
    const wchar_t* base = wcsrchr(exe, L'\\');
    base = base ? base + 1 : exe;
    tqlog("host:     %S", base);
}

/**
 * Is the D3D11 hook wanted this run?
 *
 * **This exists to make the control run one launch instead of two installs.**
 * Comparing "our DLL installed" against "our DLL removed" changes two things at
 * once — the winmm proxy and the hook — so it cannot say which one a change in
 * behaviour belongs to. With `TQFLICKER_HOOK=0` the same binary loads, forwards
 * winmm exactly as before, and installs nothing, which isolates the hook alone.
 *
 * Set it per-run with `cxstart`, no bottle edit needed (docs/rev/observed.md
 * O15) - but note O15's caveat, which this stage confirmed the hard way: the
 * process that renders is launched by *Steam*, not by us, so it inherits Steam's
 * environment. The variable has to be set where Steam can see it.
 */
bool hookWanted() {
    wchar_t v[8];
    DWORD n = GetEnvironmentVariableW(L"TQFLICKER_HOOK", v, 8);
    if (n == 0 || n >= 8) return true;                 // unset: hook, as normal
    return !(v[0] == L'0' && v[1] == 0);
}

/**
 * A line every two seconds for the first minute, and then silence.
 *
 * **The reason this is not decoration.** On Stage 3's first real launch the log
 * ended at device creation and the process was gone moments later, and there was
 * no way to tell from the file whether the game had run and been quit or had
 * died on the spot - which is the difference between "the stage worked" and "we
 * broke the game". A heartbeat makes the last line a timestamp of life, so the
 * next launch answers that without a second one.
 *
 * Bounded at a minute because it is a startup question, and thirty lines is a
 * thing a log can afford. Stage 4's per-frame counting replaces it.
 */
void heartbeat(HANDLE cancel) {
    const int kSeconds = 60, kStep = 2;
    for (int t = kStep; t <= kSeconds; t += kStep) {
        if (WaitForSingleObject(cancel, kStep * 1000) != WAIT_TIMEOUT) return;
        tqlog("alive:    %ds after the watcher started", t);
    }
    tqlog("alive:    %ds - past startup, heartbeat stops here", kSeconds);
}

/**
 * Everything that has to wait, waits here.
 *
 * `DllMain` runs under the loader lock, and the two things this stage needs —
 * sleeping until `Direct3D11.dll` is loaded, and then reading its import table —
 * are both things that must not happen there. `Engine.dll` loads the renderer by
 * name at runtime (docs/rev/substrate.md), so the module does not exist yet when
 * we attach, and waiting for it under the lock would deadlock the very load we
 * are waiting for.
 *
 * The thread does its work once and then parks. It is not a polling loop: there
 * is nothing to poll for in Stage 3, and a thread waking up every half second in
 * a game we are trying to measure is a variable we would have added ourselves.
 */
DWORD WINAPI watcher(LPVOID) {
    tqlog("watcher:  up (tid %lu)", (unsigned long)GetCurrentThreadId());

    // First, and before anything of the game is touched: prove both patch
    // primitives work in *this* process, on a table of our own. Under FEX, in
    // this bottle, on a page we made read-only ourselves. It costs a dozen lines
    // and it runs everywhere the DLL runs, including the off-game self-test.
    tq::patch::selfTest();

    if (hookWanted()) {
        tq::device::install(g_wake);
    } else {
        tqlog("d3d11:    TQFLICKER_HOOK=0 - not hooking anything this run. The winmm proxy is"
              " still forwarding; this is the control.");
    }

    heartbeat(g_wake);

    // Park. `g_wake` is only set by an orderly detach, so this returns either
    // when the library is being unloaded or never — and "never" is the normal
    // case, because the process is killed on the way out.
    WaitForSingleObject(g_wake, INFINITE);
    tq::device::report("watcher stopping");
    tq::frames::report("watcher stopping");
    tqlog("watcher:  out");
    SetEvent(g_finished);        // the last touch of our code before ExitThread
    return 0;
}

}  // namespace

/**
 * `DLL_PROCESS_ATTACH` runs under the loader lock, so the rule is: resolve the
 * slots, write a handful of lines, start the thread that does the waiting, and
 * return. Nothing here waits on another thread, loads a module it does not have
 * to, or calls into the game.
 *
 * `LoadLibraryW` on the system winmm is the one load we do, and it is
 * unavoidable — the slots are useless without it, and every winmm call in the
 * process is broken until they are filled.
 */
extern "C" BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            g_self = self;
            // We are winmm; nothing here needs thread-attach callbacks, and
            // refusing them is a few thousand no-ops saved in a game that makes
            // threads.
            DisableThreadLibraryCalls(self);

            tq::logOpen(self);
            tqlog("%s", "");
            tqlog("tqflicker %s - Stage 4, counting draws. Vtable patches: Present, Draw*, Map,"
                  " CreateBuffer, shader and sampler creation - observe only.", TQFLICKER_BUILD);
            logHost();
            tq::winmm::resolve(self);
            tq::winmm::report("attach");
            tqlog("log:      %S", tq::logPath());

            // Manual-reset events, so a detach that beats the thread to the
            // starting line still stops it.
            g_wake     = CreateEventW(NULL, TRUE, FALSE, NULL);
            g_finished = CreateEventW(NULL, TRUE, FALSE, NULL);
            g_thread   = CreateThread(NULL, 0, watcher, NULL, 0, NULL);
            if (!g_thread) tqlog("!! watcher: CreateThread failed (%lu) - no device this run",
                                 (unsigned long)GetLastError());
            return TRUE;
        }

        case DLL_PROCESS_DETACH: {
            // `reserved` non-null means the process is exiting rather than the
            // library being unloaded. On that path the loader is about to tear
            // everything down anyway and other threads have been killed wherever
            // they stood: unpatching another module's import table then is a way
            // to crash on quit, and a crash on quit looks exactly like our
            // patches having broken the game. So on process exit we do less.
            tq::device::report(reserved ? "process exit" : "detach");
            tq::frames::report(reserved ? "process exit" : "detach");
            tq::winmm::report(reserved ? "process exit" : "detach");

            if (!reserved) {
                // FreeLibrary: the process lives on, so everything we changed
                // goes back. Not `WaitForSingleObject(g_thread)` — the thread
                // cannot finish exiting until the loader lock we are holding is
                // released, so waiting on the *handle* always burns the full
                // timeout. Waiting on an event the thread sets on its way out
                // costs milliseconds. (The sibling repo paid for this one.)
                if (g_wake) SetEvent(g_wake);
                if (g_thread && WaitForSingleObject(g_finished, 2000) != WAIT_OBJECT_0)
                    tqlog("!! watcher did not stop in 2s - unpatching anyway");
                Sleep(20);                 // it is two instructions from ExitThread
                if (g_thread) { CloseHandle(g_thread); g_thread = NULL; }
                tq::patch::unpatchAll();
            }
            tq::logClose();
            return TRUE;
        }
    }
    return TRUE;
}
