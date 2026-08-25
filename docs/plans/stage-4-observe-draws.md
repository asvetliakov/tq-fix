# Stage 4 — Count the draws, and find the one that goes missing

**Goal:** identify, from inside the process, the draw call that fails to render,
and establish whether the game issued it. Fix nothing.

**Precondition:** Stage 3's gate met — **it is** (O27, 2026-08-25).

## What Stage 3 handed this stage

- **The device is reachable and captured** — device, context and swapchain all
  come from one `D3D11CreateDeviceAndSwapChain` call (O19). There is only that
  one entry point; `Direct3D11.dll` does not import `D3D11CreateDevice`.
- **Risk 3 is closed for the device** (O20): `ID3D11Device1` is the same object
  with the same vtable. **Ask the same question of `ID3D11DeviceContext1` before
  patching the context** — it has not been asked.
- **The patch primitives exist and are proven in-process**: `src/patch.{h,cpp}`
  has `vtableSlot`, `findSlot` and an undo list, and its self-test patches a
  read-only page, fires through it and restores it on every run, under FEX, in
  this bottle.
- **Every log line carries a pid** (O17), which is what makes a two-process
  per-frame log readable.
- **`BufferCount = 1`, `DISCARD`, windowed, 5120×1440** (O21) — the game asks for
  the shallowest pipeline DXGI allows.
- **The renderer does not inherit our environment** (O18). Anything this stage
  wants to switch at run time must go somewhere Steam can see it, or go in a file
  beside the exe. `TQFLICKER_HOOK=0` already exists as the "install nothing"
  control and is subject to the same rule.
- **Only the process-exit detach path is ever taken** (O23), so anything this
  stage wants to know must be logged when it happens. A per-frame table
  accumulated for a summary at exit will simply be lost.
- **`Direct3D11.dll` loads at a fixed base and the vtables are stable across
  runs** (O25) — device `76465E84`, context `764759E4`, swapchain `7646C808` on
  three runs. A slot index found once can be checked against a later log. The
  *object* pointers change every run; key nothing on them.
- **`TQFLICKER_HOOK=0` gives a control from the same binary**, and reaches the
  renderer only when Steam itself is started with it (O24).

## How to run a measurement, which Stage 3 paid to learn

**State the launch route, and keep it constant.** It is not a detail: it changes
the process topology, the environment the renderer inherits, and whether the
process can exit cleanly at all. Stage 3 lost an afternoon to a "crash" that was
the direct-launch stub being killed by design (O24).

- **Launch from the Steam UI** for anything being measured.
- To set a variable for the renderer, start `steam.exe` through `cxstart` with it
  set, then launch from the Steam UI (O24).
- **Never hard-kill Steam or `wineserver`** to recover from a wedged launch
  (Risk 15, O26). Quit Steam through its own menu.
- Confirm a launch actually happened by looking for `App Running` in Steam's
  `content_log.txt` (Risk 14) — three of Stage 3's launches never reached Steam
  at all and looked identical to a broken hook.

