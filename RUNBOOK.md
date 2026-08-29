# Runbook

## Next session: paste this

> Read `CLAUDE.md`, then `RUNBOOK.md`, then `docs/rev/`. **Stage 4 is done:
> the game issues the draw, DXMT renders nothing for it (O30).** Stage 1's
> `.gputrace` still needs Xcode — but **2026-08-29 found three things to do
> without it**, all in `observed.md` under "Ideas after Stage 4":
>
> 1. **Metal validation without Xcode** — `MTL_DEBUG_LAYER=1` /
>    `MTL_SHADER_VALIDATION=1` on the Steam launch (O24 route). One launch,
>    may name the fault; and its zero-fill of out-of-bounds reads is DXVK's
>    `constantBufferRangeCheck` for free, so "flicker gone under validation"
>    would revive H-B1.
> 2. **Map-pointer reuse diagnostic** in `hookMap` — shortest reuse distance
>    per frame into the frame table, correlated with the recording.
> 3. **Reroute the game's DYNAMIC constant buffers to DEFAULT +
>    `UpdateSubresource`** from the shim (`docs/plans/stage-5-fix.md`) — if the
>    flicker stops, that is the diagnosis and the fix in one.
>
> Why these: **O37** — the shipped DXMT is a CodeWeavers fork of `3Shain/dxmt`
> (`v0.80-131-g2befd18`), and on **i386 only** dynamic buffers are `CpuPlaced`
> pages rotated per `Map(DISCARD)`; the game discards its two 2048-byte
> constant buffers per draw. That is **H-F**, untested. Two new facts from the
> reporter: **`/dx9` has no flicker** (O35, Risk 1 closed) and **the main-menu
> character flickers with no shadows** (O36) — use the menu as the bench.
>
> Housekeeping: the 10fps cap is **commented out** (normal play). Logs from the
> last run are in `cache/logs/stage4-run1-002554-*`; re-derive with
> `cache/venv/bin/python tools/recording.py cache/captures/*12.24.39*.mov
> cache/logs/stage4-run1-002554-tqflicker-frames.log`. `npm run doctor` reports
> whether Xcode has appeared. Upstream DXMT source is cloned to the scratchpad
> of session `b8b5af5a…`; re-clone `3Shain/dxmt` if it is gone.

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

**Stage 4 added the half that was missing** (O30): **the game issues the draw
anyway.** The engine submits it, `Map` never reports the resource busy, the
draw is never empty — and no pixels appear. So the sentence above is a
description of what **DXMT** does with a draw the game made correctly.

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

### What is alive — **one thing after Stage 4, plus one from reading DXMT's source**

- **H-F — the i386-only dynamic-buffer page path** (O37). Untested; the
  most specific Metal-side hypothesis the project has, and testable from the
  shim. See "Ideas after Stage 4" in `observed.md`.

- **H-D, second branch only.** *The game submits the draw and DXMT renders
  nothing for it.* O30 measured the first branch dead: 56 objects vanished
  against 4 draw-count dips, with the count flat frame-to-frame 93.3% of the
  time. This is now the project's position rather than a hypothesis, and going
  further needs a Metal-side instrument.
