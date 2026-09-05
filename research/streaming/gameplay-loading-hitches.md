# Gameplay loading hitches on the alternate route

## Scope

Investigation after commit `896db79`. Exclude initial world-loading frames,
their immediate warmup, and the first world frames following device recreation.
The two events below are established gameplay, before any VSync change. No
runtime behavior or diagnostic build was changed for this investigation.

Sources are the archived `tqflicker-frames.csv`, `tqflicker-debug.log` and INI in
`cache/runs/recreation-vsync-off-on` and
`cache/runs/recreation-different-route-repeat`. Both use budget eight, cold
shadow/pose deferral and terrain-layer preload. Installation succeeds.

## Repeated event

| Measurement | VSync test, frame 3316 | Previous repeat, frame 4109 |
|---|---:|---:|
| Time from capture start | 45.200 s | 51.123 s |
| Whole frame | 376.304 ms | 232.639 ms |
| Main-thread resource loads | 69 / 174.246 ms | 56 / 114.131 ms |
| Texture resource loads | 44 / 154.258 ms | 34 / 97.065 ms |
| First deferred owner, geometry setup resource loads | 41 / 124.049 ms | 34 / 96.205 ms |
| Second deferred owner, geometry scene native draw time | 171.592 ms | 92.086 ms |
| Same scene GPU timestamp interval | 7.450 ms | 5.162 ms |
| New secondary identities admitted | 0 | 1 |
| Secondary draws suppressed | 0 | 0 |
| Grass draws / crossed draws | 172 / 172 | 172 / 172 |
| Newly adopted grass streams | 0 | 0 |
| Grass crossing CPU time | 0.153 ms | 0.175 ms |
| Native Present | 0.051 ms | 0.029 ms |

In both runs the preceding frame has one deferred owner; the event and following
frame have two. The event first observes five terrain plugs, two terrain blocks
and 184/185 mesh identities in the second owner's geometry-setup consumer.
These are first observations in that consumer, not proof of newly allocated
objects or resources. The previous frames already have 172 crossed grass draws.
The workload signature strongly suggests the same portal/region visibility
transition. The static renderer flow in `disassembly-targets.md` confirms that
owner invocations represent recursive region branches, not fixed pipeline stages.
The historical label "geometry setup" includes execution of a sorted scene list.

Resource loading in the first owner and later native draw blocking are distinct
CPU costs. The latter could be a downstream driver/queue wait; the small GPU
interval of that later scene does not identify the earlier producer by itself.
Likewise the first owner's long GPU timestamp interval can include gaps while
the CPU loads assets. Do not add CPU and GPU intervals or interpret either as
proof of a particular GPU operation.

## Why the existing controls do not prevent it

* `shadow_defer.cpp` gates cold actor roots, root meshes and alpha-base textures
  only inside the main-thread directional-shadow bracket. Colour rendering
  remains stock. All resource loads in these two events are outside that bracket.
* `secondary_admission.cpp::shouldDeferSecondaryAdmission` applies only to
  reflection and directional-shadow contexts. Its scopes still execute normal
  resource/material preparation and suppress only the final renderer draws.
  Previously admitted object/kind identities remain admitted; residency loss or
  a new consumer does not by itself reset this state. This is an object budget,
  not a resource-loading or millisecond budget. Both events are below eight.
* `terrain_preload.cpp::hookTerrainRtLoadTextures` calls stock
  `TerrainType::PreLoad(true)` immediately after layer textures are created.
  It does not refresh layer residency at later runtime-owner preload visits.
  Both events have 12 `TerrainRT::PreLoad` calls visiting 118 layers, but zero
  `TerrainType::PreLoad(true)` and zero `LoadTextures` calls. The owner visits
  must not be read as 118 successful layer-resource queue requests.
* Progressive uploads retain eligible mapped loose-file payloads, not archive
  decompression output. The latest event starts 20 loose upload jobs and performs
  one 1,000 KiB step: the uploader is active but is not a global loading budget.
  It also records 49 archive reads requesting 35,155 KiB and 129.497 ms of
  aggregate inflate time across threads. This overlaps load durations. Two cache
  hits and 169 stores/evictions cannot distinguish compulsory misses from reuse
  beyond capacity, so they do not justify increasing cache size alone.

