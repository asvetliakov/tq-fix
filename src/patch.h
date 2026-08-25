// The two patch primitives, and the undo list that makes them reversible.
//
// Adapted from ../grimdawn-trash/src/patch.{h,cpp}, which is 64-bit; nothing
// here is architecture-dependent, but the reasoning below is theirs and was
// paid for over there.
//
// Both are **data writes** (CLAUDE.md): an import table is an array of pointers
// and a vtable is an array of pointers, so redirecting a call means storing one
// pointer, not rewriting an instruction. Nothing in this project leaves the data
// side.
//
// Both remember what they overwrote and both are put back by `unpatchAll()` on
// an orderly unload — and *only* on an orderly unload. On process exit the
// address space is going away and touching another module's memory then is a way
// to crash on quit, which would look exactly like our patches breaking the game.
//
// Both are safe to install twice: a second patch of a slot we already hold is
// not applied, and the original recorded the first time is returned, so a caller
// that stores it still calls through to the real function rather than to itself.
//
// Nothing here allocates. The undo list is a fixed array, because a failure to
// allocate on the way in would leave a patch installed with no way to remove it.

#pragma once

#include <windows.h>

namespace tq {
namespace patch {

/**
 * The IAT slot `target` uses for `impDll!impFunc`, or null if it does not import
 * it. **Exposed separately from `iat()` on purpose**: a module found by
 * `GetModuleHandleW` may still be mid-load, with its imports not yet snapped, and
 * a slot patched before the loader writes it is a slot the loader then
 * overwrites — a hook that silently never fires. `device.cpp` reads the slot
 * first and waits until it holds something real. See the note there.
 */
void** iatSlot(HMODULE target, const char* impDll, const char* impFunc);

/** Redirect `target`'s import of `impDll!impFunc`; returns what was there. */
void* iat(HMODULE target, const char* targetName,
          const char* impDll, const char* impFunc, void* replacement);

/**
 * Overwrite one vtable slot. `what` is only for the log, and it is worth
 * spending: that line is what a future session reads to see which slot index
 * this build decided on.
 *
 * The page is made PAGE_READWRITE — not PAGE_EXECUTE_READWRITE. We are writing
 * data, and asking for execute permission on a data page is how a patch that is
 * really a code patch disguises itself.
 *
 * Unused in Stage 3, and built anyway: Stage 4 patches `Present` and the four
 * `Draw*` slots, and `selfTest()` below proves the primitive works in this
 * process — under FEX, in this bottle — before a stage depends on it.
 */
void* vtableSlot(void** vt, int slot, const char* what, void* replacement);

/** The index of `fn` in `vt`, or -1. */
int findSlot(void* const* vt, int maxSlots, const void* fn);

/** Is this range mapped and readable? The difference between a wrong log line
 *  and a wild pointer read. */
bool readable(const void* p, size_t bytes);

int  installed();
void unpatchAll();

/**
 * Prove both primitives on a table of our own, in whatever process we are in.
 *
 * Worth its dozen lines: a real vtable is in a read-only page, and the whole
 * design rests on being able to open one, store a pointer and close it again —
 * under Wine, under FEX, in a process we do not control. It runs before anything
 * real is patched, so the undo it performs cannot disturb another patch, and it
 * is the only check here that needs no game (scripts/selftest-offgame.sh).
 */
bool selfTest();

}  // namespace patch
}  // namespace tq
