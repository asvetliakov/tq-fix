# Titan Quest Anniversary Edition resource and region-streaming audit

This directory records a reproducible reverse-engineering audit of the game's
own resource loading and region streaming on the supported GOG/CrossOver build.
It exists so that a frame hitch which survives with every mod feature disabled
can be attributed to Titan Quest with evidence, rather than by elimination.

The regeneration tools perform a **static** audit and do not launch the game.
The reviewed conclusions also include archived runtime captures.

The game binaries are not copied into this repository.
`../shadows/supported-build.md` identifies them by SHA-256 — the same four
hashes gate this audit — and `tools/run-audit.sh` regenerates the mechanical
evidence from a local installation.

## Current conclusion

The earlier secondary-pass fixes improved the accepted old-route transition.
On the later alternate route, successful scenery preloading was undone by idle
eviction before visible use; active cooldowns also rejected renewed requests.
Bounded resident mesh preload refresh reduced the matched frame from 323 to
79 ms and main-thread loading from 212 to 29 ms. It preserves native eviction
budgets and cooldowns, and runs independently of tracing. Particle realization,
native draw waits and separate update spikes remain outside that result.
The [remaining-hitch investigation](residual-gameplay-hitches.md) separates
those costs and records the native particle path and remaining measurement gaps.

Read [findings §112](findings.md#112-alternate-route-residency-loss-and-bounded-mesh-preload-refresh),
[gameplay loading hitches](gameplay-loading-hitches.md), and the
[mesh preload lifecycle audit](mesh-preload-lifecycle.md) before using older
prepared-run notes as current conclusions.

## Layout

- `exports.md` is the record of how `seeds.txt` was derived: the 5599 exported
  symbols of `Engine.dll`, the per-family counts, the classes they resolve to,
  and what was deliberately left out.
- `seeds.txt` lists the entry points that start the call-graph walk.
- `disassembly-targets.md` is the durable RVA map for the measured resource,
  runtime-terrain, color-terrain, and directional-shadow chains. It separates
  exported identities from working names assigned to unexported functions.
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
with Ghidra 12.1.3 and OpenJDK 21 and contains **1,596 `Engine.dll` functions
rooted at 211 seed matches**. The later runtime-terrain roots are explicit, so
the vtable-only `TerrainRT` methods remain in the export even when no direct
call graph discovers them.

The mesh additions have separate verification tools, using the same game path:

```sh
TQ_GAME_DIR='/path/to/Titan Quest - Anniversary Edition' \
  python3 research/streaming/tools/verify-resource-trace.py
TQ_GAME_DIR='/path/to/Titan Quest - Anniversary Edition' \
  python3 research/streaming/tools/verify-mesh-refresh.py
```

They verify native hook windows, relocation operands, return cleanup and the
Actor/Entity instruction fixtures used by the executable off-game tests.
`tools/verify-sites.py` remains the verifier for the existing streaming sites.

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
synchronous level load on the render thread. Run 68 later resolves the
remaining marked cold-mesh class inside that same DX11 directional gather to
`Actor::AddToScene -> Actor::UpdateMeshInstance ->
GraphicsMeshInstance::UpdatePose -> Resource::EnsureAvailable`; the exact
targets and Run 69's earlier call-patch boundary are indexed in
`disassembly-targets.md`.
