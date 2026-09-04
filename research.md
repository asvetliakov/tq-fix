# Stutter investigation: findings and fix conclusion

This is a current synthesis of the streaming investigation, not a new CSV
analysis. Measured acceptance comes from Runs 84–85; subsequent defaults and
trace-off refactoring are recorded through findings §111 and commit `79ef3ab`.
Run 87 is prepared/installed but its in-game confirmation remains pending.

The detailed record is [streaming findings](research/streaming/findings.md).
Sections are corrected forward: read §111–§108 before treating earlier
conclusions as current. The [handoff](docs/plans/handoff.md) records the active
state; the [disassembly target index](research/streaming/disassembly-targets.md)
records exact identities, call chains, byte contracts, and implementation owners.

## What was wrong

The player-visible problem was a pause or burst of long frames while walking
into a new area on the normal route. The fix is not simply “make shadows
cheaper”: queue known dependencies earlier, avoid blocking on cold resources
solely for secondary visibility, and spread first secondary draws across
presented frames instead of admitting the whole population together.

The investigated class is the repeated **in-play scene-transition burst**, not
every long frame in the session. The strongest supported explanation is
unbudgeted first use: a newly encountered scene population can demand CPU
resource realization and substantial GPU work together. The pass with the
longest observed wait is not necessarily the producer. This is an engine-side
mechanism compatible with the user's report of unmodded native-Windows
stutters, not evidence that Wine or a host round trip is the cause. The recorded
experiments were on CrossOver/DXMT, not a native-Windows validation campaign.

Three actionable mechanisms emerged:

- **Directional-shadow cold resources.** The verified DX11 gather reaches
  `Actor::AddToScene -> Actor::UpdateMeshInstance ->
  GraphicsMeshInstance::UpdatePose -> Resource::EnsureAvailable` before the
  later caster eligibility check. Separately, mesh shadow-pass counting
  ensures the root mesh, and material setup can ensure textures that the
  active shadow shader will never consume. These are reached DX11 paths,
  not merely leftover DX9 interfaces (§81–§84 and earlier shadow findings).
- **Runtime terrain's missing layer preload.**
  `TerrainRT::LoadRenderData -> TerrainType::LoadTextures` creates layer
  Resources, but the runtime owner's preload does not queue those layer
  textures. Their first colour use can therefore become a synchronous load.
  Calling the stock semantic preload at creation addresses that omission;
  it does not guarantee permanent residency (§77–§79).
- **First-use secondary rendering and queue pressure.** After those CPU
  dependencies were reduced, reflection and directional-shadow work could
  still arrive in bursts. Later D3D submissions exposed the resulting wait.
  Identity tracing and the successful progressive-admission experiments
  support spreading first secondary GPU participation. Raw new-object counts
  are not a cost model: other large populations were cheap. Nor have these
  experiments isolated a single driver-internal cause such as shader
  compilation, residency management, or a particular native-Windows stall
  (§105 corrected by §106–§108).

No complete shadow, frustum-culling, or resource-loader rewrite was required
to address the observed route. That does not establish that those systems
have no other limitations.

## Accepted implementation

| Default | Behavior and boundary |
| --- | --- |
| `shadow_defer_cold_resources=1` | Queue unloaded exact mesh-instance shadow dependencies; omit cold root casters and cold alpha-base passes until resident. Skip material inputs absent from the active shadow shader and the verified redundant base-texture binding. |
| `shadow_defer_cold_actor_pose=1` | Move the root decision before directional Actor pose work. Skip only when queuing/loading is confirmed; an unconfirmed enqueue falls back to stock. Implies the complete cold-resource patch set. |
| `terrain_preload_layers=1` | Call the game's `TerrainType::PreLoad(true)` immediately after runtime layer `LoadTextures`, using its normal background loaders. |
| `secondary_pass_admission_budget=8` | Share eight newly admitted renderable identities per presented frame across exact reflection and directional-shadow contexts. Pending objects keep Resource/material preparation but suppress their secondary `Draw`/`DrawIndexed` calls until admitted. |
| `streaming=optimized` | For eligible mapped loose-file textures, show a low-mip view while uploading withheld high-mip bytes in frame-paced chunks. This budgets texture transfer, not object admission or archive reads. |
| `archive_cache_mb=8` | Reuse decompressed archive blocks where available. A useful but small separate benefit, not the explanation or cure for the remaining GPU burst. |
| `loose_texture_max=4096` | Prefer the archive copy over oversized loose texture-pack assets; retain 4K-and-smaller loose assets. This is a separate texture-pack guard, not the admission fix. |

