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

**Complete, 2026-08-25.** The proxy builds, matches the real winmm export for
export, passes an off-game self-test, and forwarded all 186 exports inside the
running game with **zero** calls to an unresolved slot.

### Risk 2 is closed, and not the way the plan expected

The plan warned about `_Foo@N` stdcall decoration and told us to use
`--kill-at`. **None of that was needed.** Windows' own 32-bit winmm exports
**186 plain names — none with `@`, none with a leading `_`** — so the `.def`
lists plain names and the link needs no decoration handling at all. Settled by
reading the real DLL's export table, which is what the plan said to do; the
reasoning about stdcall would have led somewhere else. The generator now
**refuses to run** if it ever sees a decorated name, so the assumption is
checked rather than remembered.

### What genuinely differed from the 64-bit sibling

- **`jmp *slot(%rip)` does not exist on i386.** RIP-relative addressing is
  x86-64 only, so the stub is an absolute indirect jump (`ff 25`), the slots are
  `.long` not `.quad`, and the table is `.p2align 2`. Verified by disassembling
  the result: **186 stubs, 186 distinct slots, contiguous with no gaps and no
  repeats**, every one initialised to the fallback. That check exists because
  the sibling's own comment warns that an arithmetic slip in a generator
  emitting 186 of them is a jump into the middle of a pointer.
- **Assembler symbols carry a leading underscore** on i386 MinGW.
- **The i386 winmm is in `syswow64`.** This bottle is ARM64, so
  `system32\winmm.dll` is a **PE32+ Aarch64** binary; generating against it
  would have built cleanly and failed at load with nothing in the log. The
  generator refuses a non-i386 input. The DLL itself asks
  `GetSystemDirectoryW`, which under WOW64 answers correctly anywhere — and the
  log confirms the redirect: it reports `system32` while the file loaded is
  `syswow64`'s.
- **An unresolved slot is a real hazard here and is not one next door.** These
  are `__stdcall`: the *callee* cleans the stack, and the byte count differs per
  export, so one generic fallback (which compiles to a bare `ret`) would corrupt
  the stack. There is no general fix — export tables carry no argument counts —
  so it is made unreachable by construction and reported with `!!` at attach.
  Documented at length on `tq_winmm_unresolved`.

### What the stage found out about the game — see O17

**`TQ.exe` runs as two processes**, and we are loaded into both. Stage 3's plan
has been amended: bound the module wait, stamp every line with a pid, and do not
rely on shutdown reporting in the first process, which was terminated rather
than exiting through the loader.

### Small things, recorded so they are not rediscovered

- `%TEMP%` is `C:\users\crossover\AppData\Local\Temp`, not
  `users\crossover\Temp`. The install script printed the wrong one once.
- **`wineserver` writes `user.reg` lazily.** Grepping it immediately after
  uninstall still shows the old override and makes a working uninstall look
  broken. Wait a few seconds.
- Wine prefers its builtin winmm, so the DLL override is required — without it
  the symptom is *nothing at all*: the game runs fine and no log appears. The
  override is scoped to `TQ.exe` under `AppDefaults`, not bottle-wide, which
  would also have applied to Steam.
