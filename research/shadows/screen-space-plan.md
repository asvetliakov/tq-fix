# Plan: screen-space contact shadows

This is the original design and experiment record. The current implementation,
defaults, tracing and validation are documented in [contact-review.md](contact-review.md).

Additive short-range occlusion for detail the shadow map cannot resolve. The
shadow map keeps doing all long-range work; the contact term can only darken,
never lighten. Combine with `min`.

## Why this is wanted

At the shipped defaults the map covers ~236 world units at ~35 texels/unit, so
one texel spans ~0.03 units. Contact detail below that -- feet on ground, cart
wheels, trellis posts, foliage stems -- cannot be represented. Raising density
costs coverage, and coverage is already the constraint on a 32:9 display, so
this is the only remaining lever that does not trade against the split.

Do this **only if** contact still looks detached after the depth-bias fix
(`shadow_bias_scale`, default 0.695). That fix removed a bias inflation we
introduced ourselves, and may account for most of the problem.

## Where it goes

One shader: the deferred screen-space receiver, matched by
`isDeferredShadowReceiver()` in `src/dxbc_patch.cpp`.

- disassembly: `research/shadows/shaders/deferred-receiver-ps.asm`
- fixture: `test/fixtures/tq-dxbc-PS-deferred-shadow.b64`
- it is patched at creation in `hookCreatePixelShader`, so the game binds the
  modified shader itself -- no per-draw shader swapping is involved.

Relevant structure, in execution order:

| what | where |
| --- | --- |
| screen UV | `v1.xy` |
| scene depth (NDC z) | `sample t2, s2` |
| inverse view-projection | `cb0[8..11]` |
| world position | after the first four DP4s and their divide |
| world-to-shadow | `cb0[4..7]` |
| light colour / direction | `cb0[0]` / `cb0[1]` |
| camera position | `cb0[3].xyz` |
| **final shadow term** | `r0.w`, after `mad r0.w, r1.x, r0.w, l(1.0)` |

`r0.w` multiplies both the diffuse (`mul o0.xyz, r0.w, r2.xyzx`) and the
specular contribution, so one `min` there covers the whole light.

Note the world position is destroyed shortly after it is produced by
`add r0.xyz, -r0.xyzx, cb0[3].xyzx` (it becomes the view vector). Save it into
a fresh temporary at the point it is still live, exactly as the earlier vertex
cascade transform did.

## The one real design problem

The march needs **world-to-clip**, and the shader only has its inverse. Solve
it on the CPU and supply the result; do not attempt to invert in-shader.

The march is affine in clip space, which keeps the per-step cost low:

    clip(P + L*t) = clip(P) + t * (VP * L)

So upload:

- `b13[0..3]` -- view-projection, laid out to match the DP4 convention already
  used for `cb0` matrices (register *i* dotted with the input gives component
  *i*);
- `b13[4]` -- `VP * L` as a direction (w = 0), precomputed on the CPU;
- `b13[5]` -- `(stepLength, depthBias, maxThickness, strength)`.

Then each march step is one `mad`, a divide, a `mad` for UV, one `sample`, and
two compares -- roughly 8-10 instructions. Eight steps is about 80
instructions of straight-line code; no loop opcodes are needed and none should
be used.

### Getting the view-projection: two options

**A. Read `cb0` back and invert on the CPU.** `cb0` is 12 float4s (192 bytes).
Copy the bound buffer to a staging resource, map it, take `cb0[8..11]` (the
inverse VP) and invert. Cheap in bandwidth, but `Map(D3D11_MAP_READ)` on a
resource the GPU just wrote will stall. **Double-buffer it**: copy this frame,
read the previous frame's copy. One frame of camera latency is imperceptible
for a short-range effect.

**B. Take the camera from the engine.** The renderer holds it at `+0x28`
(`cpu-path.md`). Avoids readback entirely but couples to engine structure.

Start with A. It is self-contained and does not add a new engine dependency.

## The march origin is free, so do not compute it

Added after phase 1, and it changes step 1 below.

The origin does not need computing at all. Writing `A` for the inverse the
receiver holds and `B = A^-1` for what goes in `b13`, the receiver's own
reconstruction is `h = A * (ndc, 1)` and `P = h.xyz / h.w`, so

    B * (P, 1) = B * h / h.w = (ndc, 1) / h.w

