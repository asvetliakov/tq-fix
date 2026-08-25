# Stage 3 — Reach the device

**Goal:** hold a valid `ID3D11Device*` and `ID3D11DeviceContext*`, log them, and
change nothing else.

**Precondition:** Stage 2's gate met.

## Two processes — read this first

**O17: `TQ.exe` runs as two processes** (pid 40 then pid 1008 on the observed
launch), and our DLL is loaded into **both**. This was discovered by Stage 2's
own log and is invisible from outside.

What it means here:

- **A module-wait for `Direct3D11.dll` may never complete** in the first
  process, because it may never load a renderer at all. That is not a broken
  hook and must not be diagnosed as one. Bound the wait, log the giving-up, and
  say which pid gave up.
- **Every line this stage writes must carry its pid.** Two processes share one
  log file. Without a pid the log is ambiguous, and an ambiguous log becomes a
  wrong fact in `docs/rev/`.
- **Shutdown reporting is unreliable in the first process.** It logged no
  detach at all on the observed launch, so it was terminated rather than
  exiting through the loader. Anything worth knowing must be logged when it
  happens, not accumulated for a summary at exit.

## The hook point

`Direct3D11.dll` imports `d3d11.dll` **statically** (`substrate.md`), so its
import table has a resolvable entry for `D3D11CreateDevice` and
`D3D11CreateDeviceAndSwapChain`. Rewriting that entry is a data write — no
trampoline, no code patching, in keeping with the house rule.

`Engine.dll` loads the renderer by name at runtime, so `Direct3D11.dll` is
**not** in the process at `DllMain` time. Wait for it.

> **Corrected during the stage.** This plan said the sibling repo's
> `src/modules.{cpp,h}` "already does this". It does not — next door `Game.dll`
> is already loaded at attach, so that file only walks the PEB to *describe*
> modules and has no wait at all. A bounded, cancellable poll was written here
> (`src/modules.{h,cpp}`), and O19 shows the poll interval matters: the game
> called through 147ms after the module appeared.

Hook **both** entry points. The game may use either, and a miss looks identical
to a hook that does not work.

> **Corrected during the stage.** Only one of them exists.
> `Direct3D11.dll` imports `D3D11CreateDeviceAndSwapChain` and **not**
> `D3D11CreateDevice` (O19). Still try both — the absent one is a fact about the
> game and gets a log line saying so, rather than a silent nothing that reads as
> a failed patch.

## What to do in the hook

1. Call through to the real function first.
2. If it succeeded, take the returned device and context.
3. Log, as one-off lines: the device pointer, the immediate context pointer, the
   returned feature level (expect `11_0` — DXMT reports `11_1` as its maximum but
   the game asks for `11_0`), the flags the game passed, and whether it was the
   swapchain variant.
4. Return. **Patch nothing this stage.**

## Traps

- **`QueryInterface` may hand back a different object.** DXMT may implement
  `ID3D11Device1` on a separate object with a separate vtable. Whatever we patch
  in Stage 4 must be the vtable of the pointer the game is actually calling
  through. Log the result of `QueryInterface` for `ID3D11Device1` now, while it
  is cheap, so Stage 4 knows what it is dealing with. This is Risk 3.
- **Do not hold a reference you do not release**, and do not `AddRef` casually —
  a device the game thinks it destroyed but we are keeping alive is a shutdown
  hang, and a hang at exit is hard to attribute.
- **No exception may cross back into the game.** Standing rule.

## A working reference

The THQ Nordic overlay does this exact job successfully in this exact bottle
(`prior-art.md`, O5). If our hook misbehaves, its log at
`AppData\Local\Temp\Indicium-Supra.log` shows the order of operations that works.

## Gate

- Our log names the device pointer, the context pointer and the feature level.
- The game reaches gameplay and plays normally.
- Exit is clean — no hang, no crash, no error dialog.

## Outcome

**Run 2026-08-25. Gate MET (O27), after a false alarm that is worth reading.**

| Gate clause | |
|---|---|
| Our log names the device pointer, the context pointer and the feature level | **met** — O19, O27 |
| The game reaches gameplay and plays normally | **met** — the reporter played with the hook installed (O27) |
| Exit is clean | **met** — exited through the loader, both summaries written, hook called exactly once (O27) |

### The false alarm, kept because the lesson is the valuable part

The first run looked like a crash: the render process lived **seven seconds** and
was terminated without logging a detach (O22). The stage stopped there rather
than building on it, and constructed a control — the same binary with
`TQFLICKER_HOOK=0`, so the hook rather than the whole DLL was the variable.

**The hook was exonerated** (O24). What actually differed was the *launch route*:
the direct `cxstart TQ.exe` route runs the Steam handoff stub, which is killed
once Steam takes over. Held constant, the game exits cleanly with the hook
installed. **The uncontrolled variable was the one that mattered**, and it was
not on anyone's list.

A second scare followed and was also not ours: character create/select stopped
working, with the hook on, with the hook off, **and with the DLL removed
entirely** (O26). The probable cause was the hard `wineserver` kill used to
recover from a wedged Steam — self-inflicted, now Risk 15. A normal restart fixed
it with no save data lost.

### What the stage produced

- **O18** — the two processes explained. The `TQ.exe` we launch is a Steam
  handoff stub; the renderer is **Steam's** child, launched as `TQ.exe /dx11`.
  This narrows O15: per-run `cxstart` environment injection does **not** reach
  the process that renders.
- **O19** — device `06536BC0`, context `06901448`, swapchain `067F0748`, feature
  level **11_0**, via the one entry point that exists. The hook landed 147ms
  before the game called through it.
- **O20** — **Risk 3 closed.** `ID3D11Device1` is the same object with the same
  vtable, so Stage 4 can patch one vtable.
- **O21** — the swapchain description, including `BufferCount = 1`.
- **O22** — the unresolved seven-second session, and the two things built so the
  next session can attribute it in one launch.
- **O23** — `FreeLibrary` does not unload us here, so only the process-exit
  detach path is ever taken.

### What was built

`src/patch.{h,cpp}` (IAT and vtable data-writes, undo list, in-process
self-test), `src/modules.{h,cpp}` (bounded cancellable module wait, PEB module
census), `src/device.{h,cpp}` (the hook), a watcher thread in `dllmain.cpp`, a
**pid on every log line** (O17's demand, stamped in `log.cpp` so no caller can
forget it), a `TQFLICKER_HOOK=0` kill-switch and a startup heartbeat.

### Two things found by writing the tests, not by reasoning

- The 180-second module wait was **not interruptible**, so an orderly
  `FreeLibrary` would have unmapped our code while the watcher was still parked
  in it. Every wait now goes through `modules::nap`, which a cancel event cuts
  short. *(O23 then showed `FreeLibrary` never unloads us here anyway — the fix
  is defensive, not load-bearing.)*
- `GetModuleHandleW` answers **before** the loader has snapped the module's
  imports, and an IAT slot patched in that window is one the loader then
  overwrites — a hook installed, reported as installed, and never firing.
  `device.cpp` waits for the slot to hold a mapped address before patching, and
  re-reads it afterwards to confirm it survived.

### Where it stopped

After that launch, Steam stopped responding to launch requests: three further
attempts never reached it, so **no control run exists** and the bottle needs a
`wineserver` restart. The DLL was left **uninstalled** rather than leaving a
build of unknown safety in the game directory.
