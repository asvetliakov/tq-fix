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
transformers. It performs no buffer readbacks, GPU waits, or rendering
synchronization. Resolution-dependent AA targets are reused.

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
edge_updates=expanded
bloom=enhanced
bloom_strength=0.85
hdr=off
tonemap=original
paper_white_nits=203
peak_nits=auto

[performance]
streaming=optimized
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

`shadow_stabilize=off` restores the game's frame-by-frame refit. Stabilization
has two halves and each can be disabled on its own:
`shadow_stabilize_basis=off` lets the light-space basis follow the camera
again, and `shadow_stabilize_steps` (1-64, default 8) sets how finely the
fitted extent is quantized -- fewer steps hold the extent for longer at the
cost of covering more world than the camera needs. Pinning the basis is the
half that matters; snapping without it stabilizes nothing.

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

For startup or crash diagnosis without changing the rendered image, enable the
lightweight trace instead:

```ini
[debug]
trace=1
```

This creates `tqflicker-debug.log` beside `TQ.exe` and records proxy loading,
renderer discovery, device and swap-chain creation, hook installation, and the
first presented frame. `npm run debug-release` builds a trace-enabled archive
that forces this log on without requiring an INI change; its symbols and linker
map remain under `build/debug` for resolving a reported crash offset.

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