**This plan replaces the original Stage 3 ("Observe the samplers, and the
buffers"), which was built on H-A. O10a refuted H-A: the flicker does not move
when shadow-map resolution moves the shadow frustum boundary, which that
hypothesis required. The sampler work that survives is at the bottom of this
file, demoted to a cheap side-errand.**

## What Stage 0 handed us

From `observed.md` O9–O16, and this is the specification the stage has to meet:

- Individual draws fail to render for **exactly one frame**, never two.
- Failures are **per-object and independent** — one draw vanishes while every
  other draw in the same frame lands (O12: 0.004% of pixels outside the beam
  change on the frames where the beam disappears).
- Each affected object fails on **1–2% of frames**; ~9% of frames contain at
  least one failure somewhere (O14).
- Intervals are **irregular** — no period, no cadence (O12).
- The rate is **per frame, not per second** (O11b).
- It is **not** `Map` returning `WAS_STILL_DRAWING` (O13, p = 0.803).
- DXMT's own log says **nothing** per draw or per frame at any level (O16).

## The instrument

The measurement that made Stage 0 work was a 10fps cap plus a screen recording,
which yields a 1:1 per-frame trace (O12). **Keep using it.** This stage's log and
that recording must share a frame index, or the log cannot be aligned to the
frame the eye saw fail.

So: **log a frame counter, and make it the same counter the capture uses.**
Increment it in a `Present` hook and stamp every line with it.

## The patch

Vtable data-writes on the objects Stage 3 captured. Patch at device-creation
time, before the render thread exists — never mid-frame (Risk 4). `VirtualProtect`
around each write; the sibling's `src/patch.{cpp,h}` has the primitive. Confirm
every slot index against the real interface layout rather than a remembered
number, and remember Risk 3 — patch the vtable of the pointer the game actually
calls through.

**On `IDXGISwapChain::Present`:** increment the frame counter; emit one line per
frame with the frame index and the draw count for the frame just ended.

**On `ID3D11DeviceContext::DrawIndexed` (and `Draw`, `DrawIndexedInstanced`,
`DrawInstanced` — cover all four, a miss looks like a hook that does not work):**
count them per frame. Logging every call at 10fps will be large but is bounded;
if it is unmanageable, log per-frame *counts* first and only go per-call once a
suspect frame is known.

## The question this settles

Align the log against the recording, find the frame where the beam vanished, and
compare its draw count with its neighbours:

- **Draw count is one lower on the bad frame** → the game **did not issue** the
  draw. The defect is upstream of DXMT's rasterisation: the engine decided to
  skip it, and the next question is what it asked DXMT that made it decide that.
- **Draw count is identical** → the game issued the draw and it produced nothing.
  The defect is in the translation, and Stage 5 is either a state fix or a bug
  report.

Nothing else in this project distinguishes those two, and everything after
depends on which it is.

## If the counts are identical — what to log next

Then the draw happened and produced no pixels, so something bound to it was
wrong for that frame. In rough order of cost:

- the **vertex/index buffer** bound at the failing draw, and its `Map`/`Unmap`
  history that frame;
- the **constant buffer** contents for the object's transform — a garbage
  transform flings geometry off-screen, which looks identical to "not drawn"
  (this is **H-B1**, and it is the surviving specific hypothesis);
- the **viewport, scissor and depth state** at the time of the draw.

## H-B1, which is nearly free while we are in here

DXVK ships `d3d11.constantBufferRangeCheck = True` keyed on **`TQ.exe`**
(`prior-art.md`), because Titan Quest binds constant buffers smaller than its
shaders declare. Out-of-range reads return zero on D3D11 and adjacent memory on
Metal; garbage in a transform is invisible geometry.

Log, for every buffer created with `D3D11_BIND_CONSTANT_BUFFER`: `ByteWidth`,
`Usage`, `CPUAccessFlags`. `D3DCOMPILER_43.dll` is already in the process
(`substrate.md`), so `D3DReflect` is available; if it can be used without
disturbing anything, reflect each shader's declared constant-buffer sizes and log
any that **exceed** a width the game actually created. One logged instance
promotes H-B1 from inference to observation.

Still log only. Padding a buffer has a real hazard — `UpdateSubresource` with a
null destination box copies the whole resource and would read past the end of the
game's own source pointer — and that belongs in Stage 5 with its own plan.

## The sampler errand, demoted

H-A is refuted (O10a) and this is no longer on the critical path. But DXMT's
border-colour warning (O2) is real, unexplained, and **it is the evidence the
CodeWeavers bug report in Stage 6 will be built on**. So while
`CreateSamplerState` is one more vtable slot away:

Log the complete `D3D11_SAMPLER_DESC` per call — `Filter` (in particular whether
it is a comparison filter), `AddressU/V/W`, `BorderColor[4]` at full precision,
`ComparisonFunc`, `MinLOD`, `MaxLOD`, `MipLODBias`, `MaxAnisotropy`, a sequence
number and the returned pointer. Count how many samplers exist and how many use
`ADDRESS_BORDER`.

That is a **documentation errand for the bug report**, not a fix attempt. Do not
let it grow.

## Gate

- Our log carries a frame index that can be aligned against a screen recording.
- We can name the frame on which an object visibly failed, and state its draw
  count against its neighbours.
- **An explicit, recorded answer to: did the game issue the draw?**
- Constant-buffer widths logged, with any reflection mismatch called out.
- The game is unharmed, the flicker is unchanged, and exit is clean.

## What was built (2026-08-26) — and proven off-game before any launch

**`src/frames.{h,cpp}`** — fourteen vtable data-writes, installed inside the
`D3D11CreateDeviceAndSwapChain` hook on the game's own thread, before the game
has the device (Risk 4):

| Object | Slots | What the hook does |
|---|---|---|
| `IDXGISwapChain` | `Present` (8) | frame counter; one row per frame in the table; summary every 600 frames |
| `ID3D11DeviceContext` | `DrawIndexed` 12, `Draw` 13, `DrawIndexedInstanced` 20, `DrawInstanced` 21, `DrawAuto` 38, `*Indirect` 39/40, `ExecuteCommandList` 58 | per-frame counts by kind, vertex total, **empty draws** (count 0) |
| `ID3D11DeviceContext` | `Map` (14) | per-frame Map count and how many returned `WAS_STILL_DRAWING` — H-E measured directly |
| `ID3D11Device` | `CreateBuffer` 3 | every `BIND_CONSTANT_BUFFER`: width, usage, CPU access; width histogram at exit — **H-B1** |
| `ID3D11Device` | `CreateVertexShader` 12, `CreatePixelShader` 15 | `D3DReflect` (D3DCOMPILER_43) declared cbuffer sizes per slot; `!!` line if one exceeds every width created so far |
| `ID3D11Device` | `CreateSamplerState` 23 | the full `D3D11_SAMPLER_DESC`; every `ADDRESS_BORDER` one always logged — the O2 errand |

**No slot index is typed by hand.** `scripts/gen-slots.sh` reads them off the
MinGW `*Vtbl` structs at build time into `build/gen/slots.h`, and the install
log prints the method counts (18/115/43) so a header mismatch would show.

**The per-frame table** is a separate file, `%TEMP%\tqflicker-frames.log`,
tab-separated, truncated at device creation, one row per `Present`:
`time pid frame dt_ms sync draws DrawIndexed Draw DrawIndexedInstanced
DrawInstanced other empty verts maps maps_busy new_buffers`. The main log keeps
the one-off facts and a summary line every 600 frames (a minute at 10fps), so a
run that dies still leaves numbers behind (O23).

**The off-game self-test now creates a real DXMT device** (`TQFLICKER_D3D_HOST`
tells the watcher to hook the test exe's own d3d11 import instead of
`Direct3D11.dll`'s) and drives `Present`, `Map`, `CreateBuffer` and
`CreateSamplerState` through the patched slots — **O28**. Everything above was
proven through the 32-bit DXMT in this bottle before the game was launched once.

**Tools:** `npm run frames` (the table: dt distribution, draws/frame, dip
frames), `npm run recording -- REC.mov FRAMES.log` (the Stage 0 detector,
finally committed, plus the alignment search), `npm run keep-log -- label`
(copies both logs into `cache/logs` — Risk 12).

## The measurement run — protocol

1. `npm run doctor` must say the cap is **active** (`d3d11.preferredMaxFrameRate
   = 10` in `dxmt.conf`) and the DLL installed. Both are set now.
2. Launch **from the Steam UI** (O24). Get to the O9 shrine, stand still.
3. Start a macOS screen recording; hold **60 seconds or more** at the shrine.
   Stop the recording, then quit the game normally.
4. `npm run keep-log -- stage4-run1` **before anything else**.
5. `npm run frames` — confirm `dt` is ~100ms (1:1 with the recording), then
   `npm run frames -- --dips`.
6. Move the recording into `cache/captures/`, and run
   `cache/venv/bin/python tools/recording.py cache/captures/*<time>*.mov
   cache/logs/stage4-run1-*-frames.log` (glob the name: it contains a U+202F).
   It prints the anomaly frames, the alignment offset, and for each anomaly the
   draw count on that frame against its neighbours.
7. Write the answer into `observed.md`, whichever way it goes.

**Reading the result.** The recording's anomaly frames either land on draw-count
dips (the game *skipped* a draw) or they do not (the game *issued* it and DXMT
lost it). The tool prints the chance-coincidence rate next to the hit count so a
weak match is not over-read. Also check `empty`: a draw *issued with zero
indices* is a third answer, and it would look like "identical count" in a
naive read. And `maps_busy`: any non-zero on a bad frame reopens H-E.

## Outcome

**Run 2026-08-26. Gate MET. The answer is: the game issued the draw.**

| Gate clause | |
|---|---|
| A frame index that can be aligned against a screen recording | **met** — aligned by wall clock, verified `birth + duration == mtime` (O30) |
| Name the frame an object failed on, and its draw count against its neighbours | **met** — 56 such frames listed, each with `prev \| this \| next` |
| **An explicit, recorded answer to: did the game issue the draw?** | **met — YES. O30.** |
| Constant-buffer widths logged, reflection mismatch called out | **met** — O32, no instance, and the test's weakness stated |
| The game is unharmed, the flicker unchanged, exit clean | **met** — 909 frames, played normally, exit through the loader |

**The measurement.** Over the 387 frames the recording covers: **56 frames show
an object vanish; 4 show the draw count fall.** The count is identical between
consecutive frames **93.3%** of the time, so a missing draw would have read as a
clean −1 — and the four dips are −3/−4, the wrong size and the wrong number.
`empty` draws: **0**. `Map` returning `WAS_STILL_DRAWING`: **0**. Instanced,
indirect, `DrawAuto` and `ExecuteCommandList`: **all 0**, so no draws happened
anywhere we were not looking.

> **The game submits the draw and it produces no pixels. The fault is on the
> DXMT/Metal side.**

**What it produced:** O30 (the answer), O31 (H-E refuted directly — `Map` never
said busy), O32 (H-B1 un-instanced, with the test's weakness stated), O33 (the
one border sampler, fully described — the Stage 6 bug report's evidence), O34
(Risk 3 closed in-game for the context).

**What it cost, worth not repeating:** aligning by timing fingerprint does not
work at a 10fps cap — both series are ~100ms everywhere, best offset 1.70ms
median error against 2.60ms worst. Use the wall clock. And the alignment must
never be done by matching anomalies to dips, which would assume the answer.

### Where this leaves the project

**Stage 1 is now the critical path**, exactly as its own plan predicted:
*revisit if Stage 4 answers "the game did issue the draw" — at which point the
fault is on the Metal side and this becomes the only way to see it.* It needs
**Xcode** for a `.gputrace` viewer, which this machine does not have.

**Stage 5 cannot be written yet.** There is no D3D11-side fix to attempt: the
D3D11 side is correct. What can be done without Xcode is in
`docs/plans/stage-5-fix.md`.
