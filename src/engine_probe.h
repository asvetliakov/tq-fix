#pragma once

#include <windows.h>

namespace tq {
namespace engineprobe {

// Instrumentation written into Engine.dll's .text.
//
// Every instrument before this one was a vtable slot or mod-side code. This
// one patches the game's own instruction stream on paths that run thousands of
// times a second, so the whole module is built around not installing:
//
// - nothing installs unless the performance probe is on *and* [debug]
//   engine_trace is not 0, so the shipping configuration is byte-identical to
//   a build without this file;
// - every target is resolved by decorated export name and then asserted
//   against the RVA the audit recorded, so a different Engine.dll resolves to
//   a different address and installs nothing;
// - every site verifies 16-24 bytes before it writes 4-7, because the four
//   busiest targets share the same six-byte prologue;
// - each hook installs independently and one failure leaves the others alone;
// - shutdown() restores every site in reverse, and only where the bytes are
//   still the ones we wrote.
//
// What it answers is in probe.h, below CounterEngineLevelLoad.

// Reads [debug] engine_trace. 0 disables the module outright; 1 (the default)
// installs everything; any larger value is a mask, so a run that misbehaves
// can be bisected from the INI rather than from a rebuild:
//   2  region and resource loads      4  archive reads and block inflates
//   8  the loader fence wait         16  the region lock on the render path
//  32  the resource-manager sweeps   64  Region::WaitForLoadingToFinish
// 128  Engine::Update and Engine::Render, bracketed whole
// 256  GameEngine::Update -- the one hook that is in Game.dll
// 512  TQ.exe's main loop, through its import table, patching nothing: its
//      sleep and waits, the platform pump, graphics options, music, sound,
//      Engine::PresentSurface, the collision fixup, quest triggers, and
//      EWindow::ProcessMessages -- eleven imports covering every call in the
//      loop that does work rather than return a pointer
//1024  inside the pump: PeekMessageA and DispatchMessageA, which are
//      Engine.dll's imports rather than the executable's
//2048  Engine.dll's operator new[] and operator delete[]. Run 23 broke the
//      freeze frame down and found 61% of it named by nothing; the archive
//      File constructor allocates two buffers of up to 256 KiB per compressed
//      entry opened, and that frame opened 1,299 of them.
//4096  the seek and the read under the archive block routine, so the inflate
//      can be recovered from engine_arc_inflate_us by subtraction -- the
//      number 4.2 and 4.3 are gated on
//8192  everything in Engine.dll that can block: every critical section
//      (contended acquisitions only), both waits, and Sleep. Run 24 killed
//      the heap candidate and left 996 ms of a 1,534.8 ms frame unnamed; the
//      main thread waiting on the loader thread has never been measured
//      outside three lock sites and one fence. Installs last, because the
//      region-lock and fence groups check these same slots still hold
//      kernel32's exports.
void readOptions(const wchar_t* iniPath);

// [performance] timer_period_ms, an experiment rather than a fix.
//
// 76% of the slow message retrievals return WM_TIMER, and WM_TIMER is
// synthesized rather than queued -- PeekMessage has to ask the host whether a
// timer has expired, which under CrossOver is the round trip that costs. The
// game sets one via TQ.exe's SetTimer import at about 14 messages a second.
//
// 0, the default, changes nothing and is byte-identical to not having this.
// Any other value replaces the period TQ.exe asks for, so a run can test
// whether the stalls scale with the timer rate -- which is the difference
// between WM_TIMER causing them and WM_TIMER merely being what a slow peek
// happens to come back with. It only takes effect while the engine trace is
// installed, so a shipping boot never reaches it.

// Installs whatever the mask selects and the build supports. Returns true if
// at least one hook went in. Safe to call when the probe is disabled, when
// `engine` is null, or twice.
bool install(HMODULE engine);

// Restores every patched site. Safe when nothing was installed.
void shutdown();

#ifdef TQ_SELFTEST
// How many hooks the last install() put in, so a test can assert that a run
// with the trace off installed nothing at all.
unsigned installedForTest();
void setTraceMaskForTest(unsigned mask);
// Whether install() would install one trace group, decided the way install()
// decides it. archive_cache_mb can reach install() with the performance probe
// off, and this is what says the trace does not come with it.
bool wantsForTest(unsigned group);
// The region-lock thunk, so the off-game test can drive it both contended and
// not. It enters the section exactly as EnterCriticalSection would.
void enterCriticalSectionForTest(LPCRITICAL_SECTION section);
#endif

}  // namespace engineprobe
}  // namespace tq
