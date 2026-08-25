# Runbook

## Next session: paste this

> Read `CLAUDE.md`, then `RUNBOOK.md`, then `docs/rev/`. **Stage 3 is written and
> half-proved.** The device is reached — `device 06536BC0`, `context 06901448`,
> **feature level 11_0** (O19) — and **Risk 3 is closed**: `ID3D11Device1` is the
> same object with the same vtable (O20), so Stage 4 can patch one vtable.
>
> **The box is NOT ticked, and here is why.** On the one launch that worked, the
> render process lived **seven seconds** and was terminated without logging a
> detach (O22). Nobody was at the keyboard and **no control run exists**, so this
> is *unattributed*, not *our fault* — but it is the shortest render session in
> the log and it ended right after the first hook this project has installed in
> the game. **Attribute it before building anything on top of it.**
>
> **Do this first, and it is one launch each:**
> 1. `wineserver -k` and restart Steam. Steam stopped responding to launch
>    requests after that run — `console_log.txt` wrote nothing after 23:29:20 and
>    three further attempts never reached it.
> 2. `npm run build && npm run install-dll` — the DLL was left **uninstalled**.
> 3. **Control:** set `TQFLICKER_HOOK=0` and play for a minute. Same binary, winmm
>    forwarded, **no hook**. Compare against a run without it. The heartbeat line
>    (`alive: Ns`) tells you whether the game died or was quit.
> 4. Only when the game demonstrably plays normally *with* the hook, tick Stage 3
>    and go to `docs/plans/stage-4-observe-draws.md`.
>
> **Read O18 before setting any environment variable.** The `TQ.exe` you launch is
> a **Steam handoff stub**; the process that renders is **Steam's** child
> (`TQ.exe /dx11`). So `cxstart` env injection — O15 — **does not reach the
> renderer**. Use `cxbottle.conf`, Steam launch options, or a file beside the exe
> like `dxmt.conf` (O11a).
>
> `npm run doctor` is green. `build`, `selftest`, `install-dll`, `uninstall-dll`
> and `log` all work; every log line now carries a **pid** (O17), and the off-game
> self-test proves the vtable patch primitive works under FEX in this bottle.
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

### ☑ Stage 2 — Get inside the process

**Plan:** `docs/plans/stage-2-proxy.md` — **complete, 2026-08-25.**
186 exports forwarded in the running game, zero calls to an unresolved slot.
Off-game self-test passes; uninstall restores the directory. Produced **O17**.
**Gate met.** `npm run build` / `selftest` / `install-dll` / `uninstall-dll` /
`log` all work.

### ☐ Stage 3 — Reach the device — **built, one gate clause of three met**

**Plan:** `docs/plans/stage-3-device.md` — code complete, run 2026-08-25,
**gate not met.**
Waits for `Direct3D11.dll`, IAT-hooks its imported
`D3D11CreateDeviceAndSwapChain` — the only one it imports (O19) — and captures
the device, context and swapchain.

| Gate clause | |
|---|---|
| Log prints device, context and feature level (`11_0`) | **met** (O19) |
| The game reaches gameplay and plays normally | **not established** (O22) |
| Exit is clean | **not met** — terminated, no detach logged (O22) |

Produced **O18–O23**, and closed **Risk 3**. What remains is one control launch
with `TQFLICKER_HOOK=0` against one without, to attribute the seven-second
session — see the block at the top of this file.

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
| 2 | 32-bit stdcall decoration breaks the winmm proxy | **CLOSED** | Not a problem, and not for the expected reason: Windows' 32-bit winmm exports **186 plain names**, none decorated, so no `--kill-at` and no `@N` handling was needed. What *did* differ was the stub form — i386 has no `jmp *slot(%rip)` — and the DLL's location: on this ARM64 bottle the i386 winmm is in **syswow64**, `system32`'s being Aarch64. The generator now refuses a decorated name or a non-i386 input |
| 3 | DXMT hands out different vtables per interface version | **CLOSED** | **O20 asked it in the running game and the answer is the convenient one:** `QueryInterface` for `ID3D11Device1` returns the *same pointer* with the *same vtable* as the `ID3D11Device` the game holds. Stage 4 can patch one vtable. Not yet asked of `ID3D11DeviceContext1` — ask before patching the context |
| 4 | Patching a vtable a thread is already inside | **OPEN** | Hook at device creation, before the render thread is running. Never patch mid-frame |
| 5 | H-A is wrong and the border colour is a red herring | **CLOSED — the risk happened** | **O10a refuted H-A.** The flicker does not move when shadow-map resolution moves the frustum boundary. The shadows-off evidence was correlational and was labelled as such; it was correlational. O2's warning survives as bug-report evidence only |
| 6 | The defect is unfixable from outside the game | **OPEN** | Unchanged in substance, widened in scope now that there is one defect. If Stage 1 or 4 shows a shader-internal or engine-internal cause, the honest outcome is a bug report, not a hack |
| 7 | A fix that crashes is worse than the flicker | **OPEN — and it may have just happened** | Standing, and no longer hypothetical. The first hook this project installed was followed by a **seven-second** render session ending in termination (O22). Unattributed — no control run exists — but it is why Stage 3 is not ticked and why `TQFLICKER_HOOK=0` was built. Attribute it before adding a single further patch |
| 13 | The renderer does not inherit our environment | **OPEN — newly found** | **O18.** The `TQ.exe` we launch is a Steam handoff stub; the renderer is Steam's child. `cxstart` env injection (O15) reaches the stub and **not** the process that renders. Every `DXMT_*`, `FEX_*` or `TQFLICKER_*` variable an experiment depends on must go where Steam can see it, or the experiment silently measures the baseline |
| 14 | Steam wedges and stops launching the game | **OPEN — newly found** | Happened at the end of Stage 3: after one abrupt game exit, Steam wrote nothing further to `console_log.txt` and ignored three launch requests. Costs a `wineserver -k` and a Steam restart. **Check `content_log.txt` for an `App Running` line before believing a launch happened at all** |
| 8 | CrossOver rewrites `cxbottle.conf` on exit | **CLOSED** | Known: quit CrossOver before editing it. Largely moot for experiments now — `dxmt.conf` (O11a) and `cxstart` env injection (O15) both avoid the file entirely |
| 9 | Measurements polluted by the THQ overlay's Present hook | **CLOSED** | Cleared in O5. **Note: it has since been restored to `THQNOnline/overlay` and was enabled for all Stage 0 measurements** (O9). Re-disable it if a future measurement needs it excluded |
| 10 | Trusting DXVK's log when DXVK does not work here | **CLOSED** | Only the app-profile line and the capability report are used, and why is written down (O6) |
| 11 | Judging "did it help?" by eye | **CLOSED** | Cost this project a wrong entry: O7 recorded `ignoreMapFlagNoWait` as tried-and-useless by eyeball, which was right, but only O13's frame count could prove it. **Every future "did it help?" is a frame count from a 10fps recording, never an impression** |
| 12 | Deleting evidence while tidying up | **CLOSED** | Happened. A completed `DXMT_LOG_LEVEL=trace` run was destroyed by an `rm` of `C:\dxmtlog\TQ_*.log` issued to "clear stale logs", costing one launch. **Copy logs into `cache/` before clearing anything, and never clear a directory the user may have just written to** |
