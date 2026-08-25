# Observed — what has actually been proven

Each entry says how it was established. Anything not backed by an experiment or
a log line is in **Hypotheses** at the bottom and is labelled as such.

---

## The artefact

Under `CX_GRAPHICS_BACKEND=dxmt` on the DX11 renderer, the game runs at full
speed and looks correct **except that things which move flicker**. Static world
geometry is fine. Reported affected: shadows, light/FX effects, boats, and
character clothes/hair.

## O1 — ~~There are two defects, not one~~ — **SUPERSEDED by O10b and O14**

> **Read this entry with its conclusion crossed out.** The *experiments* below
> are sound and their results stand. The *inference* — that a residual surviving
> shadows-off implies a second, independent defect — was wrong. O10b showed the
> residual survives at an **unchanged rate**, and O14 showed every symptom is one
> failure mode with many victims. Shadows off removes *victims*, not a *cause*.
> Kept in full because the AO result below is still load-bearing.


*Established by turning game options off, 2026-08-25.*

| Experiment            | Result                                                    |
|-----------------------|-----------------------------------------------------------|
| **Shadows off**       | Flicker **greatly reduced, but not eliminated**            |
| **HBAO+ / SSAO off**  | **No effect at all**                                       |

So the majority of the artefact lives in the **shadow pass**, and a distinct
residual survives it. AO is cleared of involvement.

**Residual, with shadows off:** the FX/light effect in front of the
**resurrection shrine**, and intermittently **character hair or clothes** (the
reporter was not certain which). Both are alpha-blended or alpha-tested dynamic
geometry, which is a different pass from anything shadows touch.

Call these **Defect A (shadow pass, majority)** and **Defect B (residual,
transparency/FX, cause unknown)**.

## O2 — DXMT cannot honour the game's border colour

*Established by DXMT's own log at `info`, and independently corroborated.*

```
warn:  CreateSamplerState: Unsupported border color (-3.40282e+38, 0, 0, 0)
```

`-3.40282e+38` is `-FLT_MAX`. It appears **exactly once**, at renderer
initialisation, and it is the **only warning DXMT emits for the whole session**.

Metal offers three fixed border colours — transparent black, opaque black,
opaque white — so no D3D11-on-Metal backend can supply an arbitrary one. DXVK
reports the same platform limitation from the other side:
`customBorderColors: 0` and `warn: DXVK: Custom border colors not supported`.

On a linear view-space depth target, `-FLT_MAX` means *"nothing here /
infinitely far"*. Whatever DXMT substitutes cannot mean that.

## O3 — DXMT's `debug` log level prints no more than `info`

*Negative result. Established by setting `DXMT_LOG_LEVEL=debug` and re-running:
byte-for-byte the same three lines.*

Do not spend another launch on `debug`. `trace` is untried.

## O4 — `DXMT_LOG_PATH` needs the directory to exist

*Negative result, cost one launch.* Pointed at `C:\dxmtlog` before creating it,
DXMT wrote nothing and gave no indication why. `mkdir` first.

## O5 — The THQ Nordic overlay is not the cause

*Established by renaming `THQNOnline/overlay` to `overlay-disabled` and
re-running: flicker unchanged.*

Worth recording anyway, because it hooks every frame and had to be excluded
before any other measurement could be trusted. It is Indicium-Supra driving an
ImGui renderer; it logs to `AppData\Local\Temp\Indicium-Supra.log`, and had
hooked `IDXGISwapChain::Present` successfully on all 16 runs before it was
disabled. **That log is the proof that D3D11 vtable hooking works in this bottle
under DXMT** — which is the foundation the rest of this project stands on.

## O6 — Three of the four graphics backends are unusable here

*Established by trying them.* `dxvk`, `d3dmetal` and `wine` all render black on
the DX11 renderer; only `dxmt` produces a picture. `d3dmetal` is not even
offered in the Preview's UI selector.

**Corollary about trusting DXVK's log.** DXVK does not work here, so nothing it
says about *how it renders* is evidence — not the failed geometry-shader
pipeline, not the present path. Two lines survive that objection, because
neither is a claim about DXVK's own rendering:

- `Found built-in config: d3d11.constantBufferRangeCheck = True` is DXVK's
  **static per-application profile table, keyed on `TQ.exe`** and compiled into
  the binary. It is a claim its authors made about *Titan Quest*, true whether or
  not DXVK draws a pixel on this machine.
- `customBorderColors: 0` is DXVK **reporting the substrate's capabilities**, and
  DXMT independently warned about the same thing (O2). Two unrelated
  implementations agreeing about Metal.

Use those two. Discard the rest of that log.

## O7 — DXMT has no knob for either suspected cause