The control is demonstrably active outside initial loading: excluding the first
120 frames after grass first appears, the pre-event gameplay windows contain
21 suppressed draws in four frames in the latest run and one suppressed draw
in one frame in the repeat. Admission never exceeds eight; identity overflow
is zero. Run 85's accepted transition (frame 6490, findings section 108) instead
admits eight secondary identities and suppresses ten draws. It is a different
controlled workload, not a same-route baseline for this resource-loading burst.

## Attribution before the F12 run

The older investigation in findings section 97 already observed two terrain
textures cold at first reflected use despite a successful loading-time semantic
preload. This proves that the one-time preload is not a residency guarantee;
it does not identify the 44 textures in the current event as those same assets,
or prove that all are terrain textures. Material resources of ordinary meshes,
late requests, and resource eviction remain possibilities.

Neither current run has an F12 resource report for the event. Existing full trace
retains resource names, state before loading, caller chain, owner and terrain
preload history, plus the slowest geometry draws and their bindings. A fresh run
of this route with `trace=1`, `performance_trace=full`, `stutter_marker=1`, and
one F12 press immediately after the established-gameplay hitch should expose
that information without a new instrument. Avoid VSync changes in that run.
The detailed history covers 120 frames and resource retention is also bounded
to 128 loads, so a prompt press matters. Inspect explicit truncation and the
64 KiB session-log limit before treating missing records as zero work.

Use that attribution to choose a narrow fix: earlier/near-use resource queueing
if the identified material dependencies missed their preload opportunity, or
bounded texture realization if the dominant wait follows resource creation.
Extending draw suppression to ordinary colour rendering would require a visible
fallback and correctness work; simply lowering the secondary budget cannot
address these two events. The earlier intermittent five-second shadow stalls
remain a separate unresolved class and are not explained by this comparison.

## F12 run: scenery mesh dependencies identified

Archived under `cache/runs/gameplay-loading-f12`. The CSV contains 3,739
consecutive frames, 3,722 resolved GPU frames, nine timeouts and zero dropped
rows. First grass is frame 2074; the selected gameplay event is frame 3462,
46.023 seconds into the capture. F12 is reported at frame 3473, approximately
0.3 seconds after the hitch. The INI still has no `stutter_marker=1`, so there
is no CSV marker column, but the active Engine message-pump diagnostic emitted
the F12 resource report. This is sufficient to locate the reaction event.

Frame 3462 lasts 250.923 ms. Its 61 synchronous main-thread Resource calls
take 125.964 ms: 37 textures / 105.339 ms, 22 meshes / 19.920 ms and two
shaders / 0.705 ms. The first deferred owner's geometry setup contains
37 resource calls / 100.735 ms. Later, the second owner's geometry scene
spends 93.863 ms in native draw calls (96.014 ms whole child, 5.396 ms GPU).
Present is 0.040 ms, directional shadow 3.027 ms, and terrain ground 0.235 ms.
There are zero newly admitted secondary objects and zero suppressed draws.
The owner count again changes from one to two, with first observations of
182 meshes, five terrain plugs and two terrain blocks in the second setup
consumer. Grass remains 172/172 crossed draws; two streams are adopted and
grass crossing takes 0.203 ms.

The 64 KiB debug-log buffer fills during F12 output. It retains complete primary
records for 56 of the 61 event loads, accounting for 117.084 ms (92.9% of the
measured Resource time): all 22 meshes, 33 textures and one shader. The missing
five event records are four textures and one shader, totalling 8.880 ms.
The detailed slow-draw report has only its header, so no individual slow draw
or bound-resource attribution can be inferred from its missing lines. The
frame CSV remains complete. Parsed complete resource records are preserved in
the archive as `retained-resources.json`.

The costliest retained textures are:

