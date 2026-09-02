# Resource loading and region streaming on the supported build

Every claim below carries the virtual address, exported symbol, or `file:line`
that proves it.  Addresses are preferred virtual addresses using `Engine.dll`'s
image base `0x10000000` (`research/shadows/supported-build.md`).  Anything not
proven from the binary is labelled **unproven**.

The evidence is the closure produced by `tools/run-audit.sh`: **1363 functions
rooted at 155 seed matches**, exported to `generated/` (not committed).

Mod-side line numbers are as of this audit; prefer the symbol names, which are
stable, if `src/` has moved since.

---

## 0. The two file classes, and which one the mod hooks

`GAME::FileSystem` resolves a path through sources.  There are two `File`
implementations and they behave completely differently.

**Loose files** — `FileSourceDirectory::OpenFile` at `0x1014fde0` allocates a
0x38-byte `FileDirectory` with vtable `PTR_FUN_102f71ec`, storing the file
mapping handle at `+0x30` and the mapped view at `+0x34`.  Its `Lock` and
`Unlock` are the whole I/O path:

```
0x1014e560  FileDirectory::Lock(offset, size)
              GetSystemInfo(&si);
              base = (offset / si.dwAllocationGranularity) * si.dwAllocationGranularity;
              view = MapViewOfFile(this[0x30], FILE_MAP_READ, 0, base, offset - base + size);
              this[0x34] = view;
              return view + (offset - base);

0x1014e540  FileDirectory::Unlock()
              UnmapViewOfFile(this[0x34]); this[0x34] = 0;
```

There is no `ReadFile` on this path at all: the bytes arrive as demand-paged
faults against the mapped view, wherever they are first touched.

**Archive entries** — `FileSourceArchive::OpenFile` at `0x1014ed30` calls
`Archive::FindFile` (`0x1011e240`), `Archive::ReOpen` (`0x1011e590`),
`CreateFileMappingA` over the whole `.arc`, and constructs a *different*
0x28-byte `File` at `0x1014d020` with vtable `PTR_LAB_102f719c`.  When the entry
flag bit `2` is set that constructor allocates two `operator new[]` scratch
buffers of `archive[0x40]` (the archive block size) — the decompression working
set.  Reads go through `Archive::ReadFromFile` at `0x1011d320`:

- uncompressed: `EnterCriticalSection(archive + 0x60)`, `SetFilePointerEx`,
  `ReadFile`, `LeaveCriticalSection`;
- compressed: `FUN_1011d240` → `FUN_1011d0e0`, which takes the same
  `archive + 0x60` section, `ReadFile`s one block, and decompresses it with
  `FUN_10065760`.  The identity of that step is proven by its own error string
  at `0x1011d0e0`: *"Archive: Block decompression error in file '%s', block %u
  of %u. Expected size %u, actual size %u."*

**This matters for the mod.**  `src/visual.cpp:617-650` requires
`vtable[2] == Engine.dll+0x14e560` and `vtable[4] == Engine.dll+0x14e540` before
it will lease a mapping or install `hookArchiveUnmap`.  Those are precisely
`FileDirectory::Lock` and `FileDirectory::Unlock`.  The archive `File`'s vtable
is `0x102f719c`, a different table, so **the progressive texture uploader
engages only for textures served from a loose-file source, never for a texture
read out of a `.arc`**.  The name `hookArchiveUnmap` is a misnomer; it hooks the
loose-file unmap.  Whether the audited installation actually serves the hot
textures loose or from archives is **unproven** here — it is a property of the
install, not the binary — but it is directly checkable from the mod's own
`g_diagUploadsCreated` counter (`src/visual.cpp:750`).

## 1. Where the resource load actually happens

`FUN_10213c80` is the entire load body.  Its disassembly is unambiguous:

```
10213c8c  CALL dword ptr [EAX + 0x8]      ; resource->vtbl[2]()          -> length
10213ca1  CALL dword ptr [EAX + 0x8]      ; file->Lock(0, length)        -> mapped pointer
10213caa  CALL dword ptr [EDX + 0xc]      ; resource->vtbl[3](ptr, len)  -> Initialize
10213cad                                  ; <- return address
10213cb3  CALL dword ptr [EDX + 0x10]     ; file->Unlock()
```

`0x10213cad` is exactly the `parserReturn` the mod scans the stack for
(`src/visual.cpp:561`), and `0x10213cb3` is exactly the call its vtable patch
replaces.  So the mod's stack walk is anchored to the correct frame.