*Established by extracting the config-key strings from the shipped DLL.* The
complete list is seven keys (`docs/rev/substrate.md`). None is a constant-buffer
range check; none is a border-colour override. This is consistent with the
reporter having tried `ignoreMapFlagNoWait`, `defuseFma`, `sampleNaNToZero`,
`shaderMetalVersion` and `maxFeatureLevel` to no effect — those address map-flag
stalls, FMA contraction, NaN handling and feature level, none of which is either
defect.

**There is no configuration fix. A patch is the only route left inside DX11.**

---

# Stage 0 — free experiments

Run 2026-08-25. Each entry says whether it was actually run, because two of the
five were settled without a launch.

## D1 — E1 (`/dx9`) is descoped, not tested

*Not an observation. A scope decision by the reporter, 2026-08-25.*

The DirectX 9 renderer was never run. The requirement is that the game work on
its **DX11 renderer**, so "fall back to `/dx9`" is not an acceptable outcome even
if it would render perfectly.

This does **not** close Risk 1 — the risk is untested, not retired. If the DX11
route later proves impossible, `/dx9` is still sitting there unspent, and it is
still the cheapest experiment in the repo. `substrate.md` records that
`d3dx9_42.dll` is already installed, so the path will start.

## O8 — Reduced x87 precision is not the cause

*Negative result. Established by the reporter setting
`FEX_X87REDUCEDPRECISION=0` and re-running: **no effect on the flicker.***

This kills **H-C**. The prior was already low — 32-bit MSVC 11.0 emits SSE2 for
most float math, so little of the hot path is x87 at all — and the experiment
agrees with the prior.

It also removes a whole class of suspect: the defect is **not** a host-level
float-precision artefact under FEX. Whatever is wrong is wrong in the D3D11→Metal
translation or in what the game hands it, which is where the remaining hypotheses
already point.

## O9 — E0 baseline: the artefact is whole-object **on/off toggling**, in bursts, and it happens while the scene is static

*Established by direct observation at a resurrection shrine, 2026-08-25, on the
unmodified baseline configuration: `CX_GRAPHICS_BACKEND=dxmt`, DX11 renderer,
`FEX_X87REDUCEDPRECISION=1`, THQ overlay **enabled** (it had been restored since
O5 renamed it; O5 already cleared it as a cause).*

This is the reference scene for every later experiment.

**1. It flickers with the player standing still.** Character on its idle
animation, camera not moving, no input. The shrine FX flickers anyway. NPCs were
wandering in the scene, so the *contents* of the shadow map were still changing
frame to frame — but nothing the player controls was moving.

**2. It comes in bursts, not steadily.** Roughly: flicker-flicker-flicker-flicker
over about a third of a second, then a few seconds of calm, then another burst.
Not a per-frame shimmer, and not periodic on a short cycle.

**3. It is a binary on/off, not a wrong value.** The shrine FX is a yellow sun
effect, and it reads as *the light being switched off and back on* — not as it
changing brightness or colour. The character's ground shadow does the same
thing: on/off, on/off.

**4. Hair: the player's own character never flickered. Other characters do** —
some or all of the hair vanishes entirely and the character is momentarily bald,
for a few frames, then it comes back. Rare: on the order of once every 30
seconds. The reporter could not separate clothes from self-shadow or AO by eye,
so "clothes" from the original residual report is **not** confirmed.

**5. It is the shadow shape itself** that toggles, not the lit surface beside it.

### Why this matters more than the rest of Stage 0

**It weakens H-A considerably.** An unrepresentable border colour produces a
*wrong sample value* where the shadow lookup lands outside the shadow map — an
edge artefact, a band, a gradient at the frustum boundary, worst case a shimmer
along an edge as geometry crosses it. It does **not** plausibly switch an entire
character shadow off and back on, and it does not explain a burst pattern with
seconds of calm between bursts.

**It suggests Defect A and Defect B may be one defect, not two.** Every symptom
in this observation is the same shape: *a thing that should be drawn is entirely
absent for a few frames, in bursts.* The shrine FX, the ground shadow and another
character's hair are three different passes, but they are one behaviour. O1's
"shadows off greatly reduces the flicker" is equally well explained by shadows
simply being the largest and most numerous dynamic thing on screen — a reduction
in **how much** flickers, not evidence of a distinct cause.

O1 is not overturned; it is re-read. The shadows-off evidence was always
correlational and was labelled as such (Risk 5).

## O10 — E4: the defect is **one** defect, and it scales with frame rate

*Established in-game at the O9 shrine, options menu only, no relaunch,
2026-08-25.*

### O10a — Shadow quality does not move the artefact

