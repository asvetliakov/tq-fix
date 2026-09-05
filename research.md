# Stutter investigation: findings and fix conclusion

This synthesis includes the accepted old-route Runs 84–85 and the later
alternate-route mesh residency investigation and gameplay validation. The latter
reduced a matched transition from 323 to 79 ms, with main-thread resource loading
falling from 212 to 29 ms. Remaining particle, native draw and update costs are
separate unresolved work; this is not a claim of universally smooth play.

Read [streaming findings §112](research/streaming/findings.md#112-alternate-route-residency-loss-and-bounded-mesh-preload-refresh)
and the [gameplay loading analysis](research/streaming/gameplay-loading-hitches.md)
for the current evidence. Older prepared-run notes in §111 and earlier sections
are historical. The [disassembly target index](research/streaming/disassembly-targets.md)
and [mesh lifecycle audit](research/streaming/mesh-preload-lifecycle.md) record
native identities, calling conventions and hook ownership.

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

Four actionable mechanisms emerged:

- **Directional-shadow cold resources.** The regular caster path can block
  on a root mesh merely to read its shadow-pass count, then on an alpha-tested
  base texture to construct a shadow pass. Material setup can additionally
  load textures the active shadow shader never uses. The Actor pose path is
  another, earlier root-mesh dependency—not the whole shadow problem. The
  separate interception points and their fixes are explained below. These
  are reached DX11 paths, not merely leftover DX9 interfaces.
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

- **Loss of preloaded scenery residency.** On the alternate route, 68 of 71
  blocking resource calls reloaded previously preloaded, evicted meshes/materials.
  Native idle eviction and a 200-frame cooldown defeated renewed preload
  requests; shadow deferral and draw admission do not budget normal colour
  resource loading. Advancing a stale resident root's normal Actor preload,
  within an eight-visit-per-frame bound, prevents much of that idle loss.
  Memory-pressure eviction and already-cold roots retain stock behavior.

No complete shadow, frustum-culling, or resource-loader rewrite was required
to address the observed route. That does not establish that those systems
have no other limitations.

## Directional-shadow resources: regular casters and the earlier Actor path

`shadow_defer_cold_resources` handles the regular caster/material path,
independently of the additional Actor pose option:

- **Root mesh / pass count.** The shadow renderer asks
  `GraphicsMeshInstance::GetNumShadowRenderPasses()`, which calls
  `Resource::EnsureAvailable(mesh)` before returning the mesh's pass count.
  The hook checks residency first, queues an unloaded root through the stock
  loader, and returns zero shadow passes while the root is cold. This covers
  both opaque and alpha-tested exact mesh-instance casters; it does not
  require reaching `Actor::UpdateMeshInstance`. Resident roots resume the
  normal pass-count path.
- **Alpha-tested base texture / shadow record.** A resident mesh can still
  have a cold base texture needed for its cutout shadow. At the verified
  shadow-record construction call, the fix checks the exact mesh-instance
  class and shadow style, preserves caster eligibility, queues the unloaded
  texture, and omits that caster/pass while the texture is cold. The cutout
  shadow returns when resident; it is not replaced with solid foliage.
- **Material textures / shader inputs.**
  `GraphicsMesh::SetShaderParameters -> GraphicsTexture::GetTexture` can
  ensure a texture before the subsequent setter discovers that the shadow
  shader has no corresponding parameter. The fix checks the resident active
  shader first and skips unused inputs, including the separate instance bump
  ensure. It also skips the generic base texture when a verified non-null
  instance override immediately replaces its binding. An opaque caster with
  a resident root can still cast without loading irrelevant textures; shader
  inputs actually required by the pass retain their stock behavior.

`shadow_defer_cold_actor_pose` adds the **earlier Actor boundary**. During
directional gathering, `Actor::AddToScene -> Actor::UpdateMeshInstance ->
GraphicsMeshInstance::UpdatePose -> Resource::EnsureAvailable` can load the
root before the regular pass-count gate sees it cold. This fix queues the
root and defers that pose update only when loading/queuing is confirmed;
otherwise it calls stock. `AddToScene` still submits the renderable, and the
regular root gate above omits its shadow while cold. A resident root takes
the stock pose update. This is why the Actor option requires the complete
regular cold-resource patch set rather than replacing it (§81–§84).

These resource changes are scoped to the verified main-thread directional-
shadow context. They do not defer normal colour rendering, point shadows,
or arbitrary renderable classes. Cold-resource omission also remains distinct
from the later shared secondary-draw budget: one avoids specific synchronous
dependencies; the other spreads first drawing of otherwise eligible objects.
Exact sites for all four paths are in the
[disassembly target index](research/streaming/disassembly-targets.md).

## Accepted implementation

| Default | Behavior and boundary |
| --- | --- |
| `shadow_defer_cold_resources=1` | Queue unloaded exact mesh-instance shadow dependencies; omit cold root casters and cold alpha-base passes until resident. Skip material inputs absent from the active shadow shader and the verified redundant base-texture binding. |
| `shadow_defer_cold_actor_pose=1` | Move the root decision before directional Actor pose work. Skip only when queuing/loading is confirmed; an unconfirmed enqueue falls back to stock. Implies the complete cold-resource patch set. |
| `mesh_preload_refresh=1` | At an existing dependency-aware Actor preload visit, advance the countdown for a root untouched for at least 400 Engine frames if resident or evicted with an expired nonzero deadline and no queue entry. Share eight accelerations per frame. Remove the requeue cooldown only from automatic mesh/texture age eviction; preserve native eviction ages, budgets and loading. First combined validation: 255 → 177 → 55 ms, with no evicted-resource demand in the latest transition window. |
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

The conclusion is **keep the combined, targeted fixes**: defer cold root
meshes at regular shadow-pass counting, defer cold alpha-base textures at
shadow-record construction, avoid unused/redundant shadow material textures,
and catch the additional root-mesh dependency before Actor pose work. Queue
terrain layers earlier through stock preload, then spread new reflection and
directional-shadow draws with the shared admission budget. The regular shadow
resource fixes are not limited to Actors, and the Actor hook does not replace
them. The separate progressive uploader spreads eligible loose-file texture
bytes; it is not an archive loader or a substitute for secondary admission.

Together, the accepted changes made the old-route **play** hitch no longer
noticeable in Runs 84–85 without reducing shadow distance or resolution, or
rewriting the renderer. This supports the combination, not a claim that every
component independently eliminates the hitch. It is not “all stutter is
fixed”: save loading, first-world-frame cost, other routes, long-session
identity/residency behavior, and native-Windows results are not established
by those two route confirmations. The later alternate-route validation below
addresses one additional residency failure.

## Alternate-route residency validation

The [complete lifecycle capture](research/streaming/gameplay-loading-hitches.md)
resolved the missing-preload versus eviction question: successful background
loads were followed by idle eviction before visible use. The refresh build
performed 57 accelerated visits and had no budget deferrals in its first matched
run. Resource history and marker output had no recorded loss.

| Measurement | Before | Refresh |
| --- | ---: | ---: |
| Matched transition frame | 323.015 ms | 79.485 ms |
| Main-thread resource loading | 211.727 ms / 71 calls | 29.480 ms / 12 calls |
| Resource loading across ±120 frames | 255.215 ms | 42.494 ms |
| Gameplay median / p95 | 19.860 / 25.531 ms | 19.624 / 23.985 ms |

The wider window supports reducing synchronous reloads rather than just moving
them a few frames earlier. One instrumented route repetition per configuration
does not prove zero cost or a general speedup. The shipping refresh path has no
Present work or lock; native dependency traversal can run earlier, and current
preload interest can retain resources longer within unchanged budget rules.
`mesh_preload_refresh=0` restores the original Actor preload timing and automatic
mesh/texture eviction cooldowns. A later 255 ms repeat identified 63 previously
worker-loaded resources still under cooldown after eviction, costing 144 ms to
reload on demand. The follow-up changes only the two native idle-eviction
cooldown arguments; see the [latest lifecycle evidence and candidate](research/streaming/residual-gameplay-hitches.md#batched-text-repeat-and-idle-requeue-candidate).

Subsequent cooldown and countdown fixes reduced the repeated transition from
255 to 177 to 55 ms. In the latest capture, the ±120-frame window contains no
synchronous demand for previously evicted resources; its main-thread loading
falls from 202 to 118 to 35 ms. The user tentatively reported no felt hitch.
The remaining transition is mostly first-use particle loading, with 12 ms of
native draw time. A separate earlier 204 ms frame is dominated by Engine update
and remains unexplained. This validates the scenery mechanism for this run,
not universal hitch-free gameplay. See the [combined validation](research/streaming/residual-gameplay-hitches.md#combined-recovery-validation-scenery-reloads-absent).

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
`mesh_preload.cpp`, `secondary_admission.cpp`, and `archive_hooks.cpp`, coordinated by
`engine_hooks.cpp`. `engine_probe.cpp`, `resource_trace.cpp`, and `probe.cpp`
provide optional observers/recording. With `performance_trace=0`, fixes still install, tracing-
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
Those counts describe the historical refactor audit, not the full current
verification inventory. The mesh refresh additionally passes native Actor/Entity
execution, call-patch/entry-observer coexistence, bounded admission and restoration
tests. Its matched gameplay validation used full tracing; normal trace-off
performance must not be inferred from mocks alone. The opt-in lifecycle recorder
has separate rebased-signature and history tests, and concurrent logger tests
verify output beyond the former 64 KiB session limit.
