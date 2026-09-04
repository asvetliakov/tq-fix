# Supported build

Audited on 2026-09-01 using the 32-bit GOG Titan Quest Anniversary Edition
installation in CrossOver.

| File | SHA-256 | PE timestamp |
| --- | --- | --- |
| `TQ.exe` | `491c72a6145285ee1cf38ab8fb8656b4adec022e79e434595356ce89b5cbe2d0` | 2022-07-22 17:03:38 |
| `Engine.dll` | `0aedbb1805b4a5616f74e34d4f609f392e2c2dd4561c64c118f4772ab4f694f6` | 2022-07-22 16:48:32 |
| `Game.dll` | `754907eacf552656945ff9eaf1763630e138506517e91b698ab28a0c3186aa86` | 2022-07-22 16:53:06 |
| `Direct3D11.dll` | `589d636746eaad93adbbb920f192478c47fa9ea6745b56438abde06f2aa158e7` | 2022-07-22 16:56:54 |

Engine.dll is PE32/i386, preferred image base `0x10000000`, with a
`0x0044b000` image size.  Runtime addresses must be rebased for ASLR; the
audit uses preferred virtual addresses and RVAs.

## User Engine.dll with UI patches (2026-09-05)

The supplied `Engine (1).dll` has SHA-256
`42bd9c2bbfd669cc8eb9b8f57d3f9bb9a494c616f352a3ebf8917b4805546c49`.
Its file size (3,781,632 bytes), PE timestamp, image size, section layout and
all 5,599 export names/RVAs match the Engine above. Only 58 file bytes differ:

- `Engine::SetUIScale` branches at RVA `0x140cdc` to a stub at `0x2ab69c`.
- `StyleManager::LoadStyle` branches at `0x225825` to a stub at `0x234499`.
  The stub preserves the original `fontSize` lookup, adds two to its result,
  and returns to `0x22582a`.

Both grass render exports (`0x2390b0`, `0x23afc0`) and their verified
prologues are unchanged. The full site verifier passes with the supplied
Engine and Game DLLs together; no new runtime Engine profile is needed:

```sh
TQ_VERIFY_ENGINE_DLL='Engine (1).dll' TQ_VERIFY_GAME_DLL='Game (1).dll' \
  python3 research/streaming/tools/verify-sites.py
```

This establishes Engine hook compatibility. It does not establish that the
separate Direct3D11 renderer reaches our device-context Draw hooks on that
machine. The user's older crash capture recorded grass fills/twin creation
but no indexed or crossing draws; a post-fix capture and their Direct3D11.dll
are needed to investigate the reported missing enhancement.

The device implementation is not part of `Engine.dll`. `Direct3D11.dll` is a
PE32/i386 plugin exported through `CreateRenderDevice`; it has its own preferred
image base of `0x10000000` and must be treated as a separate address space. Its
embedded PDB path is
`C:\Program Files (x86)\Jenkins\workspace\TQ\trunk\Code\Binary\Ship\Direct3D11.pdb`;
the PDB itself is not distributed with the game.

## Supplied HekTo Game.dll variant

Audited on 2026-09-05 from the reporter's `Game (1).dll`:
SHA-256 `5f816173647526e3a6792d1a8136768dd68f30c5344da01c5bb86170a20a5bfb`.
This is a modification of the Game.dll above, with the same PE timestamp and
all 18,060 exported names at the same RVAs. The original `.rdata`, `.data`,
`.rsrc`, and `.reloc` raw sections are byte-identical. Only 12 bytes differ in
the original `.text`: two six-byte entry replacements. SizeOfImage grows from
`0x59a000` to `0x59c000` with `.rxHekTo` and `.rwHekTo` sections.

| Entry | RVA | Existing modification |
| --- | --- | --- |
| `Item::GetDropMeshOverride` | `0x80150` | Jumps to `0x59a008`; optional callback through `0x59b008`, then original prologue at `0x59a05c` and body at `0x80156`. |
| `GameEngine::Update` | `0x19a230` | Jumps to `0x59a035`; calls original prologue at `0x59a067`, which jumps to `0x19a236`, then runs an optional callback through `0x59b00c`. |

Both callback slots are initially zero in the supplied file. Their runtime
owners and behavior are not established by this static audit.

The optional Game update timer verifies the modified entry (including its
relocated SEH operand), all 39 wrapper bytes, all 11 relocated-prologue bytes,
and the appended section layout. It replaces the existing six-byte jump with
an absolute hook branch and forwards through the existing wrapper. This
preserves the callback, stack argument, and original body; its timing includes
the wrapper and any callback. Teardown restores the exact original jump.
Copying the existing relative jump into a generic trampoline would be invalid.
The item hook and both callback slots remain owned by the existing modification.

The original `.text` is admitted to Game caller attribution after these checks.
The widescreen frustum hook already accepts the unchanged unique viewport and
frustum import-call window. Engine shadow, terrain, and archive behavior hooks
are independent of this Game.dll profile.

Validation: byte-site audits pass against both binaries; executable synthetic
PE tests run both update paths at a relocated base, preserve callback order,
`this` and delta, restore entry bytes, and reject changed branches, signature
operands, section permissions, and unknown image sizes. The full off-game
regression suite passes. Native Windows gameplay with this variant remains
unverified, and compatibility does not establish the fountain crash's cause.

To repeat the supplied-variant audit without replacing the installed Game.dll:

```sh
TQ_VERIFY_GAME_DLL='/path/to/Game (1).dll' \
  python3 research/streaming/tools/verify-sites.py
```