Changing shadow quality low↔high did **not** move the flicker. The same things
flicker in the same places; only the amount changes.

*(Reported as "shadows off — it doesn't move, same thing flickering". Read as the
shadow-quality comparison, since shadows-off is answered separately in O10b. The
conclusion is the same either way.)*

Changing shadow-map resolution moves the shadow frustum boundary. Under H-A the
flicker pattern should have **moved** with it. It did not. **H-A predicted this
and got it wrong.**

### O10b — Everything survives shadows-off, at the same rate

With shadows **off**, at the shrine:

- the shrine FX still flickers — *and at the same burst rate as with shadows on*;
- other characters still go bald occasionally.

The rate being **unchanged** is the important half. If the shadow pass were a
separate defect, removing it would be expected to change the residual's
character or timing. It changed neither. Together with O9 this is strong
evidence that **Defect A and Defect B are one defect**, and that O1's
"shadows off greatly reduces it" only ever measured *how much* of the screen is
dynamic, not *what is wrong*.

### O10c — Vertical sync **off** makes it much worse, and recruits new geometry

Toggling vsync off:

- the flicker got **much worse**, and the burst rate appeared to **increase**;
- **character clothes began to flicker**, which they had **not** been doing with
  shadows off a moment earlier.

That last point is the strongest single fact in Stage 0. Raising the frame rate
did not merely make the existing artefact more visible — it **pulled in a class
of geometry that was not affected before**. This is a dose-response relationship
between frames per second and the defect, and it is the signature of a
**race between CPU writes and GPU reads**, not of any sampling or precision
error. More frames per second means deeper pipelining and more overlap between
the frame being written and the frame being read.

It also resolves part of the open hair-or-clothes question from the residual
report: **clothes do flicker** — at high frame rates.

### O10d — Vsync cannot be turned back on without a restart

*Negative result, and a DXMT behaviour note.* Setting vsync back to **on** did
**not** restore the previous behaviour; the degraded state persisted until the
game was restarted. Presumably the present mode / swapchain is not fully
reconfigured in place.

**Consequence for every later experiment: after touching vsync, restart before
measuring anything.** A stuck present mode would silently poison the next
result.

## O11 — E5: the artefact is periodic in **frames**, not in time — and capping the frame rate is not a workaround

*Established by writing `dxmt.conf` beside `TQ.exe` with
`d3d11.preferredMaxFrameRate = 30`, vsync on, shadows on, same shrine,
2026-08-25.*

### O11a — `dxmt.conf` beside the executable works

*Positive mechanism result, and a workflow win.* The frame rate visibly capped.
DXMT reads a `dxmt.conf` sitting next to `TQ.exe`, so **configuration does not
require editing `cxbottle.conf`** and therefore does not require quitting
CrossOver or risking the rewrite-on-exit trap (Risk 8). Use this file for every
later experiment.

### O11b — Halving the frame rate halves the flicker *speed* and removes none of it

The flicker still happens. What changed is its **tempo**: the reporter describes
it as having flickered "very fast" before and "slowly" now. *(Recorded with the
reporter's own uncertainty — this was an impression, not a count. **O12** exists
to measure it properly.)*

This is the discriminating result of Stage 0:

- **It is not a workaround.** Running at half frame rate does not fix the game.
  We have nothing to offer a user today, and the project still needs to exist.
- **It is not a wall-clock phenomenon.** If the defect were a timer, a cache
  eviction, a garbage collection or a periodic reallocation, its rate in seconds
  would have stayed put while the frame rate fell. It did not.
- **It is counted in frames.** The artefact toggles on a frame cadence. Halve the
  frames per second and you halve the toggles per second.

Something alternates **per frame** between a good state and a bad one. That is
the shape of a rotating resource pool — a dynamic buffer that DXMT backs with
several allocations and cycles between, where not every allocation carries the
data the draw needs. A draw reading the buffer on a "good" frame renders; on a
"bad" frame it renders nothing, or renders somewhere off-screen.

### O11c — What this does to O10c

O10c read the vsync result as a dose-response relationship: more frames per
second, more racing. **O11b makes that reading incomplete.** Much of what looked
like "worse at high frame rate" is simply the same frame-cadence pattern running
faster and therefore reading as a harsher strobe.

The part of O10c that is **not** explained away is the recruitment: clothes
began flickering at high frame rate when they had not been flickering moments
before. Tempo alone does not add new objects. That remains open and is worth
returning to.

## O12 — E6: measured. The FX is **entirely absent for exactly one frame**, at irregular intervals, and nothing else drops with it

