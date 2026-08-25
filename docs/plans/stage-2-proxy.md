# Stage 2 — Get inside the process

**Goal:** a 32-bit `winmm.dll` sitting beside `TQ.exe` that forwards every call
to the real one and writes a single line to a log. Nothing else. If this stage
does anything clever it has failed.

**Precondition:** Stage 0 decided to continue.

## Why `winmm`

`TQ.exe` imports `WINMM.dll` directly (`substrate.md`), so a proxy in the game
directory is loaded before anything else we care about. This is the same vector
`../grimdawn-trash` uses against Grim Dawn, and that repo's `CLAUDE.md` explains
why the proxy must resolve the real `winmm` from `<system32>` **itself, at
attach**, rather than relying on a `.def` forwarder to a second copied file: the
second file cannot go in a ZIP.

Do not re-derive that. Reuse `scripts/gen-winmm-proxy.sh`.

## The 32-bit difference — read this before copying anything

The sibling repo is x86-64. We are i386. What changes:

- Build with **`i686-w64-mingw32-g++`**, not `x86_64-w64-mingw32-g++`.
- **stdcall decoration.** 32-bit `WINAPI` exports are decorated `_Name@N`, where
  `N` is the argument byte count. The generated stubs and the `.def` must account
  for this or the game will fail to resolve imports and refuse to start. Use
  `--kill-at` in the link, and confirm the resulting export names against the
  **real** 32-bit `winmm.dll` in the bottle's `system32` — not against the
  64-bit one, and not against your memory of it.
- Verify with `tools/pe.ts` (adapted from the sibling) that our export table
  matches the system DLL's, name for name, before ever launching the game.

Getting this wrong is Risk 2 in the runbook and is the single most likely way
this stage burns an afternoon.

## Steps

1. `npm run doctor` — toolchain, bottle path, game path, and that
   `i686-w64-mingw32-g++` exists. Write the script now; every later stage starts
   with it.
2. Generate the stub list from the bottle's real 32-bit `winmm.dll` export table.
3. Build `tqflicker.dll` and rename/ship it as `winmm.dll`.
4. `DllMain` on `DLL_PROCESS_ATTACH`: resolve the real `winmm` from
   `<system32>`, fill the slot table, open the log, write one line naming the
   build, and return. **Do no work on the loader lock.**
5. Off-game self-test: load the DLL from macOS-side tooling, check every slot
   resolved and that a forwarded call reaches the real implementation.
6. Install into the game directory and launch.

## Logging

Adopt the sibling's two-level pattern from the start, because retrofitting it is
tedious: one file that is always written and stays about a hundred lines a
session, and a verbose channel enabled by an ini key or a marker file beside the
log. A new line is verbose **unless it names something that happened once**.

Put the log somewhere a terminal can read it. The sibling repo's `CLAUDE.md`
records that macOS TCC makes `~/Documents` unreadable from a shell — so `%TEMP%`,
not `Documents`.

## Gate

- The game launches, reaches gameplay, and plays exactly as it did before —
  including the flicker, unchanged. We have added an observer, not a variable.
- Our log file exists and contains our line.
- The off-game self-test passes with no game running.
- Uninstall puts the directory back exactly as it was.

## Outcome

*(fill in at the end of the stage)*
