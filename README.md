# Titan Quest DX11 Fix and Visual Upgrade

A runtime fix and visual upgrade for Titan Quest Anniversary Edition when its
DirectX 11 renderer runs through CrossOver/DXMT.

Titan Quest sometimes supplies bone indices outside its 27-entry animation
matrix array. DXMT can read undefined constant-buffer data for those indices,
making an object disappear for a frame. The DLL recognizes the affected vertex
shaders and inserts one integer clamp before the bone lookup.

It also recognizes the game's exact FXAA and shadow shader shapes. By default
it:

- replaces the FXAA draw with canonical SMAA 1x High (luma edges, lookup-based
  pattern weights, and neighborhood blending) at the same pre-UI point;
- upgrades the game's trilinear wrapped material samplers to 16x anisotropic
  filtering for clearer terrain and surface textures at oblique angles;
- widens the directional shadow projection, which the engine fits to a fixed
  fraction of the camera frustum, and enlarges the shadow depth maps and their
  viewport/scissor to match;
- stabilizes that projection, which the engine otherwise rebuilds from scratch
  every frame around a light-space basis that follows the camera, so shadow
  edges stop crawling while the camera zooms, pans, or rotates;
- changes TQ's four cardinal bilinear shadow taps into the corners of an
  optimized 3x3 PCF footprint, scaled to hold edge softness constant as the
  projection widens.
- progressively uploads eligible large loose-file textures in bounded,
  frame-paced chunks instead of submitting every high-resolution mip at once.
- keeps the final scene/post-process chain in FP16 with a look-preserving
  Frostbite-style display mapper by default, with an optional modern AgX-derived
  output transform for SDR and HDR displays.
- replaces the game's fixed, low-resolution bloom with an aspect-correct
  multi-scale float bloom while the enhanced FP16 output path is active.

For widescreen play, it also replaces the game's fixed 4:3 CPU-side
entity-update frustum construction (expressed internally as 1024x768) with the
live display aspect ratio. Characters and other entities visible near
ultrawide screen edges therefore continue receiving animation, activation, and
AI updates.

Every enhancement fails open to the game's original draw or resource. The DLL
contains the `winmm.dll` proxy, one D3D11 device-creation hook, the narrowly
matched device/context hooks needed by the visual path, and the two DXBC
transformers. Resolution-dependent AA targets are reused. The one place it
reads back from the GPU is the grass crossing's fallback seed, a staging copy
mapped without waiting a frame after a Present has carried it.

## Stutter findings and fixes

The problem was a noticeable pause when entering a new area during play:
cold scene work arrived together instead of being spread across frames.
This **in-play scene-transition burst** was not one slow shadow function
or an established Wine-only problem. The evidence supports several interacting
costs: synchronous cold-resource loads during directional-shadow gathering,
runtime terrain layers whose textures were never semantically preloaded, and
bursty first GPU participation across reflection and directional shadows, and
previously preloaded scenery assets evicted before visible use.
A slow later D3D draw can be where queued work is waited on, not where it
originated. These engine paths also exist on native Windows; the measurements
here were made on the supported CrossOver/DXMT installation.

Our fix is to prepare resources earlier and spread secondary drawing across
frames. The defaults queue and temporarily omit cold directional-shadow casters,
covering both root meshes and alpha-tested base textures, and avoid unused
shadow material inputs. An additional earlier Actor pose hook prevents loading
the root before those regular caster checks. Terrain layers receive the stock
preload, and reflection and directional shadow share an eight-new-object-per-
frame draw-admission budget. Normal colour drawing remains unchanged;
pending local shadows/reflections may appear later. This does not lower shadow
resolution or undo the shadow-distance fix, and required no renderer rewrite.
When the game revisits a stale but still-resident scenery mesh for preloading,
the mesh refresh fix advances its normal dependency preload, up to eight such
visits per Engine frame. This reduces idle eviction before the player reaches
the mesh while preserving memory-pressure handling. The separate 8 MiB archive
cache remains enabled for its limited reuse benefit.

In Runs 84 and 85 the user no longer noticed the old-route **play** hitch.
Run 85's old-location transition was 40.117 ms CPU / 40.780 ms GPU, with no
large postponed rebound; that is a route-specific result, not a promise of
stutter-free loading or universally smooth play. All fixes operate with
`performance_trace=0`. On a later alternate route, mesh preload refresh reduced
the matched transition from 323 to 79 ms and synchronous resource loading from
212 to 29 ms. Normal frame times held steady in that comparison; remaining
particle loads and other spikes mean this is not a stutter-free guarantee. See
[research.md](research.md) for the evidence, trade-offs, and rejected explanations.