`GraphicsTexture::Initialize` at `0x10194120` is the `vtbl[3]` for textures.  It
checks for the `"DDS "` or `"TEX"` magic, fetches the render device from
`GraphicsEngine + 0x34`, guards on device vtable `+0x1e8`, and then calls device
vtable `+0xb4` **with the pointer `File::Lock` just returned**.  That is the
`CreateTexture2D` the mod intercepts, and it confirms the comment at
`src/visual.cpp:885`: `pSysMem` points into a memory-mapped file view.
`GraphicsTexture::GetIsReadyToUse` (`0x10194610`) and
`GraphicsMesh::GetIsReadyToUse` (`0x1016b360`) only test `state == 2`; there is
**no** deferred, main-thread GPU-creation step.  Whichever thread runs
`FUN_10213c80` is the thread that calls the D3D11 device.

---

## Question 1 — does file or archive I/O run on the render/present thread?

**Yes, on four distinct paths.  Two of them are inside the renderer itself.**

### 1a. The renderer force-loads levels while gathering elements

`GraphicsDeferredRendererX::AddElementsInBox` at `0x101677e0` — a virtual
override on `GraphicsSceneRenderer` — begins with:

```c
GraphicsSceneRenderer::SetRegionToSceneCoords(this, region, coords);
FUN_10167490(box, &frustum);
if (region != 0) {
    Region::LoadLevel(region, false);      /* <-- 0x1020bec0 */
    region[0x6c] = 0;
    if (region[0x74] == 0) {
        EnterCriticalSection(region + 8);
        level = region[0x50];
        ...
```

`GraphicsForwardRenderer::AddElementsInBox` at `0x1017d850` has the same shape.
`Region::AddToScene` (`0x1020e6f0`) and `Region::GuaranteedGetLevel`
(`0x1020e7b0`) likewise call `LoadLevel` before touching the level.

`Region::LoadLevel` at `0x1020bec0` returns immediately only if the level is
already resident (`region[0x50] != 0`) and the caller passed `false`.
Otherwise it logs, at engine severity 2, one of

- *"Forcing load of level %s in main thread.(%d)"*
- *"^gForcing load of level %s render data in main thread."*

and then performs the load synchronously via `FUN_1020a6b0`.  The game's own
authors named the hazard.

### 1b. A synchronous load blocks the whole region behind one critical section

`FUN_1020a6b0` takes `EnterCriticalSection(region + 8)` at its top and holds it
across `Region::GetLoadFileName` (`0x1020a060`) and `Level::Load`
(`0x101b3fb0`), releasing it only at the end.  `Level::Load` constructs and
loads `GridRegion` (`0x1019b6a0`), `ImpassableData` (`0x1019fa30`), `Terrain`,
`Water` (`0x102643b0`), `SectorLayers` (`0x10218800`), and runs
`Terrain::ProcessDirtyRects` (`0x1022c2b0`, 1615 bytes) — a layer-opacity
rasterization, not just a read.

That same `region + 8` section is entered by:

| Caller | Address |
| --- | --- |
| `Region::GetEntitiesInFrustum` (const overload) | `0x10209840` |
| `Region::AddToScene` | `0x1020e6f0` |
| `Region::GuaranteedGetLevel` | `0x1020e7b0` |
| `GraphicsDeferredRendererX::AddElementsInBox` | `0x101677e0` |
| `RegionLoader::Update` | `0x1020ec60` |
| `World::UpdateRegionUsage` (via `TryEnterCriticalSection`) | `0x1027aaf0` |

So while the loader thread is inside a level load for region R, **any caster or
entity frustum query that reaches region R blocks for the full duration of that
load** — file read, decompress, terrain rasterize and all.  `UpdateRegionUsage`
is the one caller that uses `TryEnterCriticalSection` and skips; the render-path
callers all use the blocking form.

### 1c. Any unavailable resource loads inline on the touching thread

`Resource::EnsureAvailable` at `0x102130f0`:

```c
if (this[0x30] != 2)                       /* not loaded */
    ResourceLoader::LoadResource(this[0x24], this);   /* 0x10213ed0, synchronous */
```

`ResourceLoader::LoadResource` at `0x10213ed0` opens with

```c
if (resource[0x30] == 1) {                 /* currently loading */
    EnterCriticalSection(resource + 0x4c);
    LeaveCriticalSection(resource + 0x4c); /* block until the worker finishes */
}
```