The clip-space origin is the pixel's own NDC over a scalar the shader already
computes -- `h.w` is the fourth DP4, `dp4 r0.x, r0.xyzw, cb0[11].xyzw`, before
the divide consumes it. Scaling a homogeneous vector changes no UV, so multiply
through by `h.w`:

    clip_i = (ndc, 1) + (i * step * h.w) * (VP * L)

Exact at `i = 0` by construction, and the matrix survives only in the direction
term. The cost is two `mov`s to keep `(ndc.xy, depth)` and `h.w` alive past the
instructions that overwrite them, against the four DP4s it removes.

`b13[0..3]` still carries the matrix: it is what `VP * L` is derived from, and
it is what the in-game probe validates.

### How large the matrix route's error actually is

Worth stating precisely, because a first pass at this got it wrong. Round trip
of a pixel through the float inverse and back through the float forward matrix:

| camera position | NDC error |
| --- | ---: |
| synthetic, five-figure coordinates | 3.7e-3 |
| synthetic, near the world origin | 4.3e-5 |
| measured, menu at `(3.05, 0.47, 12.09)` | 9.9e-7 |
| **measured, open world at `(200..237, 40..70, 55..79)`** | **1.7e-5 .. 5.9e-5** |

So the matrix route would have been usable throughout. Titan Quest's receiver
space keeps the camera in the low hundreds of units, not thousands, and the
five-figure case was an assumption about world scale the log did not support.

The identity origin is still the right choice -- cheaper, exact by
construction, and insensitive to a world scale nobody has bounded, since the
camera was only ever sampled in the first outdoor area. It is just not a rescue
from a problem that was shown to exist.

## What the readback actually does on DXMT

Measured over 1011 receiver draws in one session:

| outcome | count |
| --- | ---: |
| readback landed | 355 |
| `DO_NOT_WAIT` refused | 656 |
| of which resolved by a blocking map | 153 |

The plan's double buffer is not enough on its own -- a staging buffer copied
last frame is usually still not mapable, and without a fallback the transform
froze for 89 frames at a stretch. After four consecutive refusals the map now
waits, which brings the average to a refresh every three frames.

That staleness is harmless *because* of the identity origin: the matrix only
supplies the march direction and step scale, both of which change slowly. Had
the origin come from `VP * P`, a three-frame-old matrix would have displaced
every march origin during camera movement.

The blocking map's cost has not been measured and belongs with the phase 5
timing work.

## What the march actually costs on screen

Measured at the receiver's own viewport, 5120x1440, walking the first outdoor
area, for the original `length = 0.5` in 8 steps:

| depth | distance | total px | px/step |
| ---: | ---: | ---: | ---: |
| 0.10 | 1.1 | 1259 | 112 |
| 0.50 | 2.0 | 583 | 61 |
| 0.90 | 9.6 | 104 | 12.5 |
| 0.98 | 40.2 | 24 | 3.0 |

The top rows are a distraction: the near plane is 1.0 and the far plane about
40, and in an isometric view nothing sits a unit from the camera. **Geometry
lives at depth 0.92 to 0.99.** In that band half a world unit is 24 to 105
pixels at 3 to 13 pixels a step, which steps over exactly the thin detail the
effect exists to catch, so the default drops to `0.25` -- 12 to 52 pixels at
1.5 to 6. Provisional until the phase 4 A/B.

## Algorithm

For each pixel, after the native shadow term is final:

1. `clip0 = (ndc.x, ndc.y, depth, 1)`, saved before it is overwritten, and the
   step vector `(step * h.w) * clipL`.
2. For `i` in `1..N`: `clip = clip0 + i * stepVector`.
3. `uv = clip.xy / clip.w * (0.5, -0.5) + 0.5`; `z = clip.z / clip.w`.
4. `d = sample(t2, s2, uv).x` -- the depth buffer stores NDC z directly, which
   is why the receiver can feed it straight into the inverse VP.
