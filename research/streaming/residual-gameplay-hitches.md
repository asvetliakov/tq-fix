# Remaining gameplay hitches after resident mesh refresh

Follow-up: text logging now uses ordinary 250 ms batches, one early wake per
half-capacity crossing, and a durable flush only at shutdown. The historical
analysis below describes the original capture. The installed comparison
keeps performance/lifecycle recording enabled and turns only text output off
on a release build; the earlier DLL, INI and logs are preserved under
`cache/runs/text-logger-before-batching`. No gameplay result for that comparison
was available when the comparison was prepared. Its result is recorded below.
Text batching is also active when logging is re-enabled.

Validation: all six off-game suites pass, including all 12,000 concurrent text
records exactly once, the final shutdown sentinel, zero message-triggered
wakeups for that below-watermark workload, one file open and one durable flush.
The release DLL installed for the text-off run has SHA-256
`bbe68360aab948ea289691b73bf7842c0b9c2e92ebba2a1a7aa17ce369b3a8f8`;
the INI has SHA-256
`b68c6a38aaec40e77c65752384b63c92eee94577768df0b7b28a11cce9b8b269`.
Copies and a manifest are in `cache/runs/text-logger-off-prepared`.
`hdr_debug=0` is explicit as well as `trace=0`. Graphics, mesh refresh, CSV,
lifecycle and marker settings match the baseline. Existing logs were archived
and removed from the live directory so stale text cannot masquerade as output
from this comparison. Use the same route and mark gameplay hitches with F12.

Investigation following `b359bc3`. Source capture:
`cache/runs/mesh-refresh-first/{tqflicker-debug.log,tqflicker-frames.csv,tqflicker.ini}`.
CSV SHA-256: `a14b36da956bee12d3873de7c2ac52d68c5ef324f2804de206d440557d7c9fba`.
Log SHA-256: `69da64eed433217257150f9f54d56418b6ee6a77d6b116965774372914d3a4b4`.
This is a diagnostic capture, not a trace-off performance benchmark. Initial
world frames and their 120-frame warmup are excluded. The exit transition at
5054 is also excluded: it contains a 100 ms game-loop sleep and grass disappears
on the following frame. That sleep alone does not explain its whole duration.

## The 79 ms transition is several costs

Frame 4773 changes from one to two deferred owners, matching the workload
signature used for the earlier 323 ms comparison. Its top-level CPU partition is:

| Interval | Time | Included work |
|---|---:|---|
| Engine update | 24.272 ms | 20.419 ms of synchronous particle texture loading |
| Engine render | 52.570 ms | 9.061 ms of other resource loading; 29.201 ms in native draw submission |
| Remaining frame interval | 2.643 ms | Presentation, upload step and other work outside those two scopes |
| Whole frame | 79.485 ms | Sum of the three disjoint intervals above |

Resource loading totals 29.480 ms across 12 calls. Texture creation (5.791 ms),
shader creation (0.165 ms) and archive inflation overlap these measurements;
do not add them to the frame partition. Archive and off-main creation counters
can additionally aggregate work from several threads.

The largest resource is `XPack4\effects\textures\smoke_8x8__01.tex`:
20.060 ms during update. `XPack2\effects\particles\candleflame01.tex` adds
0.359 ms. The following frame loads `XPack4\effects\textures\smoke_ball_01.tex`
for another 9.231 ms. Each has zero recorded prior queue, worker load or unload
at demand, and first appears in the lifecycle recorder on its demand frame.
There is no recorder replacement, demand overwrite or report truncation here.
This establishes missing earlier observed queueing, not when effect data first
became available or whether there was enough time to complete an earlier load.

Five remaining previously worker-loaded and evicted resources total 7.149 ms
in frame 4773: two scenery meshes and three Knossos statue textures. The floor
mesh still has a future cooldown; the other four deadlines have passed at
demand, but their last observed queue attempts were earlier. A resident parent
does not guarantee that all dependencies remain resident. For example, the
statue root was still resident while its material textures had been evicted;
the root-age refresh deliberately does not scan all child resources each visit.

The second owner's geometry scene spends 28.585 ms in native draw calls. The
largest retained call is a 19.445 ms indexed draw, ordinal 36, count 594, with
another 2.554 ms at ordinal 37 and 2.116 ms at ordinal 48. They share the captured
VS/PS and first SRV identities. Their buffers are not recorded as newly created
in the recent creation history. Pointer identities do not establish asset names.
The scene's GPU timestamp interval is 4.632 ms against 29.680 ms of CPU time.
Driver/translation work, queue synchronization or CPU descheduling are possible;
these timings do not establish shader compilation or a particular GPU producer.
GPU intervals can include gaps in CPU submission and cannot be added to CPU time.