| Resource | Load time |
|---|---:|
| `XPack3/underground/atlantis/necropolis tomb/necropolistomb01normal.tex` | 24.826 ms |
| `XPack4/scenery/structure/setdress/ouranosstatue01_nrm.tex` | 16.064 ms |
| `XPack4/scenery/structure/setdress/ouranosstatue01_dif.tex` | 12.898 ms |
| `XPack3/scenery/atlantis/07atlantiscity/structure/atlantis_floor02_normals.tex` | 7.020 ms |
| `XPack3/scenery/atlantis/07atlantiscity/structure/atlantis_floor01_normals.tex` | 6.827 ms |
| `SceneryGreece/Structure/Building/City/Athens Wall/AthensWall01BMP.tex` | 4.896 ms |

All 56 retained event loads enter state 0. All 33 texture records have no active
TerrainType context. The expensive group occurs in `i1/geometry/gsetup` with
`GraphicsTexture::GetTexture` return `E+0x1948ba`, mesh RenderPass material
dispatch return `E+0x1730c5`, and sorted-list dispatch return `E+0x1885b4`.
The static RenderPass call at `0x1730c2` invokes the instance's shader-parameter
virtual at vtable offset `0x38`. Raw stack scans also contain unrelated-looking
candidate addresses; those are not all asserted to be active callers.
Twenty mesh loads precede the deferred owners, including
`XPack4/scenery/structure/setdress/ouranosstatue_01.msh` (6.462 ms). Their
UpdatePose/Actor return sites match the known cold-root dependency, now outside
the directional-shadow bracket. The other two meshes load within geometry setup.

This narrows the earlier terrain-preload hypothesis: the dominant captured
cost belongs to ordinary scenery mesh/material resources, not TerrainType
layer textures. The absence of TerrainType preload history is therefore not
proof that these objects missed Actor/mesh preloading; that history was never
instrumented for this class.

Static preload candidates already exist in the generated Engine reconstruction:
`Actor::PreLoad` at RVA `0x114f00` forwards to `GraphicsMeshInstance::PreLoad`
at `0x174e50` after its Entity gate. The latter queues the root mesh and, when
its boolean argument is true, its texture overrides. The root queue request
also carries that boolean. `GraphicsMesh::PreLoadDependentResources` at
`0x16eb50` queues its own material dependency arrays. In the resident branch,
`ResourceLoader::EnqueueResource` invokes the dependency virtual when this
boolean is true. This provides a stock dependency-aware route to investigate;
it is not evidence that the affected objects took it early enough in this run.

The next investigation should distinguish never queued, queued but not started,
and previously resident then evicted mesh/material resources, using their
preload/enqueue/lifetime history. Outside-directional resource records currently
retain the state but not a queue flag or queue history, so state 0 alone cannot
choose between these explanations. Earlier bounded mesh/dependency preloading
is a better-supported target than lowering the secondary draw budget or adding
terrain-layer refresh for this event. Do not skip visible colour objects or
enqueue their dependencies only at the already-blocking draw as an assumed fix.
No additional identical run or runtime change is needed merely to reconfirm
this resource class. Future detailed capture should first prevent the session
log limit from discarding its requested reports.

## Lifecycle run: successful preload followed by idle eviction

Archived `cache/runs/resource-lifecycle-first` (diagnostic DLL SHA-256
`c73c5c6b223527382a8adfc2c57a4af1fe9881ca1f4a2f0ac91ce9eac5d163aa`).
All seven lifecycle hooks installed. F12 at frame 3399 emitted 92 lifecycle
demands, with zero table replacements, demand overwrites or logger loss.
The previously missing caller/slow-draw/binding reports are complete. The CSV
has 3,761 rows, 3,747 resolved GPU frames, six timeouts and no dropped rows.
Initial grass appears at 2057; exclude that loading/warmup interval.

The route transition is frame 3381, starting at 46.232 seconds, lasting
323.015 ms. All 71 main-thread resource calls are retained, totalling
211.727 ms (32 meshes, 37 textures, two shaders). Of these, 68 calls / 208.427 ms
are resources previously loaded on the worker and subsequently evicted.
64 calls / 205.958 ms enter with a future cooldown deadline. The remaining
three previously unqueued calls cost 3.300 ms. This identifies residency loss,
rather than failure of initial mesh/material dependency preloading, as the
principal cause of this captured loading burst.

