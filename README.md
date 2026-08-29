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
- doubles eligible square shadow depth maps and their viewport/scissor;
- changes TQ's four cardinal bilinear shadow taps into the corners of an
  optimized 3x3 PCF footprint.

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

Visual upgrades are enabled when no configuration file exists. To select the
original game paths, create `tqflicker.ini` beside `TQ.exe`:

```ini
[graphics]
aa=smaa
shadows=enhanced
edge_updates=expanded
```

Accepted rollback values are `aa=fxaa` and `shadows=original`. The in-game AA
toggle remains authoritative: SMAA replaces the FXAA draw only while the game
has AA enabled. Shadow Quality should remain High for the intended result.
Use `edge_updates=original` to restore the game's fixed 4:3 entity-update
frustum. The expanded mode changes update coverage only; it does not alter the
camera FOV, far plane, or rendering culling.

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
