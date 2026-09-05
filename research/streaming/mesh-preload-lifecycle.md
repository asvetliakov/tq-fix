# Mesh/material preload lifecycle audit

Engine.dll SHA-256:
`0aedbb1805b4a5616f74e34d4f609f392e2c2dd4561c64c118f4772ab4f694f6`.
The installed native image matches the pinned streaming audit. Re-exported the
existing Ghidra project with Actor/Entity/mesh preload and level/unload roots
(1,596 functions, 211 roots). Cross-checked native x86 instructions with MinGW
objdump, including internal worker dispatch missing from the old export closure.
Generated artifacts are ignored; the inputs and signature verifier are tracked.

## Native flow, RVAs

| Site | Verified behavior |
|---|---|
| Region update `20ab20` / preload `20a970` | Ongoing region update forwards to preload; traversal requires a present, non-loading level. Terrain, grid and level entity preload follow. |
| Level entity preload `1b0a50` | Queries the preload frusta and sets Entity preload-frustum flags. Unless forced, selects one entity-ID group modulo ten, advancing Level `+6a44` each visit. |
| Entity preload `148050` | Subtracts the supplied step from Entity `+e8`. Returns false while positive. When due, visits attached children and resets the countdown to 500. These are native step units, not a demonstrated wall-clock interval. |
| Actor preload `114f00` | Calls Entity preload; only on success calls mesh-instance preload through Actor `+184`. Two stack arguments, `ret 8`; its boolean return must be preserved. |
| Mesh-instance preload `174e50` | Queues root `+4` at priority one with the supplied dependency boolean. Queues overrides `+14/+18` only when true. Two stack arguments, `ret 8`. The rest of this function has unrelated game behavior and must remain native. |
| Mesh dependencies `16eb50` | Queues material dependency arrays at `+ec..f0` and `+e0..e4`, with recursive dependencies enabled. No explicit arguments, `ret`. |
| Shader dependencies `18b100` | Queues its dependency collection. No explicit arguments. Native return is at `18b1a1`; do not derive its end/ABI from the Ghidra function-size column. |
| Enqueue `2145c0` | State zero queues only when unqueued and resource `+34` is no later than Engine frame. Existing requests can upgrade the dependency flag. State two touches recency and calls the dependency virtual only when requested. |
| Actual load `213a40` | Common main/worker loading body, state 0→1→2. Two explicit stack arguments (resource and queued-at value), `ret 8`, despite incomplete decompiler argument recovery. Does not itself call the dependency virtual. |
| Worker `213bd0` | Calls actual load at `213c37`, then conditionally calls dependency virtual `+4` at `213c47`, according to the queued dependency flag. |
| Main load `213ed0` | Calls actual load at `214009`; there is no equivalent following dependency-preload dispatch. Material realization can therefore occur synchronously at subsequent rendering. |
| Common unload `212c70` | Waits for state one as needed, destroys state two, clears residency/LRU fields and records cooldown deadline at `+34`. One explicit argument, `ret 4`. |
| Manager eviction `11f830` | Calls the common unload directly, including sites `11f934/11f96c`. Observing only the exported loader unload would miss these evictions. |
| Resource construction `212e60` | Three arguments, `ret 12`. Invalidates recorded history for reused addresses after construction. The destructor requires nine stolen bytes; the shared detour supports eight, so it is not hooked. |

Native Resource layout used here: filename accessor already verified by engine
hooks; state `+30`, cooldown deadline `+34`, queue node `+60`. Engine singleton
pointer is at `3743f0`, its frame counter at `+3f0`. All numbers above are hex.
The queue's third explicit argument is a dependency-preload request, not merely
a completion-notification flag as some historical variable names suggest.

The existing shadow and secondary admission controls do not budget primary
colour-scene resource loading. The latest retained gameplay hitch is described
in [gameplay-loading-hitches.md](gameplay-loading-hitches.md). Its state-zero
resources do not establish whether preloading was absent, late, or undone by
eviction. The static gates are potential explanations, not a proven diagnosis
for the affected objects. Globally forcing preload booleans or removing gates
could create a larger worker/memory burst; no such behavior change is justified
yet. The approximately 94 ms native draw wait also needs the previously lost
binding report before it can be attributed to a specific producer.

