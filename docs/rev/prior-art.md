# Prior art — who has already done part of this

## DXVK's per-application profile for `TQ.exe`

DXVK carries a compiled-in app-profile table and matches on executable name. For
`TQ.exe` it applies:

```
d3d11.constantBufferRangeCheck = True
```

That option exists to emulate a D3D11 guarantee the underlying API does not
provide: **reads past the end of a bound constant buffer return zero.** Its
presence tells us DXVK's authors found Titan Quest binding constant buffers
smaller than its shaders declare, and had to work around it.

DXMT has no equivalent (`docs/rev/substrate.md`), which is why the same class of
bug would be visible here and not on Windows or on Linux/DXVK. See H-B1 in
`observed.md`.

**Read the profile line, not the render log** — the reasoning is in O6.

## DXMT

CodeWeavers' D3D11→Metal translation, shipped inside CrossOver at
`Contents/SharedSupport/CrossOver/lib/dxmt/`. Seven config keys, ten environment
variables, a `dxmt.conf` file mechanism, and a Metal frame capture hook. All of
it enumerated in `substrate.md` by reading strings out of the shipped DLLs,
because there is no documentation in the bundle.

**It is open source, and the shipped build is a fork of it.** `3Shain/dxmt` on
GitHub; the bundle's `d3d11.dll` reports `v0.80-131-g2befd18`, a CodeWeavers
commit 131 past the public `v0.80` tag that does not exist upstream. Upstream's
d3d11 side has barely changed since v0.80, so its source is a usable map of
the shipped binary — see O37 for what it says about the 32-bit dynamic-buffer
path, and for why a newer upstream DLL cannot simply be dropped in.

The one thing it tells us loudly is O2 — it cannot honour a custom sampler
border colour, and says so.

## Indicium-Supra, already in the process

`THQNOnline/overlay/OverlayReleaseWin32.dll` is the game's own THQ Nordic Online
overlay, built on Nefarius' Indicium-Supra with an ImGui renderer. Not our code,
but it is an existence proof: it hooks `IDXGISwapChain::Present`, takes the
`ID3D11Device` and `ID3D11DeviceContext`, and renders successfully **through
DXMT, inside this bottle, on a 32-bit process** — sixteen logged runs.

Its log at `AppData\Local\Temp\Indicium-Supra.log` is worth re-reading if our own
hooking misbehaves; it is a working reference implementation of the thing Stage 2
has to do.

It has been renamed to `overlay-disabled` and is not the cause of the flicker
(O5). Remember to put it back before shipping anything to anyone else.

## `../grimdawn-trash`

The same trick against Grim Dawn — the same engine lineage, Iron Lore to Crate,
and the same `.arc`/`.arz` asset formats one major version apart. Proven on this
machine, and directly reusable:

| Piece | What it gives us |
|---|---|
| `scripts/gen-winmm-proxy.sh` | Generates the forwarding `winmm` proxy, resolving the real one from `<system32>` at attach — no second file, so it fits in a ZIP |
| `src/iathook.h` | IAT entry rewriting |
| `src/patch.{cpp,h}` | The vtable / data-write primitives, with `VirtualProtect` handling |
| `src/log.{cpp,h}` | Two-level logging, the "log file is the debugger" pattern |
| `src/config.{cpp,h}` | ini reading beside the DLL |
| `src/modules.{cpp,h}` | Waiting for a module to be loaded before hooking it |
| `scripts/selftest-offgame.sh` | Load / forward / IAT / unpatch with no game running |
| `scripts/{doctor,install-dll,uninstall-dll}.sh`, `tools/{pe,imports,log}.ts` | Patterns to adapt |

**It is 64-bit and we are 32-bit.** Everything above needs the stdcall and
`i686-w64-mingw32` check applied before it is trusted. Its `CLAUDE.md` is worth
reading in full first; its rules were paid for in game launches.

`../grimdawn-core`'s `src/db/arc.ts` reads ARC version 3 and asserts on the
version field; Titan Quest's archives are version 1. Not usable as-is.