— a bare lock/unlock pair whose only purpose is to wait — and then, if
`GetCurrentThreadId() == DAT_1041a5dc` (the recorded main thread), logs

- *"Resource '%s' loaded in main thread while in queue %d"* (severity 2), and
- *"Resource '%s' loaded from the main thread"* (severity 1)

before doing the load anyway through `PurgeResource` → `FUN_10213a40` →
`FUN_10213c80`.  The existence of a main-thread-specific warning inside a
function that otherwise proceeds normally is the proof that the same function is
the worker thread's load entry point too.

### 1d. `Region::WaitForLoadingToFinish` is a raw spin

`0x1020bde0`:

```c
do { } while (this[0x78] == 1);
```

No `Sleep`, no `SwitchToThread`, no event.  Whatever thread calls it burns a
core until the loader clears the flag.  No caller exists inside `Engine.dll`; it
is exported for `Game.dll`, so **which thread calls it is unproven** from this
audit.

### 1e. What does *not* run I/O on the present thread

`Engine::PresentSurface` at `0x10143b60` is:

```c
ResourceLoader::Update(this[0x16c]);       /* 0x102142e0 */
if (!renderDevice->vtbl[0x1e8]() && this[0x54])
    (*(...vtbl + 0x5c))(this[0x54]);       /* the device Present */
```

so the loader pump genuinely runs on the present thread, immediately before the
device Present the mod hooks (`src/streaming.cpp:62-67` wraps the render
device's present slot, and `visual::onPresent` runs *before* it).  But
`ResourceLoader::Update` is **statistics only**: every branch it takes calls
`Engine::AddStatisticText`, and the whole body is gated on the debug byte at
`ResourceLoader + 0x28` (set by the exported
`?EnableDebugging@ResourceLoader@GAME@@QAEX_N@Z`) or on the graphics stats flag
at `GraphicsEngine + 200`.  With both off it does nothing.  With debugging on it
takes the loader's queue critical section (`ResourceLoader + 0x40`) on the
present thread — the same one `EnqueueResource` (`0x102145c0`) holds — so
enabling the engine's own resource statistics is itself a stutter source.

---

## Question 2 — the cost of `Region`/`World::GetEntitiesInFrustum`, and what a new region costs

### The query is a *world* query, whatever you call it

`Region::GetEntitiesInFrustum` has two overloads (both exported; see
`exports.md`).  The non-const one at `0x1020e580` is a wrapper: it copies the
frustum (`0x101` dwords = **1028 bytes**) onto the stack, packs it into a
`WorldFrustum{region, frustum}`, and calls `World::GetEntitiesInFrustum`.  So
asking a region for casters actually asks the world.

`World::GetEntitiesInFrustum` at `0x1027ac90`:

1. `World::GetLoadedRegionsInFrustum` (`0x102774b0`) — walks the **entire**
   loaded-region list at `world + 0x5c`, rebases each region's bounding box by
   the origin delta, tests it with `FUN_1028bf90`, and `push_back`s the hits
   into a heap `std::vector<const Region*>` (grown by `FUN_10015200`, freed with
   `operator delete` on the way out).  Cost is O(loaded regions) regardless of
   frustum size; the *output* count grows with frustum volume.
2. For each surviving region: another **1028-byte frustum copy** onto a
   1036-byte stack buffer, a rebase (`FUN_10288820`), then
   `Region::GetEntitiesInFrustum` (const, `0x10209840`).

### Per region, the const overload costs

```c
if (region[0x74] == 0) {                      /* not discarded */
    EnterCriticalSection(region + 8);         /* <-- the level-load lock */
    space = region[0x50];
    region[0x6c] = 0;                         /* <-- marks the region used */
    LeaveCriticalSection(region + 8);
    if (space && frustum) {
        FUN_1003ccb0(out.size() + 200);       /* reserve 200 more slots */
        FUN_1003eb20(out, frustum, 1, -1, flags);   /* recursive spatial walk */
    }
}
if (portalTraversal)
    for each portal:
        ... 1028-byte frustum copy ... GetEntitiesInFrustum(connected, ..., true);
```

Three costs scale with frustum volume:

- **`FUN_1003eb20` / `FUN_1003e480`** are recursive (they call themselves) and
  their per-node test is `FUN_1028bf90`, the same frustum-vs-box predicate.
  The work is the number of spatial-tree nodes whose box intersects the frustum,
  plus the entities in the accepted leaves.  Volume grows the intersected node
  set super-linearly in a 3-D tree, so a 3.4× area widening costs more than 3.4×
  here, not less.
- **`FUN_1003ccb0(count + 200)`** reserves the output vector to 200 beyond the
  current size on *every region*, so a query that touches N regions performs N
  reserve/possible-realloc steps on a vector that ends up holding all their
  entities.
- **1028-byte frustum copies**: one per region in `World::GetEntitiesInFrustum`,
  one more per traversed portal in the const overload.  With the widened split
  covering more regions and more portals, this is a memcpy count that scales
  with the region count, on the render thread.

`Region::PreLoad` (`0x1020a970`) is worse per call: it reserves **16 frustums ×
1032 bytes = ~16 KB of stack** (with `__alloca_probe`) and zero-initializes them
before doing anything.  `RegionLoader::Update` (`0x1020ec60`) itself carries
four such 1032-byte frustum buffers, ~4 KB of stack frame.

### The residency side effect — this is the part that matters for the split

`region[0x6c]` is the frames-not-updated counter:
`Region::MarkAsUsedThisFrame` (`0x1020bc10`) is exactly `region[0x6c] = 0`, and
`World::UpdateRegionUsage` (`0x1027aaf0`, called from `Engine::Update`) does

```c
for each region:
    region[0x6c]++;
    if (region[0x6c] > 1000 + (region[0x28] * 7) % 23)
        if (TryEnterCriticalSection(region + 8)) {
            Region::UnloadLevel(region, true, false);   /* 0x1020e040 */
            LeaveCriticalSection(region + 8);
        }
```

**Querying a region resets its unload countdown.**  A shadow caster query over a
3.4× wider box therefore keeps every region it touches resident for another
~1000 updates.  In a 32-bit process that is a memory-pressure change, not just a
CPU one, and it interacts with the mod's own ~336 MiB of shadow targets.

### What a new region pull-in actually does

The streaming driver is `RegionLoader::Update` (`0x1020ec60`).  Per candidate
region it computes a frustum (`Region::GetEnclosingFrustum` `0x10209500`, or
`WorldFrustum::GetRelativeFrustum` `0x1027f0b0`), calls `Region::PreLoad`, takes
a completion token from `ResourceLoader::CreateMarker` (`0x10214070`), and later
compares that token against the loader's completed counter (`loader + 0x60`)
under the loader's critical section (`loader + 0x30`) to decide whether the
region's loads have drained.  It clears `region[0x6c]` under `region + 8` for
each region it is driving.

`Region::PreLoad` fans out to the terrain's preload (level `+0xc`, vtable
`+0x34`), `GridSystem::PreLoad`, and `FUN_101b0a50` — all of which end at
`ResourceLoader::EnqueueResource` (`0x102145c0`): take `loader + 0x40`, insert
into one of two priority lists, bump the per-priority count, `SetEvent(loader +
0x6c)` to wake the worker, and set the workload class at `loader + 0x70` to 2
once more than 0x14 (20) items are queued.

