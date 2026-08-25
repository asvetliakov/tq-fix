# Titan Quest Flicker

Titan Quest Anniversary Edition renders correctly under CrossOver **except** that
things which move flicker: shadows, FX/light effects, and some character
geometry. This repo is the investigation and, if it comes to it, the fix — a
32-bit shim DLL loaded into `TQ.exe` that corrects what the D3D11→Metal
translation gets wrong.

The displayed name is **"Titan Quest Flicker"**, the on-disk identity is
`tq-flicker`, and the injected library is `tqflicker.dll`, shipped as a
`winmm.dll` proxy.

## Start every session here

0. **`npm run doctor`** — says what is missing from this machine.
1. **`RUNBOOK.md`** — the "Next session" block at the top says which stage is
   next; below it, every stage and its gate.
2. **The stage plan** it points to, under `docs/plans/`.
3. **`docs/rev/`** — the notebook, and the point of the project:
   `substrate.md` (where the code runs and what the numbers are),
   `observed.md` (what has actually been proven, **including the negative
   results**), `prior-art.md` (DXVK's per-app profile, DXMT's internals, and the
   sibling repo whose harness we are reusing).

Stage plans are self-contained. Trust them over re-deriving; if one is wrong, fix
the plan as part of the stage.

## The rules this project is built around

- **This is a 32-bit target.** `TQ.exe` is PE32. That means `i686-w64-mingw32-g++`,
  stdcall name decoration (`_Foo@4`), and a `winmm` proxy whose exports are
  decorated differently from the 64-bit one next door. Every instinct carried
  over from `../grimdawn-trash` needs this check applied first.
- **Never guess a call's semantics — observe it.** Hook it, log its arguments,
  reproduce the artefact by hand, read the log. Every claim in `observed.md` is
  backed by a real logged call or a real experiment, and the ones that are not
  are labelled as hypotheses.
- **Data patches, not code trampolines.** A vtable slot and an IAT entry are
  data writes. Stay on the data side; if you ever have to leave it, say so in
  `docs/rev/`.
- **Free experiments before expensive ones.** A game option, an env var, or a
  command-line switch costs one launch. A DLL costs a weekend. `docs/plans/stage-0`
  exists because the project may not need to exist.
- **A negative result is a result.** "DXMT's `debug` log level prints no more
  than `info`" saved the next session a launch. Write those down first, because
  they are the ones nobody thinks to record.
- **The log file is the debugger.** There is no usable debugger inside the bottle
  under FEX. Everything we learn, we learn from a log line and from looking at
  the screen.
- **Do not break the game to fix the flicker.** A crash is worse than a flicker.
  Anything we cannot do safely is logged and skipped, not attempted anyway.

## The environment, in one paragraph

CrossOver **Preview 27.0.0** (build 20260821), bottle **"New Bottle"**, on an
**Apple M5 Pro**. `TQ.exe` is 32-bit, so it runs under **FEX**
(`libwow64fex.so`), not Rosetta. The game is on its **DX11** renderer
(`Direct3D11.dll`), translated by **DXMT**. Full numbers in
`docs/rev/substrate.md`.

## Conventions

- **The DLL is C++, cross-compiled with 32-bit MinGW** (`i686-w64-mingw32-g++`),
  `-static -static-libgcc -static-libstdc++`, and **no exception ever crosses
  back into the game**.
- **The host-side tooling is TypeScript**, `"type": "module"`, strict, run with
  `tsx`, consistent with the sibling repos.
- **Never commit game-derived data.** Extracted archives, shader dumps and
  texture output regenerate into `cache/` and stay out of git. Game data is
  © THQ Nordic / Iron Lore.

## The sibling repo

`../grimdawn-trash` is the same trick against Grim Dawn — which is the same
engine lineage, Iron Lore to Crate. It is 64-bit where we are 32-bit, but its
harness is directly reusable and already proven on this machine:

- `scripts/gen-winmm-proxy.sh` — generates the forwarding proxy, resolving the
  real `winmm` at attach so no second file is needed.
- `src/iathook.h`, `src/patch.{cpp,h}` — the IAT and vtable data-write primitives.
- `src/log.{cpp,h}`, `src/config.{cpp,h}`, `src/modules.{cpp,h}` — logging, ini,
  module-wait.
- `scripts/{doctor,selftest-offgame,install-dll,uninstall-dll}.sh`, `tools/pe.ts`,
  `tools/imports.ts`, `tools/log.ts` — patterns to adapt.

Read its `CLAUDE.md` before lifting anything. Its rules were paid for.

## End every session by writing down what you learned

1. Tick the box in `RUNBOOK.md`; note any deviation in the stage plan's
   **Outcome** section.
2. Append to `docs/rev/` — **especially a negative result**.
3. Close or update the row in the risk log at the bottom of `RUNBOOK.md`.
4. Write the next stage's plan, now that the facts exist.
5. Update the "Next session: paste this" block at the top of `RUNBOOK.md`.
6. Commit.