*Established by capping the frame rate to 10fps (`dxmt.conf`,
`d3d11.preferredMaxFrameRate = 10`), screen-recording 46.7s at the shrine, and
counting frames with ffmpeg + numpy. This is the first **quantitative** result in
the project; everything before it was eyeball judgement.*

### The instrument

macOS screen recording only captures on change, so with the game pinned at 10fps
the recorder produced **exactly one recorded frame per game frame** — frame
intervals are 100.0ms to the millisecond from index 32 onward. **Recorded frame
== game frame, 1:1.** 674 frames, 1136×860, in `cache/captures/`.

This is a reusable measuring instrument and it is the reason to keep a frame-rate
cap in place while diagnosing: *at 10fps the recording is a per-frame trace of the
renderer.* Not a workaround for the user — an oscilloscope for us.

### What was measured

- **8 single-frame dropout events** in 641 clean frames — **1.2% of frames**.
- Every event lasts **exactly one frame**. Never two.
- Every event is **the same screen region**: the shrine's light beam, blocks
  spanning x 192–256, y 0–192 in 568×430 analysis space.
- Every event has **the same magnitude**: total block drop 946–996 across 17–19
  blocks; global frame brightness dips 4.00–4.20 gray levels and never anything
  else. The same object failing the same way, eight times.
- Intervals between events, in frames: **22, 37, 46, 110, 9, 6, 65**.
  **Irregular** — no period, no constant cadence, mean 42 and median 37 with a
  standard deviation of 34. Whatever triggers it is not a counter.

### What the bad frame looks like

`cache/captures/beam.png` — frames 72, 73, 74 side by side. On frame 73 the light
beam is **completely gone**, revealing the shrine architecture that it normally
hides. It is not dimmed, not displaced, not stretched, not thinner. **It is not
drawn.** Frame 74 it is back, identical to frame 72.

This settles the question O9 could not: **the object is absent, not misplaced.**

### Nothing else drops on those frames

Measured directly (the "together or independently" question from E6). Outside the
beam region, on the eight event frames, **0.004%** of pixels drop by more than 25
gray levels — against the beam losing ~950 gray-levels of block sum. The static
scenery, the ground, the buildings and the other characters are untouched on
exactly the frames where the beam vanishes.

`cache/captures/diff.png` shows it: differencing a good frame against the bad one
leaves the beam and the walking NPCs lit up and everything else black — and the
NPCs are there only because they are walking, which moves them every frame
regardless.

**The failure is per-object, not per-frame.** No global present, swap, or
frame-wide event is dropping. One draw goes missing while every other draw in the
same frame lands.

### Caveat, stated plainly

No character-shadow dropout was detectable in this recording. That does **not**
contradict O9/O10 — the player was standing at the shrine and the character
shadow is both small and partly inside the beam analysis region, so an event of
the same kind could be under the detection threshold. What is proven is the
**mechanism** on the FX; that shadows and hair share it remains inference from
O10b, not measurement.

## O13 — E7: `ignoreMapFlagNoWait` does nothing, measured. H-E is refuted

*Negative result, and the first one in this project backed by a statistic rather
than an impression. Established by re-running E6's protocol with the single added
variable `d3d11.ignoreMapFlagNoWait = True`, same scene, same 10fps cap, 69.5s.*

| | E6 baseline | E7 `ignoreMapFlagNoWait=True` |
|---|---|---|
| Clean frames | 641 | 786 |
| Dropout events | **8** | **8** |
| Rate per frame | 1.25% | 1.02% |
| Event duration | 1 frame, always | 1 frame, always |
| Region | x 192–256, y 0–192 | **identical** |
| Blocks affected | 17–19 | 17–18 |
| Total block drop | 946–996 | 925–1004 |
| Global brightness dip | 4.00–4.20 | 3.97–4.28 |

Exact two-sided binomial test on the event counts against the two exposures:
**p = 0.803**. There is no difference. The events are not merely as frequent —
they are the *same events*, with the same footprint and the same magnitude.

**H-E predicted elimination** — if the engine were skipping the draw because
`Map` returned `DXGI_ERROR_WAS_STILL_DRAWING`, then making `Map` wait removes the
reason to skip. Eight events survived unchanged. **The strong form of H-E is
refuted.**

*(A partial effect smaller than roughly 60% could not have been detected with
only 8 events per run — the 95% Poisson interval on the baseline rate is
0.38%–2.11% per frame. But H-E did not predict a partial effect.)*

**This also confirms O7's original judgement**, which recorded this setting as
tried to no effect. That was an eyeball call on a strobing screen and was worth
re-testing properly; it was right. Recorded here so nobody re-tests it a third
time.

### What it leaves standing

