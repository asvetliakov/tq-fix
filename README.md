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
- progressively uploads large streamed terrain textures in bounded,
  frame-paced chunks instead of submitting every high-resolution mip at once.
- can keep the final scene/post-process chain in FP16 and apply either a
  look-preserving Frostbite-style display mapper or a modern AgX-derived output
  transform for SDR and HDR displays when explicitly enabled.
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
hdr=off
tonemap=original
paper_white_nits=203
peak_nits=auto

[performance]
streaming=optimized

[debug]
frame_overlay=0
trace=0
performance_trace=0
```

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

HDR and custom tone mapping are disabled by default, retaining the complete
original 8-bit color-output path. To opt in, set `hdr=auto` and select
`tonemap=frostbite` or `tonemap=agx`. `hdr=auto` enables true HDR when the
operating system and active display report HDR support; the enhanced FP16 path
also remains active on an SDR desktop and maps its extended scene highlights
back into SDR. Frostbite is the neutral display mapper: it preserves the game's
graded midtones and color ratios while rolling off only the upper part of the
display range. AgX provides a more modern contrast and highlight response.
Use `tonemap=original` for the complete original path and `hdr=off` to prevent
HDR output.
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
the point shadow passes, grass, SMAA and bloom. GPU regions are timed with
timestamp queries read back several frames later without ever flushing, so
nothing waits on the GPU.

Rows go to `tqflicker-frames.csv` beside `TQ.exe`, written by a worker that
appends rather than rewriting. At `performance_trace=1` only frames slower
than 20 ms are written, each carrying a final `unusual` column naming how far
every field sat from its own median over the preceding sixty frames -- which is
the part that names the cause; the file grows by a few hundred bytes per hitch,
so this mode is cheap enough to leave on. `performance_trace=full` writes every
frame instead, about 12 KB a second, for a measurement session. At `0`, the
default, nothing is measured, allocated, or written; what remains compiled in
is one branch per instrumented call site.

`tools/frames.py cache/run.csv` summarizes one or more runs: the frame-time
distribution, the hitch count, and a ranking of which phase dominated the
hitches and what it cost above its own baseline.

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

Streaming keeps the game's original level/entity preload distances. Large
eligible BC1/BC2/BC3 terrain textures are created with their low mips ready
immediately, then their high mips are uploaded progressively using an adaptive
512 KiB–2 MiB frame budget. The original mapped archive data is retained only
until those uploads finish, avoiding an extra texture-sized copy. Exact Engine
and renderer build checks guard this path; any mismatch or resource-pressure
condition falls back to the game's original synchronous upload. Use
`streaming=original` to disable the optimization. The mod does not replace the
resource thread or move D3D11 immediate-context work onto an unsafe worker
thread.

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

## Third-party code

The SMAA shader and area/search lookup textures under `third_party/smaa` are
from the official SMAA repository at revision
`71c806a838bdd7d517df19192a20f0c61b3ca29d` and retain its MIT license.
