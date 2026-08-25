# Runbook

## Next session: paste this

> Read `CLAUDE.md`, then `RUNBOOK.md`, then `docs/rev/`. **Stage 0 is done** and
> it changed the model: there is **one** defect, not two — individual draws fail
> to render for exactly one frame, independently, each on 1–2% of frames. H-A
> (sampler border colour) and H-E (`Map` DO_NOT_WAIT) are both **refuted**, and
> there is no configuration fix left.
>
> **Stage 1 (Metal capture) is deferred — there is no Xcode on this machine and a
> `.gputrace` has no viewer.** We are at **Stage 2**, the 32-bit `winmm` proxy —
> `docs/plans/stage-2-proxy.md` — on the way to **Stage 4**, which answers the one
> question everything else depends on: **did the game issue the missing draw?**
> Stage 4 answers it on the D3D11 side, which is the side that matters.
>
> **`npm run doctor` is broken** — `scripts/doctor.sh` does not exist, and
> `scripts/`, `src/` and `tools/` are empty but for `.gitkeep`. Stage 2 builds
> that scaffolding. The 32-bit cross-compiler is present and working:
> `i686-w64-mingw32-g++` (GCC 16.2.0).
>
> Before measuring anything, set up the instrument from O12: cap the frame rate
> to 10 in `dxmt.conf` and screen-record. At 10fps a macOS screen recording is a
> 1:1 per-frame trace of the renderer, and it is the only reason Stage 0
> produced numbers instead of impressions.

---

## Where things stand

Titan Quest Anniversary Edition runs correctly under CrossOver's DXMT backend
**except that moving things flicker**. Stage 0 (2026-08-25) measured the artefact
rather than describing it, and the result replaced the model the project started
with.

### The defect, as measured

> **Individual draws fail to render for exactly one frame, independently of one
> another, each at a rate of one to two percent of frames.**

Per-object, never global. Always exactly one frame, never two. Irregular — no
period. Counted in **frames**, not seconds. It hits the shadow pass, the FX pass
and skinned character geometry alike. Full evidence in `docs/rev/observed.md`
O9–O16; the pictures are in `cache/captures/`.

**There is no Defect A and Defect B.** O1 read the artefact as two defects
because turning shadows off "greatly reduced" it. O10b and O14 show that shadows
are simply the most numerous dynamic draws on screen — removing them removes
*victims*, not a *cause*. One defect, many victims.

### What is dead

| | Was | Killed by |
|---|---|---|
| **H-A** — sampler border colour `-FLT_MAX` | the prime suspect | **O10a** — flicker does not move when shadow-map resolution moves the frustum boundary, which H-A required |
| **H-E** — `Map` with `DO_NOT_WAIT` fails, engine skips draw | best-fitting hypothesis of Stage 0 | **O13** — `ignoreMapFlagNoWait=True` changes nothing, p = 0.803 |
| **H-C** — FEX reduced x87 precision | listed as one cheap flip | **O8** — tested at `0`, no effect |
| **Configuration fixes** | seven DXMT tunables | **O7 + O13** — none relevant, and the one plausible knob measured as doing nothing |

DXMT's border-colour warning (O2) is still real and still unexplained. It is no
longer a suspect; it is **evidence for the Stage 6 bug report**.

### What is alive

- **H-B1** — out-of-range constant-buffer reads. Strengthened, not weakened, by
  Stage 0: garbage in a transform flings geometry off-screen, which looks
  identical to "not drawn", and DXVK ships `constantBufferRangeCheck` keyed on
  **`TQ.exe`** specifically. DXMT has no equivalent knob, so it cannot be tested
  for free.
- **H-D** — draws or their resources intermittently missing. The general form;
  true almost by construction, and too unspecific to act on until Stage 1 or
  Stage 4 says which side of the translation it lives on.

### The one question

**Was the missing draw ever submitted?** The two answers need opposite fixes, and
nothing in the project distinguishes them yet. **Stage 4 asks the game**, which
is the side that matters and the side we can reach. Stage 1 would have asked
Metal, and is deferred for want of a `.gputrace` viewer.

