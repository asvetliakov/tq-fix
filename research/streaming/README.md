# Titan Quest Anniversary Edition resource and region-streaming audit

This directory records a reproducible reverse-engineering audit of the game's
own resource loading and region streaming on the supported GOG/CrossOver build.
It exists so that a frame hitch which survives with every mod feature disabled
can be attributed to Titan Quest with evidence, rather than by elimination.

It is a **static** audit.  Nothing here launches the game.

The game binaries are not copied into this repository.
`../shadows/supported-build.md` identifies them by SHA-256 — the same four
hashes gate this audit — and `tools/run-audit.sh` regenerates the mechanical
evidence from a local installation.

## Layout

- `exports.md` is the record of how `seeds.txt` was derived: the 5599 exported
  symbols of `Engine.dll`, the per-family counts, the classes they resolve to,
  and what was deliberately left out.
- `seeds.txt` lists the entry points that start the call-graph walk.
- `tools/run-audit.sh` is a thin caller over the shared `research/tools/audit.sh`.
- `arc-format.md` decodes the `.arc` container and records what was checked
  across every archive in the installation — whether anything is stored
  uncompressed, whether the data region is contiguous, the block size, and what
  `Resources/Levels.arc` actually contains.  `tools/arcinfo.py` is the program
  that produced those numbers; unlike the Ghidra export it needs nothing but
  Python and a read-only installation.
- `generated/` output is not committed; `tools/run-audit.sh` reproduces it.  It
  contains the function inventory, call graph, assembly, decompiler output,
  data references, and the PE dump.
- `findings.md` is the reviewed prose: the two file classes and which one the
  mod hooks, then the three questions this audit was opened to answer — whether
  file or archive I/O reaches the render thread, what the frustum entity queries
  cost and what a region pull-in does, and what the game already serializes
  against Present.  §4–§7 were added later: why the mod's own probe could not
  see any of this, why the texture uploader must copy rather than take
  ownership, the archive `File` class, and the verified patch sites.

Generated output is evidence, not source code.  Addresses are virtual addresses
using `Engine.dll`'s preferred image base (`0x10000000`) unless a file
explicitly says RVA.

## Regeneration

```sh
TQ_GAME_DIR='/path/to/Titan Quest - Anniversary Edition' \
  research/streaming/tools/run-audit.sh
```

The script refuses binaries whose hashes do not match
`../shadows/supported-build.md`.

The first run imports and analyzes `Engine.dll` into
`build/streaming-audit/ghidra` as project `tq-engine`; later runs reuse that
project with `-noanalysis` and only re-export.  The reviewed export was produced
with Ghidra 12.1.3 and OpenJDK 21 and contains **1363 `Engine.dll` functions
rooted at 155 seed matches**.

## Shared tooling

`research/tools/audit.sh` carries everything both audits need: `TQ_GAME_DIR`
resolution, the four pinned hash checks, Ghidra and JDK discovery, the headless
import/re-export decision, verification that no exported artifact is empty, and
the `objdump -p` PE dumps.  It takes `(project, binary, seeds, outdir)` plus
optional `PE_BINARY=NAME` pairs, and is overridable through `AUDIT_PROJECT_DIR`,
`AUDIT_SCRIPT_PATH`, `AUDIT_EXPORT_SCRIPT`, `AUDIT_ANALYSIS_TIMEOUT`,
`GHIDRA_HOME`, and `JAVA_HOME`.  `research/shadows/tools/run-audit.sh` is now a
caller of the same script and produces the same artifacts it always did.

The Ghidra exporter is shared too: both audits run
`research/shadows/tools/ExportShadowAudit.java`, which stays where it is so the
shadow audit's provenance is unchanged.  It matches a seed as a plain substring
of the function's display name, which is why `seeds.txt` lists every
class-qualified target in both the demangled `Class::Method` and the mangled
`Method@Class` spelling.

## Relationship to the shadow audit

`../shadows/cpu-path.md` records that the directional shadow fit queries casters
through `Region::GetEntitiesInFrustum` / `World::GetEntitiesInFrustum` and that
"selected entities are added to the temporary shadow scene together with their
owning regions".  `findings.md` §2 establishes what that costs and, more
importantly, that the *scene add* — not the query — is what can force a
synchronous level load on the render thread.