## Complete diagnostic pass

Opt in with `[debug] resource_lifecycle=1`, `trace=1`,
`performance_trace=full`, `stutter_marker=1`. The release default remains off.
Seven additional entry hooks are installed as one group after the existing
load, enqueue, filename/state and mesh render observers are available. Every
head, ASLR operand, stolen instruction boundary and native return cleanup is
verified. Unsupported bytes reject the group; attachment failure rolls back.
All original calls and arguments are preserved. No preload request is added.

The bounded recorder covers every observed resource type, without filename
filters: Actor/mesh preload visits and countdown outcomes; root and recursive
mesh/shader dependencies; all enqueue requests via the existing hook; actual
main/worker starts and completion; central eviction/unload and caller/deadline;
and construction/address reuse. Mesh RenderPass provides the current root for
material-demand attribution. It stores 8,192 resource histories and 1,024 cold
main-thread demand snapshots, each with its parent's history **before** loading.
F12 emits previously unreported retained demands in the last 600 frames alongside the existing
phase, terrain, caller, slow-draw/binding, buffer and GPU reports.

Queue flags: low two bits priority, 4 dependencies, 8 touch, 16 queued before,
32 queued after. Queue acceptance is observed from before/after state, not an
atomic native queue receipt: the worker can consume a node concurrently.
`notQueued` is deliberately not labelled an eviction/cooldown count. Compare
recorded queue deadline/Engine frame, worker history and explicit unload events
before assigning a reason. Tick fields use GetTickCount milliseconds; frame
fields use the CSV frame index unless explicitly labelled Engine frame. Counts
indicate whether zero-valued first/last frame or tick fields are meaningful.
Completion tick zero denotes an in-progress observed load. Parent links are
best-effort dependency/render associations, not proof of exclusive ownership.
A generation mismatch prevents attribution to a new object at the same address.
Replacement and unreported-demand overwrite counts explicitly flag history loss.
Spatial traversal that never reaches an Actor is represented as absent Actor
history, not a fabricated reason for that absence.

The logger now appends drained batches rather than rewriting a lifetime-limited
64 KiB buffer. A 4 MiB bounded pending queue and writer-owned snapshot accommodate
full marker bursts; overflow and write failure are explicit. File I/O stays on
the writer thread. Long individual lines carry a truncation suffix. Diagnostic
recording has overhead; it is not a performance-neutral benchmark mode. With the
option off, the seven extra native hooks and recorder allocations are absent;
the ordinary non-tracing mesh draw path does not enter the recorder.

Verification: `tools/verify-resource-trace.py` checks the source signatures
against the pinned PE and objdump. Off-game tests mutate every verified byte at
a rebased address, exercise before-demand state, worker attribution, eviction,
address reuse and ring overwrite handling. A separate trace-on process writes
12,000 records concurrently and verifies every record exactly once, beyond the
old log limit, including the final shutdown sentinel. Existing device recreation,
grass activation/rollback and trace-off regressions also run.

Next gameplay capture: use the same route, wait past initial world warmup,
press F12 promptly after the hitch and exit normally. Exclude initial world
frames, post-recreation warmup and the F12 report frame from performance analysis.
Check `Resource lifecycle installed: 7/7` and report overflow counters before
using absence of history as evidence. Choose the behavioral fix from resource
queue lead time, worker progress, dependency flags and residency loss in that
capture; do not call this diagnostic build a verified hitch fix.

## Follow-up

The completed lifecycle run and bounded resident-refresh candidate are recorded
in [gameplay-loading-hitches.md](gameplay-loading-hitches.md). The original
missing-history question above is now resolved for 68 of the 71 marked loads.
The diagnostic observer still forwards its original calls; the independent
Actor-to-Entity call-site fix is installed afterward and defaults on.
