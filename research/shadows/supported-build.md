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

The device implementation is not part of `Engine.dll`. `Direct3D11.dll` is a
PE32/i386 plugin exported through `CreateRenderDevice`; it has its own preferred
image base of `0x10000000` and must be treated as a separate address space. Its
embedded PDB path is
`C:\Program Files (x86)\Jenkins\workspace\TQ\trunk\Code\Binary\Ship\Direct3D11.pdb`;
the PDB itself is not distributed with the game.