- **H-B1 — demoted.** O32 reflected 541 shaders and found no constant buffer
  declared larger than any created — by a test that admits it could only have
  caught the blatant form (it compares against the largest buffer, not the one
  bound at the draw, and 16 of the game's 20 constant buffers are 32 bytes). And
  it is a D3D11-side explanation, which O30 has made unlikely on principle.
  Kept as a Stage 5 fallback because its prior art names `TQ.exe`.

### The one question — **ANSWERED, 2026-08-26**

**Was the missing draw ever submitted? Yes.** (O30.) Over the 387 frames a
recording covers: **56 objects vanished, 4 draw-count dips**, against a count
that is flat frame-to-frame 93.3% of the time. Zero empty draws, zero busy
`Map`s, zero draws on any path we had not hooked.

> **The game submits the draw and DXMT renders nothing for it.**

The D3D11 side is exonerated, and with it every hypothesis in which the engine
decides to skip. What remains is a fault in the D3D11→Metal translation, and
the only instrument that can see one is **Stage 1's Metal capture** — which
needs **Xcode**. That is the project's open decision.

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

### ☐ Stage 1 — Metal frame capture — **THE CRITICAL PATH, blocked on Xcode**

**Plan:** `docs/plans/stage-1-frame-capture.md`
No code. `DXMT_CAPTURE_FRAME` + `MTL_CAPTURE_ENABLED` via `cxstart` (O15),
producing a `.gputrace` for Xcode.

**Un-deferred 2026-08-26 by O30**, which met this stage's own revival
condition: the game issues the draw, so the fault is on the Metal side and this
is the only way to see it. It is now the main line of the investigation.

**Still blocked:** this machine has Command Line Tools only — no
`/Applications/Xcode.app`, and `xcrun -f metal` fails. **A `.gputrace` has no
viewer here.** The Metal debugger ships only with the full ~15 GB Xcode app; the
Command Line Tools are not enough. **This is a decision for the reporter:
install Xcode, or ship the bug report (Stage 6).**

**Better than when it was written:** the capture is keyed on a frame index, and
Stage 4's table now **names the bad frames**, so catching a defective one no
longer depends on luck.

*The 2026-08-25 reasoning, kept because it was right: the capture shows what
DXMT submitted to Metal, whereas "did the game issue the draw?" is a D3D11-side
question that Stage 4 could answer directly and cheaply. Stage 4 answered it —
and the answer is the one that hands the investigation back to this stage.*

### ☑ Stage 2 — Get inside the process

**Plan:** `docs/plans/stage-2-proxy.md` — **complete, 2026-08-25.**
186 exports forwarded in the running game, zero calls to an unresolved slot.
Off-game self-test passes; uninstall restores the directory. Produced **O17**.
**Gate met.** `npm run build` / `selftest` / `install-dll` / `uninstall-dll` /
`log` all work.

### ☑ Stage 3 — Reach the device

**Plan:** `docs/plans/stage-3-device.md` — **complete, 2026-08-25.**
Waits for `Direct3D11.dll`, IAT-hooks its imported
`D3D11CreateDeviceAndSwapChain` — the only one it imports (O19) — and captures
the device, context and swapchain.

**Gate met (O27):** the log names device `064CBE20`, context `0691B508` and
feature level **11_0**; the reporter played the game with the hook installed; the
process exited through the loader with both summaries written and the hook called
exactly once.

Produced **O18–O27**. **Closed Risk 3**; opened Risks 13, 14 and 15. Built
`src/patch.{h,cpp}`, `src/modules.{h,cpp}`, `src/device.{h,cpp}`, a watcher
thread, a pid on every log line, `TQFLICKER_HOOK=0` and a startup heartbeat.

### ☑ Stage 4 — Count the draws, and find the one that goes missing

**Plan:** `docs/plans/stage-4-observe-draws.md` — **complete, 2026-08-26.**
`src/frames.{h,cpp}` patches fourteen vtable slots at device creation —
`Present`, all seven `Draw*`, `Map`, `CreateBuffer`, both shader creators and
`CreateSamplerState` — writing a per-frame table beside the main log. Slots are
generated from the MinGW headers (`scripts/gen-slots.sh`), and the off-game
self-test drives all of it through the real 32-bit DXMT before any launch (O28).

**Gate met (O30):** 909 frames measured in a real play session; the recording
aligned by wall clock; **56 objects vanished against 4 draw-count dips**, with
the count flat 93.3% of the time. **The game issues the draw.**

Produced **O28–O34**. Closed Risk 3 on the context; opened Risk 16 (the frames
table is truncated by the next launch). Built `src/frames.{h,cpp}`,
`scripts/gen-slots.sh`, `scripts/keep-log.sh`, `tools/frames.ts`, and
`tools/recording.py` — the Stage 0 detector, finally committed (O29), now with
a clock-based aligner and an offset-independent verdict.

### ☐ Stage 5 — Fix it — **no D3D11-side target exists**

**Plan:** `docs/plans/stage-5-fix.md` — still a stub, and O30 is why. The fix
for "the engine skipped it" and the fix for "DXMT dropped it" have nothing in
common, and **O30 chose the second**: the D3D11 side is submitting correctly, so
there is nothing to correct there. A shim cannot fix a translation fault it
cannot see. **This stage stays unwritten until Stage 1 says what DXMT did with
the draw.**
**Gate:** the same scene, shadows **on**, no dropouts in a 60-second 10fps
recording measured the same way as O12/O14/O30 — **a number, not an impression**
— no new artefact, no crash.

### ☐ Stage 6 — Ship it

**Plan:** `docs/plans/stage-6-ship.md` — stub.
Config file, install/uninstall scripts, a ZIP a stranger can unpack, the THQ
overlay put back, and a CodeWeavers bug report carrying the evidence — DXMT's own
border-colour warning (O2), the frame-counted dropout measurements (O12/O14), and
the note that DXVK ships a per-app profile for this exact executable.
**Gate:** a clean bottle, a fresh install, someone else's hands.

**O30 changed this stage's standing.** It was the fallback; it is now a
*legitimate outcome* — possibly the only one available without Xcode — and its
evidence is **already complete**: O30's measurement (the game submits the draw
and DXMT renders nothing for it, with the method and the numbers), O33's full
description of the single `ADDRESS_BORDER` sampler DXMT warns about, and DXVK's
per-app profile naming `TQ.exe`. If the reporter does not want to install Xcode,
this is the next stage and it can be written today.