The draw is missing, and it is **not** missing because DXMT made a `Map` fail.
That removes the one mechanism DXMT shipped a knob for, and with it the last
free configuration experiment. **O7's conclusion now holds with measurements
behind it: there is no configuration fix.**

## O14 — The full picture: **many objects, each independently missing for one frame, a few percent of the time**

*Established by re-analysing the E6 and E7 recordings with a detector that (a)
looks for **both** polarities and (b) rejects motion. No extra launch.*

O12 found only the shrine beam because its detector looked for regions getting
**darker**. The beam is bright, so its absence darkens the screen — but **a
shadow is dark, so a missing shadow makes the screen brighter**, and every
shadow dropout was invisible to that detector. This was a hole in the
instrument, not in the game.

The motion-rejection matters too: a genuine one-frame dropout has frame *i-1*
and frame *i+1* nearly **identical to each other** (the object returns exactly
where it was), whereas a walking NPC makes them differ. Requiring
`|f(i-1) - f(i+1)| < 0.35 × |f(i-1) - f(i)|` separates the two cleanly.

### What the corrected scan shows

**Roughly 9% of frames contain at least one object anomaly**, and about **1.0
block-anomaly per frame** overall — an order of magnitude more than O12's
beam-only count.

The anomalies land on a **small set of fixed screen locations**, each with its
own characteristic magnitude, recurring at irregular intervals:

| Location (568×430 space) | What it is | Peak Δ | E6 rate | E7 rate |
|---|---|---|---|---|
| x 192–256, y 0–192 | the shrine light beam | −130 to −175 | 1.25% | 1.02% |
| x 320–336, y 224–240 | a standing NPC's ground shadow | +21 to +23 | 1.87% | 1.15% |
| x 448–496, y 384–400 | two NPCs' ground shadows | +31 to +41 | 0.94% | 1.91% |
| x 448, y 128 | a prop's shadow | +31 to +34 | 2.18% | 1.15% |
| x 240–272, y 224–256 | shadow near the shrine base | +37 to +41 | 1.25% | 2.42% |

`cache/captures/shadow_vanish.png` — frames 144/145/146. Two NPCs cast solid
ground shadows on 144 and 146; on 145 the shadow is gone. Same one-frame
signature as the beam, opposite polarity.

### The unified statement of the defect

Putting O9–O14 together, and this supersedes the two-defect framing of O1:

> **Individual draws fail to render for exactly one frame, independently of one
> another, each at a rate of one to two percent of frames.** The failure is
> per-object, never global; always exactly one frame, never two; irregular, with
> no period; and it affects the shadow pass, the FX pass and skinned character
> geometry alike.

"Shadows off greatly reduces it" (O1) now reads correctly: shadows are simply the
most numerous dynamic draws on screen, so removing them removes most of the
*opportunities* for the same single failure mode. There was never a Defect A and
a Defect B. **There is one defect with many victims.**

### And it confirms O13 on the richer metric

Total block-anomalies per frame: **E6 1.02, E7 0.91**. `ignoreMapFlagNoWait`
does not help on this measure either — the same conclusion O13 reached on the
beam alone, now over a hundred-fold larger sample of events. H-E stays refuted.

## O15 — Environment variables can be set per-run via `cxstart`, with CrossOver already running

*Positive result, established by probe, no game launch spent. This changes how
every remaining experiment is run.*

`cxbottle.conf`'s `[EnvironmentVariables]` is not the only way to get a variable
into the guest. A variable exported by the launching shell is **inherited by the
process `cxstart` starts**, and this works **even when the bottle and its
`wineserver` are already running**:

```sh
CX="/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver"
TQFLICKER_PROBE=hello DXMT_LOG_LEVEL=trace \
  "$CX/bin/cxstart" --bottle "New Bottle" --no-convert -- \
  cmd /c "set > C:\\dxmtlog\\envtest.txt"
```

The resulting `envtest.txt` contained **both** our injected variables *and* the
bottle's own (`CX_GRAPHICS_BACKEND=dxmt`), so per-run variables **layer on top
of** the bottle configuration rather than replacing it.

**Consequences:**

- **Risk 8 stops mattering for experiments.** No quitting CrossOver, no editing
  a file that gets rewritten on exit, no lost edits. Reserve `cxbottle.conf` for
  settings that must persist.
- **One variable at a time becomes cheap**, which is the ground rule Stage 0 is
  built on.
- **Stage 5 is unblocked before it starts.** The Metal frame capture needs
  `DXMT_CAPTURE_EXECUTABLE`, `DXMT_CAPTURE_FRAME` and `MTL_CAPTURE_ENABLED`
  (`substrate.md`); all three can now be set for a single run.

