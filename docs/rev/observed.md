# Observed — what has actually been proven

Each entry says how it was established. Anything not backed by an experiment or
a log line is in **Hypotheses** at the bottom and is labelled as such.

---

## The artefact

Under `CX_GRAPHICS_BACKEND=dxmt` on the DX11 renderer, the game runs at full
speed and looks correct **except that things which move flicker**. Static world
geometry is fine. Reported affected: shadows, light/FX effects, boats, and
character clothes/hair.

## O1 — There are two defects, not one

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

## Hypotheses — not yet proven

### H-A — Defect A is the unrepresentable border colour (O2)

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

### H-B1 — Defect B is out-of-range constant-buffer reads

DXVK ships a per-app profile enabling `constantBufferRangeCheck` **for `TQ.exe`**
(O6), which exists because a game binds constant buffers smaller than its
shaders declare. D3D11 specifies out-of-bounds constant reads return zero; on
Metal they return whatever is adjacent. Garbage in per-object constants would hit
skinned and FX geometry.

*Fits:* hair/clothes are skinned; the shrine FX has per-effect constants.
*Against:* it should have hit shadows too, and shadows-off changed things — so
if this is Defect B it is genuinely a second, independent bug.
*Test:* `D3DReflect` declared CB sizes against logged `CreateBuffer` widths.
Cheap once Stage 2 exists.

### H-B2 — Defect B is something else in the transparency pass

The residual set (shrine FX, hair/clothes) is alpha-blended or alpha-tested
dynamic geometry. Candidates not yet examined: alpha-to-coverage, dual-source
blend, depth sorting, `MAP_WRITE_DISCARD` on dynamic vertex buffers.

*Test:* a Metal frame capture (`DXMT_CAPTURE_FRAME`) of a frame containing the
shrine flicker. This is the highest-information single experiment available and
nothing has been spent on it yet.

### H-C — FEX reduced x87 precision

`FEX_X87REDUCEDPRECISION=1` is set and untested at `0`. Prior is low: 32-bit
MSVC 11.0 defaults to SSE2 codegen. Listed only because the test is one flip and
one launch.