The progressive texture uploader addresses a separate problem: submitting all
mip data for a large texture in one creation call concentrates upload work.
With `streaming=optimized`, eligible mapped **loose-file** textures start with
a low-detail view while high-detail mip data uploads in frame-paced chunks;
the full view returns when the job completes. This trades temporary softness
for smaller upload steps. It does not progressively upload `.arc` textures,
skip archive decompression, or replace the shadow/reflection admission fix.
See [progressive texture uploading](#progressive-texture-uploading).

## Visual settings

Visual upgrades are enabled when no configuration file exists. To customize
them or select individual original game paths, create `tqflicker.ini` beside
`TQ.exe`:

```ini
[graphics]
aa=smaa
anisotropy=16
shadows=enhanced
shadow_split=0.45
shadow_map_scale=4
shadow_point_map_scale=2
shadow_filter=corners
shadow_stabilize=on
grass=enhanced
edge_updates=expanded
bloom=enhanced
bloom_strength=0.85
hdr=auto
tonemap=frostbite
paper_white_nits=203
peak_nits=auto

[performance]
streaming=optimized
loose_texture_max=4096
archive_cache_mb=8
shadow_defer_cold_resources=1
shadow_defer_cold_actor_pose=1
terrain_preload_layers=1
mesh_preload_refresh=1
secondary_pass_admission_budget=8

[debug]
frame_overlay=0
trace=0
performance_trace=0
engine_trace=1
stutter_marker=0
resource_lifecycle=0
```

`loose_texture_max` refuses a loose texture whose base level is larger than
the given number of pixels on either side, so the game falls back to its own
copy from the `.arc` archives. It defaults to `4096`; `0` restores stock loose-
file selection. This exists for high-resolution texture packs: one measured
install carries 984 loose textures over 4096 on a side, up to 16384x16384 and
341 MiB for a single file, and although they are only 7.9% of its files they
are 46% of its bytes.
The archive copies of those same assets total 6.4% of the size. Setting
`loose_texture_max=4096` keeps every 4K and smaller asset from the pack and
takes the rest from the archive; a texture with no archive copy is simply not
found, which is the engine's own behaviour for a missing file. The redirect
costs nothing at runtime beyond reading each loose file's 128-byte header, and
with `[debug] trace=1` the first sixty-four redirects are named, with their
dimensions, in the mod's trace log, followed by a total at shutdown.

`archive_cache_mb` puts a decompressed-block cache in front of the game's own
archive reads. Every `.arc` entry the game loads is stored as a chain of 256
KiB zlib blocks, and the engine's cache for those is a single slot per open
file -- so a read that crosses a block boundary, or that comes back to a block
that slot has since lost, pays a fresh seek, a fresh `ReadFile` and a complete
256 KiB inflate. Against `Resources/Levels.arc`, which is one 2 GB entry of
7,646 blocks holding every level of every act, two measured sessions inflated
1.88 GiB to serve 1.03 GiB in Eternal Embers and 1.14 GiB to serve 0.48 GiB in
Greece -- 4.4 and 2.1 seconds of a hundred-second session, all of it inside the
game's render pass.

The value is a size in MiB, up to 256, held as a fixed slab of 256 KiB slots
with a clock victim. It defaults to the measured useful ceiling of `8`; at
`0`, nothing is allocated and the block routine is left exactly as the game
ships it. A `verify` suffix --
`archive_cache_mb=8verify` -- commits the slab but never serves from it: every
block is read and inflated by the engine as usual and then compared, byte for
byte, against what the cache holds for it. That run costs what an uncached run
costs and proves the cache never returns a wrong block instead of asserting it;
any disagreement disables the cache for the rest of the session and says so in
`tqflicker-hdr.log`. It is worth one boot before trusting a cached one.

**It buys less than the amplification above suggests, and that is measured
rather than estimated.** Two verification boots on that route found the cache
never returns a wrong block — 914 blocks compared byte for byte, no
disagreements — and also found that only 3.8% of block requests at 8 MiB, and
8.2% at 256 MiB, come back for a block the slab still holds. With a
quarter-gigabyte cache resident, 91.8% of requests are for a block nothing has
seen before: the amplification is mostly blocks being *partly consumed* and
never asked for again, which no cache of any size recovers.

Before trusting the default on a different executable or archive set,
`archive_cache_mb=8verify` with `[debug] trace=1` and no `performance_trace`
is worth a few hours of ordinary play: the cache installs on its own, never
serves, and compares every block byte for byte against what the engine
produced. Its log reports back off after the eighth, so a long session writes
about a kilobyte. `archive_cache_mb=0` remains the immediate stock-path
rollback.

What reuse there is clusters in the frames that hurt — 19.4% of blocks in the
archive-heaviest frame against 8.2% overall — and nearly all of it fits in 32
slots, so `archive_cache_mb=8` captures most of the available benefit and
larger values are close to pointless. Expect roughly a ninth off the single
worst frame of a session and about 0.15% of wall clock.

The cache is keyed on the archive, the open file handle, and the block's
offset, compressed size and decompressed size -- all five read out of the
operands of the seek, the read and the inflate that consume them, and all five
re-checked against the installed `Engine.dll` before anything is patched. With
the performance probe on, `arc_cache_hit` against `engine_arc_blocks` is the
result, `arc_cache_evict` says whether the slab is too small, and
`arc_cache_bad` must be zero.

`shadow_defer_cold_resources=1` changes only exact `GraphicsMeshInstance`
casters in the directional shadow map. This is the regular caster/material
fix, not an Actor-only optimization. The renderer calls
`GraphicsMeshInstance::GetNumShadowRenderPasses()`, which synchronously ensures
the root mesh merely to read its pass count, before even asking for shadow style.
A root mesh that is still unloaded or loading now makes that caster report
zero passes; an unloaded mesh is explicitly queued through the engine's normal
preload path, and the caster returns automatically when it is resident. This
applies to opaque and alpha-tested mesh-instance casters. Resident roots take
the normal pass-count path; normal colour rendering and point shadows are
unchanged.

For an alpha-tested caster whose root is resident but whose base texture is
still unloaded or loading, the later shadow-record construction gate omits
that caster/pass and queues an unloaded texture.
Opaque resident casters still cast normally, but their material textures are
not loaded when the active shadow shader has no matching parameter. The same
check also covers an instance's optional `bumpTexture` override: stock code
ensured that Resource before discovering that a directional-shadow shader
had no bump input. The
generic mesh `baseTexture` is also omitted in the directional pass when the
same instance has a distinct non-null base override that the verified stock
path immediately ensures and binds to the same Name. The normal colour pass is
unchanged, and a shadow shader that does use the bump texture still takes the
stock path. The temporary alpha trade is a missing cutout shadow rather than
solid-looking foliage or a missing visible object. It defaults to `1`, works
with the performance probe off, and installs no trace group by itself.
Run 51 found no visible flicker or shadow popping from the texture omission.
Run 59 then removed every directional-shadow texture load in play, leaving
cold root meshes and GPU work as the marked burst. These resource fixes are
part of the subsequently accepted Run 84–85 combination, but are not claimed
to remove the complete burst alone. Temporary local-shadow omission remains
their deliberate quality trade.

`shadow_defer_cold_actor_pose=1` adds another interception point, moving the
cold-root decision to the earlier exact `Actor::AddToScene` call used while
`GraphicsShadowMapDx11::RenderDirectional` gathers its scene. Stock code calls
`Actor::UpdateMeshInstance`, which enters `GraphicsMeshInstance::UpdatePose`
and synchronously loads the root mesh before the later caster gate can see it
as cold. The switch queues a state-0 root and skips that pose update for this
directional gather only after the root is already loading, already linked to a
queue, or the new queue link is confirmed. If queuing cannot be confirmed—or
if the request makes the root resident immediately—it runs the stock pose
update instead. The existing exact-class root gate then omits a confirmed-cold
caster until residency reaches state 2. It implies the complete
`shadow_defer_cold_resources` patch set, defaults to `1`, works with the performance
probe off, and installs no trace group by itself. Color rendering, point
shadows, resident actors, and every other `Actor::UpdateMeshInstance` caller
remain stock. With tracing enabled, the `engine_shadow_actor_pose_*` count
columns show the exact state and enqueue outcome. Run 69 removed the targeted
synchronous directional loads and exposed the need for the stock fallback on
an unconfirmed enqueue.

`terrain_preload_layers=1` fixes the runtime terrain path's omitted semantic
preload. `TerrainRT::LoadRenderData` creates each layer `TerrainType`'s base,
bump, and grass texture Resources during loading, but `TerrainRT::PreLoad`
never queues those layer Resources. The switch retargets the exact existing
`LoadTextures` call: after the original returns, it calls the game's stock
`TerrainType::PreLoad(true)` on that same object. That method uses the normal
background ResourceLoaders and does not wait. The switch defaults to `1`,
works with the performance probe off, and installs no trace group by itself.
It does not omit colour or shadows, change culling, or replace resource
loading; it moves the texture queue request from first colour use to the point
where those Resources first exist.

`mesh_preload_refresh=1` reduces reloads of scenery that was preloaded successfully
but became stale before the player reached it. At an existing main-thread Actor
preload visit requesting material dependencies, a resident root untouched for
at least 400 Engine frames can advance the normal Entity preload countdown.
The native Actor then refreshes the mesh and its dependencies. At most eight
visits per Engine frame are accelerated; ordinary visits already due still run.
This works with tracing off and defaults to `1`; `0` restores stock timing.
It adds no Present work, lock, timer query, allocation, or resource sweep.
Native dependency work can occur earlier and resources can remain resident
longer within the existing eviction rules. The fix also removes the 200-frame
requeue cooldown from the game's two automatic mesh/texture age-eviction calls:
a renewed native preload request can queue an evicted asset immediately.
The 800-frame touched-age and 1,600-frame used-age thresholds, memory-pressure
eviction, and other unload callers retain stock behavior. These two one-byte
patches add no per-frame callback. Continued native preload
interest can keep a mesh resident; once those visits stop, this fix stops
refreshing it and normal aging applies. It does not pin resources or retain
every previously visited area's meshes. The bound counts root
visits, not dependency bytes or milliseconds. See the
[matched gameplay validation](research/streaming/gameplay-loading-hitches.md#first-gameplay-validation-of-resident-refresh).

`secondary_pass_admission_budget=N` progressively admits first-use objects to
reflection and directional-shadow drawing. The two secondary consumers share
one budget: an object admitted by reflection does not spend a second slot when
directional shadow sees it. The first `N` previously unseen identities in a
presented frame render normally; identity `N+1` proves an actual backlog and
self-arms deferral without requiring a reflection-buffer or shadow-region
signal. Deferred renderables still run their ordinary Resource and material
preparation, but their `Draw`/`DrawIndexed` calls wait for a later frame's
slot, so postponed GPU first use does not return as one lump on the next
consumer. Normal colour rendering remains unchanged. It defaults to `8`; `0`
is stock/off; accepted positive values are `1` through `64`. The switch works
with the performance probe off, uses the two renderer draw-submission hooks,
and installs no trace group by itself.

Rejected experimental behavior keys have been removed from the current
configuration interface: `async_level_load`, `timer_period_ms`,
`pump_timer_min_ms`, `shadow_transition_reuse`,
`reflection_defer_admission_mesh`, and `reflection_defer_admission_all`. The
first three did not improve their measured class; whole-map shadow reuse
flickered and deferred the cost; and both one-consumer reflection omissions
moved work without changing the felt stutter. Their findings and old CSV
columns remain as the historical record, but old INI entries are ignored.

Accepted anisotropy values are `1` through `16`; use `anisotropy=1` for the
game's original trilinear filtering. Accepted rollback values are `aa=fxaa` and
`shadows=original`, which restores every shadow path at once. The in-game AA
toggle remains authoritative: SMAA replaces the FXAA draw only while the game
has AA enabled. Shadow Quality should remain High for the intended result.

`shadow_split` sets how far the directional map reaches; coverage scales as
`split^1.90`, so a wider split costs texel density and `shadow_map_scale`
(directional) and `shadow_point_map_scale` (point and spot) pay it back. At the
defaults the shadow maps occupy about 384 MB. `shadow_filter=cross` restores
the game's tap placement.

The directional map is the most expensive thing this mod adds, and its cost is
measured rather than guessed: with `[debug] performance_trace` on a 5120x1440
display, in frames averaging 13.9 ms, the directional pass costs **3.73 ms at
`shadow_map_scale=4` (8192 square) and 2.31 ms at `shadow_map_scale=2` (4096
square)**. A quarter of the texels buys back only 38% of the time, because the
pass is bound more by the geometry it rasterises than by the map it rasterises
into. The default stays at 4 for the crisper shadows; `shadow_map_scale=2` is
the way to recover the 1.4 ms. Point and spot maps together cost 0.39 ms.

`shadow_stabilize=off` restores the game's frame-by-frame refit. Stabilization
has two halves and each can be disabled on its own:
`shadow_stabilize_basis=off` lets the light-space basis follow the camera
again, and `shadow_stabilize_steps` (1-64, default 8) sets how finely the
fitted extent is quantized -- fewer steps hold the extent for longer at the
cost of covering more world than the camera needs. Pinning the basis is the
half that matters; snapping without it stabilizes nothing.

`grass=enhanced`, the default, crosses each grass blade with a second card
turned a quarter turn about its own centre, so a field keeps its volume as the
camera comes round to the blades' edges. Density is unchanged; the cost is a
second draw per grass block and twice the grass overdraw. `grass=original`
leaves the game's own grass alone and removes the buffer hooks entirely.

Measured on the same display, grass costs 1.8 ms of GPU time a frame, of which
the second card is **0.04 ms** -- priced by drawing it on alternating frames
against the same terrain and camera. Drawing every blade twice adds under 2%,
so the cost is the blades themselves rather than the crossing, and the crossing
carries no switch of its own.

Use `edge_updates=original` to restore the game's fixed 4:3 entity-update
frustum. The expanded mode changes update coverage only; it does not alter the
camera FOV, far plane, or rendering culling.

The defaults are `hdr=auto` and `tonemap=frostbite`. Existing explicit INI
settings take precedence. `hdr=auto` enables true HDR when the
operating system and active display report HDR support; the enhanced FP16 path
also remains active on an SDR desktop and maps its extended scene highlights
back into SDR. Frostbite is the neutral display mapper: it preserves the game's
graded midtones and color ratios while rolling off only the upper part of the
display range. AgX provides a more modern contrast and highlight response.
Use `tonemap=original` for the complete original path and `hdr=off` to prevent
HDR output.

The enhanced FP16 output path enables DXGI tearing support when available.
With in-game VSync off, windowed and borderless presentation can use it;
VSync-on and exclusive-fullscreen Present calls retain the game's original
behavior. No extra INI setting is needed. Unsupported systems keep the
non-tearing path, and a rejected tearing-capable creation retries FP16 without
that flag before falling back to the original output path.

When a graphics-setting change recreates the device, the mod releases the old
swap chain and rebuilds its visual resources on the replacement device, so
changes such as toggling VSync can retain FP16 output and the visual fixes.

`paper_white_nits` defaults to 203;
`peak_nits=auto` uses the display-reported peak and falls back to 1000 nits when
HDR is available but the report is unusable. A numeric `peak_nits` overrides
automatic detection.

With Frostbite or AgX selected, `bloom=enhanced` replaces Titan Quest's fixed
512x512/8-bit bloom with a quarter-resolution, up-to-five-level float pyramid
sized to the current display aspect ratio. It runs once on every completed game
scene, uses a consistent soft-threshold profile, and does not clamp brightness
above reference white. The old regional bloom parameters and the in-game Bloom
toggle are intentionally ignored; the INI setting is authoritative. The same
bloom runs before display mapping in enhanced SDR and HDR modes. Use
`bloom=original` to restore the game's original regional bloom, or `bloom=off`
to disable bloom entirely. With `tonemap=original`, `bloom=enhanced` safely
falls back to the original bloom because the float post-processing path is not
active; `bloom=off` still disables it. `bloom_strength` controls the enhanced
composite intensity from `0.0` to `4.0` and defaults to `0.85`.

Temporary HDR diagnostics can be enabled for testing:

```ini
[debug]
hdr_debug=1
```

This creates a concise `tqflicker-hdr.log` beside `TQ.exe` and replaces pixels
above reference white with a yellow/orange/red/magenta highlight heatmap. Log
writes are buffered and handled by a worker rather than the render thread.
There is no on-screen legend. Diagnostics are disabled by default and require a
game restart when changed.

To enable live bloom comparison, use:

```ini
[debug]
bloom_toggle=1
```

Press `Ctrl+Shift+B` to switch between Titan Quest's original bloom and enhanced
bloom. The toggle is disabled by default and requires a game restart when the
setting is changed.

To find out *why* a particular frame was slow rather than merely that it was,
enable the performance trace:

```ini
[debug]
performance_trace=1
```

This records, for every frame, where the render thread spent its time (our
Present callback, the grass readback, one streaming upload chunk, shader and
resource creation, the grass rewrite and crossing draw, SMAA, bloom, and the
overlay itself), what the frame was asked to do (draw calls, maps, grass fills
and rotations, adoptions and staged readbacks, uploaded kilobytes, shadow binds
and shadow-fit steps), and what the GPU spent on the directional shadow pass,
the point shadow passes, grass, SMAA, bloom, and six coarse children of the
DX11 deferred renderer. GPU regions are timed with
timestamp queries read back several frames later without ever flushing, so
nothing waits on the GPU.

Rows go to `tqflicker-frames.csv` beside `TQ.exe`, written by a worker through
one file handle retained for the session. Ordinary rows are appended in 250 ms
batches; a half-full 64 KiB buffer wakes the writer early. Do not change this
back to an open/write/close per row: run 44 showed those repeated opens
contending with the game thread's USER calls in wineserver and manufacturing
the pump tail the trace was meant to measure. At `performance_trace=1` only
frames slower than 20 ms are written, each carrying a final `unusual` column
naming how far every field sat from its own median over the preceding sixty
frames -- which is the part that names the cause; the file grows by a few
hundred bytes per hitch, so this mode is cheap enough to leave on.
`performance_trace=full` writes every frame instead, tens of KB a second with
the current columns, for a measurement session. Full mode also times the
game's high-frequency `Draw`, `DrawIndexed`, and `Map` calls; hitch-only mode
does not. At `0`, the default, nothing is measured, allocated, or written;
what remains compiled in is one branch per instrumented call site.

`tools/frames.py cache/run.csv` summarizes one or more runs: the frame-time
distribution split into menu, the load-game frame, loading screen, first world
frame, and play, plus the hitch count and a ranking of which phase dominated
the hitches and what it cost above its own baseline. World entry is the first
frame with `draw_indexed` of 500 or more; the loading screen draws one indexed
primitive a frame, so a whole-file median describes none of those five parts.

Full mode's `draw_submit_ms` and `map_resource_ms` columns bracket the driver
calls themselves, not the surrounding hook. These are the *game's* time, not
the mod's, and `tools/frames.py` reports them separately for that reason. The
CSV retains a `# draw_timing=` header so old and new runs can distinguish a
zero measurement from an unarmed high-frequency clock. There is no separate
setting now: selecting full measurement selects the complete measurement.

`stutter_marker=1` adds a `stutter_marker` column. Press `F12` immediately
after you notice a stutter; you do not need to catch it while it is happening.
The marker recognizes the F12 key-down when the game's existing message pump
retrieves it, adding no second input poll. A value of `1` is therefore a
reaction-time anchor rather than a claim that the marked row itself stuttered.
`tools/frames.py` reports every frame of 40 ms or more in the preceding two
seconds, newest first, with its distance from the marker and its render/pump
split. The marker does nothing without `performance_trace`, and a
`# stutter_marker=F12` CSV header distinguishes an armed run with no presses
from one where the marker was disabled. Marked rows are written even in the
hitches-only trace mode.

While the performance trace is on, the `engine_*` columns report Titan Quest's
own resource work rather than the mod's: forced level loads and how much of
their cost landed on the game's main thread, resource loads and the queue
behind them, region unloads, archive reads, the 256 KiB block inflates under
them, the directional-shadow build and any main-thread resource loads nested
inside it, and the loaded state sampled immediately before each of those
shadow loads. `engine_shadow_res_state0` / `state1` / `state2` / `state_other`
are mutually exclusive; their `_us` partners carry the corresponding complete
`LoadResource` durations. `engine_shadow_res_in_queue` is an overlapping
cross-check, not another bucket to add. State 0 means the shadow traversal
demanded an unloaded resource; state 1 means it met resource work already in
flight. `engine_shadow_res_mesh` / `shader` / `texture` / `type_other` and
their `_us` partners partition those same nested loads by the filename suffix
returned by the engine.

The complementary `engine_res_outside_dir` pair counts every main-thread
`ResourceLoader::LoadResource` call outside the directional build. Its
`render` / `update` / `other` pairs and its `mesh` / `shader` / `texture` /
`type_other` pairs are two independent partitions of that same population.
When F12 is pressed, the trace also writes each such load from the preceding
120 frames to `tqflicker-debug.log`, including its frame, duration, phase,
pre-call state, filename class, engine filename, immediate caller, and a
bounded upstream stack of call-shaped return candidates. Addresses are labeled
by module and RVA only when they are inside a verified module and immediately
follow a valid call instruction; the immediate caller is otherwise explicitly
`unverified`. The upstream list is a raw stack superset rather than a claimed
call stack because this engine omits frame pointers. The fixed 128-record
rolling window is reported by `engine_res_outside_dir_marker_truncated` if it
could not retain the whole marker window. Records are buffered during the
candidate frame and formatted only after F12. Recording still has overhead;
background trace output can also affect scheduling and I/O.

`resource_lifecycle=1` additionally records Actor/mesh preload visits, dependency
requests, queue timing, worker loads, eviction/cooldown and address reuse. It
requires a nonzero `engine_trace` and `performance_trace` enabled. Use `trace=1`,
`performance_trace=full` and `stutter_marker=1` for a complete investigation;
`trace=0` suppresses text output while those observers remain active.
F12 prints up to 1,024 previously unreported cold main-thread demands from the
last 600 frames, including each resource and associated mesh history **before**
the load. History uses a bounded 8,192-entry table; replacements and overwrites
are reported. The seven additional lifecycle hooks are absent by default and
add diagnostic overhead when enabled. The normal refresh fix is independent.
The debug logger keeps one file handle and appends ordinary text every 250 ms,
waking earlier when pending output crosses half capacity. It flushes to disk
at shutdown, rather than after every batch. This reduces diagnostic I/O traffic;
recent buffered output can be lost if the process crashes. Its pending queue
remains bounded, with explicit overflow and line-truncation notices. See the
[lifecycle audit](research/streaming/mesh-preload-lifecycle.md) for interpretation.

The terrain diagnostic adds `engine_terrain_preload` / `_us`, its true/false
argument counts, the two `TerrainType` shader-parameter entry counts, and
`engine_terrain_ground` / `_us`. For each retained outside-directional load,
the F12 log records the exact `TerrainType*`, material/grass path, material
index, and that same object's preload counts and last-call frames as they
stood before the load. `gpu_terrain_ground_ms` brackets the matching DX11
`TerrainRenderInterfaceRT::RenderGround` work with non-blocking timestamp
queries. `engine_terrain_preload_table_overflow` makes an exhausted identity
table explicit rather than indistinguishable from “never preloaded.” This is
passive instrumentation; it neither invokes `PreLoad` nor changes a missing
texture's fallback.

The same trace group also follows the shipping runtime terrain class rather
than the exported editor `Terrain` class. `engine_terrain_rt_load` / `_us`,
`engine_terrain_rt_load_render` / `_us`,
`engine_terrain_rt_load_textures` / `_us`, and
`engine_terrain_rt_preload` / `_us` time the runtime owner's load,
render-data creation, exact per-layer `TerrainType::LoadTextures` call, and
nearby-object preload. `engine_terrain_rt_preload_layers` records how many
layer identities were associated with an owner preload, while
`engine_terrain_rt_layer_overflow` makes the 64-layer diagnostic bound
explicit. `engine_terrain_plug` / `_us` and `engine_terrain_block` / `_us`
time the two unexported color-terrain render classes that call
`TerrainType::SetShaderParams`. Runtime render-data construction retains the
non-blocking game-time `gpu_terrain_rt_load_render_ms` span. TerrainPlug and
TerrainBlock deliberately have CPU counters only: Run 66 proved that issuing
hundreds of timestamp-query ends per frame for those high-frequency classes
was intrusive. The F12 record for an exact cold terrain texture also
retains the first, last, and count of layer attachment, texture admission, and
runtime-owner preload for that same `TerrainType*`. The static identities and
the wider shadow/resource chain are indexed in
`research/streaming/disassembly-targets.md`.

The deferred-render diagnostic adds six ordered `engine_deferred_*` CPU/draw classes:
`geometry`, `shadows`, `lighting`, `resolve`, `late_scene`, and `post`. Each
has a whole-child-call count and `_us` duration, an overlapping `*_draw_us`
total for the game's `Draw` / `DrawIndexed` calls inside that class, and a
matching invocation/site detail. The CPU and draw durations end in `_us`
because they are engine time. `engine_deferred_i1_*` and `_i2_*` split the two
`GraphicsDeferredRendererX::Render` invocations; setup and scene each expose
call, CPU, draw, Resource-load, texture-creation, and buffer-creation fields,
while `other` carries creation/load work elsewhere in that owner. The four
`gpu_deferred_i{1,2}_geometry_{setup,scene}_ms` columns use one non-blocking
pair around each exact geometry child. They are game time and are not charged
to the mod. The earlier six group-wide GPU spans were removed after Run 70
showed that each overlapped both owner invocations.

On F12, this group also reports a bounded identity sample for geometry-heavy
frames in the preceding 120 frames: at most eight frames, twelve slow draws
per frame, and 32 same-frame texture creations. Draw records include their
owner/site, arguments, bound vertex/index buffers, shaders, and first eight
pixel resources. The bindings are maintained by D3D setter hooks; no state
getter, per-draw GPU query, or extra draw clock read is issued.

`engine_shadow_mesh_cold` / `_us` is narrower and overlapping: a state-0 mesh at
`GraphicsMeshInstance::GetNumShadowRenderPasses`, before that caster enters the
directional draw list. `engine_shadow_material_tex` / `_us` is another
overlapping subset: state-0 texture getters reached from the generic material
loop during the directional build. Its `used`, `unused`, and `unknown`
counter/duration pairs partition that subset according to whether the active
shadow shader contains the material parameter name. An `unused` load is work
performed before the game's parameter setter discovers it has nowhere to bind
the texture; `used` includes textures that may affect shadow coverage, such as
opacity maps on cutout foliage. For every `used` load,
`engine_shadow_material_used_style0` through `style5` (or `context_unknown`),
`base_match` / `base_other` / `base_unknown`, and `pass0` / `pass_other` /
`pass_unknown` identify the originating `GraphicsMeshInstance` call.
The verified adapter supplies `pass0` / `pass_other` even when the accepted
record lookup misses; `pass_unknown` means that adapter context itself was
absent, not merely that the record-table join missed.
`base_unknown` is expected for opaque styles: their base texture is not looked
up merely for instrumentation. The overlapping
`engine_shadow_material_lookup_*` pairs explain whether the same-call record
join found the exact base class, another/overriding class, the same instance
under a different pass, or no accepted record. Table overflow is explicit.
The `engine_shadow_material_outer_instance_site` / `outer_other_site`
counter/duration pairs independently partition those used loads by the
enclosing `GraphicsMesh::SetShaderParameters` caller. The seven
`engine_shadow_context_patch_*` counters partition every actual directional
build by the install state of the optional base-`GraphicsMeshInstance` context
call patch; they make a missing dependency, one of three signature failures,
a call-patch failure, or rollback after a material-hook failure visible in the
CSV rather than only in an exit-time log.

When F12 is pressed, the trace also writes state-0 mesh Resource loads from
the preceding 120 frames of the directional build. Each delayed record carries
the engine filename, pre-call queue state, immediate call-shaped caller, and a
bounded list of verified call-shaped upstream candidates. The fixed
128-record ring reports truncation. This is caller attribution only: it adds
no GPU query, per-draw hook, queue operation, or behavior change, and performs
no log formatting on the candidate frame.

Independently, the `engine_shadow_tex_from_*` counter/duration pairs partition
every shadow-nested texture load by the direct `GraphicsTexture::GetTexture`
caller; `unresolved` includes indirect callers. All engine durations end in
`_us`. These columns are diagnostic only and change no resource or rendering
behaviour. The loader-fence wait in `Engine::Update`, the seven
resource-manager
sweeps beside it, any time the render path blocked on a region lock, and
`Engine::Update` and `Engine::Render` bracketed whole so the rest can be read
against the half of the frame they happened in, and `GameEngine::Update` from
`Game.dll` for the simulation that is in neither. Beside them the `loop_*`
columns time every call TQ.exe's own main loop makes that does work rather
than return a pointer -- the online platform pump, music and sound, graphics
options, `Engine::PresentSurface`, the collision fixup, quest triggers, the
window message pump, and its sleep with what it asked for beside what it got
-- and
`proc_avail_va_mib` gauges the free address space. That
last one is why a hitch row that used to say only "38 ms" can now name the
load that caused it. Durations there are microseconds and end in `_us`, so
`frames.py` counts them separately from the mod's own millisecond phases.

Run 48 found the lifecycle split entirely in state 0: all 214 nested shadow
loads in play, totaling 451.588 ms, were unloaded and none had a queue link.
For this class the renderer discovers cold work; it is not joining work the
loader already has in flight.

The `arc_cache_*` columns beside them are the mod's, not the game's: they price
`archive_cache_mb` against the `engine_arc_*` columns it exists to reduce.
`engine_arc_blocks` keeps counting every block the engine asked for, hit or
miss, so a cached session's amplification is still comparable to an uncached
one's; `engine_arc_inflate_us` then covers only the blocks that were actually
read and inflated.

Those columns come from instrumentation written into `Engine.dll`'s own code,
so they are gated twice: no *instrument* is installed unless
`performance_trace` is on **and** `engine_trace` is not `0`, which means a
normal boot installs none of the trace groups. The behavior fixes live in
`archive_hooks.cpp`, `shadow_defer.cpp`, `terrain_preload.cpp`,
`mesh_preload.cpp`, and `secondary_admission.cpp`; `engine_hooks.cpp` coordinates their shared sites.
They install with the probe off. `engine_probe.cpp` owns the optional Engine
observers, `resource_trace.cpp` owns the optional resource lifecycle recorder,
and `probe.cpp` owns the frame recorder.

With `performance_trace=0`, shared behavior hooks bypass the observers. There
are no probe clock reads, GPU queries, frame recording, or trace-only Engine
hooks; even the reflection `BuildScene` observer is absent. The fixes still
need their own hooks, and small inline enable checks remain. A separate DLL
build is unnecessary to disable recording. Full mode enables the same
instrumentation in the same DLL.

Exported sites are resolved by name and checked against their audited RVA;
internal sites use audited RVAs and instruction signatures. Relocated operands
are verified against the loaded module base. Mismatches skip the affected hook
or roll back a dependent group, preserving stock behavior. A build that is not the audited `Engine.dll` installs nothing
at all and says so in `tqflicker-hdr.log`. `engine_trace=1` is everything;
larger values are a mask -- `2` loads, `4` archive reads, `8` the fence, `16`
the region lock, `32` the sweeps, `64` `WaitForLoadingToFinish`, `128` the
update/render brackets, `256` `Game.dll`'s simulation tick, `512` TQ.exe's
main loop, `1024` the inside of the window message pump, `2048`
`Engine.dll`'s array allocator, `4096` the seek and read under the archive
block routine, `8192` everything in `Engine.dll` that can block -- so a run
that misbehaves can be narrowed without a rebuild. `16384` brackets the direct
directional-shadow build; combine it with `2` to populate both the nested-load
totals and their loaded-state/queue lifecycle split. `32768` enables the
atomic `TerrainType`, runtime `TerrainRT`, both color-terrain render classes,
and DX11 ground diagnostic group. `65536` partitions the direct children of
`GraphicsDeferredRendererX::Render` into the six coarse CPU/draw classes and
the owner-exact geometry/resource/D3D identity trace above. `131072` brackets
the unique reflection-manager call in each recursive DX11 portal/region
branch and the first two water-plane forward renders inside each of the first
two manager invocations. It also splits each plane's unique `BuildScene` and
`RenderLightStyle` children and correlates newly created main-thread vertex
and index buffers across exact reflection-plane, directional-shadow, and
deferred-owner scopes for 120 frames. It reuses the existing Resource,
game-draw, D3D-creation clock, and IA setter snapshot; it adds no per-draw
clock, query, or state getter. Non-blocking GPU intervals cover the manager,
plane, and two children. Explicit overflow/eviction counters and the bounded
F12 report prevent a third branch, plane, or lost recent identity from being
silently folded into the named classes. Selecting `131072` therefore installs
the directional and deferred scope brackets as dependencies, while still
changing no rendering choice.

When the terrain (`32768`) and reflection (`131072`) groups are selected in a
full measurement run—as they are under `engine_trace=1`—the trace can arm one
sparse reflection-only GPU subdivision. An exact second-manager/first-plane
`BuildScene` lasting at least 2 ms selects the following whole
`RenderLightStyle`. `gpu_chunk_reflection_00` through `_15` continuously cover
draws 1--320 in 20-draw intervals, including work issued between adjacent
renderables. F12 logs each
interval's draw/index/element totals and first/last tracked shader, SRV0, VB0,
and IB identities. It also logs a bounded list of exact `TerrainPlug`,
`TerrainBlock`, and `GraphicsMeshInstance::RenderPass` calls overlapping that
draw window, with their class, object, draw range, CPU duration, and nested
Resource/texture/buffer creation totals. The first two are existing exact
unexported terrain hooks; the mesh call is its exact exported virtual override.
Directional shadow opens no chunk query and receives no executor patch.
Ordinary frames open none of these queries; collision, draw overflow, and
renderable-call overflow are explicit.

The `512`, `1024`, `2048`, `4096` and `8192` groups patch nothing at all: they
redirect entries of an import address table, so each is scoped to the one
module being measured and every other caller in the process -- the mod's own
included -- keeps the real function.

For startup or crash diagnosis without changing the rendered image, enable the
lightweight trace instead:

```ini
[debug]
trace=1
```

This creates `tqflicker-debug.log` beside `TQ.exe` and records proxy loading,
renderer discovery, device and swap-chain creation, hook installation, and the
first presented frame. `npm run debug-release` builds a trace-enabled archive
that forces this log on through a build define (`TQ_FORCE_TRACE`) without
requiring an INI change; its symbols and linker map remain under `build/debug`
for resolving a reported crash offset.

## Progressive texture uploading

The problem: the game supplies an entire texture's mip chain to
`CreateTexture2D` at once. Large initial-data uploads concentrate work on the
calling thread and GPU queue. Moving resource loading to a worker alone does
not make that GPU work disappear.

The fix (`streaming=optimized`, the default): for eligible BC1/BC2/BC3
textures on the verified mapped loose-file path, create the full-size texture
with its small mips populated and substitute a low-detail shader view. That
view starts at the first mip no larger than 512 pixels on either axis (or
the last mip). Before each Present, upload one chunk of one pending job on
the render thread. Restore the full view when all withheld mip data is ready.
Visible textures can therefore look soft temporarily; final resolution is
unchanged.

The controller uses high-resolution timings to aim for roughly 3 ms of CPU
time per upload step, adjusting within a 256 KiB–2 MiB range, with a smaller
per-job ceiling for smaller textures. This is a feedback target, not a hard
3 ms limit: source-page faults or a driver wait can still make a chunk slow.
The timing drives the fix and remains active with `performance_trace=0`;
optional reporting does not drive it.

Reference-counted leases keep the loose file's mapping alive until the engine
has finished with it and its upload jobs complete, avoiding another full-size
source copy. This retains address space while jobs are pending. Despite old
internal names containing “archive,” this path does **not** handle decompressed
`.arc` texture payloads; those keep stock creation. Unsupported candidates,
failed ownership checks, or unavailable job/lease capacity fall back to stock.
Use `streaming=original` to disable this optimization. It preserves the game's
level/entity preload distances and does not move D3D11 immediate-context work
to a worker thread. This uploader and the secondary-pass object budget address
different stages; neither is a replacement for the other.

Set `[debug] frame_overlay=1` to show a frame-pacing overlay for A/B tests
(the key's old `[performance]` home is still honoured). It reports
the active streaming mode, the current frame time and FPS, a rolling average,
the 99th percentile, the worst frame, and a hitch count above 25 ms, over a
window of the last 4,096 presented frames -- about 68 seconds at 60 FPS, and
the panel states the time the window actually covers. Below it, a graph of the
same window is drawn one screen pixel per column at the display's own
resolution; while there are fewer frames than columns each column is one frame,
and past that a column reports the worst frame it covers, so a spike is never
averaged or sampled away. The border is cyan for optimized streaming and orange
for the original path. At `0`, the default, nothing is measured, allocated, or
drawn.

For a measurement run, leave the overlay off and use `performance_trace`
instead: drawing the panel costs a full pipeline save and restore on every
frame and an upload several times a second, and both land inside the very
frame times being recorded.

The shadow enhancement does not quantize per-object matrices. Shimmer is
addressed at its source instead: the directional light projection is quantized
and snapped onto the shadow map's texel grid once per frame, before the engine
derives either the caster projection or the receiver's world-to-shadow matrix
from it, so the two cannot disagree and object geometry is never touched.

## Build and install

```sh
npm run doctor
npm run build
npm run selftest
npm run install-dll
```

`npm run release` performs a fresh build and creates
`dist/tq-dx11-fix-v<version>.zip` with `winmm.dll` at the archive root, ready
for distribution. The version is read from `package.json`.

On native Windows, extract `winmm.dll` beside `TQ.exe`. The proxy exposes the
union of the named 32-bit WinMM exports found on native Windows and
CrossOver/Wine, then resolves whichever implementations the host provides. All
WinMM functions imported by Titan Quest and its bundled runtime DLLs are
required on both platforms; platform-specific compatibility exports do not
abort game startup when they are unavailable.

Frame callbacks are installed in Titan Quest's signature-verified renderer
wrapper rather than in the shared `IDXGISwapChain::Present` vtable. Steam,
THQN, and driver overlays therefore retain their normal DXGI hook ownership and
ordering. An unknown renderer build is left untouched and presentation-dependent
enhancements fail back to the game's original path.

`npm run uninstall-dll` removes the proxy and the TQ-specific Wine override.

The tested environment is Titan Quest Anniversary Edition (32-bit GOG build),
CrossOver Preview with DXMT, and Apple Silicon. The regression test runs all 37
captured skinning variants, a captured shadow receiver, and a complete captured
FXAA replacement draw through the real 32-bit DXMT device. It also verifies
shadow resource selection, reflection-pass isolation, and pipeline-state
restoration. Bloom tests validate both exact Engine callers, the unclipped
extraction curve, and every runtime bloom shader. The build also validates the
proxy's named export surface against
native Windows x86 and CrossOver WinMM reference DLLs when those references are
available locally. Native Windows runtime testing is still required before
calling that environment fully validated.

The supplied HekTo-modified `Game.dll` is also supported by the optional
`GameEngine::Update` timer and Game caller attribution. Its existing update
wrapper and callback are preserved after exact layout checks; the widescreen
frustum hook already recognizes its unchanged call site. See the
[binary audit and validation limits](research/shadows/supported-build.md#supplied-hekto-gamedll-variant).
This compatibility support does not establish a fix for the reported fountain
crash.

## Third-party code

The SMAA shader and area/search lookup textures under `third_party/smaa` are
from the official SMAA repository at revision
`71c806a838bdd7d517df19192a20f0c61b3ca29d` and retain its MIT license.
