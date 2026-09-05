# Renderer draw-submission sites

The native Draw/DrawIndexed refresh experiment and grass-specific diagnostics
were removed on 2026-09-05. The committed grass buffer-lifetime and context fixes
remain. The replacement in `src/renderer_draw.cpp` now redirects the two game
renderer submission sites described below. Native Draw/DrawIndexed slots are
left untouched.

## Binary and grass path

The uploaded `Direct3D11.dll` and the local game renderer are byte-identical:
SHA-256 `589d636746eaad93adbbb920f192478c47fa9ea6745b56438abde06f2aa158e7`,
32-bit x86, preferred base `0x10000000`, image size `0x192000`.
Addresses below are RVAs unless explicitly described as object offsets.

Both Engine grass routines call the abstract render-device method at object
vtable offset `+0xfc`:

- TerrainRenderInterface::RenderGrass: call at Engine RVA `0x2393a8`.
- TerrainRenderInterfaceRT::RenderGrass: call at Engine RVA `0x23b265`.
- Renderer vtable starts at `0x86200`; entry `0x862fc` points to `0x602e0`.
- Wrapper `0x602e0` prepares state, converts topology, applies existing renderer
  gates, and calls the indexed helper `0x5dad0` at `0x60362`.
- That helper calls native DrawIndexed at `0x5db2c`, after topology binding.

The same renderer vtable has immediate-geometry wrappers at `0x5f9d0`,
`0x5fe20`, and `0x5f4a0` (slots `+0x100`, `+0x104`, and `+0x108`). Their direct
calls at `0x5fdfd`, `0x602bb`, and `0x5f9b3` converge on helper `0x5d9a0`, which
uploads temporary vertices, binds buffers/topology, and calls native Draw at
`0x5daa3`.

## Interception points

| Path | Helper entry | Native call | 7-byte patch start |
| --- | --- | --- | --- |
| Draw | `0x5d9a0` | `0x5daa3` | `0x5da9f` |
| DrawIndexed | `0x5dad0` | `0x5db2c` | `0x5db28` |

Immediately before both calls, EAX holds the context from renderer object
offset `+0x2c`. The sequences are whole x86 instructions:

```asm
; Draw, 0x5da9f: 8b 08 57 50 ff 51 34
mov  ecx, [eax]
push edi                  ; vertex count; start vertex 0 is already pushed
push eax                  ; context
call dword ptr [ecx+34h]

; DrawIndexed, 0x5db28: 8b 08 53 50 ff 51 30
mov  ecx, [eax]
push ebx                  ; index count; start index and base already pushed
push eax                  ; context
call dword ptr [ecx+30h]
```

Each becomes two one-byte pushes followed by a five-byte `call rel32` to
the existing WINAPI handler. The result is exactly seven bytes; no trampoline
or custom calling-convention shim is needed. Stack arguments and the return
address after the window remain in the same positions. ECX is caller-saved in
this ABI; the native call could already clobber it. Renderer counters and its
cleanup instructions remain after the call, and all existing state preparation
stays before it. This is more precise than timing or duplicating an entire
renderer wrapper, which would also include uploads and state setup.

Installation checks the complete surrounding windows as signatures:

```text
Draw: RVA 0x5da90, 34 bytes; patch offset 15
8b 46 2c 55 8b 08 50 ff 51 60 8b 46 2c 6a 00 8b 08
57 50 ff 51 34 01 9e b0 00 00 00 ff 86 b4 00 00 00

DrawIndexed: RVA 0x5db13, 40 bytes; patch offset 21
8b 46 2c 52 8b 08 50 ff 51 60 ff 74 24 14 8b 46 2c
ff 74 24 20 8b 08 53 50 ff 51 30 01 be b0 00 00 00
ff 86 b4 00 00 00
```

Neither window contains an ASLR relocation. Both signatures must match in the
renderer .text section before either is written. Failed installation/shutdown
restores exact original bytes only if the current bytes are ours. Unsupported
sites cause visual installation to roll back; there is no native-slot fallback.
The runtime check uses the x86 PE section bounds and instruction signatures,
without pinning linker timestamps or requiring the whole file hash.

## Forwarding requirements

Native context Draw/DrawIndexed slot patching has been removed. The handlers
submit the original draw through the context's live virtual method. No native
draw function pointer is cached. The same applies to the extra crossed-grass
draw after IASetVertexBuffers and the mod's post-processing draws. Native
dispatch changes remain effective at each submission without removing our
interception in the renderer.

Native context calls no longer re-enter the renderer hook. Other draw consumers
(SMAA, secondary admission, draw timing, and post-processing) use these same
sites, avoiding duplicate interception. The mechanism needs no recurring
refresh, memory query, or page-protection write during rendering. Existing
feature costs still apply.

The off-game fixture contains executable copies of the audited windows at the
same RVAs, with small caller/return adapters in place of the full game renderer.
Tests execute both original and patched sites, pass nonzero counts/start indices
and a negative base vertex, switch native targets during and between submissions,
check the original return addresses, reject altered signatures before any write,
and restore both windows on shutdown. Production DLL tests cover enhanced grass
as the sole draw-hook consumer, original grass leaving sites unchanged, and
SMAA rendering through the new path. Existing secondary-admission tests and
source audits continue to cover the shared suppression guard.

These checks do not prove the cause of the user's jitter or replace a native
Windows gameplay run. Extra grass geometry has its existing GPU cost; removal
of polling does not imply a measured zero-overhead guarantee for all features.

Reproduce the binary checks with:

```sh
python3 research/tools/verify-renderer-draw.py Direct3D11.dll 'Engine (1).dll'
```
