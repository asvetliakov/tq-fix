# Screen-space contact shadow review — 2026-09-05

Merged `origin/main` at `4364801` into `screen-space-shadows` in `85a2cf4`.
The merge retains main's renderer submission hooks, independent skinning hook,
grass path, device recreation cleanup, and frame tracing. Contact shadows do
not restore the old context Draw/DrawIndexed vtable hooks.

## Corrections

- **GPU backlog:** the old two-slot scheme overwrote pending copies and forced
  a blocking Map after four refusals. Four pending slots are now polled with
  `DO_NOT_WAIT` before reusing completed slots. A full ring skips capture.
  Results older than eight frames are discarded; old results cannot replace a
  newer upload. Expired history uploads zero strength until fresh data arrives.
- **Projection validity:** reject nonfinite/singular matrices and inverse-W
  projections with X/Y terms or no perspective depth term. Copy the 192-byte prefix used by the shader from any source allocation
  at least that large. The live game binds a 2048-byte dynamic allocation. Resize invalidates both
  uploaded history and pending captures without overwriting in-flight buffers.
- **Device/state lifetime:** b13 is initialized before shader patching becomes
  eligible, saved before each marched draw and restored afterward. Failed
  resource creation leaves the shader original. Receiver identity is private
  data owned by the shader, replacing four unretained pointer slots. Original
  fallback shaders are marked as such and incur no readback. Tagging failure
  falls back to original bytecode. Other devices do not receive the march.
- **Shader work:** skip the ray at zero strength, fully shadowed pixels,
  invalid homogeneous depth, and normals below the configured threshold.
  Reject ray points outside XY/Z clip bounds or behind the camera. Samples use
  explicit LOD zero inside divergent branches. No additional render targets,
  fullscreen draw, normal decoding, temporal accumulation, or shader worker is
  added. The existing PCF retune still follows the contact transform.
- **Tracing attribution:** contact Maps/Unmaps call the native entry points,
  avoiding game Map timers and grass tracking. Readback CPU work has one
  inclusive phase, so it is not double-counted as nested mod phases.
- **Test query race:** the existing timestamp capability test now waits for its
  two timestamps as well as its disjoint query; these retire independently on
  DXMT. Its timeout remains bounded and missing timestamps still fail.

The projection algebra still uses the current pixel's own NDC/homogeneous W as
its ray origin. Camera translation cancels from VP times the light direction;
zoom, camera orientation and light direction changes can have readback latency.
The audited renderer applies this receiver in one fullscreen sunlight pass.
The once-per-frame capture assumes that camera; a different renderer using the
same shader for several cameras would require separate histories. Receiver draw
counts make unexpected multiple invocations visible.

## Initial review defaults (superseded)

The table below records the initial conservative review profile. After gameplay
comparison the user approved enabled-by-default contact shadows with 12 steps,
length 0.35, bias 0.012, thickness 0.30, strength 0.70 and upright 0.0. See
[the current defaults and approval](contact-tuning.md).

| Setting | Default | Reason |
| --- | --- | --- |
| `shadow_contact` | `off` | Added cost and appearance still need gameplay A/B validation. |
| `shadow_contact_steps` | `8` | Sampling density for a short ray at normal zoom without the 16-tap maximum. |
| `shadow_contact_length` | `0.20` | Shorter than the old 0.25; concentrates on local grounding and limits grass streaks. |
| `shadow_contact_bias` | `0.02` | Retains the prior world-depth self-occlusion slack. |
| `shadow_contact_thickness` | `0.15` | Much narrower than the old 0.5; limits unrelated foreground depth being treated as contact. |
| `shadow_contact_strength` | `0.5` | Moderate darkening instead of the previous full-strength maximum. |
| `shadow_contact_upright` | `0.5` | Upward-facing receivers suit the fixed elevated camera; vertical surfaces are excluded. |

The normal threshold is a geometric filter, not a material classifier: grass
can still cast onto ground. Short rays can miss small objects between taps,
hidden/off-screen geometry cannot cast, and there is no temporal denoiser.
Averaged hit weights and thickness falloff reduce abrupt changes but do not
eliminate shimmer. These defaults are reasoned tuning choices based on the
captured TQ projection/depth footprint in `screen-space-plan.md`, not a claim of
measured visual superiority. Normal play should remain zoomed out during A/B.

Eight taps at 5120×1440 allow up to 58,982,400 added depth fetches per frame,
plus one normal fetch per eligible pixel. Branches reduce executed work in
rejected areas but also add instructions; only a real run determines net GPU
cost. Existing native PCF and sunlight work remain in the receiver.

