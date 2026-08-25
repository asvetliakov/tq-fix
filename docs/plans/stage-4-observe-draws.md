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

## Outcome

*(fill in at the end of the stage)*