For example, `ouranosstatue_01.msh` was queued at frame 2285, loaded by the
worker at 2299 and refreshed at 2465. It was evicted at 3268, with a cooldown
through Engine frame 3471; first visible demand is Engine frame 3383. Its normal
Actor preload had just visited at frame 3374, but the Entity countdown was
67 with a supplied step of 30, so it did not refresh the mesh. The associated
normal texture similarly loaded on the worker at 2310, was last touched by
preloading at 2469, was evicted at 3273 and cost 16.704 ms to reload. The
necropolis normal texture costs another 25.022 ms after the same sequence.
Other roots did receive renewed queue requests before visible use, but their
cooldown rejected those requests (for example floor-group mesh at frame 3343,
Engine frame 3346, deadline 3481).

The common unload's last recorded state is often zero at caller `11f971`.
Native disassembly explains this: the eviction branch first unloads the
resident resource at call `11f934`, then calls the same helper again at
`11f96c` to set its cooldown. The recorder's unload count is one; the second
call explains the zero state and is not a second residency loss.

Native `Engine::EvictOldResources` at `1418a0` passes idle-touch age 800,
last-used age 1600, and cooldown 200 for the affected graphics managers.
`MaintainBudget` is a separate pressure path and remains unchanged. The
800-frame idle test is consistent with the recorded refresh/eviction gap.

Secondary admission is active in this run: eight shadow identities admitted,
1,850 shadow draws suppressed, zero identity overflow. It does not prevent
ordinary scene loading or failed-queue shadow fallbacks. Grass remains 172/172
crossed draws, zero adoptions, 0.190 ms crossing. Native Present is 0.032 ms.
The later geometry draw cost is 17.870 ms this time; its largest retained draw
is 4.454 ms, so the older approximately 94 ms native draw burst did not repeat
at the same magnitude. Do not claim a separate driver-wait fix from this run.

Frame 3614 (648.902 ms) precedes the disappearance of world grass and transition
to very small render/update workloads. It includes an explicit 100 ms game-loop
sleep and only 10.227 ms of resource loading. It is consistent with leaving the
world/menu transition, not another instance of the marked scenery hitch; the
capture does not attribute its entire wall-clock gap.

## Candidate fix: refresh renewed resident interest

`mesh_preload.cpp` patches only Actor's call to Entity::PreLoad, at `114f0b`.
When a main-thread, in-frustum Actor visit would be held by the countdown, its
root is still resident, and its last native touch is at least 400 frames old,
allow the existing countdown to expire for this visit. Admit at most eight
such accelerated visits per Engine frame. The original Entity and Actor bodies
then execute the usual mesh/dependency preload and reset the countdown. A fresh
native root touch deduplicates shared meshes without a pointer cache.

This targets a renewed preload visit before idle eviction. It does not bypass
cooldowns, force loads of cold roots, alter pressure or age eviction, change
secondary admission, or pin resources indefinitely. Calls already due proceed
normally even when acceleration slots are exhausted. The shipping path adds
only constant-time checks at the existing Actor preload site; it has no new
Present work, lock, allocation, resource sweep or timer call. Native dependency
walks/queue work can occur earlier, bounded by root visits rather than total
material fan-out. Memory residency can increase for currently preloaded content
within the existing eviction rules; zero performance impact is not asserted.

The fix defaults on, independently of tracing, with
`[performance] mesh_preload_refresh=0` providing a comparison switch. The
call-site window starts after the optional Actor entry detour's stolen bytes;
installation orders observers before the behavior patch, and teardown reverses
that order. Signature/export/layout checks fail closed. Executable off-game tests
use the pinned native Actor and Entity bodies, with a stand-in only for the mesh
preload callee. They verify countdown/return/stack behavior, eight-call admission,
stock-due bypass, untouched direct Entity calls, restoration and coexistence with
an Actor entry observer. The full regression suite passes. Same-route gameplay
validation is still required; resources already evicted before renewed interest
are deliberately outside this fix's scope.