Native Present is 0.040 ms, mod presentation 0.075 ms, the upload step 1.604 ms,
and grass crossing 0.148 ms for 173/173 draws. Secondary admission is two, with
zero suppression and overflow. Neither VSync blocking nor exhaustion of that
admission budget explains this frame.

## Particle loading: checked against native instructions

Re-read the existing Ghidra export and freshly disassembled the installed,
pinned Engine.dll with MinGW objdump. Engine SHA-256:
`0aedbb1805b4a5616f74e34d4f609f392e2c2dd4561c64c118f4772ab4f694f6`.
Addresses below are RVAs. Focused outputs are retained under the ignored
`cache/mesh-preload-audit/` directory.

| Function | Native evidence |
|---|---|
| `Emitter::UpdateParticles`, `139180` | Call at `1391a5` to `Resource::EnsureAvailable` (`2130f0`), returning at the captured `1391aa`; immediately reads the texture collection at resource `+74/+78`. |
| `Emitter::UpdateTexture`, `13ac40` | Obtains the named texture through a manager virtual call at `13acfc`, stores it at emitter `+1d0`; this body contains no direct enqueue call. |
| `Emitter::PreLoad`, `13e1f0` | When requested, enqueues texture `+1d0` and shader `+1d4`; calls at `13e21d/13e23f` target `2145c0`. |
| `Effect::PreLoad`, `1346d0` | Iterates existing emitters and queues those same resources; calls at `134719/13473b`. |
| `EffectEntity::PreLoad`, `136930` | Existing decompilation calls the normal Entity countdown gate, then queues effect data and preloads an existing live effect. |

This is an update-time dependency, not solely a render-time texture bind. Merely
skipping `EnsureAvailable` could expose missing metadata to simulation. The
useful next investigation is when effect/emitter data, texture identities and
native preload visits become available relative to the first update. Queueing
at emitter creation alone may provide no lead time if its first update follows
immediately. No generic effect-preload behavior change is justified by that
call site alone, and hardcoding these filenames would not solve the mechanism.

## Two other spikes have different signatures

Frame 3688 is 99.245 ms: update 1.877 ms, render 59.834 ms, and native draws
51.278 ms within render. It also spends **34.206 ms in the mod's presentation
phase**, plus 2.577 ms in the upload step. Native Present itself is only
0.059 ms. The probe subtracts nested grass/upload/overlay phases from
`present_ms` in `endFrame`; 34.206 ms is therefore not the uploader counted twice.

`visual.cpp::onPresent` performs the FP16 presentation work inside this phase:
optional back-buffer restoration, presentation preparation/state capture,
`CopyResource`, fullscreen submission and state restoration. Preparation reuses
existing size resources, and only allocates on missing resources or size change.
The current counters do not split these D3D calls. Six off-main texture creations
aggregate 86.460 ms in the frame and could contribute driver contention, but
their overlap is not proof of causality. Do not describe this entire spike as
outside the mod simply because native Present was fast.

Frame 4149 is 100.810 ms, dominated by **82.902 ms in Engine update**. Render is
15.290 ms; there are no synchronous main-thread resource loads. The measured
main-thread object waits total 0.060 ms, the unload fence 0.001 ms, critical-section
contention zero and main-thread sleep zero. The aggregate object-wait counter
is 1,020.209 ms across threads, which is not a main-thread stall duration.

Fresh disassembly of `Engine::Update` (`1443a0`) confirms its unpartitioned work:
pending-object destruction, resource sweeps, callback lists, sound/ambiance/music,
world post-load, physics, world update/region usage, forced entities, client
update, resource eviction, input processing and unload-fence setup/completion.
Seven measured resource sweeps total only 0.001 ms here. The existing update
total cannot distinguish the remaining children or CPU descheduling. The verified
fence call at `1447a2` is already measured and is not the missing 83 ms.

## Next diagnostic pass, if pursuing the remaining spikes

Extend one opt-in capture to cover the whole unresolved surface together:

* Effect/emitter creation and texture/shader identity assignment; effect and
  emitter preload visits; first update/demand, linked to current queue/load history.
* All major Engine update children listed above, preserving an explicit remainder
  and separating parent/child totals. Do not add frequent `GetThreadTimes`
  queries: run 43 already demonstrated that these can manufacture CrossOver
  server-call stalls. Distinguishing execution from descheduling needs a separate,
  validated sampling method if elapsed-time attribution does not suffice.
* FP16 presentation preparation, restoration, copy, draw and state operations;
  retain native draw bindings and overlapping worker resource/creation intervals.
* Automatically retain slow-frame detail across the route, with bounded storage
  and loss counters. The current F12 draw report covers only the recent 120 frames;
  pressing it at 4806 cannot recover detailed draw records for 3688 or 4149.

Verify native windows and ABI before adding observers. These are measurement
requirements, not new shipping hooks or fixes introduced by this investigation.
Another identical uninstrumented run would not resolve the missing child timings.

## Could logging contribute?