The admission budget counts **objects, not buffers, draws, bytes, or
milliseconds**. An identity admitted through reflection costs no second slot
in directional shadow. The ninth unseen identity proves a backlog and waits;
the next presented frame has a fresh budget. No reflection, 32-buffer
threshold, or region-pointer change is required. Already admitted identities
remain admitted; this is not a general recurring resource-residency budget.
The bounded identity table fails open to stock rendering on overflow.

Normal colour drawing is not deferred. The intentional trade is temporary
absence of pending local shadows/reflections, not missing visible geometry.
Cold casters return when resident; pending admission retries on subsequent
frames. This preserves `shadow_split` and shadow-map resolution. No fixed
maximum number of delayed frames is promised for arbitrary populations.

## Progressive uploader: a separate problem and fix

**Problem.** The stock texture-creation path already has the mip bytes in
CPU memory and passes them together to `CreateTexture2D`. A large initial
upload concentrates work on the calling thread and GPU queue. This is distinct
from reading/decompressing the file, queuing a terrain Resource, or admitting
an object's first reflection/shadow draw. The uploader is an earlier mitigation;
the later accepted route results do not isolate its individual contribution.

**Fix.** The implementation in [upload.cpp](src/upload.cpp) and its
[visual integration](src/visual.cpp) does the following:

1. Accept only the verified renderer call and mapped loose-file owner, with
   supported BC1/BC2/BC3 format, a single default-usage shader-resource texture,
   a supported mip count, and at least 2 MiB of top-level payload.
2. Create the full-size texture with the small mip tail populated. Substitute
   a shader-resource view starting at the first mip at or below 512 pixels
   on both axes, or the last available mip. High-detail sampling waits until
   the retained data has been uploaded; the visible trade is temporary
   softness, not a permanent resolution reduction.
3. Before each Present, send one block-row-aligned chunk of one pending job
   through `UpdateSubresource` on the render thread. Once all withheld mip
   levels are uploaded, stop substituting the low-detail view.
4. Hold the original loose-file mapping with a reference-counted lease until
   both the engine's use and all associated jobs permit release. Multiple
   sub-textures from one container share that lifetime. Completed sealed
   mappings normally go to the unmap worker; a full/unavailable worker queue
   falls back to immediate unmapping instead of leaking address space.

**Budget correction.** The README formerly described a 512 KiB–2 MiB budget
and “mapped archive data.” The actual adaptive range is 256 KiB–2 MiB, targeting
approximately 3 ms of CPU time inside each `UpdateSubresource` call. Smaller
jobs have a 1 MiB adjustment ceiling; the initial chunk is chosen from the
shared recent-rate estimate. Chunks are limited by remaining block rows, so a
final chunk can be smaller than the controller's floor. This is neither a
per-texture completion deadline nor a guaranteed maximum frame/GPU cost.
The feedback responds to actual source-page and submission waits but cannot
prevent the first unexpected slow step.

The uploader's clock is its own `QueryPerformanceCounter` callback, not the
optional probe clock. It must run with tracing off because it controls the
upload size. Earlier coarse tick timing could report short chunks as zero
and incorrectly increase the budget; high-resolution feedback and a
history-based initial chunk address that controller problem. Trace-off means
no recording work, not disabling measurements needed by gameplay algorithms.

**Scope and limits.** Owner validation accepts `FileDirectory` mapped loose
files, not the archive `File` class. Decompressed `.arc` payloads have a
different lifetime and still use stock texture creation. Supporting them
would require bounded staging/ownership and readiness handling; retaining a
loose-file mapping is not a drop-in archive solution. See findings §5's
in-place correction and the later discussion in §85, read with §105–§108.
No archive staging feature is claimed here. Full-size GPU allocation and
upstream file work are not eliminated, and pending leases still consume
32-bit address space. Unsupported candidates or failed job/lease acquisition
fall back to stock. `streaming=original` disables this path.