**The caveat that matters:** a Steam game launched from the Steam UI is a child
of the already-running Steam process and inherits *Steam's* environment, not
ours. To use this, launch `TQ.exe` **directly** through `cxstart` while Steam is
running in the background.

## O16 — E3: `trace` **does** print more than `info`, but nothing per-frame. The key question stays open

*Established by `DXMT_LOG_LEVEL=trace` in `cxbottle.conf`, 10fps cap, ~15s at the
shrine. 532 lines: 526 `trace`, 5 `info`, 1 `warn`.*

**This partially overturns the expectation set by O3.** `debug` prints no more
than `info` (O3, still true), but **`trace` prints substantially more** — 526
extra lines against `info`'s three. O3's advice not to retry `debug` stands;
its implication that the log had nothing more to give did not.

### The whole vocabulary of the trace log

Only five distinct messages exist. There is no per-draw, per-`Map`, per-resource
or per-`Present` logging **at any level**:

| Message | Count | Where |
|---|---|---|
| `Start compiling 1 PSO` | 216 | lines 4–390 |
| `Compiled 1 PSO` | 96 | lines 5–391 |
| `staging map ready` | 210 | lines 314–532 |
| command-queue construct/destruct, thread exit | 4 | lines 21–24 |
| feature level / config / border-colour warning | 6 | lines 1–28 |

### Three facts worth having

**1. The log has two phases, and they are cleanly separated.** Lines 1–313 are
startup: PSO compilation during load, with essentially no `staging map ready`.
From line 314 — gameplay — it is almost entirely `staging map ready`, with only
five late PSO compiles. **PSO compilation is a loading-time activity here and has
converged by the time we are standing at the shrine**, which argues against any
hypothesis that the flicker is draws waiting on pipelines that are not ready yet.

**2. `staging map ready` runs at roughly the rate of the anomalies.** 210
occurrences across gameplay; the session was ~15s at a 10fps cap, so ~150 frames,
giving **~1.4 per frame** — against the ~1.0 block-anomalies per frame measured in
O14. A staging map is a **CPU↔GPU synchronisation point**. The similar rate is
suggestive and **nothing more**; two things happening about once a frame in a
renderer is weak evidence, and it is recorded here as an observation, not a
finding.

**3. `Start compiling` (216) exceeds `Compiled` (96) by 120.** Unexplained.
Either compilation is asynchronous with many still in flight at exit, or the two
messages are not on the same code path. Noted, not interpreted.

**Also confirmed independently:** `TQ_dxgi.log` contains
`info: Found config file: dxmt.conf` — corroborating O11a from DXMT's own mouth.

### The negative result, which is the point of the experiment

**E3 cannot answer whether the missing draw was submitted.** DXMT emits nothing
per draw call and nothing per frame, so there is no way to count draws in a frame
or to identify which frame was the bad one. The log is not a frame trace and
cannot be made into one.

**The last free experiment is spent.** No configuration setting and no log level
will take this further. The next instrument must be either the **Metal frame
capture** or **in-process observation**, and both cost real work.

*(Checked while here, since it was free: `Direct3D11.dll` contains no occlusion,
predication or visibility-query strings. This is only weak evidence — `CreateQuery`
is a vtable call and would leave no string — but it is recorded so nobody spends
a launch on the idea without knowing.)*

---

# Stage 2 — the proxy

## O17 — The proxy works, and **`TQ.exe` runs as two processes**

*Established by the proxy's own log on the first real game launch, 2026-08-25.*

```
23:13:10  tqflicker afaf188 - Stage 2, proxy only. No patches installed.
23:13:10  host:     TQ.exe  (pid 40)
23:13:10  winmm:    186 of 186 exports forwarded to C:\windows\system32\winmm.dll
23:13:22  host:     TQ.exe  (pid 1008)
23:13:22  winmm:    186 of 186 exports forwarded to C:\windows\system32\winmm.dll
23:14:21  winmm (process exit): 186 of 186 exports forwarded, 0 call(s) to one we could not forward
```

**All 186 exports forwarded, in the real game, and nothing ever called an
unresolved one.** The i386 stack-cleanup hazard documented on
`tq_winmm_unresolved` was never reached, as designed.

### The finding that matters for Stage 3

**`TQ.exe` starts twice** — pid 40, then twelve seconds later pid 1008 — and our
DLL is loaded into **both**. This was not known before; it is invisible from
outside, and the DXMT log gives no pid.

It explains a detail of the trace log in O16 that was recorded as unexplained:
two `Maximum supported feature level` blocks, the second selecting
`D3D_FEATURE_LEVEL_10_0` and immediately destroying its command queue. That is
**two different processes**, not one process probing itself.

**Consequences for Stage 3, which must be designed around this:**

