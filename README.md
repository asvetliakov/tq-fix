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
- doubles eligible square shadow depth maps and their viewport/scissor;
- changes TQ's four cardinal bilinear shadow taps into the corners of an
  optimized 3x3 PCF footprint.
- progressively uploads large streamed terrain textures in bounded,
  frame-paced chunks instead of submitting every high-resolution mip at once.
- keeps the final scene/post-process chain in FP16 and applies either a
  look-preserving Frostbite-style display mapper or a modern AgX-derived output
  transform for SDR and HDR displays.

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
edge_updates=expanded
hdr=auto
tonemap=frostbite
paper_white_nits=203
peak_nits=auto

[performance]
streaming=optimized
```

Accepted anisotropy values are `1` through `16`; use `anisotropy=1` for the
game's original trilinear filtering. Accepted rollback values are `aa=fxaa` and
`shadows=original`. The in-game AA toggle remains authoritative: SMAA replaces
the FXAA draw only while the game has AA enabled. Shadow Quality should remain
High for the intended result.
Use `edge_updates=original` to restore the game's fixed 4:3 entity-update
frustum. The expanded mode changes update coverage only; it does not alter the
camera FOV, far plane, or rendering culling.

`hdr=auto` enables true HDR when the operating system and active display report
HDR support. The enhanced FP16 path remains active on an SDR desktop and maps its
extended scene highlights back into SDR. Frostbite is the default neutral
display mapper: it preserves the game's graded midtones and color ratios while
rolling off only the upper part of the display range. Use `tonemap=agx` for a
more modern contrast and highlight response, or `tonemap=original` for the
complete original 8-bit color-output path. Use `hdr=off` to force SDR output.
`paper_white_nits` defaults to 203;
`peak_nits=auto` uses the display-reported peak and falls back to 1000 nits when
HDR is available but the report is unusable. A numeric `peak_nits` overrides
automatic detection.

Temporary HDR diagnostics can be enabled for testing:

```ini
[debug]
hdr_debug=1
```

This creates `tqflicker-hdr.log` beside `TQ.exe` and replaces pixels above
reference white with a yellow/orange/red/magenta highlight heatmap. There is no
on-screen legend. Diagnostics are disabled by default and require a game
restart when changed.

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

The shadow enhancement deliberately does not quantize per-object matrices. No
safely isolated global light-projection upload has been established, so the
resolution and PCF changes address shimmer without risking geometry stepping or
deformation.

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

`npm run uninstall-dll` removes the proxy and the TQ-specific Wine override.

The tested environment is Titan Quest Anniversary Edition (32-bit GOG build),
CrossOver Preview with DXMT, and Apple Silicon. The regression test runs all 37
captured skinning variants, a captured shadow receiver, and a complete captured
FXAA replacement draw through the real 32-bit DXMT device. It also verifies
shadow resource selection, reflection-pass isolation, and pipeline-state
restoration.

## Third-party code

The SMAA shader and area/search lookup textures under `third_party/smaa` are
from the official SMAA repository at revision
`71c806a838bdd7d517df19192a20f0c61b3ca29d` and retain its MIT license.