Thus the two progressive fixes are complementary: the uploader spreads
**texture bytes**, potentially changing visible texture detail temporarily;
secondary admission spreads **new objects' reflection/shadow draws**, leaving
their ordinary Resource/material preparation and colour draws in place.
Terrain preload moves the **queue request earlier**. None alone guarantees
that every remaining source of an in-play or loading pause is removed.

## What the accepted runs establish

These numbers describe the exact old-location **play transition**, not the
menu, load-game frame, loading screen, or first world frame. F12 in both
accepted runs was deliberately pressed at the old location **without a felt
hitch**, not as a reaction to one. A reaction marker in earlier runs requires
looking backward; the nearest frame or global CSV maximum is not automatically
the event the user experienced.

- **Run 84, play frame 6764:** 38.229 ms CPU / 31.758 ms GPU. Thirty-nine new
  secondary identities were admitted over five frames with no one-frame
  rebound. Relative to 58 same-run, collision-active full-scene play frames
  under 60 ms in the 1,300–1,699 indexed-draw band, the transition surcharge
  was 17.548 ms CPU / 11.112 ms GPU (§106).
- **Run 85, play frame 6490:** 40.117 ms CPU / 40.780 ms GPU. Relative to 90
  same-run, collision-active full-scene play frames under 60 ms in the
  1,200–1,699 indexed-draw band, the surcharge was 19.633 ms CPU / 20.309 ms
  GPU. The next two play frames were 32.247 and 20.943 ms CPU, with no large
  postponed rebound. The user again reported no noticeable old hitch (§108).

These are controlled within-run references, not across-run medians. Run 85
also respected the shared eight-identity limit with zero identity-table
overflow. A separate unmarked play population was handled without either old
transition proxy; it is supporting mechanism evidence, not another claimed
felt event. Nested CPU intervals and queued GPU intervals must not be summed
as independent costs.

The conclusion is **keep the measured fixes**. It is not “all stutter is
fixed”: save loading, first-world-frame cost, other routes, long-session
identity/residency behavior, and native-Windows results are not established
by these two route confirmations.

## Rejected explanations and experiments

“The host” and “the message pump” are not accepted root-cause conclusions.
The pump experiments failed; their attribution was withdrawn. Inline sent-
message dispatch is a possible source of a slow `PeekMessage`, not proof that
it caused the felt transition. The earlier identical-run stall-rate variation
has not been explained by the successful secondary-admission result.

Larger archive caches, bounded archive prefetch, buffer pooling, lock
contention, sleep granularity, async level loading, and libdeflate did not
provide the required solution for their measured classes. Whole-shadow reuse
introduced flicker and postponed work. Mesh-only and whole-reflection
omissions in Runs 81–82 moved the work without improving the felt hitch;
they are not additions to the accepted shared admission budget. Lower shadow
resolution was rejected on quality grounds. Historical findings preserve the
individual tests and corrections; this summary does not reopen them.

## Trace-off operation and validation

Performance behavior now lives in `shadow_defer.cpp`, `terrain_preload.cpp`,
`secondary_admission.cpp`, and `archive_hooks.cpp`, coordinated by
`engine_hooks.cpp`. `engine_probe.cpp` and `probe.cpp` provide optional
observers/recording. With `performance_trace=0`, fixes still install, tracing-
only hooks do not, and shared behavior wrappers bypass observers. Small
cached-flag checks and the hooks/state required by gameplay fixes remain;
this is not a claim of literally zero extra instructions. One DLL supports
both normal operation and full tracing.

The refactor corrected unconditional clock reads and unnecessary observer
entry while disabled, including the obsolete reflection `BuildScene` observer.
It claims no measured frame-time gain from those removals. Doctor, build, and
self-test passed, including GPU timestamp retirement and actual shared-wrapper
tests with mock stock callees. Verification passes 865 checks; mutation audits
rejected 362 scalar/window perturbations and 22 trace-off gate regressions.
Off-game mocks do not replace game validation. Run 87 keeps the normal live
settings with both traces off; its subjective regression check remains the
next step, with no CSV or F12 record expected.
