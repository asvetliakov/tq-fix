# Substrate — where this code runs

Everything here was read off the machine on 2026-08-25. Re-verify the version
numbers after any CrossOver update; the rest is stable.

## Host

- macOS on **Apple M5 Pro** (as reported to the guest by the graphics backend).
- **CrossOver Preview 27.0.0**, build `20260821`,
  `/Applications/CrossOver Preview.app`.
- Bottle **"Titan Quest"** —
  `~/Library/Application Support/CrossOver/Bottles/Titan Quest`.
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

**`/dx9` has never been tried, on any backend** — and it is now **descoped**
rather than pending. The requirement is that the DX11 renderer work, so falling
back to D3D9 is not an acceptable outcome even if it renders perfectly (D1 in
`observed.md`). It remains the cheapest unspent experiment in the repo should
the DX11 route ever prove impossible.

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

`DXMT_LOG_LEVEL` takes `trace|debug|info|warn|error|none`. **`debug` prints no
more than `info`** (O3) but **`trace` prints substantially more** (O16) — 526
extra lines in a short session. Its entire vocabulary is five messages:
`Start compiling 1 PSO`, `Compiled 1 PSO`, `staging map ready`, command-queue
construct/destruct, and the init/warning lines. **There is no per-draw, per-frame
or per-`Present` logging at any level**, so the trace log cannot be turned into a
frame trace.
**`DXMT_LOG_PATH` must name a directory that already exists** — DXMT silently
writes nothing otherwise, which cost one launch to discover. It then writes
`<exe>_d3d11.log` and `<exe>_dxgi.log` into it, fresh each launch.

`DXMT_CAPTURE_EXECUTABLE` + `DXMT_CAPTURE_FRAME` (with `MTL_CAPTURE_ENABLED=1`)
produce a Metal `.gputrace` openable in Xcode. **This is the only real graphics
debugger available here** and it has not been used yet — it is Stage 1.

The implementation, from symbols in the shipped 32-bit `d3d11.dll`:

```
dxmt::CaptureState::shouldCaptureNextFrame()
dxmt::CaptureState::scheduleNextFrameCapture(unsigned long long)
dxmt::CaptureState::getNextAction(unsigned long long)
MTLCaptureManager_{sharedCaptureManager,startCapture,stopCapture}
"DXMT capture enabled"   "A new capture will be saved to "
_%H'%M'%S_%m-%d-%y.gputrace
```

`scheduleNextFrameCapture` taking a frame number means the capture is **keyed on
a frame index**. **Whether `DXMT_CAPTURE_FRAME` accepts a count or a range is not
known** — Stage 1 must find out and record the answer here, because nothing
documents it and a range would make catching a defective frame far cheaper.

**A `dxmt.conf` placed beside `TQ.exe` is read** and is the preferred way to set
config keys — no `cxbottle.conf` edit, so no quitting CrossOver. Confirmed by
DXMT itself: `TQ_dxgi.log` prints `info: Found config file: dxmt.conf` (O11a).

## Vtable layout, as the DLL uses it

Read off the MinGW `*Vtbl` structs by `scripts/gen-slots.sh` at build time into
`build/gen/slots.h`; never typed by hand. Confirmed in the running DXMT by the
off-game self-test (O28): the patched slots are the ones that fire.

| Interface (methods) | Slot | Method |
|---|---|---|
| `IDXGISwapChain` (18) | 8 | `Present` |
| `ID3D11DeviceContext` (115) | 12 / 13 / 20 / 21 | `DrawIndexed` / `Draw` / `DrawIndexedInstanced` / `DrawInstanced` |
| | 38 / 39 / 40 | `DrawAuto` / `DrawIndexedInstancedIndirect` / `DrawInstancedIndirect` |
| | 14 / 15 / 58 | `Map` / `Unmap` / `ExecuteCommandList` |
| `ID3D11Device` (43) | 3 / 12 / 15 / 23 | `CreateBuffer` / `CreateVertexShader` / `CreatePixelShader` / `CreateSamplerState` |

`ID3D11Device1` and `ID3D11DeviceContext1` are the same objects with the same
vtables as their base interfaces under DXMT (O20, O28).

`D3DReflect` is taken from the `D3DCOMPILER_43.dll` already in the process; its
`IID_ID3D11ShaderReflection` is the compiler-43 one,
`{0a233719-3960-4578-9d7c-203b8b1d9cc1}` (47's differs).

## Our own files and switches

| | |
|---|---|
| `%TEMP%\tqflicker.log` | the main log, appended forever, pid on every line |
| `%TEMP%\tqflicker-frames.log` | the per-frame table, **truncated at device creation** — keep it with `npm run keep-log` before relaunching |
| `TQFLICKER_HOOK=0` | install nothing (the control); reaches the renderer only via a Steam started under `cxstart` (O24) |
| `TQFLICKER_D3D_HOST=<module>` | hook that module's `d3d11.dll` import instead of `Direct3D11.dll`'s — the off-game self-test |
| `TQFLICKER_LOG=<path>` | log somewhere else — the self-test again |

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

**For experiments, do not edit it at all.** Environment variables can be injected
per-run and layered on top of the bottle's own, with CrossOver already running
(O15):

```sh
CX="/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver"
G="C:\Program Files (x86)\Steam\steamapps\common\Titan Quest Anniversary Edition"
DXMT_LOG_LEVEL=trace "$CX/bin/cxstart" --bottle "Titan Quest" --no-convert \
  --workdir "$G" -- "$G\TQ.exe"
```

**Launch `TQ.exe` directly, not from the Steam UI** — a game started by Steam is
a child of the running Steam process and inherits *its* environment, not ours.
Steam must still be running in the background.

`FEX_X87REDUCEDPRECISION=1` is set. **Tested at `0`: no effect on the flicker**
(O8). The prior was low — 32-bit MSVC 11.0 defaults to SSE2 codegen, so little of
the hot float math is x87 — and the experiment agreed. Do not re-test.

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