- Hooking `D3D11CreateDevice` will fire in **whichever process reaches it**. The
  first process may never load `Direct3D11.dll` at all, so a module-wait there
  waits forever — which would look exactly like a hook that does not work.
- **Every log line must carry its pid.** The lines above are only
  interpretable because `host:` prints one; a per-frame draw count from Stage 4
  with two processes interleaved into one file and no pid would be worse than no
  log at all.
- The first process **logged no detach**. Only one `process exit` line appears,
  from the second. So pid 40 was terminated rather than exiting through the
  loader, and **anything Stage 3 or 4 wants to report at shutdown cannot be
  relied on in that process.**

### Also confirmed

- **`%TEMP%` is `C:\users\crossover\AppData\Local\Temp`**, not
  `users\crossover\Temp`. Stage 2 printed the latter in an install message and
  briefly looked in the wrong place.
- **`GetSystemDirectoryW` returns `C:\windows\system32` and the file actually
  loaded is `syswow64`'s**, via WOW64 redirection — which is why asking is right
  and hard-coding either path would be wrong.

---

## Hypotheses — not yet proven

**Status at the end of Stage 0.** Live: **H-B1** (specific, testable, has prior
art naming `TQ.exe`) and **H-D** (general, true almost by construction, too
unspecific to act on). Refuted: **H-A** (O10a), **H-E** (O13), **H-C** (O8).
Dissolved: **H-B2**, whose premise disappeared with Defect B.

Refuted hypotheses are kept in full, not deleted. Knowing what was already ruled
out — and by which experiment — is the point of this file.

### ~~H-E — `Map` with `DO_NOT_WAIT` fails, and the engine skips the draw~~ — **REFUTED by O13**

*Kept in full because it was a good hypothesis, it fit every measured property,
and the reasoning below is still the clearest statement of what the symptom
looks like. It was refuted by measurement, not by argument.*

#### Original statement — H-E — `Map` with `DO_NOT_WAIT` fails, and the engine skips the draw

*Raised by O12, 2026-08-25. The most specific hypothesis in the project, and the
only one with a knob already shipped for it.*

A D3D11 engine updating a per-frame effect maps its dynamic vertex/constant
buffer with `D3D11_MAP_WRITE_DISCARD`. If it passes
`D3D11_MAP_FLAG_DO_NOT_WAIT` and the resource is still in use by the GPU, `Map`
returns **`DXGI_ERROR_WAS_STILL_DRAWING`** instead of blocking. The documented,
normal engine response is to **skip the update — and therefore skip the draw —
for that frame**.

If DXMT returns `WAS_STILL_DRAWING` more readily than a native driver would, the
game does exactly what it was written to do: it silently drops that effect for
one frame. Which is precisely, and in every particular, what O12 measured.

*Fits every measured property, without needing an extra assumption for any of
them:*

| Measured (O12) | Predicted by H-E |
|---|---|
| The object is **absent**, not misplaced or dimmed | The draw was never issued |
| Exactly **one frame**, never two | Next frame the resource is free and `Map` succeeds |
| **Irregular** intervals — no period | Depends on CPU/GPU timing, which is not periodic |
| **Per-object**: one draw missing, all others land | Each effect maps its own buffer independently |
| Worse at high frame rate, and it **recruited clothes** (O10c) | More frames in flight, more contention, more resources still busy — and objects that never contended before start to |
| Only **1.2%** of frames at a 10fps cap (O12) | With the GPU mostly idle the resource is almost always free |

The last two rows matter: this is the only hypothesis so far that explains
**O10c's recruitment** — why raising the frame rate pulled in a class of geometry
that had not been flickering — rather than explaining it away as tempo.

*Test — and it is free.* DXMT ships **`d3d11.ignoreMapFlagNoWait`**, whose entire
purpose is to make `Map` ignore `DO_NOT_WAIT` and wait instead. O7 records this
being tried "to no effect", but that was an **eyeball judgement on a fast-strobing
screen**. O12 gives us a way to *count*: 8 events in 641 frames, in a fixed
scene, at a pinned frame rate. Re-testing it as a measurement rather than an
impression costs one launch. **E7.**

### H-D — One defect: whole draws or their resources are intermittently missing

*Raised by O9, 2026-08-25. Currently the best fit to what is actually on screen.*

Everything O9 describes has one shape: something that should be drawn is
**entirely absent for a few frames**, in bursts, while the scene is static. The
shrine FX, the character ground shadow and another character's hair are three
different passes showing one behaviour.

If that is right there is a single defect, not two, and it lives somewhere in
resource lifetime rather than in sampling: a dynamic buffer whose contents are
not visible to the frame that reads them, a discard/rename race on
`MAP_WRITE_DISCARD`, or a draw whose resource is not ready and which therefore
produces nothing.

