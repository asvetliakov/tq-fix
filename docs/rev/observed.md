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

# Stage 3 — the device

## O18 — The two processes, explained: the second one is **Steam's** child, not ours

*Established from Steam's own `console_log.txt` and `content_log.txt`, matched
against our pid-stamped log, 2026-08-25.*

O17 recorded that `TQ.exe` runs as two processes without saying why. Steam's log
says why, and it changes how every remaining experiment must be set up:

```
23:29:14  p956    our log: TQ.exe attaches, forwards winmm, exits 2.6s later
23:29:17  Steam:  GameAction [AppID 475150] LaunchApp ... CreatingProcess
23:29:20  Steam:  Game process added : "...\TQ.exe" /dx11, ProcID 1708
23:29:20  p1708   our log: TQ.exe attaches again - this is the one that renders
```

The `TQ.exe` we launch is a **Steam handoff stub**. It asks Steam to run
`steam://rungameid/475150` and exits; **Steam** then launches the real game as
`TQ.exe /dx11`, and that second process is the renderer. The first never loads
`Direct3D11.dll` at all.

**This is O15's caveat, arriving from the other direction and biting.** O15 said
a game started from the Steam UI inherits Steam's environment rather than ours,
and to launch `TQ.exe` directly instead. Launching directly does **not** avoid it:
the direct launch is only a stub, and the process that matters is still spawned by
Steam. **Per-run `cxstart` environment injection does not reach the renderer.**

Consequences:

- Anything the render process must see — `DXMT_*`, `FEX_*`, and now our own
  `TQFLICKER_HOOK` — has to be somewhere Steam can see it: `cxbottle.conf`, or
  Steam's own launch options, or a file beside the exe like `dxmt.conf` (O11a).
  **O15 is not wrong, it is narrower than it reads:** it reaches processes we
  start, and the renderer is not one of them.
- The renderer is launched with **`/dx11`** explicitly, by Steam, from the app
  manifest — not by our command line.
- Our DLL loads into both processes, so the pid stamp on every log line (O17's
  demand) is what makes the file readable at all.

**Refinement, from three later Steam-UI launches:** each produced **exactly one**
`TQ.exe` attach, not two. So the process count is a property of the *route* and
possibly of the run, not a fixed fact about the game: the direct route always
adds the stub, and O17's two Steam-tracked processes (23:13:09 then 23:13:22)
were the game restarting itself for reasons still unknown. **Do not assume
either count.** The pid stamp is what makes the log legible whichever happens,
and that is the whole reason it is there.

## O19 — The device is reached. One entry point, and the race is won by ~150ms

*Established in the running game, 2026-08-25, by the Stage 3 build.*

```
23:29:24.670  d3d11: Direct3D11.dll at 765E0000 after 2200ms
23:29:24.670    IAT Direct3D11.dll imports d3d11.dll.D3D11CreateDeviceAndSwapChain
                    at 7664C2E8 -> was 762EEDA0, now 79BF3900
23:29:24.691  d3d11: Direct3D11.dll does not import d3d11.dll!D3D11CreateDevice
23:29:24.838  d3d11: D3D11CreateDeviceAndSwapChain succeeded
                device   06536BC0  (vtable 76465E84)
                context  06901448  (vtable 764759E4)
                swapchain 067F0748  (vtable 7646C808)
                feature level 11_0
                driver UNKNOWN, flags 0x00000000, SDK version 7
                it asked for: 11_0
```

**`Direct3D11.dll` imports exactly one of the two entry points.** It has
`D3D11CreateDeviceAndSwapChain` and **not** `D3D11CreateDevice` — confirmed by
`objdump` before the code was written and by the running game afterwards. The
Stage 3 plan said to hook both because "a miss looks identical to a hook that
does not work"; hooking both is still right, and the absent one is a *fact about
the game*, not a failure, so it is logged as one.

It also means **the game's device and its swapchain are created in the same
call**, so Stage 4's `Present` hook and its `Draw*` hooks come from one captured
moment. Nothing else in the process imports `d3d11.dll` — not `TQ.exe`, not
`Engine.dll`, not `Game.dll`, not `GFSDK_SSAO_D3D11.win32.dll` — so this one IAT
slot is the whole surface.

