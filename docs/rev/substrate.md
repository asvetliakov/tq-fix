# Substrate — where this code runs

Everything here was read off the machine on 2026-08-25. Re-verify the version
numbers after any CrossOver update; the rest is stable.

## Host

- macOS on **Apple M5 Pro** (as reported to the guest by the graphics backend).
- **CrossOver Preview 27.0.0**, build `20260821`,
  `/Applications/CrossOver Preview.app`.
- Bottle **"New Bottle"** —
  `~/Library/Application Support/CrossOver/Bottles/New Bottle`.
  Contains Steam and Titan Quest Anniversary Edition and nothing else of note.

## Guest

- **`TQ.exe` is `PE32` — 32-bit x86.** This is the single most consequential
  fact in the repo. It follows that:
  - the game runs under **FEX** (`lib/wine/aarch64-unix/libwow64fex.so`), not
    Rosetta, so `FEX_*` tunables apply to it;
  - our DLL must be built with **`i686-w64-mingw32-g++`**, and its exports carry
    stdcall decoration;
  - the 32-bit DXMT is what serves it: `lib/dxmt/i386-windows/{d3d11,d3d10core,dxgi,winemetal}.dll`.
- Built with **MSVC 11.0** — `MSVCR110.dll` / `MSVCP110.dll`, both set to
  `native, builtin` in the bottle's registry.
- Game build stamp `TQ_22072022_2.10.21415.build` → TQAE **2.10.21415**,
  Steam appid **475150**. Steam launch options for it are **empty**.
- There is **no CrossOver compat-DB entry** for `TQ.exe`, so nothing is being
  silently overridden on our behalf.

## The two renderers

The game ships both, and picks between them by command line:

| Renderer file     | Links against                                | Switch  |
|-------------------|----------------------------------------------|---------|
| `Direct3D.dll`    | `d3d9.dll`, `d3dx9_42.dll`                   | `/dx9`  |
| `Direct3D11.dll`  | `d3d11.dll`, `dxgi.dll`, `D3DCOMPILER_43.dll`| `/dx11` |

`TQ.exe` also accepts `-borderless`, `-nohwcursor`, `/debug`, `/map`, `/player`,
`/exec`, `/backup`.

`d3dx9_42.dll` **is already installed** in the bottle's `system32` (2.4 MB, the
real Microsoft redist), so the `/dx9` path will start. `D3DCOMPILER_43.dll` is
likewise present, which means `Direct3D11.dll` compiles HLSL **at runtime** and
`D3DReflect` is available to us inside the process for free.

`Engine.dll` loads the renderer by name at runtime (the string `Direct3D11` is
in it), so anything we hook in `Direct3D11.dll` must wait for that module.

## Graphics backends

`CX_GRAPHICS_BACKEND` accepts `auto`, `dxmt`, `dxvk`, `d3dmetal`, `wine`
(read out of `lib/python/bottlewrapper.pyc`). Observed on this machine, all on
the DX11 renderer:

| Backend     | Result                          |
|-------------|---------------------------------|
| `dxmt`      | **Runs, with the flicker**      |
| `dxvk`      | Black screen                    |
| `d3dmetal`  | Black screen; also not offered in the Preview's UI selector |
| `wine`      | Black screen                    |

**`/dx9` has never been tried, on any backend.** See
`docs/plans/stage-0-free-experiments.md`.

## DXMT

The complete tunable surface, pulled out of the shipped DLL — there are only
seven keys, and **none of them is a constant-buffer range check or a border
colour override**:

```
d3d11.defuseFma              d3d11.ignoreMapFlagNoWait
d3d11.maxFeatureLevel        d3d11.metalSpatialUpscaleFactor
d3d11.preferredMaxFrameRate  d3d11.sampleNaNToZero
dxmt.shaderMetalVersion
```

Environment variables it reads:

```
DXMT_CONFIG                DXMT_CONFIG_FILE          (also a dxmt.conf beside the exe)
DXMT_LOG_LEVEL             DXMT_LOG_PATH
DXMT_SHADER_CACHE_PATH     DXMT_USE_DEFAULT_METAL_CACHE   (dxgi)
DXMT_CAPTURE_EXECUTABLE    DXMT_CAPTURE_FRAME
DXMT_METALFX_SPATIAL_SWAPCHAIN                        DXMT_ENABLE_NVEXT
```

`DXMT_LOG_LEVEL` takes `trace|debug|info|warn|error|none`.
**`DXMT_LOG_PATH` must name a directory that already exists** — DXMT silently
writes nothing otherwise, which cost one launch to discover. It then writes
`<exe>_d3d11.log` and `<exe>_dxgi.log` into it, fresh each launch.

`DXMT_CAPTURE_EXECUTABLE` + `DXMT_CAPTURE_FRAME` (with `MTL_CAPTURE_ENABLED=1`)
produce a Metal `.gputrace` openable in Xcode. **This is the only real graphics
debugger available here** and it has not been used yet.

## Bottle environment as configured

```ini
[EnvironmentVariables]
"CX_GRAPHICS_BACKEND" = "dxmt"
"WINEMSYNC" = "1"
"D3DM_ENABLE_METALFX" = "0"
"DXMT_ENABLE_NVEXT" = "0"
"FEX_X87REDUCEDPRECISION" = "1"
"DXMT_LOG_LEVEL" = "info"
"DXMT_LOG_PATH" = "C:\\dxmtlog"
```

CrossOver **rewrites `cxbottle.conf` when it exits**, so quit CrossOver before
hand-editing that file or the edits are lost.

`FEX_X87REDUCEDPRECISION=1` is set and **has never been tested at `0`**. Prior is
low — 32-bit MSVC 11.0 defaults to SSE2 codegen, so little of the hot float math
is x87 — but it is one flip.

## Injection surface

| Module            | Imports (relevant)                              |
|-------------------|-------------------------------------------------|
| `TQ.exe`          | **`WINMM.dll`**                                 |
| `Engine.dll`      | `DINPUT8.dll`, `WINMM.dll`, `XINPUT1_3.dll`     |
| `Direct3D11.dll`  | `d3d11.dll`, `dxgi.dll`, `D3DCOMPILER_43.dll`   |

`TQ.exe` importing `WINMM.dll` directly means the **`winmm.dll` proxy vector from
`../grimdawn-trash` transfers**, and `Direct3D11.dll` importing `d3d11.dll`
*statically* means `D3D11CreateDevice` is reachable by an **IAT data write** once
that module is loaded.

That D3D11 hooking works at all in this bottle is not speculation: the game's own
THQ Nordic overlay (Indicium-Supra + ImGui, in `THQNOnline/overlay/`) hooked
`IDXGISwapChain::Present`, took the device and context pointers and rendered
through DXMT successfully on all 16 logged runs.

## Toolchain

Both MinGW cross-compilers are installed at `/opt/homebrew/bin`:
`i686-w64-mingw32-g++` (**the one we need**) and `x86_64-w64-mingw32-g++`.

## Assets

`Resources/Shaders.arc` is **ARC version 1**, 134 entries, contents compressed —
no HLSL text survives a `strings` pass. `../grimdawn-core/src/db/arc.ts` reads
Grim Dawn's **ARC version 3** and hard-asserts on the version field, so it will
not open this without work. The "just edit the shader source" route is not
sitting there waiting.
