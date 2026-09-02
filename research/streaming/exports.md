# Building the seed list from Engine.dll's export table

The seeds in `seeds.txt` were not guessed.  They were selected from the export
table of the supported `Engine.dll`
(`0aedbb1805b4a5616f74e34d4f609f392e2c2dd4561c64c118f4772ab4f694f6`,
`research/shadows/supported-build.md`).  This file is the record of that
selection so the choice can be re-derived and argued with.

## Extraction

```sh
i686-w64-mingw32-objdump -p "$TQ_GAME_DIR/Engine.dll" > build/streaming-audit/engine-pe.txt
sed -n '/\[Ordinal\/Name Pointer\] Table/,$p' build/streaming-audit/engine-pe.txt \
  | sed -n 's/^\t\[[ 0-9]*\] *+base\[[ 0-9]*\] *[0-9a-fA-F]* *//p' \
  > build/streaming-audit/exports.txt
```

That yields **5599** MSVC-mangled symbols, the same count
`research/shadows/README.md` records.  The raw list stays under `build/`, which
is gitignored; it is game-derived data and is not committed.

Each mangled name was split into `(class, method)` by taking the head of the
name up to the first `@@`: `?Method@Class@GAME@@…` gives `Class::Method`, and
`??0Class@GAME@@…` gives a constructor.  Free functions in namespace `GAME`
land under a `GAME` pseudo-class.

## Family counts

Case-insensitive substring counts over all 5599 exported names.  These are
overlapping — `?LoadRenderData@Terrain@GAME@@…` is counted under `terrain` and
under `load/unload` — and they are a size estimate, not the seed list.

| Family substring | Exported names |
| --- | ---: |
| `World` | 354 |
| `Region` | 308 |
| `Load` or `Unload` | 286 |
| `Terrain` | 219 |
| `File` | 178 |
| `Level` | 129 |
| `Resource` | 119 |
| `Grid` | 111 |
| `Frustum` | 104 |
| `Sector` | 97 |
| `Stream` | 72 |
| `Archive` | 45 |
| `Thread` | 18 |
| `MemoryMappedFile` or `MapView` | 7 |
| `Present`, `BeginFrame`, or `EndFrame` | 7 |

## Classes the families resolve to

Grouping by the parsed class name turns those substring counts into a small set
of real types.  Exported-method counts per class:

| Class | Methods | Why it matters |
| --- | ---: | --- |
| `Engine` | 207 | owns the frame loop and the resource loader |
| `GraphicsEngine` | 130 | `PresentSurface`, `LoadTexture`, `LoadMesh`, `LoadShader2` |
| `World` | 110 | region set, frustum queries, map file lock |
| `Terrain` | 91 | block/grass geometry, dirty-rect rebuilds |
| `GraphicsCanvas` | 83 | `BeginFrame` / `EndFrame` / `PresentSurface` |
| `Region` | 77 | level load/unload, entity queries |
| `Level` | 75 | the loadable unit behind a region |
| `GraphicsMesh` | 54 | resource with `PreLoadDependentResources` |
| `GridRegion` | 51 | lattice chunk loading |
| `GraphicsShader2` | 39 | resource |
| `SectorDataManager` | 34 | per-sector serialized data |
| `TerrainBase` | 34 | block construction, object space |
| `Resource` | 25 | the load state machine |
| `BaseResourceManager` | 21 | budget, eviction, thread fences |
| `IOStreamRead` | 21 | block-structured reader |
| `IOStreamWrite` | 20 | (write side; not seeded) |
| `Archive` | 20 | `.arc` container |
| `FileSystem` | 18 | source resolution |
| `GraphicsTexture` | 17 | resource the mod's uploader intercepts |
| `FileSourceArchive` | 13 | archive-backed file source, incl. `GetMapping` |
| `GridSystem` | 13 | cell feature meshes |
| `ResourceLoader` | 12 | **the loader thread** |
| `FileSourceDirectory` | 9 | loose-file source |
| `WorldFile` | 9 | `.wrl` reader |
| `FileDirectory` | 7 | |
| `WorldFrustum` | 6 | frustum in world/region coordinates |
| `RegionId` | 6 | |
| `WorldCamera` | 38 | region-relative frustum accessors |
| `MemoryMappedFile` | 5 | `Open` / `MapView` / `UnmapView` / `Close` |
| `RegionLoader` | 5 | **the streaming driver** |
| `ThreadMonitor` | 3 | in-engine thread census |

Two classes decide the shape of the audit and were found this way rather than
assumed:

- **`ResourceLoader`** exports `StartThread`, `StopThread`, `EnqueueResource`,
  `LoadResource`, `UnloadResource`, `PurgeResource`, `PurgeAllResources`,
  `CreateMarker`, `GetHasMarkerPast`, `EnableDebugging`,
  `EnableSingleProcessorMode`, and `Update`.  A class that owns a thread, a
  queue, and a fence marker is the game's asynchronous loader; `Update` is its
  main-thread pump.
- **`RegionLoader`** exports only `SetFrustum`, `GetFrustum`, `Update`,
  `GetIsDone`, and `GetAreLevelsLoaded` — a frustum-driven region streamer with
  a completion flag.

`BaseResourceManager` corroborates the threading: it exports
`SetThreadUnloadFence`, `SetThreadFencesPaused`, and
`RemoveThreadFenceCounter`, which only exist if unloading races a worker.

## The frustum query family

Only these exported symbols answer a frustum query with entities, regions, or
meshes:

```
Level::GetEntitiesInFrustum      Region::GetEntitiesInFrustum   (two overloads)
World::GetEntitiesInFrustum      Region::GetRegionsInFrustum
World::GetRegionsInFrustum       World::GetLoadedRegionsInFrustum
Region::GetEnclosingFrustum      TerrainBase::GetObjectsInFrustum
GridRegion::GetMeshesInFrustum   Water::GetBlocksInFrustum
Engine::GetEntitiesInPriorFrameFrustum
```

The two `Region::GetEntitiesInFrustum` overloads differ in constness and in
signature, from the mangled names:

```
?GetEntitiesInFrustum@Region@GAME@@QAEXAAV?$vector@PAVEntity@…@std@@ABVFrustum@2@_NW4EntityListType@2@2@Z
?GetEntitiesInFrustum@Region@GAME@@QBEXAAV?$vector@PAVEntity@…@std@@ABVFrustum@2@_NPBV12@W4EntityListType@2@22@Z
```

The non-const overload (`QAE`) takes `(vector<Entity*>&, const Frustum&, bool,
EntityListType, EntityListType)`; the const overload (`QBE`) takes an extra
`const Frustum*`.  `World::GetEntitiesInFrustum` is const and takes a
`WorldFrustum`, not a `Frustum`:

```
?GetEntitiesInFrustum@World@GAME@@QBEXAAV?$vector@PAVEntity@…@std@@ABVWorldFrustum@2@_NW4EntityListType@2@22@Z
```

`WorldFrustum` exports `SetFrustum`, `GetRegionFrustum`, and
`GetRelativeFrustum` — i.e. it is the frustum-plus-region-origin pair, which is
what lets the world query rebase a frustum per region.  That is the mechanical
evidence for the sentence in `research/shadows/cpu-path.md` that world queries
"translate the frustum into each region's coordinate space".

## What was seeded, and what was left out

`seeds.txt` names 108 distinct methods (216 seed lines plus 2 address seeds) across six groups: resource manager and
loader thread, archive/file system/memory mapping, region and level streaming,
the frustum query family, terrain and grid block streaming, and the frame
boundary (`PresentSurface`, `BeginFrame`, `EndFrame`, `Engine::Render`).  Every
class-qualified target appears twice, in the `Class::Method` and `Method@Class`
spellings, because `ExportShadowAudit.java` matches seeds as plain substrings of
the display name and Ghidra's MSVC demangler does not succeed on every symbol.

Deliberately excluded:

- **`IOStreamWrite` and the `Save`/`Write*` half of `SectorDataManager`,
  `Region`, `Level`, `World`, `Terrain`** — serialization out is the editor and
  save-game path, not the streaming path under investigation.
- **Pure accessors** (`GetNumRegions`, `GetFileSize`, `Archive::BlockSize`,
  `GetLastAccessCounter`, …) — they add closure width without adding a call
  path.
- **`SoundManager` / `SoundPak` / `Jukebox` (90 + 26 + 49 methods)** — they do
  load from disk and they do own a background thread
  (`SoundManager::AddBackgroundThreadSound`,
  `SoundManager::PlayBackgroundThreadSound`, `SoundManager::LockCrit`), but
  audio streaming is a separate subsystem from the render-thread question.  It
  is recorded here as a known, unaudited I/O source rather than silently
  dropped.
- **`ConnectionManager` / `Socket` / `NetworkQueue`** — network receive runs on
  `Socket::ReceiveThread`; single-player is out of scope.
- **`LoadTable` / `LoadTableBinary` (36 + 33 methods)** — database record
  loading, reachable from entity creation, not from region streaming.

Two address seeds are included because the mod knows the symbols only by RVA:
`0x1014e540` is the file-source vtable slot 4 that `src/visual.cpp:641`
replaces with `hookArchiveUnmap`, and `0x1014e560` is slot 2, which
`src/visual.cpp:578` reads to identify the source.  Naming them by
address avoids depending on the demangler's spelling of a vtable thunk.