**The race is real and it is close.** `Direct3D11.dll` appeared 2200ms after our
watcher started, and the game called through it **147ms later**. The 10ms poll
interval in `modules::waitFor` is what caught it; a 250ms poll would have lost
the device roughly half the time, and the symptom would have been an intermittent
"hooked, but never called" that looked like a bug in the hook.

## O20 — Risk 3 is answered: `ID3D11Device1` is the **same object, same vtable**

*Established by `QueryInterface` in the hook, in the running game.*

```
ID3D11Device1 06536BC0 (vtable 76465E84) - the SAME object as ID3D11Device
```

The device pointer and the vtable pointer are byte-identical to the
`ID3D11Device` the game was handed. **DXMT does not hand out a separate object
for the newer interface**, so Stage 4 can patch one vtable and reach the game's
calls whichever interface it uses.

This closes **Risk 3**, which had been open since the project started and was
carried into the Stage 3 plan as a trap to check "while it is cheap". It was
cheap, and the answer is the convenient one. *(It says nothing about
`ID3D11DeviceContext1`, which Stage 4 should ask the same question of before it
patches the context.)*

## O21 — The swapchain: 5120×1440, **one buffer**, DISCARD, windowed

*Established from the `DXGI_SWAP_CHAIN_DESC` the game passed.*

```
5120x1440, format 28, 1 buffer(s), 1 sample(s), windowed, swap effect 0
refresh requested: 1/60 Hz
```

Format 28 is `DXGI_FORMAT_R8G8B8A8_UNORM`; swap effect 0 is
`DXGI_SWAP_EFFECT_DISCARD`. **`BufferCount = 1`** is the number to notice: the
game asks for the shallowest pipeline DXGI allows. Every "the frame read the
wrong copy" hypothesis in this file is ultimately about how many frames can be in
flight, and this is a direct measurement of what the game requested — recorded
now, unused, because Stage 4 is where it becomes a question.

`RefreshRate` is `Numerator = 1, Denominator = 60`, which is 1/60 Hz rather than
60 Hz. It is the game's own field and DXGI ignores it in windowed mode, so it is
noted and not chased.

## O22 — ~~Unresolved: the render process lived seven seconds~~ — **RESOLVED by O24: it was the launch route, not the hook**

> **Read this entry with its alarm crossed out.** The measurement below is
> correct and the caution was right, but the suspect was wrong. **O24 ran the
> control and the hook is exonerated**: launched from the Steam UI, the game
> exits cleanly *with* the hook installed. The seven-second process was the Steam
> handoff stub being killed once Steam took over the launch (O18), which is what
> that route always does.
>
> Kept in full, because the reasoning is the reasoning this project is supposed
> to use — stop, do not build on an unattributed possible crash, and construct
> the control that settles it — and because the mistake is worth seeing: **the
> launch route was an uncontrolled variable, and it was the one that mattered.**

*The original entry, written before the control existed:*

On the launch above, Steam's `content_log.txt` records the render process
running from **23:29:20 to 23:29:27** — seven seconds. It created its device 4.8s
in and was gone 2.5s later, and **our DLL logged no `DLL_PROCESS_DETACH` at all**,
so it was terminated rather than exiting through the loader.

For comparison, the same log across the evening:

| Window | Duration | What it was |
|---|---|---|
| 23:13:22 → 23:14:23 | **61s** | the O17 run, Stage 2 proxy, reporter playing |
| 23:29:20 → 23:29:27 | **7s** | this run, Stage 3 build |

**This does not prove the Stage 3 build killed it, and it must not be recorded as
if it did.** Nobody was at the keyboard, the session could have ended for its own
reasons, and no control run exists. What is true is that it is the shortest
render session in the log and it ended immediately after the first hook this
project has ever installed in the game. That is enough to stop, not enough to
conclude.

**The control was attempted and could not be obtained.** After that launch,
Steam stopped responding to launch requests entirely: `console_log.txt` has
written nothing since 23:29:20, and three further launches — one with the DLL
installed, two with it removed — never reached Steam at all. The bottle needs a
`wineserver` restart before anything can be measured again.

### What was built so the next session can attribute it in one launch

- **`TQFLICKER_HOOK=0`** — the same binary loads and forwards winmm exactly as
  Stage 2 did, and installs no hook. Comparing *installed* against *uninstalled*
  changes two things at once and cannot answer this; comparing *hook* against
  *no hook* in one binary can. **Per O18 it must be set where Steam can see it**
  — `cxbottle.conf`, not `cxstart`.