*Fits:* the on/off character of every symptom (O9.3); bursts with calm between,
which is what a race or a periodic reallocation looks like and is not what a
sampling error looks like; the artefact appearing with the player static
(O9.1); shadows-off reducing rather than eliminating it (O1), because shadows
are simply the most numerous dynamic thing on screen.
*Strengthened by O10c:* raising the frame rate recruited a class of geometry
that was not flickering before. A dose-response relationship with frames per
second is what a CPU-write / GPU-read race looks like, and is not something a
sampling, precision or format error would produce.
*Against:* nothing yet. It is also the least specific hypothesis here, which is
exactly why it must not be acted on before it is observed. **H-B1 explains the
same observations and is more specific**; the two are not yet separable.
*Test:* a Metal frame capture (`DXMT_CAPTURE_FRAME`) of a burst — the only
experiment that can distinguish "the draw was never submitted" from "the draw was
submitted and read an empty resource". This is the same instrument H-B2 wanted
and it is still entirely unspent.

### ~~H-A — the unrepresentable border colour~~ — **REFUTED as the cause**

H-A made a prediction and it failed. Changing shadow-map resolution moves the
shadow frustum boundary, so a border-colour artefact should have **moved** with
it; **O10a** shows the flicker does not move. O9 had already shown whole shadow
shapes switching off and on, which a wrong border sample cannot do.

**The O2 warning is still real and still unexplained** — DXMT genuinely cannot
honour that sampler, and that is worth reporting upstream. It is simply not what
we are chasing. **Stage 4 as written no longer has a target; do not build it.**

Original reasoning, kept for the record:

The sampler with the `-FLT_MAX` border is the shadow pass's. Substituting `0`
turns "infinitely far" into "a surface at the near plane" for every sample
landing outside the source, so geometry near the shadow frustum boundary gets
the wrong shadow term — and as it moves across that boundary the term toggles
frame to frame, which reads as flicker.

*Fits:* shadows off removes most of the artefact (O1); only one warning exists
and it is this one (O2); the affected set is exactly "things that move relative
to the frustum".
*Does not explain:* Defect B.
*Test:* Stage 3 logs the full `D3D11_SAMPLER_DESC` so we learn which pass owns
it. Stage 4 rewrites it.

### H-B1 — Out-of-range constant-buffer reads — **the surviving specific hypothesis**

DXVK ships a per-app profile enabling `constantBufferRangeCheck` **for `TQ.exe`**
(O6), which exists because a game binds constant buffers smaller than its
shaders declare. D3D11 specifies out-of-bounds constant reads return zero; on
Metal they return whatever is adjacent. Garbage in per-object constants would hit
skinned and FX geometry.

*Fits, and much better after O9/O10 than when it was written.* An out-of-range
constant read returns adjacent memory rather than zero. If what lands in a
per-object transform or a bone matrix is garbage, the geometry is flung
somewhere off-screen — which on screen looks exactly like **the object vanished
for a few frames** (O9), not like it was drawn wrong. What is adjacent in memory
depends on allocator churn, which is faster at higher frame rates, so this is
also consistent with **O10c**. And unlike every other hypothesis here, it comes
with prior art that **names this executable**: DXVK ships
`constantBufferRangeCheck = True` keyed on `TQ.exe` (O6, `prior-art.md`).

*Against:* the "against" written originally — that it should have hit shadows
too — is **withdrawn**. O10b shows shadows and the residual are one defect, so
there is no longer a reason to expect this to spare the shadow pass.
*Note:* DXMT has no equivalent knob (O7), so this **cannot** be tested for free.
*Test:* `D3DReflect` declared CB sizes against logged `CreateBuffer` widths.
Cheap once Stage 2 exists. A frame capture may settle it sooner.

### ~~H-B2 — Defect B is something else in the transparency pass~~ — **dissolved**

*This hypothesis was about "Defect B", and there is no Defect B (O14).* Its
premise — that the residual set is specifically alpha-blended or alpha-tested
geometry — is false: O14 measured the same one-frame dropout on opaque NPC ground
shadows as on the FX beam. The transparency pass is not special.

What survives is the instrument it asked for, and that is now **Stage 1** in its
own right rather than a test for one hypothesis.

*Test:* a Metal frame capture (`DXMT_CAPTURE_FRAME`) of a frame containing the
shrine flicker. This is the highest-information single experiment available and
nothing has been spent on it yet.

### ~~H-C — FEX reduced x87 precision~~ — **DEAD**

Tested at `0`, no effect. See **O8**. Do not spend another launch on this.