The load itself, when it is not forced on the main thread, is `Level::Load`
(`0x101b3fb0`) under `region + 8` — grid lattice, impassable data, terrain,
water, sector layers, and `Terrain::ProcessDirtyRects`.

**Conclusion for the widened split.**  The frustum query itself only visits
*loaded* regions (`GetLoadedRegionsInFrustum`), so widening the shadow split
does **not** by itself pull new regions off disk.  What it does is (a) multiply
the per-frame walk cost as described above, (b) reset the unload countdown on a
larger set of regions, and (c) enlarge the set of regions on which a *render*
call — `AddElementsInBox` / `Region::AddToScene` — can hit a not-yet-loaded
level and force the synchronous main-thread load of §1a.  (c) is the mechanism
that turns "camera crosses into new territory" into a hitch, and it is amplified
by the split, not caused by it.

---

## Question 3 — is there a load/decompress step already serialized against Present?

**Yes: `Engine::Update` ends with a blocking rendezvous with the loader
thread.**  The tail of `Engine::Update` (`0x101443a0`) is a verbatim inline of
`BaseResourceManager::SetThreadUnloadFence` (`0x1011f660`) — the two are
instruction-for-instruction the same four calls, and the profiler scope pushed a
few lines earlier (under the `DAT_10374c88` profiling guard, main thread only)
names it outright: `FUN_102077a0("BaseResourceManager::SetThreadUnloadFence")`:

```c
FUN_1011f490();                                  /* recompute the global fence */
SetEvent(DAT_10370260);                          /* tell the loader it may unload */
WaitForSingleObject(DAT_10370258, INFINITE);     /* block until it says done */
FUN_1011f310();                                  /* republish this thread's counters */
```

`DAT_10370258` is reset by `BaseResourceManager::SetThreadFencesPaused(true)`
(`0x1011f270`) on the first pause and only re-signalled when the pause count
returns to zero, so the main thread's wait is open-ended by construction.
`FUN_1011f490` additionally walks every registered thread doing
`OpenThread` + `WaitForSingleObject(handle, 0)` to prune dead entries — a pair
of syscalls per thread, per update.

Immediately before that fence, the same function runs
`BaseResourceManager::UnloadUnreferencedResources` (`0x10120250`, 530 bytes)
**seven times** — once each for five managers hanging off `Engine + 0x160`, once
for `Engine + 0x158`, and once for the sound manager's at `+0x518` — and, unless
`Engine[0x8ea]` is set, `World::UpdateRegionUsage`, which is where synchronous
`Region::UnloadLevel` calls happen on the main thread.

So the serialization chain per frame is:

1. `Engine::Update` — 7 × unload sweeps, region usage sweep with possible
   synchronous `UnloadLevel`, then **block on the loader-thread fence**.
2. `Engine::Render` — `GraphicsEngine::Update`, inside which
   `AddElementsInBox` can call `Region::LoadLevel` and block on `region + 8`
   behind a loader-thread level load.
3. `Engine::PresentSurface` — `ResourceLoader::Update` (free unless
   statistics/debugging are on), then the device Present.

**Where the mod's uploader sits.**  `src/streaming.cpp:62-67` wraps the render
device's present entry and calls `visual::onPresent` *before* the real present;
`advanceTextureUploadsInternal` (`src/visual.cpp:845`) runs there
(`src/visual.cpp:3144`).  So the uploader runs at step 3, after the fence in
step 1 and after any forced load in step 2 — it is not literally queued behind
them within a frame.  But:

- The `UpdateSubresource` at `src/visual.cpp:888` reads from
  `job.source[mip].pSysMem`, which §1 proves is a `MapViewOfFile` view whose
  `UnmapViewOfFile` the mod deliberately deferred (`hookArchiveUnmap`,
  `src/visual.cpp:617`).  Every byte of that 1–2 MiB chunk that has not been
  touched yet is a page fault against the file, taken on the present thread,
  inside a region the mod times with `GetTickCount` (15.6 ms resolution).
- Those faults compete for the disk with the loader thread's own `ReadFile`s,
  which are serialized on `archive + 0x60` for archive sources.  A deferred
  mapping also keeps the view — and therefore address space — alive across
  frames, which in a 32-bit process is not free.
- The fence in step 1 means any loader-thread stall the uploader contributes to
  is paid by the main thread on the *next* update, not the current frame.  A
  hitch attributed to `Engine::Update` can therefore have been caused during the
  previous frame's present.

**No third serialization was found.**  There is no evidence in the closure of a
decompress step that runs on, or synchronizes with, the present call itself.
Archive decompression (`FUN_1011d0e0` → `FUN_10065760`) happens under
`archive + 0x60` on whichever thread is doing the read; `MemoryMappedFile`
(`0x101c48e0`/`0x101c4960`) is used only by `Engine::LoadDatabase`
(`0x10145630`), which is startup work.

---

## Cross-references worth acting on

1. `hookArchiveUnmap` (`src/visual.cpp:617-650`) binds to `FileDirectory`, not to
   archive entries.  If the audited install serves textures from `.arc`, the
   progressive uploader never runs and its cost is pure overhead; if it serves
   them loose, the uploader is holding `MapViewOfFile` views open across frames
   and taking their page faults on the present thread.  The mod already has the
   counter to tell these apart (`g_diagUploadsCreated`).
2. The renderer can force a synchronous level load (§1a).  A probe counter on
   `Region::LoadLevel` entries during a frame would separate "the game loaded a
   level inside our render pass" from every mod-side suspect, which is exactly
   what Stage 3 run 7 needs to interpret its residual hitches.
3. The widened `shadow_split` does not pull regions in, but it does hold them in
   (§2), and it enlarges the set of regions a render-path call can trip a
   synchronous load on.  Reverting the split is not a fix for the "new area"
   hitch; it is a reduction in how often the game's own hazard is reached.