5. The buffer must be nearer than the marched point by more than `depthBias`
   for the step to count at all, in world units. How much it counts falls off
   linearly with the depth gap and reaches zero at `maxThickness`, which is
   what stops distant geometry casting through.

   A hard cut at the thickness makes an occluder just inside it count fully and
   one just outside count nothing -- speckle, on anything thin. The falloff is
   free: the CPU uploads `1 / maxThickness` and the step is one saturating
   multiply and a subtract, the same instruction count the cut needed.

   Do not compare in NDC z. It is non-linear enough that one constant is
   0.046 world units ten units from the camera and 0.80 at forty -- wider than
   the whole march -- so a single bias cannot work at both ends and the effect
   dies with distance. `b13[6]` carries `(a, b)` from the inverse
   view-projection's fourth row; the buffer's view depth is `1 / (a*z + b)`,
   and the marched point's is its own clip `w` divided by `h.w`, which the
   setup already computes as a reciprocal and reuses.
6. Count the occluded steps and divide by the step count, then scale by
   strength: `contact = 1 - strength * occluded / steps`.

   **Not** a binary latch. A single hit at full strength reads as speckle on
   thin noisy geometry -- grass above all. The game's own AO is NVIDIA HBAO+
   (`GFSDK_SSAO_D3D11.win32.dll`, linked by `Direct3D11.dll`, not by the game
   code) and it never latches: it integrates a fraction of the horizon and
   bilateral-blurs the result. That, not any form of masking, is why it does
   not wreck grass -- its API takes a depth texture and has no per-object
   exclusion at all.
7. Gate the receiver on the G-buffer normal at `t0`, which decodes as
   `2*texel - 1` in world space (the shader dots it with the world light).
   Restricting the effect to surfaces facing up keeps it off grass blades,
   ghosts and walls. Compare against `(threshold + 1) / 2` so the shader needs
   no decode.
8. `min r0.w, r0.w, contact`.

Nothing can stop grass *casting* into the march. A screen-space pass sees a
depth buffer, not objects, and the depth buffer does not say what wrote it.

Sampling outside `[0,1]` UV must not occlude -- clamp or test and skip, or
off-screen geometry will streak.

## Configuration

Under `[graphics]`, read in `src/shadow_fix.cpp` beside the existing dials:

    shadow_contact          = on | off      (default off until validated)
    shadow_contact_steps    = 8             (4..16)
    shadow_contact_length   = 0.25          world units, total march distance
    shadow_contact_bias     = 0.02          world units
    shadow_contact_thickness= 0.5           world units, the falloff distance
    shadow_contact_strength = 1.0           0..1
    shadow_contact_upright  = -1.0          least upward the receiving normal
                                            may be; -1 accepts everything,
                                            ~0.3 restricts it to ground

Ship it **off by default** until it has been judged in motion.

Under `[debug]`, for the A/B:

    shadow_contact_toggle   = 1             Ctrl+Shift+C flips the effect

The march cannot be added to or removed from a live shader, and a second
program cannot be built in the creation hook -- DXMT deadlocks on a device
shader created re-entrantly from `CreatePixelShader`, which is why the
post-process programs are deferred to a worker. The key zeroes the strength in
`b13[5].w` instead, which makes the final `min` a no-op. The comparison is
therefore exact for appearance and useless for cost: the march still runs.
Measure cost with `shadow_contact=off` and a restart.

## Phases

1. **CPU side.** Reserve and bind `b13`; obtain VP via option A; upload the
   matrix, `VP*L`, and parameters. Verify with a run that the values are sane
   (log the matrix, check translation and that `VP * P` reproduces the pixel's
   own NDC for a known point). *Done -- `src/contact_shadow.cpp`. It produced
   the march-origin result above, which was not anticipated here.*
2. **Transform.** Extend `tuneDeferredShadowFilter`, or add a sibling that
   composes after it, to append the march. Keep it purely additive: insert
   instructions, rewrite nothing, exactly as the cascade transform did.
   *Done -- `addContactShadowMarch`. It composes **before** the retune, not
   after: the retune rewrites the tap immediates the march's shape test keys
   on, so the other order is refused outright. Fifteen instructions a step.*
3. **Offline validation.** Transform must match exactly 1 of 367 corpus
   shaders; DXMT must create the result; applying twice must be refused.
   *Done -- 1 of 367 via `scripts/corpus-match.sh contact`; DXMT creates the
   march alone, the march with the retune, and the sixteen-step worst case;
   marching twice, marching a retuned shader, marching a per-material receiver
   and step counts outside 4..16 are all refused.*