Yes, diagnostic overhead remains a confounder. Distinguish three mechanisms:

* `hdr.cpp` text logger: formats on the caller, then copies into a locked pending
  queue and signals a writer. The writer copies pending bytes under the same lock,
  releases it, then calls `WriteFile` and `FlushFileBuffers`. There is no disk I/O
  under this lock, but a caller can still wait for it, including if its owner is
  descheduled. Event wakeups and filesystem activity can create indirect pressure.
* `resource_trace.cpp` lifecycle recording: enabled in this run and takes its own
  lock during Actor/preload/queue/load/unload observations. It buffers records
  during gameplay; detailed formatting occurs at F12. This is separate overhead
  from the text writer and absent when the lifecycle option is disabled.
* `probe.cpp` CSV capture: another writer and lock, normally batching at 250 ms.
  Its pending-buffer allocation/copy occurs under its lock. This path predates
  the text-logger change and full tracing remains nonzero overhead.

There is a demonstrated precedent, not just a hypothetical risk. In
[findings §45](findings.md#45-run-44-the-full-trace-writer-was-manufacturing-the-pump-tail),
run 44 identified repeated per-row CSV open/write/close activity contending
through wineserver and delaying the main thread's `PeekMessage`. Run 43 had
already moved that delay into newly added `GetThreadTimes` calls. In
[§46](findings.md#46-run-45-removing-the-writer-defect-removes-the-felt-micro-stutter-class),
run 45 retained a CSV handle and batched rows every 250 ms; the user reported
the frequent micro-stutters gone and the >=50 ms Peek population disappeared.
That CSV fix remains intact today. The text logger also retains its handle,
but still signals on each message and flushes each nonempty batch. It does not
have the CSV writer's ordinary 250 ms batching policy. Bursty text can therefore
still create frequent writer wakeups and flushes. This policy predates the new
append implementation; logging beyond the old lifetime cutoff now continues.

Current frame pump totals are only 0.081 ms (3688), 0.105 ms (4149), and
0.724 ms (4773). The old slow-Peek signature is absent in these events, but that
alone does not exclude background writer contention appearing at another call.
Batching text output and avoiding per-batch durable flushes are sensible options
to reduce observer interference, with explicit shutdown draining and existing
loss/error reporting retained. They are not proven fixes for these three events.

The revised text logger drains only newly appended bytes; the former version
formatted under the lock and rewrote all accumulated text on every flush,
including idle timeout flushes. The revision removes that repeated work and
the 64 KiB lifetime cutoff, but it is not a zero-cost or lock-free logger.
The 4 MiB capacity is a maximum pending buffer, not a 4 MiB copy each frame.
This entire run's text file is only 71,433 bytes. Most detailed event lines are
emitted by the F12 report at frame 4806, after all three investigated spikes;
those lines were not individually written on their recorded event frames.

The Engine wait counters observe Engine.dll's instrumented imports/call sites,
not the mod's logger or lifecycle SRW locks. Therefore their low main-thread
totals cannot eliminate diagnostic lock contention. Neither logger queue wait
nor lifecycle lock wait currently has a frame attribution counter. A long
interval inside a native D3D call is not direct execution of `hdr::log`, but
background recording/I/O could still affect scheduling or driver contention.

To isolate text logging, compare matched repeats on the same non-forced-trace
build with `trace=0` and `hdr_debug=0`, retaining `performance_trace=full` and
`resource_lifecycle=1`. The current Engine tracing gate uses performance tracing,
not the text logging switch. Then disable lifecycle recording alone in a further
comparison if needed. An ordinary all-trace-off run tests shipping behavior,
but cannot distinguish the text writer from the other diagnostic observers.
`TQ_FORCE_TRACE` overrides the text switch, so a forced diagnostic DLL cannot be
used for the text-off comparison. No installed settings were changed here.
Add mod-lock wait and text/CSV batch attribution to the broad diagnostic pass
above if intermittent spikes persist; do not infer causation from one smooth run.

## Residency is renewed interest, not permanent pinning

The committed fix only acts during an existing main-thread Actor preload visit
requesting dependencies, on a still-resident root at least 400 Engine frames old.
It neither stores resource references nor runs a periodic scan of old areas.
Continuous native interest can keep a resource resident for a long time; when
those visits stop, the fix stops touching it. Normal age eviction then applies
unless some other native consumer continues using or preloading the same resource.

`MaintainBudget` (`11f750`) still invokes `EvictOldResources` (`11f830`) with its
own pressure-dependent age threshold; neither function is patched. Refresh can
affect recency and residency, but it does not exempt a mesh from that policy or
change memory budgets. This is not a promise of identical memory consumption:
avoiding premature eviction intentionally retains currently interesting assets
longer. The eight-visit limit controls acceleration, not total resident bytes.

## First text-off comparison: stalls persist

Archived under `cache/runs/text-logger-off-first`, including the tested DLL,
INI, manifest and `comparison.json`. CSV SHA-256:
`9db73fc1b8914a3e4f9f596fdcb7d3d50608ad2544b0072348a87f368accf352`.
The installed DLL and INI exactly match the prepared hashes above. There is no
debug or HDR text log: the text logger is disabled, while performance and
lifecycle recording remain requested. The CSV has 4,553 consecutive rows,
4,528 resolved GPU frames, 17 timeouts and zero dropped rows.

First grass is frame 2887; its 344 ms initial-world frame is excluded, as are
the following 120-frame warmup and the exit transition at 4417. Compare frames
3007–4416 with the earlier text-on run's 3556–5053. These gameplay windows are
28.684 and 29.994 seconds respectively. Percentiles below use linear interpolation.
The user was unsure of a subjective difference; do not claim a felt improvement.

| Measurement | Earlier text on | Text off |
|---|---:|---:|
| Median frame | 19.624 ms | 19.651 ms |
| p95 | 23.998 ms | 25.464 ms |
| p99 | 33.269 ms | 37.930 ms |
| Frames over 50 ms | 6 / 1,498 | 6 / 1,410 |
| Frames over 100 ms | 1 | 2 |
| One-to-two-owner transition | 79.485 ms at 4773 | 186.134 ms at 4249 |
| Transition main resource loads | 12 / 29.480 ms | 43 / 100.535 ms |
| Transition native draw submission | 29.201 ms | 56.746 ms |
| Main resource loads in transition ±120 frames | 22 / 42.494 ms | 63 / 178.281 ms |

Both F12 reactions have clear preceding candidates:

* Marker 3924 follows frame 3906 by 386 ms. The frame takes 191.936 ms, with
  162.824 ms in the measured Peek wrapper and only 0.038 ms in Dispatch. Two
  Peeks occur: one returns a message, one returns empty in 0.002 ms. This differs
  from the historically slow final-empty-Peek signature. The frame has no
  synchronous main-thread resource loads; update is 10.100 ms, render 17.995 ms,
  native Present 0.043 ms and mod presentation 0.072 ms. It is not an F12 report
  frame and there is no text writer to compete with the game.
* Marker 4269 follows the transition at 4249 by 508 ms. All 43 resource loads
  are in render: 16 meshes / 15.942 ms, 25 textures / 81.983 ms, two shaders /
  2.610 ms. The first owner's geometry setup contains 78.617 ms of resource
  loading; the second owner's geometry scene contains 56.163 ms of native draw
  time. Whole update is only 4.993 ms; render is 177.810 ms. Native Present is
  0.073 ms, mod presentation 0.064 ms, and the upload step 2.148 ms. Grass is
  still 172/172 crossed draws at 0.153 ms, with no secondary admissions,
  suppression or overflow in this frame.

There are also a 69.531 ms draw-dominated frame at 3164 (58.957 ms in native
draws), and update spikes at 3542 and 3970 (40.763 and 45.696 ms). These classes
also survive with text logging disabled. The former 34 ms presentation spike
is absent (maximum 4.050 ms in this gameplay window), but one run is insufficient
to credit text logging for its disappearance.

The heavier transition is a changed cold-resource workload, not just slower
execution of the same twelve loads. Particle demand is later in this run:
frame 4257 has 18.861 ms of update-time resource loading, and 4260 has 8.992 ms.
There are further render-time loads before and after the transition. Asset
names and before-demand residency histories were intentionally suppressed by
the text-off control, so those later loads cannot be identified as the earlier
smoke textures from the CSV alone. The 79 ms result is not a stable ceiling or
proof that the mesh residency problem is fully solved. The root-age refresh's
known limits still apply; this capture cannot select which limit caused the
extra demand or directly report its installation/acceleration totals.

Conclusion: the text logger is not necessary for the surviving pump, update
and draw/loading stalls. This does not rule out a contribution in earlier runs,
nor isolate CSV, lifecycle or other observer overhead. Do not revert the
validated text batching or reopen message-pump behavior changes from one event.

Next comparison: keep the same tested DLL and re-enable only `trace=1`, with
the new batching policy, to recover the existing full lifecycle and draw reports
for this variable transition. Capture F12 after each gameplay hitch. This needs
no new native hooks. Preserve the text-off run first; record the next INI under
`cache/runs/text-logger-batched-prepared`. Use the resource histories to distinguish
missing preload, evicted dependencies and cooldown rejection before extending
mesh refresh. The separate pump/update attribution gaps remain unresolved.

## Batched-text repeat and idle-requeue candidate

The next run is archived as `cache/runs/text-logger-batched-first`, with
`lifecycle.json`, `analysis.json`, DLL/INI copies and a hash manifest. It uses
the same `bbe68360...` release DLL, with text output re-enabled. CSV SHA-256:
`154ffca2bd30b49819e46956f6ec05dd94000c5a7d8aaf8244c4afebd3a5541b`;
text SHA-256: `7f43253aa8f604913cb46e584c83aa517f945a3f9008ff802c3f151f745a882c`.
There are 3,935 consecutive rows, 3,916 GPU results, 11 timeouts and zero dropped
rows. All seven lifecycle sites installed, with no history replacement, demand
overwrite or report truncation. Mesh refresh reports 27 accelerations and zero
budget deferrals. The user marked the transition and was unsure whether the
earlier hitch occurred. Its former 163 ms Peek signature is absent: maximum
gameplay Peek time is 7.167 ms. That single absence is not proof of a pump fix.

Exclude initial world frame 2231, its warmup, and exit frame 3804. Frames
2351–3803 span 28.967 seconds: median 19.601 ms, p95 23.595 ms, p99 32.483 ms,
four frames over 50 ms. There are still 41/50 ms update spikes, and frame 2906
takes 90.192 ms with 44.977 ms in the upload step. These are distinct from the
marked transition and remain unresolved.

F12 at 3620 follows frame 3597 by 550 ms. The transition is **255.408 ms**:
32.064 ms update, 219.848 ms render, and the remainder outside those scopes.
Its 70 main-thread resource loads take 172.898 ms; native draw submission takes
55.824 ms. These are measured components, not all new mod work. Present is
0.045 ms, mod presentation 0.034 ms, and the upload step 2.551 ms.

The lifecycle snapshots establish a more specific failure:

* **63 loads / 143.524 ms** were previously worker-loaded, then evicted, and
  all 63 still have future cooldown deadlines at visible demand.
* The other seven / 29.374 ms have no prior observed queue. This includes
  `smoke_8x8__01.tex` at 24.688 ms; particle realization remains separate.
* `ouranosstatue_01.msh` queued at 2607, loaded on the worker at 2611, and was
  evicted at 3413 with deadline Engine frame 3616. A renewed native queue
  request at frame 3557 / Engine 3560 was rejected; drawing demanded it at
  3597 while that deadline was still future. Its two textures then cost
  28.590 ms. Several other roots show the same request-before-demand gap.
* A root need not be cold for its material overrides to fail: the resident
  `RuinDamagePillar03.msh` had native preload visits, while its `MCRuinSource01`
  textures were evicted and their renewed requests rejected by cooldown.

The root-refresh budget was not exhausted. Refresh can only prevent eviction
during qualifying visits while the root remains resident; it cannot recover
an already-evicted root or override through the native cooldown gate. The
recorder does not retain every intermediate Actor visit, so it cannot establish
the exact spatial/thread/countdown reason for every missed resident refresh.
The renewed-but-rejected queue requests and future deadlines are directly observed.

Re-read the existing Enqueue/Load/Evict disassembly and freshly disassembled
`Engine::EvictOldResources` at `1418a0`. The texture manager `+24` call at
`1418df` and mesh manager `+2c` call at `1418f8` pass `(800,1600,0,200)` to
`BaseResourceManager::EvictOldResources` (`11f830`). The last argument produces
the future queue deadline. Native `EnqueueResource` compares that deadline at
`21462d` and branches away at `214630`, inside its existing queue lock. The
worker's completed load resets both used/touched ages (`213b43` onward), so a
reload does not inherit its previous idle age. `MaintainBudget` (`11f750`) has
its own unmodified eviction call with a zero cooldown already.

The candidate extends `mesh_preload_refresh=1` with **two one-byte patches**:
the low byte of `push 200` becomes zero at `1418cf` and `1418e8`. Whole 25-byte
windows verify the manager, all four arguments and native call destination;
both exported function RVAs are also checked. A mismatch rejects the combined
feature and restores its Actor call patch. Shutdown restores both immediates.
The recorder remains observational and no new native hook is installed.

Only future automatic mesh/texture age evictions get the shorter deadline.
Other unload callers, pressure handling, eviction ages, size filters, dependency
queueing and worker loading remain native. No resource is pinned and no queue
entry is fabricated. The change allows existing native preload requests to run
earlier after eviction; it adds no per-frame code or locks. Earlier background
reload work can increase when preload interest returns soon after eviction,
so it is not a claim of zero total performance or memory effect. Gameplay must
validate whether requests complete before visible demand without shifting the
hitch to another frame. The Actor acceleration bound remains eight root visits,
not a global loading budget. Setting the option to zero restores both changes.

Verification now includes the full 160-byte native Engine eviction caller as an
executable fixture, with only its manager callee replaced by an argument recorder.
Stock, patched and restored calls must preserve 800/1600 ages, size zero, the
other two managers and x86 stack cleanup. Every byte in both windows is mutated
in rejection tests. The verifier checks the fixture and signatures against the
pinned installed Engine. The next run retains the complete batched text/CSV/
lifecycle/F12 setup; inspect accepted worker queues, cooldowns, whole-frame costs
and the surrounding window before claiming success.

All six off-game suites passed (`build/idle-requeue-selftest.log`), together
with the updated mesh verifier and the general streaming-site verifier. The
installed release candidate has SHA-256
`cece1ab64be78444cda7f8bf03f01f5faa82262f0364f173bb272b6349e7739f`;
its INI has SHA-256
`981551347aeddea8286847f3ad3c86d7c5a5d4c39d4f3df4e6a78bf32967dde0`.
Copies and a manifest are in `cache/runs/idle-requeue-prepared`. INI values
match the preceding batched-text run. Gameplay validation is pending.

## Idle-requeue validation and cold-root countdown recovery

The result is archived as `cache/runs/idle-requeue-first`, including the DLL,
INI, manifest, parsed `lifecycle.json` and `analysis.json`. CSV SHA-256:
`d3944b5e6b5aff07bd85684c4c76d34643976794a2ef493a2350769a7e63ed52`;
text SHA-256: `926013f5c0cd2eaf5dd77e144c05d0f3943adf037f4bf31a2fc9ed3c5ee9dc58`.
The installed DLL/INI match the candidate above. The log confirms idle requeue
cooldown zero, all seven lifecycle sites, 28 refresh accelerations and zero
budget deferrals. There are no history replacements, demand overwrites or
report truncations. All 3,724 CSV rows are consecutive, with 3,710 GPU results,
six timeouts and zero dropped rows.

Initial world frame 1984, its warmup and exit frame 3569 are excluded. Gameplay
2104–3568 spans 29.049 seconds: median 19.461 ms, p95 23.486 ms, p99 30.878 ms.
The prior batched-text run was 19.601 / 23.595 / 32.483 ms across 28.967 seconds.
Five frames exceed 50 ms versus four previously; four are update-dominated
50–68 ms frames with at most 0.255 ms of main-thread resource loading.
Maximum gameplay Peek time is only 2.688 ms. This does not establish a general
performance guarantee or a fix for the separate update spikes.

The marked one-to-two-owner transition at 3339 improves from **255.408 to
176.771 ms**, and main-thread resource loading from **172.898 to 84.651 ms**.
Main-thread loading across ±120 frames drops from 202.315 to 118.058 ms;
across [-600,+120] it drops from 218.791 to 133.365 ms. The wider window supports
less synchronous demand rather than merely moving the same work slightly
earlier. Worker queue completion for resources absent from the demand report
cannot be individually inferred from their absence alone.

All 34 marked resource demands now have **no future cooldown and no pending
queue node**. Of those, 31 / 83.164 ms were previously worker-loaded and evicted;
three / 1.487 ms have no observed earlier queue. Update-time particle demand
occurs later in this run (18.951 ms at 3349 and 9.028 ms at 3352), so not all of
the transition improvement can be credited to the cooldown change. Native draw
submission remains 61.198 ms. Present takes 0.084 ms, mod presentation 0.106 ms,
and the upload step 2.652 ms.

The remaining Actor countdown delay is now explicit:

* `ouranosstatue_01.msh` last preloaded at 2389, then was evicted at 3193 with
  deadline Engine frame 3196. At the latest Actor visit, frame 3337, its countdown
  was 29 and the supplied step 28: the native Entity gate returned false.
  Drawing demanded it at 3339. Root plus two textures cost 35.901 ms.
* `candlesmall01.msh` was evicted at 3198. Its latest Actor visit at 3334 had
  countdown 58 / step 28. Its `necropolistomb01normal.tex` dependency costs
  another 24.853 ms at the transition.
* Other cold roots show the same positive-countdown condition despite recent
  Actor visits. Removing the queue cooldown cannot help until a native queue
  request actually occurs; the original acceleration path accepted only state 2.

The next candidate extends that same Actor-to-Entity call-site decision, with
no new hooks. A stale root in state 0 may share the existing **eight-per-frame
total** acceleration budget when it has an expired nonzero unload deadline and
no native queue node. The same 400-frame touched-age guard remains, including
for cold roots, to avoid immediately retrying a recently pressure-evicted mesh.
New resources with the constructor's zero deadline, state-1 loading resources,
queued resources, active cooldowns and non-main-thread calls retain stock timing.
Ordinary already-due Actor visits still bypass this acceleration budget.

The implementation continues into the original Entity and Actor bodies, which
perform native mesh/dependency queueing. It does not load a mesh synchronously
or write resource state/queue/deadline fields itself. Native queue insertion
deduplicates shared roots. This may bring bounded root requests and their native
dependency work earlier; the root count is not a byte/time budget. Resources
outside active native Actor preload visits are not swept or pinned.

Rechecked the existing Entity gate and native enqueue/unload disassembly, then
freshly disassembled the Resource constructor. Additional runtime signatures
verify constructor state/deadline zeroing at `212ec5`, unload deadline storage
and `ret 4` at `212cbb`, and the native unsigned deadline comparison at `214627`.
State and queue-node accessors were already verified by the shared install gate.
The native Actor/Entity executable fixture now exercises cold admission and
queued/new/future-deadline exclusions, alongside the trace-entry coexistence
case. Pure decision tests cover the shared budget, main-thread/include gates,
loading resources, recent pressure eviction and native deadline semantics.
F12 adds `recovered` as a subset of the total `accelerated` count. Gameplay
validation of this extension is pending.

All six off-game suites passed (`build/evicted-root-selftest.log`), as did the
mesh and general streaming-site verifiers. The installed release candidate
has SHA-256 `33e170a4901f211bc5d23554a04d21ead0db811253a2bec32b518e55c025cb76`;
its INI has SHA-256
`5a1657f49a79e30eba487cf053d9ceb80934da08de018b7dacde45c27d2caf64`.
The preparation archive is `cache/runs/evicted-root-prepared`. INI values remain
identical to the preceding run, and the prior DLL/configuration/logs are preserved.

## Combined recovery validation: scenery reloads absent

The first run of the combined fix is archived as `cache/runs/evicted-root-first`,
with the DLL, INI, manifest, `lifecycle.json` and `comparison.json`. CSV SHA-256:
`b7e4963cc4a639faeff84cfbdce431e10ab5f94e81297e85158650d80908197e`;
text SHA-256: `68cc4635d947d39fb23ce364a09f5b146663e70da3aae9c48a9aa2fe153d14d3`.
The candidate DLL and INI hashes match the preceding preparation. The log
confirms recovery enabled, idle cooldown zero, all seven lifecycle sites,
59 accelerations including **22 cold-root recoveries**, and zero budget deferrals.
History replacements, unreported overwrites and F12 truncations are zero.
There are 3,800 consecutive CPU rows, 3,745 GPU results, 47 GPU timeouts and
zero dropped rows; GPU coverage is incomplete, so CPU conclusions do not rely
on filling in missing GPU results.

The user tentatively reported no felt hitch. The transition at 3341 changes
from one to two owners as before. F12 at 3426 still falls inside the 120-frame
detail window. Exclude first world frame 2001, its 120-frame warmup and exit
frame 3621; the comparison window is gameplay 2121–3620 (29.822 seconds).

| Measurement | Before cooldown fix | Cooldown fix only | Combined recovery |
|---|---:|---:|---:|
| Transition CPU | 255.408 ms | 176.771 ms | **55.463 ms** |
| Main-thread transition loads | 70 / 172.898 ms | 34 / 84.651 ms | **7 / 22.752 ms** |
| Previously evicted transition demands | 63 / 143.524 ms | 31 / 83.164 ms | **0** |
| Main-thread loading across ±120 frames | 202.315 ms | 118.058 ms | **35.030 ms** |
| Main-thread loading across [-600,+120] | 218.791 ms | 133.365 ms | **50.570 ms** |
| Transition native draw submission | 55.824 ms | 61.198 ms | **12.095 ms** |
| Gameplay median | 19.601 ms | 19.461 ms | 19.481 ms |
| Gameplay p95 | 23.595 ms | 23.486 ms | 23.651 ms |
| Gameplay p99 | 32.483 ms | 30.878 ms | 30.882 ms |

All seven loads at the transition have no prior observed queue/load/unload:
three shaders, `Effects\auras`, the inventory transparency texture,
`smoke_8x8__01.tex` (20.539 ms) and `candleflame01.tex` (0.363 ms). There are no
mesh loads at that frame and no demand for a previously evicted resource among
the 16 loads across ±120 frames. Two small first-observed mesh loads later in
that window are ambient turtle and `davykroken` assets, not the previous scenery
reloads. The earlier statue/candle/dependency population no longer falls into
the synchronous load path in this capture. Together with 22 recovery admissions,
this supports successful preload before use. The recorder prints cold demands,
not every resident resource, so individual worker completion timestamps for
those absent demands are not emitted.

Transition update is 24.912 ms, including 20.902 ms of particle loading; render
is 26.386 ms, including only 1.850 ms of resource loading. Native Present is
0.041 ms and mod presentation 0.108 ms. The second geometry scene's native draws
fall to 11.545 ms. Earlier resource realization could reduce downstream driver
work, but the draw improvement alone does not prove a driver mechanism.
Grass remains 173/173 crossed draws, at 0.139 ms. Secondary admission is one,
with no suppression or overflow. The wider windows show no compensating
synchronous loading burst near the transition; their maximum is the 55 ms
transition itself for ±120, and a separate 68.970 ms update spike for [-600,+120].

The full gameplay window is **not hitch-free**. Frame 2302 takes 203.843 ms,
of which 189.647 ms is inside Engine update. Its only resource load is a 0.703 ms
mesh load during render, not update. Measured main-thread object waiting is
0.051 ms, the unload fence 0.001 ms, and no Engine critical-section contention
or main-thread sleep is recorded. Seven resource sweeps record zero whole
microseconds. Enqueue count is 568, versus 553 in the preceding frame; neither
that count nor the unpartitioned update duration identifies a cause. Mod observer
locks and unmeasured native update children remain possibilities. The F12 report
is far too late to recover detailed history for frame 2302. Smaller 51–69 ms
update spikes also persist. Maximum gameplay Peek time is 3.425 ms.

Conclusion: retain the combined recovery fix; this run validates the targeted
scenery reload mechanism and agrees with the user's improved experience.
Normal frame-time statistics are essentially unchanged. Do not claim zero
overhead, general smoothness, or that the separate update spike is unrelated to
all mod/diagnostic code. The existing build and trace configuration remain
installed. Further work on particle initialization and the major update children
should follow the complete diagnostic plan above, rather than blindly expanding
the resource admission budget. No runtime changes were made while reviewing
this successful capture.

## Twenty-second stop and resume validation

The user repeated the route with a reported 20-second stop: no felt hitch at
the usual transition, but possible slight slowness before it. Archive:
`cache/runs/evicted-root-stop20`, including DLL, INI, manifest and
`comparison.json`. DLL/INI hashes match `evicted-root-first`. CSV SHA-256:
`80a3617f8e9d5cf95133922b1412aa52bf4aceac65fe5dc993f4a58d98f088ee`;
text SHA-256: `f0402088e68a7eb3ad4918cabe19fc340dfcec569d06d73d1726ab585a8dc223`.
The log confirms combined recovery and all seven lifecycle sites installed.
There are 4,808 consecutive CPU rows, 4,795 GPU results, five GPU timeouts and
zero dropped rows. There is **no F12 marker**, so no per-resource lifecycle
dump or recovery/deferral counter snapshot; do not infer those counts or
resource identities from their absence.

Exclude first world frame 1991 and its 120-frame warmup, plus exit frame 4671
(100 ms requested loop sleep, followed by the renderer owner count going to
zero). Gameplay 2111–4670 spans 51.048 seconds. Median/p95/p99 are
19.672/23.426/29.035 ms, with seven frames above 50 ms and a maximum of
66.466 ms. The previous 204 ms update spike does not recur. These session
percentiles include the stop and are not a matched moving-route benchmark.
Frames 3291–4190 have an unchanged crossed-grass count of 159 and span
17.762 seconds, consistent with the interior of the reported stop. Their
median is 19.660 ms and maximum 25.708 ms; counters do not establish exact
movement start/stop times.

The one-to-two renderer-owner transition at 4399 is **58.748 ms**, versus
55.463 ms in the preceding continuous run. Its six main-thread loads total
23.929 ms, versus seven/22.752 ms. Update accounts for 26.109 ms, including
22.243 ms of loading; render is 29.103 ms, including 1.686 ms of loading.
No outside-directional mesh load is recorded at the transition. Texture and
shader costs resemble the prior particle transition, but without the F12
dump their filenames and prior eviction histories are unconfirmed. Across
±120 frames, main-thread loading totals 36.259 ms versus 35.030 ms previously.
Native Present at the transition is 0.031 ms. This supports continued benefit
after the stop; the previous large synchronous scenery reload burst has not
returned in the aggregate timings.

A plausible match for the earlier felt slowness is frame **4273, 64.624 ms**,
about 2.516 seconds before the transition. Render takes 55.210 ms; native draw
submission takes 41.580 ms, including 41.193 ms in the first geometry scene.
There are **zero main-thread resource loads**. Fourteen off-main texture
creation calls accumulate 28.427 ms in the same reporting interval, and
geometry-scene GPU elapsed time is 32.662 ms. The following frame takes
37.617 ms with 16.136 ms of native submission and 23.409 ms of off-main texture
creation. This is consistent with concurrent streaming/driver contention,
but aggregate worker counters and GPU elapsed intervals do not prove overlap
at the blocking draw, shader compilation, or a refresh-induced regression.
Crossed-grass CPU work at 4273 is 0.238 ms, Present 0.036 ms and Peek 0.708 ms.
Frames 4275–4398 return to a 19.243 ms median and 26.985 ms maximum: this is
a short spike, not evidence of sustained frame-rate reduction.

Earlier frames 2661, 2666, 2916 and 3051 take 60–66 ms, mostly in Engine
update (44.591–48.561 ms), with negligible measured main-thread waits and
little or no synchronous loading. Frame 2804 takes 54.176 ms, including
22.440 ms in Peek. These remain unresolved possibilities for felt slowness;
there is no marker tying the user's observation to one event. Keep the
current build. No timer/budget adjustment or runtime instrumentation change
is justified by this capture alone. The broader diagnostic plan above remains
applicable to native update, driver submission and observer overhead.