---

## Risk log

| # | Risk | State | Note |
|---|------|-------|------|
| 1 | The whole project is unnecessary because `/dx9` works | **CLOSED — it works, and it is not acceptable** | **O35:** the reporter confirms `/dx9` has no flicker. The requirement is DX11 (D1), so this is corroboration of O30 (a different translation layer, no defect), not an exit |
| 2 | 32-bit stdcall decoration breaks the winmm proxy | **CLOSED** | Not a problem, and not for the expected reason: Windows' 32-bit winmm exports **186 plain names**, none decorated, so no `--kill-at` and no `@N` handling was needed. What *did* differ was the stub form — i386 has no `jmp *slot(%rip)` — and the DLL's location: on this ARM64 bottle the i386 winmm is in **syswow64**, `system32`'s being Aarch64. The generator now refuses a decorated name or a non-i386 input |
| 3 | DXMT hands out different vtables per interface version | **CLOSED** | **O20** for the device, **O28** for the context: `ID3D11Device1` and `ID3D11DeviceContext1` are each the *same object with the same vtable* as the base interface. One vtable each; Stage 4 patches once. `frames.cpp` still asks at install and would say `!!` if that ever changed |
| 4 | Patching a vtable a thread is already inside | **OPEN — mitigated** | Done as designed: `frames::install` runs inside the `D3D11CreateDeviceAndSwapChain` hook, on the game's thread, before the game is handed the device. Stays open until a play session confirms it (Stage 4 gate) |
| 5 | H-A is wrong and the border colour is a red herring | **CLOSED — the risk happened** | **O10a refuted H-A.** The flicker does not move when shadow-map resolution moves the frustum boundary. The shadows-off evidence was correlational and was labelled as such; it was correlational. O2's warning survives as bug-report evidence only |
| 6 | The defect is unfixable from outside the game | **OPEN** | Unchanged in substance, widened in scope now that there is one defect. If Stage 1 or 4 shows a shader-internal or engine-internal cause, the honest outcome is a bug report, not a hack |
| 7 | A fix that crashes is worse than the flicker | **OPEN** | O22's scare was the launch route (O24), not the hook. Stage 4 adds fourteen vtable patches, and the rule applied was: **prove every one of them through the real DXMT off-game first** — the self-test now does (O28). `TQFLICKER_HOOK=0` remains the one-launch control if the play session misbehaves |
| 16 | The per-frame table is truncated by the next launch | **OPEN — newly found** | `tqflicker-frames.log` is rewritten at every device creation, so the run that just measured is one launch from gone. `npm run keep-log -- label` first, always. Same family as Risk 12 |
| 17 | The project ends in a bug report rather than a fix | **OPEN — now likely** | **O30.** The fault is in DXMT, which we do not ship and cannot patch from a shim in the game's process. Risk 6 said "if the cause is engine- or shader-internal the honest outcome is a bug report"; O30 makes it *translation*-internal, which is the same conclusion by a different route. Stage 6's evidence is already complete. Not a failure — the project set out to find out what was wrong, and it did |
| 18 | Concluding from a mis-aligned recording | **CLOSED** | The alignment is the load-bearing step of O30 and it was nearly done wrongly. Two rules came out of it: **align by wall clock** (verified `birth + duration == mtime`), never by matching anomalies to dips — that assumes the answer; and **make the verdict two counts over one span**, so a second of clock error cannot change it. A timing-fingerprint alignment was tried and does not work at a 10fps cap (1.70ms best against 2.60ms worst) |
| 13 | The renderer does not inherit our environment | **OPEN — newly found** | **O18.** The `TQ.exe` we launch is a Steam handoff stub; the renderer is Steam's child. `cxstart` env injection (O15) reaches the stub and **not** the process that renders. Every `DXMT_*`, `FEX_*` or `TQFLICKER_*` variable an experiment depends on must go where Steam can see it, or the experiment silently measures the baseline |
| 14 | Steam wedges and stops launching the game | **OPEN — newly found** | Happened at the end of Stage 3: after one abrupt game exit, Steam wrote nothing further to `console_log.txt` and ignored three launch requests. **Check `content_log.txt` for an `App Running` line before believing a launch happened at all.** Recovering from it is Risk 15, and the recovery was worse than the wedge |
| 15 | Breaking the player's saves while recovering the bottle | **OPEN — the risk happened** | **O26.** A hard `wineserver -k` plus `kill -9` on Steam's helpers was used to unwedge Risk 14, and character create/select stopped working immediately afterwards — with our DLL installed, with the hook off, and with the DLL removed entirely. TQ AE's saves are Steam Cloud synced. **Quit Steam through its own menu; never kill it.** This is the same family as Risk 12 (deleting evidence while tidying up) and it cost more |
| 8 | CrossOver rewrites `cxbottle.conf` on exit | **CLOSED** | Known: quit CrossOver before editing it. Largely moot for experiments now — `dxmt.conf` (O11a) and `cxstart` env injection (O15) both avoid the file entirely |
| 9 | Measurements polluted by the THQ overlay's Present hook | **CLOSED** | Cleared in O5. **Note: it has since been restored to `THQNOnline/overlay` and was enabled for all Stage 0 measurements** (O9). Re-disable it if a future measurement needs it excluded |
| 10 | Trusting DXVK's log when DXVK does not work here | **CLOSED** | Only the app-profile line and the capability report are used, and why is written down (O6) |
| 11 | Judging "did it help?" by eye | **CLOSED** | Cost this project a wrong entry: O7 recorded `ignoreMapFlagNoWait` as tried-and-useless by eyeball, which was right, but only O13's frame count could prove it. **Every future "did it help?" is a frame count from a 10fps recording, never an impression** |
| 12 | Deleting evidence while tidying up | **CLOSED** | Happened. A completed `DXMT_LOG_LEVEL=trace` run was destroyed by an `rm` of `C:\dxmtlog\TQ_*.log` issued to "clear stale logs", costing one launch. **Copy logs into `cache/` before clearing anything, and never clear a directory the user may have just written to** |