## First gameplay validation of resident refresh

Archived `cache/runs/mesh-refresh-first`, with `mesh_preload_refresh=1` and the
same diagnostic settings. Debug log SHA-256
`69da64eed433217257150f9f54d56418b6ee6a77d6b116965774372914d3a4b4`;
CSV SHA-256 `a14b36da956bee12d3873de7c2ac52d68c5ef324f2804de206d440557d7c9fba`.
Refresh and all seven lifecycle hooks installed. F12 at 4806 reports 57
accelerated preload visits and zero budget deferrals. Recorder replacements,
unreported-demand overwrites and report truncation are zero. The CSV has
5,216 rows, 5,191 resolved GPU frames, 17 timeouts and no dropped rows.

First world grass appears at 3436. The matched one-to-two deferred-owner
transition is frame 4773, with 173 crossed grass draws versus 172 at the
previous run's frame 3381. Exclude initial loading plus 120 warmup frames,
the F12 frame and the later world/menu exit when assessing gameplay.

| Measurement | Before, frame 3381 | Refresh, frame 4773 |
|---|---:|---:|
| Whole frame | 323.015 ms | 79.485 ms |
| Main-thread resource calls | 71 | 12 |
| Main-thread resource loading | 211.727 ms | 29.480 ms |
| Previously evicted resource calls | 68 / 208.427 ms | 5 / 7.149 ms |
| Directional resource loading | 21.141 ms | 0 ms |
| Directional render | 84.673 ms | 2.391 ms |
| First owner geometry setup | 106.094 ms | 7.958 ms |
| Second owner geometry native draw | 17.870 ms | 28.585 ms |
| Native Present | 0.032 ms | 0.040 ms |
| Grass crossing | 0.190 ms | 0.148 ms |

The marked transition improves by approximately 75%; main-thread loading by
86%. In the 120 frames before through 120 frames after each transition, resource
loading falls from 255.215 ms / 89 calls to 42.494 ms / 22 calls. Extending the
lookback to 600 frames yields 266.739 ms / 101 calls versus 53.808 ms / 35 calls.
This supports preventing the bulk synchronous reload, not merely shifting it
to immediately preceding frames. It does not measure all worker cost or prove
zero overhead.

The new transition's largest remaining resource call is an initially unqueued
particle texture, `XPack4/effects/textures/smoke_8x8__01.tex`, at 20.060 ms.
`smoke_ball_01.tex` adds 9.231 ms in the following frame. Five smaller reloads
remain: two meshes and three Knossos statue textures. These are consistent
with the candidate's intentional limit: it cannot refresh a root already cold
at renewed interest, nor bypass a dependency's active cooldown. The expensive
Ouranos/necropolis bulk reload pattern no longer appears in the complete report.

For comparable approximately 30-second gameplay windows, before world/menu exit,
median frame time is 19.860 → 19.624 ms, p95 25.531 → 23.985 ms, and p99
34.966 → 33.268 ms. Samples cover frames 2177–3613 before and 3556–5053 after.
These are one route repetition each, with diagnostic instrumentation enabled,
not controlled proof of a steady-state speedup or zero performance cost.
No broad frame-time regression is apparent.

Two separate spikes remain outside the target transition: frame 3688 at
99.245 ms (51.278 ms native draws, overlapping 86.460 ms of off-main texture
creation), and frame 4149 at 100.810 ms (82.902 ms in Engine::Update). Neither
has a main-thread Resource load. The available capture does not establish their
root causes or whether refresh contributes. Frame 5054 at 387.779 ms includes
an explicit 100 ms game-loop sleep and precedes disappearance of world grass,
as with the previous exit transition; keep it out of route hitch comparisons.

The user's subjective improvement agrees with the measured target improvement.
No further behavior change is needed merely to establish that result. Residual
particle realization/native draw and update spikes should remain separate
investigations if further smoothing is requested.
