# Remaining gameplay hitches after resident mesh refresh

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
