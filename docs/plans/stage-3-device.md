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
**not** in the process at `DllMain` time. Wait for it. The sibling repo's
`src/modules.{cpp,h}` already does this; reuse it rather than inventing a poll
loop.

Hook **both** entry points. The game may use either, and a miss looks identical
to a hook that does not work.

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

*(fill in at the end of the stage)*