4. **In-game A/B.** `shadow_contact=off` vs `on`, same scene, same camera.
5. **Cost.** Use the GPU timestamp queries. 8 steps at 5120x1440 is ~60M extra
   depth samples per frame; through DXMT this needs measuring, not assuming.

## What cannot be excluded, and what to do instead

Grass and other thin geometry look worst under this effect, and there is no way
to exclude them by identity: the pass sees a depth buffer and a G-buffer, and
neither records what drew a pixel. `shadow_contact_upright` gates the
*receiver* on the world normal, which keeps the effect off grass blades, but a
grass blade and a character's arm are both vertical, so it costs object
self-shadowing at the same time. It is a blunt instrument, off by default.

The levers that do not trade against self-shadowing:

- **Fractional occlusion and the thickness falloff**, both above. Between them
  they replace two binary decisions that turned thin geometry into speckle.
- **A shorter `shadow_contact_length`.** Long marches are what turn grass into
  noise; contact shadows want to be short. Object self-shadowing at a contact
  point is short-range anyway, so shortening costs it little.

If a real discriminator is ever needed, the only remaining candidate is the
G-buffer's specular and gloss at `t1`, which the receiver already samples. That
would need a runtime probe of those channels over grass and over a character
before anything is built on it; the archive is not conclusive, because the
material shaders it holds write render-target sets that do not match what the
deferred pass reads.

## Abort criteria

Stop and revert if any of these hold:

- the effect costs more than ~1.5 ms at the target resolution;
- streaking at screen edges is visible in normal play;
- acne or self-occlusion appears that bias tuning does not fix;
- it is simply not visible at this camera angle -- a top-down view with a high
  sun gives the march little screen-space travel, and that is a legitimate
  reason to abandon it.

## Hard-won constraints -- do not relearn these

- **Never call `RenderDirectional` a second time.** It mutates region and
  entity state. See `findings.md`.
- **Do not target the per-material receivers.** They never carry the
  directional map at runtime. Any transform aimed at them is invisible.
- **`D3DDisassemble` rejects the transformed containers** because the checksum
  is zeroed rather than recomputed. Expected; not a defect.
- **Verify a transform's match count against the corpus** before trusting it.
  The `+/-0.5` tap shape alone matches 65 shaders, including the point-light
  receiver; the structural gate narrows it to 1.
- **If you ever add a shader output**, the second byte of an `OSGN` mask is the
  components *never written* -- the inverse of the `ISGN` meaning. Getting it
  wrong silently disables the output. Not applicable here (pixel stage, no new
  outputs), but the trap is cheap to fall into.
- **Measure before concluding.** In the session that produced this file, three
  confident diagnoses were overturned by a log line: the near cascade's fit was
  not invariant, a music bug was not caused by the shadow code, and the PCF
  enhancement had never applied to the shader that draws shadows.

## DXBC encoding notes

Enough to build the instructions without re-deriving the format.

- Instruction word 0: opcode in bits 0-10, length in bits 24-30.
- Destination temp, single component: `0x00100000 | mask`, then the register
  index. Masks: `.x 0x12`, `.y 0x22`, `.z 0x42`, `.w 0x82`, `.xyz 0x72`.
- Source temp, one component broadcast: `0x00100000 | sel`, then the index.
  Selectors: `.x 0x0a`, `.y 0x1a`, `.z 0x2a`, `.w 0x3a`.
- Source temp, full swizzle: low byte `0x?6` with the four 2-bit selectors in
  bits 4-11; `.xyzw` is `0x00100e46`.
- Constant buffer source `.xyzw`: `0x00208e46`, then buffer index, then
  register index.
- Immediate scalar: `0x00004001` followed by the bit pattern; immediate vector:
  `0x00004002` followed by four.
- Opcodes used here: `add 0`, `div 14`, `dp4 17`, `mad 50`, `mov 54`,
  `min 51`, `mul 56`, `lt 49`, `movc 55`, `sample 69`.
- Declarations: constant buffer `0x04000059, 0x00208e46, <slot>, <count>`;
  resource texture2d `0x04001858, 0x00107000, <slot>, 0x00005555`.
- Remember to raise `dcl_temps` for every new temporary.