- **A heartbeat**: one line every two seconds for the first minute, then silence.
  The log for this run ends at device creation and cannot distinguish "the game
  ran and was quit" from "the game died on the spot". The next one will.

## O23 — `FreeLibrary` does not unload us in this bottle, so the orderly-detach path never runs

*Negative result, established by the off-game self-test, which now reports it on
every run.*

`selftest.exe` loads our `winmm.dll`, exercises it, and calls `FreeLibrary`. The
module is **still loaded afterwards** — `GetModuleFileNameA` on the freed handle
still answers — and our `DllMain` receives no detach until process exit.

So of the two `DLL_PROCESS_DETACH` paths, **only the process-exit one is ever
taken here**, and that is the path that deliberately does *less*: it reports and
returns without unpatching, because touching another module's import table while
the address space is being torn down is a crash on quit, which would look exactly
like our patches breaking the game.

**Consequences:** `patch::unpatchAll()` is correct, is exercised by the patch
self-test, and is — in this bottle, so far — never reached in a real run. Do not
delete it on those grounds; do not rely on it either. And a build installed into
the game directory cannot be replaced while the game is running.

*(Why the reference is held was not established. It is not our own
`LoadLibraryW` of the system winmm — that resolves to a different module, which
`winmm_proxy.cpp` checks for explicitly and would have logged. Left open; it
costs nothing.)*

## O24 — The control: **the hook is exonerated**, and the launch route was the uncontrolled variable

*Established by two paired launches, 2026-08-25, same binary, same scene, the
only difference being `TQFLICKER_HOOK`.*

| Run | Launch route | Hook | Lifetime | Exit |
|---|---|---|---|---|
| O22's run | **direct `cxstart TQ.exe`** | on | **7s** | terminated, no detach |
| Control | **Steam UI** | **off** | 64s | clean, both reports logged |
| Test | **Steam UI** | **on** | 38s | clean, both reports logged |

The hooked run exited through the loader and logged its summary:

```
d3d11 (process exit): 1 call(s), 1 succeeded. device 0679F870, context 06B4ED90,
                      feature level 11_0
```

**The hook is not what killed the seven-second process.** The variable that
actually differed was how the game was started: the direct route runs the Steam
handoff stub, which asks Steam to launch the game and is then terminated (O18).
That is what a 7-second "session" with no detach *is*. Both Steam-UI launches
exited cleanly whether the hook was installed or not.

**The methodological lesson, which is the reason this entry exists.** O22 held
two things constant that were not the interesting ones and let the launch route
float. Every measurement in this project from here on states the launch route,
because it changes the process topology, the environment the renderer inherits
(O18), and — as it turns out — whether a clean exit is even possible.

### And the environment problem from O18 has a solution

`TQFLICKER_HOOK=0` **did reach the render process**, and the mechanism is the one
O18's finding implies: **start Steam itself through `cxstart` with the variable
set, and the game Steam spawns inherits it.**

```sh
CX="/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver"
TQFLICKER_HOOK=0 "$CX/bin/cxstart" --bottle "New Bottle" --no-convert \
  -- 'C:\Program Files (x86)\Steam\steam.exe'
```

This restores what O15 promised and O18 took away: **per-run variables that reach
the renderer, with no `cxbottle.conf` edit and no Risk 8.** The cost is that
Steam must be restarted to change one, because a process's environment is fixed
at launch.

*(Stopping Steam needs `wineserver-arm64 -k` — the binary is
`bin/wineserver-arm64`, not `bin/wineserver`, which does not exist. Its CEF
`steamwebhelper` children survive the kill and have to be killed by pid; Steam
restarts correctly through the orphans.)*

## O26 — **Character create/select stopped working, and it is not ours** — probable cost of a hard `wineserver` kill

*Established by the reporter at the keyboard, across three states of our code,
2026-08-25. Recorded as a hazard, not a finding about the flicker.*

The reporter could not create or select a character from the main menu. Isolated
immediately, and the ladder cleared our code completely:

| Our code | Character select |
|---|---|
| DLL installed, **hook on** | broken |
| DLL installed, **hook off** (`TQFLICKER_HOOK=0`) | broken |
| **DLL removed entirely**, override removed | **still broken** |