### The instrument, which outlasts every stage

Cap the frame rate to 10 (`dxmt.conf`, `d3d11.preferredMaxFrameRate = 10`) and
screen-record. macOS records only on change, so at 10fps **one recorded frame ==
one game frame**, and a 60-second recording becomes a per-frame trace of the
renderer (O12). Every number in `observed.md` came from this. It is **not** a
workaround for the user (O11b) — it is the oscilloscope.

Two supporting facts that make experiments cheap:
- **`dxmt.conf` beside `TQ.exe` works** (O11a) — no `cxbottle.conf` edit, so
  Risk 8 does not apply.
- **Environment variables can be injected per-run via `cxstart`** with CrossOver
  already running (O15) — the mechanism that would have made Stage 1 free, and
  still the way to set any variable for a single run.

---

## Stages

Each stage has a **gate**. Do not tick a box until its gate has actually been
met, in the game, with your own eyes on the screen.

*Re-ordered at the end of Stage 0. The `winmm` proxy was Stage 1; the Metal
capture was Stage 5. The capture needs no code, so "free experiments before
expensive ones" moved it to the front — and then it turned out to need Xcode,
which this machine does not have, so it is deferred and the proxy is next after
all. The numbering is kept as-is rather than churned a second time.*

### ☑ Stage 0 — Free experiments

**Plan:** `docs/plans/stage-0-free-experiments.md` — **complete, 2026-08-25.**
Eight experiments (E0–E7; E1 descoped, E5–E7 added mid-stage). Produced O8–O16,
refuted H-A, H-C and H-E, unified the two defects into one, and built the
measuring instrument.
**Gate met:** every experiment recorded in `observed.md` including the negative
results, and an explicit decision — **continue**.

### ⊘ Stage 1 — Metal frame capture — **DEFERRED, no Xcode**

**Plan:** `docs/plans/stage-1-frame-capture.md`
No code. `DXMT_CAPTURE_FRAME` + `MTL_CAPTURE_ENABLED` via `cxstart` (O15),
producing a `.gputrace` for Xcode.

**Blocked 2026-08-25:** this machine has Command Line Tools only — no
`/Applications/Xcode.app`, and `xcrun -f metal` fails. **A `.gputrace` has no
viewer here.** The Metal debugger ships only with the full ~15 GB Xcode app; the
Command Line Tools are not enough.

**Why this costs less than it looks.** The capture shows what DXMT submitted **to
Metal**. The central question — *did the game issue the draw?* — is about the
**D3D11 side**, and **Stage 4 answers it directly** by counting the game's own
`Draw*` calls. Stage 1 was put first because it was **free**, not because it was
better; with Xcode required it is neither.

**Revisit if** Xcode is installed, **or** Stage 4 answers "the game *did* issue
the draw" — at which point the fault is on the Metal side and this becomes the
only way to see it.

### ☐ Stage 2 — Get inside the process

**Plan:** `docs/plans/stage-2-proxy.md`
A 32-bit `winmm.dll` proxy that forwards everything and writes one line to a log.
Adapted from `../grimdawn-trash/scripts/gen-winmm-proxy.sh`, with stdcall
decoration handled.
**Gate:** the game launches, plays normally, and our log file exists with our
line in it. Plus an off-game self-test that loads the DLL and checks forwarding.

### ☐ Stage 3 — Reach the device

**Plan:** `docs/plans/stage-3-device.md`
Wait for `Direct3D11.dll`, IAT-hook its imported `D3D11CreateDevice` /
`D3D11CreateDeviceAndSwapChain`, capture the `ID3D11Device*` and
`ID3D11DeviceContext*`.
**Gate:** our log prints the device pointer, the context pointer and the feature
level (expect `11_0`), the game still reaches gameplay and plays normally, and
exit is clean.

### ☐ Stage 4 — Count the draws, and find the one that goes missing

**Plan:** `docs/plans/stage-4-observe-draws.md`
*Replaces the original Stage 3, which was built on the refuted H-A.* Hook
`Present` for a frame counter and all four `Draw*` entry points for a per-frame
count; align the log against a 10fps recording and compare the bad frame's draw
count with its neighbours. Log constant-buffer widths for H-B1 while in there.
**Gate:** an explicit, recorded answer to **did the game issue the draw?**

