// Waiting for a module, and saying what is actually in this process.
//
// ## Why this file exists at all — O17
//
// `TQ.exe` runs as **two processes**, and our DLL is loaded into both
// (docs/rev/observed.md O17). `Engine.dll` loads the renderer by name at
// runtime, so `Direct3D11.dll` is not there at `DllMain` time in either of them,
// and one of them may never load it. **An unbounded wait in that process is
// indistinguishable from a broken hook**, and would have been diagnosed as one.
//
// So: the wait is bounded, the giving-up is a log line, and when it gives up it
// prints what the process *does* have loaded — which is the fact that says which
// of the two processes this was.
//
// The module list is walked from the PEB rather than with `EnumProcessModules`,
// following ../grimdawn-trash/src/modules.{h,cpp}: psapi may itself load a DLL,
// and that is how a process deadlocks under the loader lock.

#pragma once

#include <windows.h>

namespace tq {
namespace modules {

/**
 * Sleep for `ms`, unless `cancel` is signalled first. Returns false if it was.
 *
 * Every wait in this DLL goes through here, and that is not tidiness. An
 * orderly `FreeLibrary` unmaps our code as soon as `DllMain` returns; a thread
 * of ours still inside a `Sleep` at that moment resumes into unmapped memory and
 * crashes — and a crash on unload would read as our patches breaking the game.
 * The off-game self-test does exactly this, which is how it was found.
 */
bool nap(HANDLE cancel, DWORD ms);

/**
 * Poll for `name` until it is loaded, `timeoutMs` has passed, or `cancel` is
 * signalled.
 *
 * Returns the module, or null. `*waitedMs` gets how long it took, which is worth
 * having in the log: it is the number that says whether the wait was nearly a
 * race. `*cancelled` distinguishes "this process does not render" from "we were
 * asked to stop", which are different facts about a run.
 *
 * **Must not be called from `DllMain`.** It sleeps, and sleeping under the loader
 * lock while another thread is trying to load a library is a deadlock.
 */
HMODULE waitFor(const wchar_t* name, DWORD timeoutMs, HANDLE cancel, DWORD* waitedMs,
                bool* cancelled);

/**
 * Every loaded module that is not one of Windows' own, one per line.
 *
 * The path is the test, not the name: `c:\windows\` covers system32 and
 * syswow64 and nothing else. What is left is the game's own DLLs, Steam's,
 * DXMT's and ours — and with two processes in play, that list is what tells them
 * apart.
 */
void logLoaded(const char* when);

}  // namespace modules
}  // namespace tq