**Nothing of this project is involved.** Same symptom with no `winmm.dll` in the
game directory and no `DllOverrides` entry.

### The probable cause, and it was self-inflicted

To unwedge Steam (O22), the bottle's `wineserver` was killed and Steam's
surviving `steamwebhelper` processes were `kill -9`'d, at roughly 23:43. **Every
character-select attempt happened after that point**; the last session known to
reach gameplay was 23:13. Titan Quest AE's saves are **Steam Cloud synced**, and
a client killed mid-session is a well-known way to leave cloud state
inconsistent.

Not proven — the failure was never observed before the kill, so there is no
before-and-after. It is the leading candidate and it is ours.

### Resolved: a proper restart fixed it, and nothing was lost

The reporter restarted normally and character create/select worked again, with
our DLL installed and the hook active. **No save data was lost.** The damage was
to session state, not to the saves, and it healed on a clean start — which is
consistent with the Steam Cloud explanation and inconsistent with anything having
touched the files.

### The rule this buys

**Do not hard-kill Steam or `wineserver` to recover from a wedged launch.** Quit
Steam through its own menu and let it exit, even when that is slower. The
recovery for a wedged bottle cost more than the wedge did: it was cheap to do,
and it may have cost the reporter their characters.

If it must be done, **check the game's save directory first** —
`Documents\My Games\Titan Quest - Immortal Throne\SaveData\Main\`, one `_Name`
folder per character — so there is a record of what existed beforehand. Note that
the bottle's `Documents` is a **symlink to the real `~/Documents`**, which is
unreadable from a macOS terminal under TCC, so that check has to be done by the
reporter, not by tooling.

## O25 — `Direct3D11.dll` loads at the same base every run, so vtable addresses are comparable

*Established by comparing two runs.*

`Direct3D11.dll` was at `765E0000` in both, and all three vtable pointers were
**byte-identical across runs** — device `76465E84`, context `764759E4`,
swapchain `7646C808` — while the object pointers themselves differed
(`06536BC0` vs `0679F870`), as they should.

There is effectively **no ASLR on this module here**, which means a vtable
address written in one session's log can be compared against another's. Useful
for Stage 4, and worth knowing before someone treats a matching address as
suspicious. **The object pointers are not stable; do not key anything on them.**

Held across **three** runs by the end of the stage — devices `06536BC0`,
`0679F870`, `064CBE20`, all three with vtable `76465E84`.

## O27 — Stage 3's gate, met: a real play session with the hook installed

*Established by the reporter playing the game, 2026-08-25, DLL installed, hook
active, launched from the Steam UI.*

```
23:58:47  p1520   host: TQ.exe
23:58:51  p1520   d3d11: Direct3D11.dll at 765E0000 after 2630ms
23:58:52  p1520   d3d11: D3D11CreateDeviceAndSwapChain succeeded
                    device 064CBE20  context 0691B508  feature level 11_0
23:59:34  p1520   d3d11 (process exit): 1 call(s), 1 succeeded.
23:59:34  p1520   winmm (process exit): 186 of 186 exports forwarded, 0 call(s)
                  to one we could not forward
```

Forty-seven seconds, **heartbeat unbroken**, gameplay reached, **exit through the
loader** with both summaries written. The hook was called **exactly once** — no
capability probe, no second device, no driver-type fallback loop.

**This is what Stage 3 set out to prove and it is now proved in the running
game:** we hold a valid `ID3D11Device*` and `ID3D11DeviceContext*`, we got them
by a data write and nothing else, and the game does not care that we are there.
Stage 4 may proceed.

---

# Stage 4 — counting the draws

## O28 — Fourteen vtable slots patched through the real DXMT, off-game, and every hook fires

*Established 2026-08-26 by the off-game self-test, which now creates a real
D3D11 device through the bottle's 32-bit DXMT and drives it through the patched
slots. No game launch spent.*

`selftest.exe` imports `d3d11.dll` statically; with `TQFLICKER_D3D_HOST=selftest.exe`
the watcher hooks *its* import of `D3D11CreateDeviceAndSwapChain` instead of
`Direct3D11.dll`'s, so the device it makes goes through exactly the code path
the game's will. Then:

```
frames:   patching vtables (... swapchain has 18 methods, context 115, device 43)
  VT IDXGISwapChain::Present      slot   8 at 79CBC828 -> was 79B38820, now 799951D0
  ID3D11DeviceContext1 00BAF188 (vtable 79CC59E4) - the SAME object as ID3D11DeviceContext
  VT ID3D11DeviceContext          slot  12 ... slot 13, 20, 21, 38, 39, 40, 58, 14
  VT ID3D11Device                 slot   3 ... slot 12, 15, 23