### ☐ Stage 5 — Fix it

**Plan:** `docs/plans/stage-5-fix.md` — stub. Write it once Stages 1 and 4 have
said which side of the translation the defect lives on. *Replaces the deleted
`stage-4-fix-defect-a.md` (targeted refuted H-A) and `stage-5-defect-b.md` (there
is no separate Defect B).*
**Gate:** the same scene, shadows **on**, no dropouts in a 60-second 10fps
recording measured the same way as O12/O14 — **a number, not an impression** — no
new artefact, no crash.

### ☐ Stage 6 — Ship it

**Plan:** `docs/plans/stage-6-ship.md` — stub.
Config file, install/uninstall scripts, a ZIP a stranger can unpack, the THQ
overlay put back, and a CodeWeavers bug report carrying the evidence — DXMT's own
border-colour warning (O2), the frame-counted dropout measurements (O12/O14), and
the note that DXVK ships a per-app profile for this exact executable.
**Gate:** a clean bottle, a fresh install, someone else's hands.

---

## Risk log

| # | Risk | State | Note |
|---|------|-------|------|
| 1 | The whole project is unnecessary because `/dx9` works | **OPEN — descoped** | **Never tested.** The reporter requires the DX11 renderer to work, so `/dx9` is not an acceptable outcome (D1). Still the cheapest experiment in the repo if the DX11 route ever proves impossible |
| 2 | 32-bit stdcall decoration breaks the winmm proxy | **OPEN** | Deferred to Stage 2. The sibling repo is 64-bit; its `.def` and stub generation do not transfer unexamined |
| 3 | DXMT hands out different vtables per interface version | **OPEN** | Patch what `D3D11CreateDevice` returned; `QueryInterface` for `ID3D11Device1` may yield a different object. Verify before assuming one vtable |
| 4 | Patching a vtable a thread is already inside | **OPEN** | Hook at device creation, before the render thread is running. Never patch mid-frame |
| 5 | H-A is wrong and the border colour is a red herring | **CLOSED — the risk happened** | **O10a refuted H-A.** The flicker does not move when shadow-map resolution moves the frustum boundary. The shadows-off evidence was correlational and was labelled as such; it was correlational. O2's warning survives as bug-report evidence only |
| 6 | The defect is unfixable from outside the game | **OPEN** | Unchanged in substance, widened in scope now that there is one defect. If Stage 1 or 4 shows a shader-internal or engine-internal cause, the honest outcome is a bug report, not a hack |
| 7 | A fix that crashes is worse than the flicker | **OPEN** | Standing. Every patch guarded; anything uncertain is logged and skipped |
| 8 | CrossOver rewrites `cxbottle.conf` on exit | **CLOSED** | Known: quit CrossOver before editing it. Largely moot for experiments now — `dxmt.conf` (O11a) and `cxstart` env injection (O15) both avoid the file entirely |
| 9 | Measurements polluted by the THQ overlay's Present hook | **CLOSED** | Cleared in O5. **Note: it has since been restored to `THQNOnline/overlay` and was enabled for all Stage 0 measurements** (O9). Re-disable it if a future measurement needs it excluded |
| 10 | Trusting DXVK's log when DXVK does not work here | **CLOSED** | Only the app-profile line and the capability report are used, and why is written down (O6) |
| 11 | Judging "did it help?" by eye | **CLOSED** | Cost this project a wrong entry: O7 recorded `ignoreMapFlagNoWait` as tried-and-useless by eyeball, which was right, but only O13's frame count could prove it. **Every future "did it help?" is a frame count from a 10fps recording, never an impression** |
| 12 | Deleting evidence while tidying up | **CLOSED** | Happened. A completed `DXMT_LOG_LEVEL=trace` run was destroyed by an `rm` of `C:\dxmtlog\TQ_*.log` issued to "clear stale logs", costing one launch. **Copy logs into `cache/` before clearing anything, and never clear a directory the user may have just written to** |
