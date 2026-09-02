# Plan: screen-space contact shadows

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

## Algorithm

For each pixel, after the native shadow term is final:

1. `clipP = VP * P` (four DP4s).
2. For `i` in `1..N`: `clip = clipP + (i * step) * clipL`.
3. `uv = clip.xy / clip.w * (0.5, -0.5) + 0.5`; `z = clip.z / clip.w`.
4. `d = sample(t2, s2, uv).x` -- the depth buffer stores NDC z directly, which
   is why the receiver can feed it straight into the inverse VP.
5. Occluded when `d < z - depthBias` **and** `z - d < maxThickness`. The
   thickness test is what stops distant geometry from casting through.
6. Any occluded step sets the contact term to `1 - strength`; otherwise 1.
   Accumulate with `min`, or fade by step index for a softer edge.
7. `min r0.w, r0.w, contact`.

Sampling outside `[0,1]` UV must not occlude -- clamp or test and skip, or
off-screen geometry will streak.

## Configuration

Under `[graphics]`, read in `src/shadow_fix.cpp` beside the existing dials:

    shadow_contact          = on | off      (default off until validated)
    shadow_contact_steps    = 8             (4..16)
    shadow_contact_length   = 0.5           world units, total march distance
    shadow_contact_bias     = 0.0005        NDC z
    shadow_contact_thickness= 0.01          NDC z
    shadow_contact_strength = 1.0           0..1

Ship it **off by default** until it has been judged in motion.

## Phases

1. **CPU side.** Reserve and bind `b13`; obtain VP via option A; upload the
   matrix, `VP*L`, and parameters. Verify with a run that the values are sane
   (log the matrix, check translation and that `VP * P` reproduces the pixel's
   own NDC for a known point).
2. **Transform.** Extend `tuneDeferredShadowFilter`, or add a sibling that
   composes after it, to append the march. Keep it purely additive: insert
   instructions, rewrite nothing, exactly as the cascade transform did.
3. **Offline validation.** Transform must match exactly 1 of 367 corpus
   shaders; DXMT must create the result; applying twice must be refused.
4. **In-game A/B.** `shadow_contact=off` vs `on`, same scene, same camera.
5. **Cost.** Use the GPU timestamp queries. 8 steps at 5120x1440 is ~60M extra
   depth samples per frame; through DXMT this needs measuring, not assuming.

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