frames:   14 vtable slot(s) patched. Present=ok Draw*=ok Map=ok CreateBuffer=ok shaders=ok/ok sampler=ok
cbuffer:  #1 00BB8378  256 bytes, DYNAMIC, cpu 0x10000, misc 0x0, frame 0
sampler:  #1 00BBC908  filter 0x94 (comparison)  addr BORDER/BORDER/BORDER  border (-3.40282e+38, 0, 0, 0)  cmp 4 ...
frames:   first Present (sync interval 1, flags 0x0) - the frame counter is running; ...
frames (process exit): 5 frame(s), 0 draw(s) ... Table: C:\users\crossover\AppData\Local\Temp\tqflicker-frames.log
```

and the table has five rows, one per `Present`, each with `maps 1`.

**Four facts from one run:**

1. **DXMT's vtable pages take a `VirtualProtect` data write** — swapchain,
   context and device alike — and the hooked slots are the ones called: every
   hook fired exactly as many times as the test called it. The primitive proven
   on our own page (Stage 3) is now proven on DXMT's.
2. **Risk 3 is closed for the context too.** `QueryInterface` for
   `ID3D11DeviceContext1` returns the *same pointer with the same vtable*, as
   O20 found for the device. One vtable each; nothing to patch twice.
3. **The `-FLT_MAX` sampler reproduces, and DXMT accepts it** — `S_OK` with a
   valid object — so O2's warning is a substitution, not a refusal. The hook
   logs the full description, which is what the Stage 6 report needs.
4. **The vtable addresses differ from the game's** (`79CB5E84` here against
   `76465E84` in O25) because `d3d11.dll` is at a different base in a
   different process. O25's "stable across runs" is per-executable; do not
   compare a self-test address with a game address.

**Not proven here:** a `Draw*` call. The test has no pipeline, and a draw with
nothing bound would test DXMT's tolerance rather than our hook. The seven draw
slots are the same primitive on the same vtable as `Map`, which did fire.

## O29 — The Stage 0 detector, committed at last, reproduces O12 and O14 from the same recording

*Established 2026-08-26 by running `tools/recording.py` on the E6 recording
(`cache/captures/*10.25.03*.mov`). No launch spent.*

The Stage 0 analysis lived in a session scratchpad and was one `rm` from gone
(Risk 12, again). It is now `tools/recording.py`, and on the E6 file it finds:

- the **same eight** beam dropouts — recording frames **51, 73, 110, 156, 266,
  275, 281, 346**, each `DARK`, 55–62 blocks, peak 128–177 gray levels, all in
  x 192–272 y 0–192 (O12: "8 events, x 192–256, y 0–192");
- **9.5% of frames** with at least one confirmed one-frame anomaly (O14:
  "roughly 9%"), on the same fixed locations — the NPC shadow at (320–352,
  224–256), the prop shadow at (448–464, 128–144), the two NPC shadows at
  (448–512, 384–416).

Same recording, same numbers, independent code path (ffmpeg → raw gray, no PNG,
no PIL). The instrument is reproducible and it is in the repo.

**Two things it cost to learn, recorded so the next run does not pay again:**

1. **`ffmpeg` duplicates frames unless told not to.** Piping to rawvideo
   defaults to a constant frame rate, which turned 674 recorded frames into
   more, broke the "exactly one frame" test, and found **2** anomalies instead
   of 61. `-fps_mode passthrough` is mandatory; the tool now checks the frame
   count against `ffprobe`'s and says so if they differ.
2. **The recording's 1:1 region ends before the file does.** From about frame
   400 the intervals drop to 17–42ms — the recorder was catching up or the cap
   had been lifted — and the tool marks anomalies there `(NOT 1:1 here -
   ignore)` and excludes them from alignment. Stage 0 only checked where the
   region *started*.

*(And the filename: macOS puts a U+202F narrow no-break space before `PM`. A
path typed with a normal space is "No such file or directory" with the sandbox
on or off. Glob it.)*

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
