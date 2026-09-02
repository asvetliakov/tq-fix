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

## The fit is rebuilt every frame with no texel snapping

`RenderDirectional` refits the directional projection from scratch each frame
around the camera-frustum corners at `t = 0 .. split`. Nothing quantizes the
result, so the shadow map's texel grid sits at a different sub-texel phase
every frame and shadow edges crawl for as long as the camera keeps changing.
This is original game behaviour, not something the split widening introduced.

Zoom is the worst case because it changes the box *extents* as well as its
position, and texel snapping alone cannot repair a texel size that changes
every frame. Titan Quest's zoom is smooth-damped, which is why the crawl lasts
a second or two after the input stops.

### The fit lands in one struct, and everything downstream reads it

After the eight-point min/max loop ends at `0x1018e9c4`, the routine builds a
single stack-resident orthographic camera at `esp+0x12c` and passes it to the
Camera setup routine by `call 0x10123e30` at `0x1018ec69`, with `ecx` holding
the struct.

| Offset | Contents |
| --- | --- |
| `+0x00` | camera type; `1` is the orthographic fit |
| `+0x04`, `+0x10`, `+0x1c` | light-basis rows, world space, orthonormal |
| `+0x28` | world position: box centre pushed back along the light direction |
| `+0x38` | full extent measured along basis row 0 |
| `+0x3c` | full extent measured along basis row 1 |
| `+0x40` | near depth, always `0.0` |
| `+0x44` | far depth, the fitted depth extent times `1.5` |

The five stack arguments and the `push %ecx` at `0x1018ec16` are the compiler's
stack-reservation idiom, not a real `ecx` use; the last argument is the far
depth passed by value.

What makes this the right seam is that both consumers derive from these same
fields, after the call: the receiver's projection is rebuilt from `+0x38`,
`+0x3c` and `+0x44` at `0x1018f00c`, and its view matrix by inverting `+0x04`
at `0x1018f089`. Adjusting the struct once, before `0x1018ec69`, therefore
cannot desynchronise what is rasterised from what is sampled. Adjusting the
projection anywhere later would.

The mod retargets that one relative call to a thunk that preserves `ecx`,
passes a copy to a stabiliser, and tail-jumps to `0x10123e30` with the stack
exactly as it found it. `eax`/`ecx`/`edx` are dead at the site and only `xmm0`
is live across the call, which the ABI already treats as volatile.

The stabiliser rounds each lateral extent up to the next eighth of an octave
and snaps the centre onto the resulting texel grid, projecting the position
onto the basis rows in double precision because world coordinates are large
enough to lose a useful fraction of a texel in float. The quantised extent
carries 1% slack so the snapped box still covers what the tight fit enclosed.

Camera *rotation* is not fully solved by this: the tight box's extents change
as the camera turns, so rotation crosses quantization thresholds more often
than zoom does. The complete cure is a rotation-invariant bounding sphere,
which needs the eight light-space points rather than the finished box.

## The real cause of shadow trembling: the basis rotates with the camera

Measured, not inferred. A 240-frame trace across a zoom, with the fit
stabiliser already installed:

- the quantised extent held at a single value for the whole capture, and the
  snap offset moved smoothly through `[-1, 0]` texels and wrapped -- the
  snapping worked exactly as designed;
- the tight extents varied by under 1% and the depth range by under 4%;
- **basis row 0 rotated 3 degrees, at up to 0.88 degrees per frame.**

At the edge of a ~180-unit box that slides the texel grid 1.38 world units per
frame: about 15 texels on a 2048 map and 62 on an 8192 one. The grid is
completely re-quantized every frame at any resolution, which is why neither
texel snapping nor a four-times-larger map changed the symptom at all.

The source is the look-at helper at `0x10283df0`, called from the fit at
`0x1018e7fa` (cdecl; the caller cleans its two stack arguments at
`0x1018e815`). It normalizes the light direction into row 2, then crosses it
with a **caller-supplied reference vector** to build rows 0 and 1. That
reference tracks the camera, and Titan Quest pitches the camera as it zooms, so
the light-space frame spins about the light axis.

The mod retargets that call to a thunk that rewrites only the reference
argument in place, to the world axis the light is least aligned with. The
engine still derives the light axis itself. The basis becomes a function of the
light alone, which is the precondition for snapping to mean anything.

The cost is a looser box: a world-aligned basis cannot hug a rotated camera
frustum the way a camera-aligned one does. That is the standard price of a
stable shadow map and is paid for by the enlarged directional map.

**The order that matters:** a stable shadow map needs a fixed orientation, a
quantized extent, and a snapped centre, and the first is the dominant term. Two
of the three on their own are worth nothing, which is precisely what the first
in-game test of the stabiliser showed.
