# Titan Quest Anniversary Edition DX11 shadow audit

This directory records a reproducible reverse-engineering audit of the
directional-shadow path used by the supported GOG/CrossOver build.  It exists
separately from the runtime mod so experimental patches cannot become the only
record of how the original renderer works.

The game binaries are not copied into this repository.  `supported-build.md`
identifies them by SHA-256, and `tools/run-audit.sh` regenerates the mechanical
evidence from a local installation.

## Layout

- `supported-build.md` identifies the exact audited binaries.
- `seeds.txt` lists the renderer entry points that start the call-graph walk.
- `tools/` contains the headless Ghidra exporter and orchestration script.
- `generated/` output is not committed; `tools/run-audit.sh` reproduces it.
  It contains function inventories, call graphs,
  assembly, and decompiler output.
- `constants.md` records constants that materially affect the shadow path.
- `object-layouts.md` records only object offsets proven by reads or writes.
- `cpu-path.md` follows setup, fitting, caster selection, and rendering.
- `backend.md` records the D3D11 resource and state configuration.
- `shaders.md` classifies caster and receiver shaders.
- `shaders/` contains representative runtime captures, a generated
  shader-family inventory, and assembly for every unique shadow-bound DXBC.
- `pipeline.md` is the reviewed end-to-end model and root-cause conclusion.
- `findings.md` records what runtime measurement established: which shader
  actually receives directional shadows, why RenderDirectional cannot be
  invoked twice, and how coverage and blur scale with the split.
- `logs/` keeps the runtime evidence behind those conclusions.

Generated output is evidence, not source code.  Addresses are virtual
addresses using Engine.dll's preferred image base (`0x10000000`) unless a file
explicitly says RVA.

## Regeneration

```sh
TQ_GAME_DIR='/path/to/Titan Quest - Anniversary Edition' \
  research/shadows/tools/run-audit.sh
```

The script refuses binaries whose hashes do not match `supported-build.md`.

The reviewed export was produced with Ghidra 12.1.3 and OpenJDK 21. It
contains 433 Engine.dll functions rooted at 63 shadow-related entry points and
43 Direct3D11.dll functions rooted at 13 backend entry points.

## Shader archive inventory

The installed shader archives are copyrighted game data and are not checked
in. Extract their `.ssh` resources read-only into the following local layout:

```text
build/shadow-audit/archives/base
build/shadow-audit/archives/xpack
build/shadow-audit/archives/xpack2
```

Then regenerate the checked-in DXBC inventory:

```sh
python3 research/shadows/tools/inventory-shaders.py \
  build/shadow-audit/archives \
  research/shadows/shaders/inventory.csv
```

The audited installation contains 148 `.ssh` resources. Seventy containers
mention `worldToShadowMatrix`; 68 contain recoverable DXBC programs with
shadow bindings. `grass.ssh` and `vertexcolorlayers.ssh` carry the name only in
container metadata, not in an embedded DXBC program.

To extract and disassemble all unique shadow-bound programs offline using the
game bottle's D3D compiler:

```sh
research/shadows/tools/disassemble-shaders.sh
```

Set `TQ_WINE`, `TQ_BOTTLE_NAME`, or `TQ_SHADER_ARCHIVES` when the defaults do
not match the local installation. The script passes `--no-update` to CrossOver
and does not launch or alter the game. The supported archive set produces 367
content-addressed files under `shaders/generated/`, which is likewise not
committed. `inventory.csv` is kept because it is the provenance record: it maps
each hash back to every resource and permutation that contains it, so the
family structure survives without carrying 1.4 MB of bytecode.

`shaders/deferred-receiver-ps.asm` is the one shader worth reading by hand: the
deferred screen-space pass that actually applies directional shadows. The same
program is committed as `test/fixtures/tq-dxbc-PS-deferred-shadow.b64` so the
self-test can exercise the filter transform without the archive.

Note that the per-material receiver families this inventory enumerates are
legacy: a runtime census found the directional shadow map bound only for the
single deferred screen-space pass. See `findings.md` before drawing
conclusions about receiver coverage from archive counts alone.
