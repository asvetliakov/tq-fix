# Stage 1 — Metal frame capture

**Goal:** find out whether the missing draw was **submitted to Metal at all**.
Write no code.

**Precondition:** Stage 0's gate met.

## Why this is the next stage and not the proxy

Stage 0 ended with one question, and both of its answers demand different fixes:

- **The game never submits the draw.** Then the game is deciding not to draw,
  because something DXMT returned made it decide that. A shim would have to
  change what DXMT tells the game.
- **The game submits it and DXMT drops it.** Then the game is innocent and the
  fix is in the translation — or the honest outcome is a CodeWeavers bug report.

A frame capture answers this directly, and it needs **no DLL, no compiler and no
injection** — only environment variables, which O15 showed can be set per-run
with CrossOver still running. The house rule is free experiments before expensive
ones, and a weekend of `winmm` proxy work spent before this question is answered
could easily be spent on the wrong half of the problem.

## The mechanism

From the strings in the shipped 32-bit `d3d11.dll` (`substrate.md`, and confirmed
again in Stage 0):

```
DXMT_CAPTURE_EXECUTABLE   DXMT_CAPTURE_FRAME   MTL_CAPTURE_ENABLED
dxmt::CaptureState::shouldCaptureNextFrame()
dxmt::CaptureState::scheduleNextFrameCapture(unsigned long long)
dxmt::CaptureState::getNextAction(unsigned long long)
"DXMT capture enabled"   "A new capture will be saved to "
_%H'%M'%S_%m-%d-%y.gputrace
```

`scheduleNextFrameCapture` taking a frame number says the capture is **keyed on a
frame index**. Whether `DXMT_CAPTURE_FRAME` accepts a count, a range or a single
index is **not known** — find out by trying, and write down what the syntax turns
out to be, because nothing documents it.

Launch via `cxstart` with the variables set (O15), *not* from the Steam UI — a
game started from Steam's UI inherits Steam's environment, not ours:

```sh
CX="/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver"
G="C:\Program Files (x86)\Steam\steamapps\common\Titan Quest Anniversary Edition"
MTL_CAPTURE_ENABLED=1 DXMT_CAPTURE_EXECUTABLE=TQ.exe DXMT_CAPTURE_FRAME=<n> \
  "$CX/bin/cxstart" --bottle "New Bottle" --no-convert --workdir "$G" -- "$G\TQ.exe"
```

## The sampling problem, and how to beat it

A capture names a frame in advance; the defect picks its own frames. From O14 the
odds are known and they are better than they look:

- any given **object** drops on ~1–2% of frames;
- but **~9% of frames contain at least one** object anomaly somewhere on screen.

So a blind capture has roughly a **1 in 11** chance of containing a defect. Three
things make that workable:

1. **Keep the 10fps cap** (`dxmt.conf`, `d3d11.preferredMaxFrameRate = 10`). It
   does not change the per-frame probability (O11b) but it makes frame indices
   countable and the session controllable.
2. **A good frame is not a wasted capture.** The first capture's real job is to
   teach us the frame's structure — how many draws, which one is the shrine beam,
   what resources it binds, what the shadow pass looks like. Without that
   vocabulary a bad frame is uninterpretable anyway. **Capture a good frame
   deliberately first.**
3. **Then compare.** With the good frame understood, take captures until one
   contains an anomaly. Ten attempts is better than even odds.

If `DXMT_CAPTURE_FRAME` turns out to accept a **range or a count**, this problem
mostly evaporates — capture twenty consecutive frames and at least one will be
bad. **Establish that first**, before grinding through single captures.

## What to look for in Xcode

Open the `.gputrace`. For the frame in hand:

- **The draw list.** How many draw calls, grouped into passes. Identify the
  shadow pass, the main opaque pass and the transparency/FX pass.
- **The beam.** Find the draw that produces the shrine light beam — it is the
  object with the largest and best-characterised signature from O12/O14. Note its
  vertex buffer, its constant buffers, its pipeline state.
- **On a bad frame, the single question:** is that draw **present in the command
  buffer with zero effect**, or **absent from the command buffer entirely**?

That is the whole stage. Everything else is a bonus.

## Traps

- **The capture is of the Metal side.** It shows what DXMT submitted, not what
  the game called. "Absent from the command buffer" therefore does **not** by
  itself prove the game did not call `Draw` — DXMT could have swallowed it. It
  narrows the question to one side of the translation, and Stage 4 settles which.
  Do not overclaim this in `observed.md`.
- **`MTL_CAPTURE_ENABLED=1` must be set or the capture silently does nothing** —
  the same failure mode as O4, where `DXMT_LOG_PATH` wrote nothing because the
  directory did not exist. Check for the `"DXMT capture enabled"` line in the
  log before trusting a run.
- **Captures are large.** Keep them in `cache/`; they are game-derived and must
  not be committed.

## Gate

- A `.gputrace` opens in Xcode and we can describe the frame's pass structure and
  name the draw that produces the shrine beam.
- The syntax `DXMT_CAPTURE_FRAME` actually accepts is written down in
  `substrate.md`, including whether ranges work.
- Either a bad frame is captured and the submitted-or-dropped question is
  answered for the Metal side — or, if enough attempts fail, that is recorded as
  a negative result with the number of attempts, and we proceed to Stage 2 to
  answer it from inside the process instead.

## Outcome

*(fill in at the end of the stage)*