## Performance trace

Both `performance_trace=1` and `full` enable instrumentation even with the
effect off. Full mode retains every row; hitch mode retains selected rows.
No separate contact timestamp ring is created while shared tracing is active.
The legacy `shadow_contact_timing=1` log summary still works independently when
`trace=1`; it does not duplicate the shared GPU queries.

- `contact_refresh_ms`: CPU polling, matrix validation/inversion, parameter
  Map/Unmap and capture submission. Does not include the game's Draw call.
- `gpu_contact_receiver_ms`: the entire native receiver plus any contact
  shading. Repeated invocations span first entry to last exit, gaps included;
  this is neither a sum of draws nor isolated contact-only GPU time.
- `contact_receiver_draw`, `contact_marched_draw`, `contact_active_draw`:
  identify baseline, transformed code, and draws with usable nonzero strength.
  Active does not mean every pixel passed the shader's normal/depth gates.
- `contact_readback_copy`, `_poll`, `_ready`, `_busy`: copies submitted,
  attempts to read, usable uploads accepted, and nonblocking refusals.
- `contact_ring_full`, `contact_invalid`, `contact_neutral`: capture backpressure,
  invalid source/map/projection/upload, and frames without usable parameters.
- `contact_history_age`: age of the accepted capture, recorded once per
  refreshed frame; zero with neutral parameters. Pair it with `contact_neutral`.
- `# shadow_contact=...`: configured settings in each CSV header.

For the GPU delta, restart with `shadow_contact=off` and then `on`, holding
resolution, other options, scene, camera and route constant. Compare settled
receiver and frame distributions. Ctrl+Shift+C with
`shadow_contact_toggle=1` changes appearance and skips the ray when off, but
retains readback/parameter overhead and the modified shader. It cannot replace
the restarted baseline. Keep the effect disabled if its benefit is invisible
or grass/edge artifacts remain; the original experiment's ~1.5 ms incremental
GPU budget is a validation criterion, not a measured result of this review.

## Verification

- Release and diagnostic builds, x86 architecture and exact 200-export manifest validation.
- Complete CrossOver/DXMT off-game suite, including production device recreation,
  skinning, grass, streaming, HDR, shader composition and tracing checks.
- Full extracted shader corpus: contact and deferred transforms each match
  exactly one of 453 programs (`95f0a104c855...`). The recorded earlier 367
  count described only the shadow subset.
- New real-device runtime fixture: neutral startup, bounded busy ring, freshness
  expiry/recovery, invalid projection, resize invalidation, b13 restoration,
  original receiver baseline, shutdown and reinstallation.
- New actual pixel draws: contact darkening and its strength bound, exact
  zero-strength identity, rejected normals, off-screen rays, behind-camera rays,
  and bias rejection. These execute the emitted DXBC, beyond shader creation.
- CSV fixture checks CPU/GPU columns, configuration metadata and nonzero
  backlog/invalid/neutral counters.

Build/test logs are local under `build/contact-review-*.log`. Gameplay appearance
and incremental GPU cost are not established by these synthetic tests.

## D3D references

[Explicit-LOD sample semantics](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sample-l--sm4---asm-)
explain why the added conditional samples do not depend on derivatives.
[Microsoft's tokenized shader format](https://github.com/microsoft/DirectXShaderCompiler/blob/main/include/dxc/Support/d3d12TokenizedProgramFormat.hpp)
defines the emitted IF/ENDIF and SAMPLE_L instruction tokens.

## First gameplay follow-up: allocated size versus shader layout

The first installed review build (`ff8ab1e`) was loaded and received the user's
Ctrl+Shift+C presses, but its exact-size readback check rejected the game's
2048-byte dynamic constant buffer. Across 4,635 recorded frames there were
3,133 marched receiver draws, 3,133 invalid/neutral refreshes, zero copies,
zero usable uploads, and zero active draws. Thus the invisible toggle was an
implementation regression, not evidence that the effect was too subtle.

The fix restores prefix-region copying for allocations at least 192 bytes long
and logs undersized allocations explicitly. The real-device fixture now starts
with the captured 2048-byte dynamic allocation size, puts nonzero data in the
unused tail, verifies usable uploaded parameters, and separately checks that
176-byte sources are rejected and 192-byte sources remain supported. The prior
fixture used only a 192-byte allocation, which missed this distinction.
