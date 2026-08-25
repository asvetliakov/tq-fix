# Stage 0 — Free experiments

**Goal:** spend four launches finding out whether this project needs to exist.

Every experiment here costs one game launch and no code. Between them they could
end the project outright, halve it, or confirm that a DLL is the only way. Do
them **before** writing anything.

## Ground rules for every experiment

- **One variable at a time.** Change one thing, launch, look, write it down,
  change it back.
- **Same scene every time.** Pick one reproducible spot with visible flicker —
  the resurrection shrine from the residual report is the obvious candidate,
  since it exercises Defect B — and go back to it every run. A wandering test
  scene has ruined more graphics debugging than any wrong hypothesis.
- **Look at the screen.** No log line will tell you the flicker stopped.
- **Quit CrossOver before editing `cxbottle.conf`** — it rewrites the file on
  exit and will discard your edits (`substrate.md`).
- Record the outcome in `docs/rev/observed.md` as you go, not at the end.

## E0 — Re-establish the baseline

Before changing anything, go to the test scene on the current configuration and
write down precisely what flickers, how often, and how badly. Two sentences.
Everything below is measured against this.

While you are there, settle the open question from the residual report: **is it
hair, or is it clothes?** Stand still and watch one character. It matters,
because hair in this engine is alpha-tested and clothes are skinned, and those
are different suspects.

## E1 — `/dx9` — the one that could end the project

**Never tried, on any backend.** The game ships a complete DirectX 9 renderer
(`Direct3D.dll`), `d3dx9_42.dll` is already installed in the bottle, and the
switch is `/dx9` (`substrate.md`). That renderer is the 2006-vintage,
heavily-travelled one; the DX11 path is the 2016 retrofit where both defects
live.

Set it in Steam → Titan Quest Anniversary Edition → Properties → Launch Options:

```
/dx9
```

Then work through the backends, because D3D9 does **not** go through DXMT at all
— DXMT ships no `d3d9.dll` — so the backend choice means something different on
this path than it did on the DX11 one:

| Run | `CX_GRAPHICS_BACKEND` | Expect |
|-----|----------------------|--------|
| 1 | `dxmt` (leave as-is) | DXMT is not involved; CrossOver picks its D3D9 handler |
| 2 | `d3dmetal` | Apple's translation. Black on DX11, but that says nothing about its D3D9 path |
| 3 | `wine` | wined3d. Slow, but "slow and correct" beats "fast and flickering" |

**If any of these renders correctly, the project is done** — write it up in
`observed.md`, close Risk 1, and tell the user. Note the frame rate and any
visual downgrade so they can decide whether the trade is worth it.

## E2 — `FEX_X87REDUCEDPRECISION=0`

One flip in `[EnvironmentVariables]`, back on `/dx11` + `dxmt`. `TQ.exe` is
32-bit and therefore runs under FEX, so the variable genuinely applies to it
(`substrate.md`).

Prior is low — 32-bit MSVC 11.0 defaults to SSE2 codegen, so little of the hot
float math is x87 — but it is one flip and one launch, and if reduced precision
is degrading skinning or shadow-matrix math it would show up on exactly the
geometry that flickers.

Expect a frame-rate cost if it changes anything.

## E3 — `DXMT_LOG_LEVEL=trace`

`debug` is already known to print nothing beyond `info` (O3) — **do not retry
it**. `trace` is untried and is the last unspent level.

Set it, walk to the test scene, quit, and read `C:\dxmtlog\TQ_d3d11.log`.
Be prepared for it to be enormous or to be the same three lines; both are
results worth recording.

What we are hoping for: a second warning, anything mentioning a fallback or an
unsupported format, or evidence about what DXMT actually substituted for the
border colour.

## E4 — Bound Defect A more tightly

Shadows off "greatly reduces" the flicker (O1). Sharpen that, because H-A rests
on it:

- **Shadow quality low vs high.** Changing shadow map resolution moves the
  frustum boundary. If H-A is right, the flicker pattern should *move* rather
  than simply get better or worse — that is much stronger evidence than
  "reduced".
- **Shadows off, at the resurrection shrine specifically.** Confirm the shrine
  FX still flickers with shadows off. The residual report said so; verify it,
  because Stage 5 is built on it.

## Deliverable

Stage 0 is done when `docs/rev/observed.md` has a numbered entry for each of
E0–E4 — **including the ones that changed nothing** — and `RUNBOOK.md` records
an explicit decision: continue to Stage 1, or stop because `/dx9` works.

## Outcome

*(fill in at the end of the stage)*
