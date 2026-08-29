# Titan Quest DX11 Flicker Fix

A minimal fix for flickering skinned objects in Titan Quest Anniversary Edition
when its DirectX 11 renderer runs through CrossOver/DXMT.

Titan Quest sometimes supplies bone indices outside its 27-entry animation
matrix array. DXMT can read undefined constant-buffer data for those indices,
making an object disappear for a frame. This DLL recognizes the affected Titan
Quest vertex shaders and inserts one integer clamp before the bone lookup.

The runtime consists of:

- a `winmm.dll` proxy that loads the fix into `TQ.exe`;
- one hook for D3D11 device creation;
- one hook for `CreateVertexShader`;
- the narrow DXBC transformer.

There are no settings, modes, log files, per-frame hooks, buffer copies, waits,
or rendering synchronizations.

## Build and install

```sh
npm run doctor
npm run build
npm run selftest
npm run install-dll
```

`npm run uninstall-dll` removes the proxy and the TQ-specific Wine override.

The tested environment is Titan Quest Anniversary Edition (32-bit GOG build),
CrossOver Preview with DXMT, and Apple Silicon. The regression test runs all 37
captured shader variants through the real 32-bit DXMT device.
