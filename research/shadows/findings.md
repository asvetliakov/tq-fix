# Directional shadows: what is true, and how we know

Everything here was measured at runtime or verified against the shader archive.
Where a claim rests on a model rather than a measurement, it says so.

## The receiver is one deferred screen-space pass

Titan Quest's DX11 renderer does **not** apply directional shadows per
material. It applies them once, in a full-screen pass whose pixel shader:

1. reconstructs world position from the depth buffer (`t2`, inverse
   view-projection in its own `cb0`);
2. projects that position with a world-to-shadow matrix, also in its `cb0`;
3. runs four PCF taps against the shadow map at `t3`, comparing with `lt` and
   accumulating `0.25` each;
4. fades to fully lit over the outer 5% of the UV footprint via
   `saturate(min(u, v, 1-u, 1-v) * 20)`.

There is exactly **one** such program in the 367-program archive
(`95f0a104c855`). `92b5f78535ef` is its point-light counterpart: it divides by
a distance and uses `cb0[2].w` as a range, and must never be confused with it.

The per-material receivers that reference `worldToShadowMatrix` are legacy. A
runtime census over ~3M draws found the shadow map bound only for the deferred
pass; the per-material families never carried it. Any transform aimed at them
has no effect on directional shadows -- this is why an earlier cascade
implementation and the `enhanceShadowPcf` 3x3 filter both did nothing visible.

Diagnostic method, if this needs re-checking: probe `t1`..`t3` at draw time for
the texture the depth target was bound to, and count draws by bound shader
pair. Shader identity plus "is the shadow map actually bound" is the only
reliable test; the `worldToShadowMatrix` name appears in shaders that never
sample it.

## RenderDirectional is not re-entrant

`GraphicsShadowMapDx11::RenderDirectional` cannot be invoked a second time to
build another cascade. Measured consequences of doing so, same camera and same
crop, differing only in whether a first pass ran:

| first pass ran | fitted near coverage |
| --- | --- |
| no | 126.7 x 78.4 world units |
| yes | 47.7 x 63.0 world units |

Beyond the shrunken fit, a second invocation leaked region-scoped music across
a portal, produced object shimmer, and dropped shadows for about a second on
elevation change. The cause is in `cpu-path.md`: the caster query adds selected
entities to the temporary shadow scene *together with their owning regions*, so
a wider crop pulls in regions the frame never intended to touch. Reordering the
passes only changes which one is corrupted.

A real second cascade must not call this function again. The sound approach is
to replay the caster draws the engine already issues during the single native
pass into a mod-owned depth target with a wider projection, which touches no
engine state.

## Coverage and blur scale with the split, not the resolution

The crop constant `0.325` is a camera-ray interpolation parameter. Fitting two
measured projections (`0.325` -> 127 world units, `1.05` -> 415) gives

    coverage ~ split^1.90

A PCF tap offset is a UV distance, so the blur it produces measures
`0.5 * bluriness * coverage`. Widening the split therefore softens shadow edges
in **world** space regardless of map resolution -- resolution controls
aliasing, not radius. This is why raising the split alone always looked worse,
at any map size, and why the tap offsets must be scaled by `(0.325/split)^1.90`
to hold softness constant.

The game computes `shadowBluriness` from the map size it requested, and never
learns the map was enlarged, so that term is unaffected by `shadow_map_scale`.
Resolution does set a floor: once the four taps fall inside one texel the
filter degenerates to point sampling. That floor is estimated, not measured --
it assumes bluriness is about one texel at the game's own resolution.

## Reference points

At `0.325` on a 4096 map: 127 world units at 32 texels/unit. The shipped
default of `shadow_split=0.45` with `shadow_map_scale=4` gives about 236 units
at 35 texels/unit -- more coverage than native at slightly better density.

| split | coverage | texels/unit at 8192 |
| ---: | ---: | ---: |
| 0.325 | 127 | 65 |
| 0.40 | 188 | 43 |
| 0.45 | 236 | 35 |
| 0.50 | 288 | 28 |
| 0.70 | 533 | 15 |

`0.70` was tested and looked clearly worse everywhere, consistent with the
table.

## Two traps in the shader tooling

**Signature masks.** In an `OSGN` chunk the second mask byte is the components
the stage *never writes*; in `ISGN` it is the components read. Emitting the
input form into an output signature declares an output unwritten, and the
linker may drop it -- shader creation still succeeds and the failure is
invisible to every algebraic check.

**Container checksums.** The transforms zero the 16-byte DXBC checksum rather
than recomputing it. `D3DDisassemble` rejects such a container, so transformed
shaders cannot be disassembled directly; this is expected and not a defect.
Zeroing a native shader's checksum reproduces it exactly.
