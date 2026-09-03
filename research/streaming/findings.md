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

**Which class serves a given texture is a property of the installation, not of
the binary — and this one is switchable.**  The audited machine has a loose
high-resolution texture pack (12,519 `.tex`, 92.4 GiB, every one a `TEX\x01`
container, up to **341 MiB for a single texture** against the archives' 21 MiB
maximum) which drops into the game's `Settings/` directory.  With it installed
the *loose* class serves those textures and the progressive uploader engages;
with it parked, everything comes from `.arc` and the uploader has never run.
`arc-format.md`'s survey, and finding (1) below, describe the parked state.
Any measurement has to say which of the two it was taken in.

**This matters for the mod.**  `src/visual.cpp:617-650` requires
`vtable[2] == Engine.dll+0x14e560` and `vtable[4] == Engine.dll+0x14e540` before
it will lease a mapping or install `hookArchiveUnmap`.  Those are precisely
`FileDirectory::Lock` and `FileDirectory::Unlock`.  The archive `File`'s vtable
is `0x102f719c`, a different table, so **the progressive texture uploader
engages only for textures served from a loose-file source, never for a texture
read out of a `.arc`**.  The name `hookArchiveUnmap` is a misnomer; it hooks the
loose-file unmap.  Whether the audited installation actually serves the hot
textures loose or from archives is **unproven** here — it is a property of the
install, not the binary — but it is directly checkable from the mod's
own counters.  *(Corrected: the `g_diagUploadsCreated` counter this originally
named no longer exists.  Since Stage 0 of the mitigation plan the answer is
reported directly, as the `upload_src_arc` / `upload_src_loose` /
`upload_src_none` columns of the performance-trace CSV — and it had to be
reported through a new channel, because the counters that would have answered
it were being silently discarded; see §4 below.)*

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

**Where the mod's uploader sits.**  `src/streaming.cpp` wraps the render
device's present entry and calls `visual::onPresent` *before* the real present,
and `advanceTextureUploadsInternal` runs there.  *(Corrected: the line
reference this carried, `src/visual.cpp:3144`, was past the end of that file
even when it was written.  Prefer the symbol names, as the head of this
document says.)*  So the uploader runs at step 3, after the fence in
step 1 and after any forced load in step 2 — it is not literally queued behind
them within a frame.  But:

- The `UpdateSubresource` at `src/visual.cpp:888` reads from
  `job.source[mip].pSysMem`, which §1 proves is a `MapViewOfFile` view whose
  `UnmapViewOfFile` the mod deliberately deferred (`hookArchiveUnmap`,
  `src/visual.cpp:617`).  Every byte of that 1–2 MiB chunk that has not been
  touched yet is a page fault against the file, taken on the present thread.
  *(Corrected: that region is timed with `QueryPerformanceCounter`, not
  `GetTickCount`.  The 15.6 ms tick resolution was a real bug — it is what let
  the upload budget ratchet to its ceiling and produce a 34 ms chunk — but it
  was fixed in `17adf8a`, before this audit was written up.)*
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

---

## 4. The instrument could not see any of this

`probe::countInternal` and `probe::addPhaseInternal` both begin with

```c
if (g_renderThread && GetCurrentThreadId() != g_renderThread) return;
```

which is correct — the frame record is one unsynchronized struct and a write
from another thread would tear it — but it means **every counter and phase
recorded from the game's loader thread was being silently dropped**.  §1 proves
that the thread which runs `FUN_10213c80` is the thread that calls the D3D11
device, and that is the loader thread, not the render thread.  So
`CounterUploadJobsStarted`, `CounterUploadRejected` and `PhaseTextureCreate`
were reading zero for exactly the loads this audit was opened to investigate,
and no run recorded before Stage 0 can be used to argue anything about them.

The fix is a second channel — `probe::engineCount` — which accumulates into an
interlocked side array and folds into the frame record at `endFrame`.  Its
duration columns are named `_us` rather than `_ms`, because `tools/frames.py`
builds its "the mod's share" total from every column ending in `_ms` and would
otherwise charge the game's own loading time to the mod.

## 5. The uploader must copy, and must not take ownership

`GraphicsTexture::Initialize` (`0x10194120`) has two branches.  The `"DDS "`
branch calls the render device once.  The `"TEX"` branch is a `while` loop
that calls it repeatedly, walking forward through the *same* buffer by a
per-sub-blob length prefix.

`arc-format.md` records the measurement: decompressing the first block of 502
`.tex` entries from three scenery archives gives `TEX\x01` for every one and
`DDS ` for none.  **The looping branch is the only one that matters on this
install.**

So any design in which the mod takes ownership of the source buffer at the
first `CreateTexture2D` is a use-after-free while the loader thread is still
iterating.  That is the constraint on the archive work, and it is not
negotiable.

*(Corrected, against an earlier draft of this section: the shipped loose-file
path is **not** already broken by this.  It defers `UnmapViewOfFile` to job
completion, but the lease is reference-counted, and the second and later
sub-blobs of a `TEX` container are handled explicitly — by then the mod has
already swapped `source+0x34` to NULL, so `findTextureOwner` reports a null
`mappedBase`, `createLease` is skipped, the existing unsealed lease is found,
and `hooked` falls through to `lease->mappedBase != nullptr`.  The job joins
the lease and takes a count.  The view therefore outlives every job that reads
it.  The "live latent bug" claim was reasoning, not a byte, and it does not
survive reading the code; the constraint above stands on its own.)*

The uploader must therefore **copy** the mips it retains.  That is also what
makes both `File` classes equivalent from its point of view, which is why the
cross-reference below no longer recommends binding to the archive class.

## 6. The archive `File` class

vtable `Engine+0x2f719c`, object 0x28 bytes, constructed at `Engine+0x14d020`
by `FileSourceArchive::OpenFile` (`0x1014ed30`).  Read out of the pinned
`Engine.dll` at the recorded RVAs:

| slot | address | member |
| --- | --- | --- |
| 0 | `0x10028310` | scalar deleting destructor — frees `+0x18`, `+0x24`, `+0x20` |
| 2 | `0x1014cf30` | `Lock(offset, size)` |
| 3 | `0x1014cfc0` | `LockAll()` → `Lock(0, GetLength())` |
| 4 | `0x1014cf20` | `Unlock()` — `c6 41 10 00 c3`. **Frees nothing.** |
| 6 | `0x1014cf00` | `GetLength()` → `entry[0x0c]` (`8b 41 08 8b 40 0c c3`) |

```
+0x00 vtable             +0x10 locked flag (byte)
+0x04 FileSourceArchive* (→ +0xc = Archive*)
+0x08 entry record*      +0x14 scratch capacity   (uint)
+0x0c entry index        +0x18 scratch pointer    (operator new[])
+0x1c BlockBuffer.cachedBlockIndex (init 0xffffffff)
+0x20 / +0x24  two 256 KiB block scratch buffers
```

For comparison, the loose-file vtable at `Engine+0x2f71ec` is a different table
with the same shape: `[2] = 0x1014e560` (`Lock`), `[4] = 0x1014e540`
(`Unlock`), `[6] = 0x1014e500`.  A class check must therefore compare the
vtable *and* its slots, and must not assume the object is 0x38 bytes — the
archive `File` is 0x28, so a `readable(source + 0x34)` guard reads past its
end.

`Lock` grows `+0x18` to `size`, sets the locked flag, calls
`Archive::ReadFromFile`, and returns the buffer.  So `pSysMem` on the archive
path is **a fully materialized heap buffer** — no mapped view, and none of the
page-fault-on-Present hazard §3 flags for the loose path.

Ownership is *technically* transferable: the destructor's `operator delete[]`
is `MSVCR110.dll!??_V@YAXPAX@Z`, reached through Engine's IAT slot
`0x102ac304`, and `Archive::FreeFileBuffer` (`0x1011dce0`) is literally
`PUSH EAX; CALL [0x102ac304]`.  §5 is why we must not.  Recorded as the escape
hatch only.

## 7. Verified patch sites

All 5-byte `E8 rel32` unless noted; each read byte-for-byte out of
`generated/disassembly.asm`, which `tools/run-audit.sh` reproduces.

| # | site | bytes | what |
| --- | --- | --- | --- |
| P1 | `0x10167853` | `e8 68 46 0a 00` | `GraphicsDeferredRendererX::AddElementsInBox` → `Region::LoadLevel` |
| P2 | `0x1017d8c3` | `e8 f8 e5 08 00` | `GraphicsForwardRenderer::AddElementsInBox` → `Region::LoadLevel` |
| P3 | `0x10144484/8c/94/9c/a4/af/bd` | seven `e8 …` to `0x10120250` | the seven `UnloadUnreferencedResources` sweeps in `Engine::Update` |
| P4 | `0x1014467a` | `e8 71 64 13 00` | `Engine::Update` → `World::UpdateRegionUsage` |
| P5 | `0x10144789` | `e8 02 ad fd ff` | → `FUN_1011f490` (thread prune) — opens the loader fence |
| P6 | `0x101447a2` | `ff 15 88 c1 2a 10` (6 B) | → `WaitForSingleObject(fence, INFINITE)` |
| P7 | `0x101447a8` | `e8 63 ab fd ff` | → `FUN_1011f310` — closes the fence |
| P8 | `0x1011d1d6` | `e8 85 85 f4 ff` | block decompress → `FUN_10065760` |

Both `AddElementsInBox` sites have the identical shape, and the two
instructions after the call are what would make an asynchronous retarget safe:

```
  6a 00 / 8b cf              PUSH 0 ; MOV ECX, Region*
  e8 <rel32>                 CALL Region::LoadLevel
  80 7f 74 00                CMP byte [EDI+0x74], 0
  c7 47 6c 00 00 00 00       MOV dword [EDI+0x6c], 0
  0f 85 <rel32>              JNZ epilogue     ; region skipped while loading
```

`MOV` does not touch flags, so the `MOV dword [EDI+0x6c],0` between the `CMP`
and the `JNZ` runs on the skip path too: a region deferred rather than loaded
still has its unload countdown reset and cannot be evicted while the load is in
flight.

`FUN_10065760` **is zlib `uncompress` built `__fastcall`**: `ECX` = dest,
`EDX` = `uLongf* destLen`, `source` and `sourceLen` on the stack, and the
**caller** cleans them (`1011d1df  ADD ESP,8`).  Its body is
`inflateInit_` (`0x10067680`) → `inflate` (`0x10067810`) → `inflateEnd`
(`0x10068eb0`) with `uncompress`'s exact `Z_BUF_ERROR` / `Z_NEED_DICT`
mapping.  Two consequences:

- a replacement cannot be declared as a plain GCC `__fastcall` function, whose
  convention is callee-pop; it needs a hand-emitted thunk;
- its prologue is `55 8b ec 83 e4 f8`, byte-identical to `src/grass.cpp`'s
  `kRenderPrologue`, and shared with `Region::LoadLevel`,
  `Archive::ReadFromFile` and `Region::GetEntitiesInFrustum`.  **A six-byte
  prologue match proves nothing about identity.**  A hook here must verify
  16–24 bytes even though it only steals 6–7.

Almost every hook target is exported by decorated name
(`?LoadLevel@Region@GAME@@QAE_N_N@Z`, `?ReadFromFile@Archive@GAME@@QBE…`,
`?LoadResource@ResourceLoader@GAME@@QAEXPAVResource@2@@Z`, …), so the right
shape is to resolve by name and use the RVA as an identity assertion rather
than as the lookup.  The recorded main-thread id is at `Engine+0x41a5dc`
(`CMP EAX,[0x1041a5dc]` at `0x1014476b`), free to read for thread attribution.

`Region::WaitForLoadingToFinish` (`0x1020bde0`) is exactly seven bytes,
`80 79 78 01 74 fa c3`, and takes no arguments.  A trampoline over it is
impossible — the stolen bytes contain the relative `74 fa` — so instrumenting
it means replacing it outright rather than detouring it.

## 8. What run 10 measured, once the instruments were in

Stage 3's hooks went into the pinned `Engine.dll` on 2026-09-03 -- all ten
groups, three of three region-lock sites, seven of seven sweeps -- and a
99.7-second session over the same Eternal Embers route as runs 8 and 9, with
the texture pack installed, cost nothing to measure: p50 9.0 ms, p99 45.8 ms,
mod share 9.5%, against run 8's 9.0 / 45.9 / 9.5%.  That matters on its own,
because the instrumentation includes a detour on a function the session
entered **12.0 million** times.

### Four hypotheses this audit raised are now closed by measurement

- **§1b, the render path blocking on the region lock, does not happen.**
  `engine_region_lock_hits` is **0** for the whole session across all three
  sites -- `Region::GetEntitiesInFrustum` and both `AddElementsInBox`
  overloads.  Every acquisition was uncontended.  The thunk takes timestamps
  only on failure, so this is a real zero and not a threshold artefact.
- **The seven `UnloadUnreferencedResources` sweeps cost nothing.**  51,443
  calls -- exactly seven per `Engine::Update` -- totalling **11.2 ms** across
  the session.  Amortizing them cannot buy anything.
- **The loader fence is already signalled, as §3 predicted.**  7,349 waits,
  one per frame, totalling **1.6 ms**: 0.22 microseconds each.  The rendezvous
  is not the cost.  (What `FUN_1011f490`'s per-thread `OpenThread` /
  `WaitForSingleObject` / `CloseHandle` walk costs is still unmeasured; only
  the wait itself is closed.)
- **`Region::WaitForLoadingToFinish` is never called.**  `engine_wait_loading`
  is 0.  The `.text` scan that found no caller is confirmed at runtime, and
  the hazard is closed rather than merely unlikely.

### What the loading path actually costs

The reproducible worst frame is fully attributed for the first time.  Frame
2202, 1,465.8 ms:

| | |
| --- | --- |
| `Region::LoadLevel` | **511 ms**, 5 calls, **100% on the engine's main thread** |
| `ResourceLoader::LoadResource` | 362 ms, 764 calls, all on the main thread |
| block read + inflate | 264 ms, 1,468 blocks, from 1,282 `ReadFromFile` over 82 MiB |

These nest -- inflate inside the resource load inside the level load -- so the
renderer really does force a synchronous level load onto the main thread, and
one of them cost half a second.  §1a's mechanism is confirmed as a measurement
rather than as a reading of the disassembly.

Two qualifications matter for what to do about it.  **511 ms of 1,466 is the
whole of what the level load explains**; roughly 950 ms of that frame is in
nothing any column can see.  And in 7,347 frames, **exactly one** has a
`Region::LoadLevel` costing more than a millisecond -- the other 203,414 calls
take the resident fast path and are free.  Making the load asynchronous would
therefore fix the worst frame in the session and almost nothing else.

Resource loading is the more frequent cost: 1.90 s over the session, 1.71 s of
it on the main thread, spread over 11 frames that spend more than 20 ms in it.

### The archive numbers Stage 4 was sized against

`Archive::ReadFromFile` was called 4,941 times for 1.03 GiB, and the block
routine below it inflated **7,527 blocks in 4.38 s** -- 582 microseconds each.
Seven thousand five hundred 256 KiB blocks is 1.88 GiB inflated to serve 1.03
GiB requested: a **1.8x amplification**, which is the single-slot block cache
of R1 re-inflating what it has already inflated.  4.1's multi-block cache is
aimed at a real number.

> **Corrected by run 21; see §19.**  The 4.38 s and the 1.8x are measurements
> and they stand.  "Re-inflating what it has already inflated" is not -- it is
> an inference, and inflated-over-served reads the same whether the excess is
> re-inflation or partial consumption of blocks that are never asked for
> again.  Run 21 is the first evidence, and it does not yet settle it.  Do not
> quote this paragraph's second clause as established.
>
> **Settled by run 22 (§20): it is false.**  With a 256 MiB cache -- 1,024
> resident blocks -- 91.8% of block requests are still for a block nothing
> has seen before.  The excess is partial consumption, not re-inflation.

### A column that means something other than its name

`engine_res_enqueued` recorded **12,003,283** calls, 1,634 a frame.
`ResourceLoader::EnqueueResource` is not a queue insert that happens when work
arrives; it is a per-resource touch that happens for everything in view, every
frame.  It is not a backlog signal and should not be read as one.

### The second class of hitch is not loading at all

Runs 8 and 9 both carried hitches that named nothing.  With the engine
instrumented, four frames of 200-242 ms show **every engine column at or near
zero**: no level-load time, no resource load, no inflate, no archive read
worth the name, no lock contention, sweeps of 1-390 microseconds.  Every mod
phase is under 0.05 ms, the game's own `Present` returns in 0.04 ms, and the
GPU's own passes total about 6 ms.  Their draw, map and level-load counts are
ordinary.

So 240 milliseconds of wall clock pass in which nothing this instrument can
see happens.  `gpu_frame` spans those frames almost entirely, but that is not
evidence of GPU work -- the whole-frame region also spans a CPU stall, so it
cannot separate the two.

This class is not explained by the archive path, and it is not explained by
anything Stages 4, 5 or 6 propose.  It is the reason `Engine::Update` and
`Engine::Render` are now bracketed as well: between them they say which half
of the game's frame the time is in, or that it is in neither -- and neither
would put it outside `Engine.dll` entirely.

### One number for a question that was left open

`upload_leased_mib` peaked at **1,064 MiB** (mean 257) with the texture pack
installed.  Over a gigabyte of address space held in `MapViewOfFile` leases at
once, in a 32-bit process already carrying about 336 MiB of the mod's shadow
targets.  The pool's only limit is a count of 128 leases, and that count is
not a safe one.

## 9. Run 11: where the frame actually goes

Stage 3 gained two more brackets -- `Engine::Update` (`0x101443a0`) and
`Engine::Render` (`0x10143fe0`), each detoured whole, once a frame -- and run
11 repeated run 10's route with them in.  For the first time the session's
time is fully accounted for:

| | | |
| --- | ---: | ---: |
| `Engine::Render` | 58,174 ms | 57.9% |
| waiting on the game's `Present` | 12,539 ms | 12.5% |
| `Engine::Update` | 10,313 ms | 10.3% |
| **outside all of them** | 10,009 ms | **10.0%** |
| the mod's own phases | 9,520 ms | 9.5% |

*(Corrected after run 12: the "outside" row above was computed by subtracting
the other four from the frame, so its summing to 100% was circular and proved
nothing.  Run 12 established that the game's `Present` is called **outside**
`Engine::Render`, not inside it -- the median frame spends 2.00 ms in
`Engine::Render` and 3.57 ms in `Present` -- so the rows really are disjoint
and the totals stand.  But the arithmetic was not the check it was described
as, and §10 states it properly.)*

### The hitch time splits three ways, not one

Over the 70 frames above 50 ms (9.68 s):

| | | |
| --- | ---: | ---: |
| `Engine::Render` | 4.54 s | 46.9% |
| **outside all of them** | 3.72 s | **38.4%** |
| `Engine::Update` | 0.67 s | 6.9% |
| the mod | 0.47 s | 4.8% |
| `Present` | 0.28 s | 2.9% |

By frame count the ranking inverts: of the 32 frames over 100 ms, **18 are
dominated by time outside everything**, 11 by `Engine::Render`, 2 by
`Engine::Update`, 1 by `Present`.

### The worst frame is where §1a said it would be

Frame 1753, 1,453.8 ms: **`Engine::Update` 0.0 ms, `Engine::Render` 1,448.6
ms, outside 5.2 ms**, with `Region::LoadLevel` accounting for 505.7 ms of the
render.  The forced synchronous load really does happen inside the render
pass, through `AddElementsInBox`, exactly as §1a read it off the
disassembly -- and the ~950 ms §8 could not place is inside `Engine::Render`
too.  Stages 4 and 5 are aimed at real ground.

### The unexplained class was never one mechanism

§8's "second class" resolves into three:

- **A simulation stall.**  Frame 1425: 245 of 253 ms inside `Engine::Update`.
- **Frames that draw nothing.**  Frames 4, 26, 83, 111, 121, 1956, 2124:
  `draw_indexed` 0-1, `map` 2-19, `Engine::Render` 0.0 ms, 55-172 ms each.
  Loading screens and zone transitions.
- **Frames that draw normally and stall anyway.**  Frames 4615, 4871, 5018,
  5146, 5398, 5585, 5743, 6044, 6218 and others: 400-1,600 indexed draws,
  800-2,100 maps, a 4-9 ms shadow pass, `Engine::Update` about 2 ms,
  `Engine::Render` about 10 ms -- and **100 to 225 ms outside everything**.
  The mod is idle on every one of them: `upload_jobs_started`,
  `upload_unmap` and `upload_kib` are all 0.

A normal 9 ms frame spends 0.30 ms in `Engine::Update`, 1.60 ms in
`Engine::Render` and **0.21 ms outside**.  The third group is a thousandfold
excursion in the one region nothing measures.

### One hypothesis tested and rejected

Address-space pressure from the mod's own mapping leases does not explain it.
The tenth of the session with the worst stalls (frames 4970-5680, 1.43 s
outside, peak 224 ms) held a p95 of **18 MiB** of leases; the tenth holding
**1,024 MiB** had the *least* outside time in the whole run, 0.15 s.  The
correlation is absent, and if anything inverted.

### What is left, and the one bracket that can decide it

The remaining candidates for that 10 s are `Game.dll`'s simulation, TQ.exe's
own main loop, and the CrossOver/DXMT layer below the process.  `Game.dll`
exports `?Update@GameEngine@GAME@@QAEXH@Z` (`Game+0x19a230`, `__thiscall
void(int)`, confirmed by the `ret 4` at `+0x5ee`), which is the top-level
simulation tick and the only candidate large enough to hold it.  It is now
bracketed the same way -- 24 bytes verified, six stolen, once a frame -- and
it is the first hook in this work that is not in `Engine.dll`.

## 10. Run 12: the dark time is not the game's, either

`GameEngine::Update` (`Game+0x19a230`) bracketed the same way, and the answer
is a clean negative.

**`Game.dll`'s simulation is 307 ms of a 102,662 ms session -- 0.3%.**  It is
1.3% of the time in frames over 50 ms and **0.0%** of the time in frames over
100 ms.  On every stall frame it reads 0.0-0.1 ms.  Whatever the stalls are,
they are not the game thinking.

With four disjoint brackets the frame decomposes exactly, and this time the
parts are measured rather than derived:

| | | |
| --- | ---: | ---: |
| `Engine::Render` | 56,655 ms | 55.2% |
| the game's `Present` | 14,542 ms | 14.2% |
| **unexplained** | 11,879 ms | **11.6%** |
| `Engine::Update` | 9,860 ms | 9.6% |
| the mod, at Present | 9,420 ms | 9.2% |
| `GameEngine::Update` | 307 ms | 0.3% |

`Present` is called **outside** `Engine::Render`, which run 11 assumed the
other way: the median frame spends 2.00 ms in `Engine::Render` against 3.57 ms
in `Present`, and only 539 of 1,852 ordinary frames have a render long enough
to contain it.  So the game's main loop calls it, not the renderer.

In the frames that hitch the residual is not a rounding error:

| | over 50 ms (9.07 s) | over 100 ms (7.60 s) |
| --- | ---: | ---: |
| `Engine::Render` | 46.9% | 46.5% |
| **unexplained** | **40.9%** | **44.7%** |
| `Engine::Update` | 6.2% | 4.7% |
| the game's `Present` | 3.2% | 3.5% |
| the mod | 1.5% | 0.6% |
| `GameEngine::Update` | 1.3% | 0.0% |

### The shape of it is the clue

The residual is not a tax spread over the session.  On ordinary frames its
median is 0.12-0.61 ms.  It arrives as twenty discrete events of 50-398 ms,
and those events come in **bursts**: four in the first 1.2 seconds, then
nothing for 27 seconds, then nine between 58.7 s and 67.2 s spaced 0.2 to
2.9 seconds apart, then quiet again.  Bursts do not look like game logic and
they do not look like content -- run 11's stalls fell on different frames of
the same route.

### What TQ.exe actually imports

The main loop is the only place left, and its import table is short enough to
be suggestive.  From `KERNEL32` and `USER32` it takes `Sleep`,
`WaitForSingleObject`, `GetMessageA`, `QueryPerformanceCounter` and
`CreateFileA` -- and **no `PeekMessage` at all**.  From `Game.dll` it imports
`?NeedsSleep@GameEngine@GAME@@QBE_NXZ`.

So the loop asks the game whether it should sleep, and then sleeps; and it
pumps messages with the **blocking** `GetMessageA` rather than the polling
`PeekMessage`.  Either of those can stall for as long as the host feels like,
neither is instrumented, and both would produce exactly the burst pattern
above on a machine that is occasionally busy.  Both live in TQ.exe's import
table, so measuring them is an IAT write and not a `.text` write -- the
cheapest and most reversible instrument in this whole investigation.

## 11. Run 13, and reading the main loop instead of guessing at it

Run 13 timed the three things TQ.exe's main loop could block in, through its
import table.  All three are negative, and decisively:

| | |
| --- | --- |
| `Sleep` | **1 call in the session.** 100 ms requested, 100 ms delivered. It is not a frame limiter and it is not being overslept. |
| `GetMessageA` | **0 calls.**  The import exists; the loop never uses it. |
| `WaitForSingleObject` | **0 calls.** |
| free address space | never below **3,445 MiB**.  Exhaustion is closed for good. |

The residual survived at 10,476 ms -- 10.8% of the session and **36.3% of the
time in frames over 50 ms**.

So the guessing stopped and the loop was read.  `Engine::Render` has exactly
one caller, `TQ.exe+0x44eea6`, and every call in the surrounding loop resolves
through the import table:

```
  GetInputDevice / WorldFrustum / GetFrustumForPlayer / IsRenderingEnabled
  Engine::Update                        <- bracketed, 10.4%
  WorldCamera::UpdateFromInput
  Engine::GetUpdateTime
  GameEngine::Update                    <- bracketed, 0.4%
  InterpenetrationManager::FixupCharacterCollisions
  GetMachineTime                        ; [ebx+0x23c] = update time
  Engine::PresentSurface                <- NOT BRACKETED
  GetMachineTime                        ; edi = present time
  Engine::Render                        <- bracketed, 57.4%
  GetMachineTime                        ; [ebx+0x240] = render time
  AreStatsEnabled / AddStatisticText / PlayStats::Display / UpdatePerfTracker
  Profile::EndFrame / GameEngine::NeedsSleep
```

The loop's own order is `Update`, then **`PresentSurface`**, then `Render` --
the present is deferred to the start of the next iteration -- and the game
times all three for itself with `GetMachineTime`.

**That is where the residual has to be, and it is a consequence of the
instrument's own geometry.**  The probe's frame boundary is the renderer's
`Present`, which is called *inside* `Engine::PresentSurface`.  So the head of
that function -- everything it does before reaching D3D, which is exactly
where a wait on the swapchain or on the GPU would sit -- has been inside every
frame's measured window and inside none of the brackets, in every run so far.

`Engine::PresentSurface` and the collision fixup are both imported by TQ.exe,
so both are now bracketed through the import table: no patched code at all.

## 12. Run 14, and the loop read properly this time

`Engine::PresentSurface` is **24.4% of the session** but only **4.6% of the
time in frames over 50 ms**, and it turns out to have no head of its own worth
speaking of: its 25,784 ms is the game's D3D `Present` (17,869 ms) plus the
mod's own Present-time work (8,777 ms).  The geometric argument in §11 was
sound about where the instrument was standing and wrong about what was
standing there.  The collision fixup is 196 ms.  The residual is unchanged:
11.8% of the session, **35.5% of the time in frames over 50 ms**.

The mistake was narrower than the hypothesis.  §11 read the loop through a
700-byte window around the `Engine::Render` call.  The loop's closing branch
is at `TQ.exe+0x44f286` and its head at `+0x44e970`: **2,326 bytes**, and the
part that was never read contains the candidates that matter.

Every call in the whole loop, with the ones that only fetch a pointer removed:

```
  THQNO_Process                              thqno_api.dll
  Jukebox::Update                            Engine.dll
  GraphicsEngine::UpdateFromOptions          Engine.dll
  SoundManager::Update                       Engine.dll
  VideoPlayer::Update / Render               (guarded by GetIsPlaying)
  Engine::PresentSurface                     x3 sites  <- 24.4%, not the stalls
  Engine::Update                             <- 10.6%
  GameEngine::Update                         <- 0.4%
  InterpenetrationManager::FixupCharacterCollisions  <- 0.2%
  Engine::Render                             <- 52.4%
  PlayStats::Display / UpdatePerfTracker / Profile::EndFrame
  GameEngine::NeedsSleep -> call ebp         (ebp = the Sleep import slot)
  QuestRepository::FireTriggers              Game.dll
  EWindow::ProcessMessages                   Engine.dll  <- ends the loop
  plus nine TQ.exe-internal direct calls
```

Two things fall out of that immediately.

**The message pump is Engine.dll's, not the executable's.**
`EWindow::ProcessMessages` is the last call before the backward branch and its
return value is the loop condition.  That is why run 13 measured **zero** calls
to TQ.exe's own `GetMessageA`: the import exists and is never used, and the
real pump was never in the measured set.

**And the game has an online platform layer that it polls every frame.**
`THQNO_Process` is the *first* call in the loop, from `thqno_api.dll`, with
`libcurl.dll` in the same import list.  Bursty network or IPC work is the
precise shape of what §10 measured -- twenty discrete events of 50-398 ms
arriving in clusters, unrelated to route or content.

Six brackets were added for run 15, in loop order: `THQNO_Process`,
`GraphicsEngine::UpdateFromOptions`, `Jukebox::Update`, `SoundManager::Update`,
`QuestRepository::FireTriggers`, `EWindow::ProcessMessages`.  All six are
imported by TQ.exe, so like run 13 and run 14 this adds **no patched code at
all** -- eleven entries of the executable's own import table now carry the
whole main-loop instrument.

Also worth recording from the disassembly: `mov ebp, ds:0x5efa6c` at
`+0x44f278` loads the `Sleep` import slot into `ebp` on every iteration, and
`call ebp` at `+0x44efee` is the sleep.  So run 13's redirect of that slot was
genuinely on the path, and its one call in a session is a real measurement of
`NeedsSleep` rather than a missed hook.

## 13. Run 15: it is the window message pump

Eleven brackets, covering every call in TQ.exe's main loop that does work
rather than return a pointer.  The residual fell from 11.6% of the session to
**2.3%**, and from 35.5% of the time in frames over 50 ms to **12.2%**.  What
took it:

| whole session | | frames over 50 ms | |
| --- | ---: | --- | ---: |
| `Engine::Render` | 57.2% | `Engine::Render` | 51.0% |
| `Engine::PresentSurface` | 21.6% | **`EWindow::ProcessMessages`** | **20.3%** |
| `Engine::Update` | 10.2% | unexplained | 12.2% |
| **`EWindow::ProcessMessages`** | **8.0%** | `Engine::Update` | 8.8% |
| unexplained | 2.3% | `Engine::PresentSurface` | 5.2% |

And the stall frames are the pump almost in their entirety:

```
 frame       ms |      pump    render    update    unexplained
  4592    224.2 |     213.9       4.4       3.9        0.1
  5858    206.6 |     192.5      11.3       2.2        0.5
  4460    193.8 |     188.8       1.7       2.3        0.1
  7195    210.1 |     188.0      15.8       4.3        0.3
  3768    178.6 |     151.2      20.5       4.6        0.2
  3482    147.8 |     118.3      23.4       3.8        0.1
```

That is the "second class" of hitch, the one §8 could not name and §9 found on
frames that draw normally with the mod idle.  It is the window message pump.

**The platform layer was a wrong guess and should be recorded as one.**
`THQNO_Process` -- the online poll with `libcurl` behind it, and the candidate
I rated most likely -- is **44 ms in the session**.  `SoundManager::Update` is
2 ms, `Jukebox::Update` and `QuestRepository::FireTriggers` round to nothing,
`GraphicsEngine::UpdateFromOptions` is 28 ms.  All negligible, all closed.

### The pump is nine instructions, and there are only two places the time can be

`EWindow::ProcessMessages` (`Engine+0x272380`) reads, in full:

```
  edi = PeekMessageA   ebx = TranslateMessage   ebp = DispatchMessageA
loop:
  PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)
  if (!result) goto done
  TranslateMessage(&msg)
  DispatchMessageA(&msg)          ; -> the game's window procedure
  if (msg.message == WM_QUIT) return false
done:
  if (result == 1) goto loop      ; drain the queue
  return this[0x24] == 0
```

It is called once per frame (7,471 calls in 7,469 frames), so a 214 ms pump is
one call, not an accumulation across the frame.  The time is therefore in
`PeekMessageA` or in `DispatchMessageA`, and the two mean opposite things:

- **many cheap peeks** is a message flood, and the count says so directly;
- **one expensive peek** is the host not answering -- under CrossOver a
  wineserver round trip, and not the game's problem at all;
- **time in dispatch** is the game's own window procedure, which is the only
  one of the three that is the game's problem and the only one fixable here.

Both are imports of **Engine.dll**, not of the executable -- the pump is
Engine.dll's code, which is the same reason TQ.exe's `GetMessageA` read zero
calls in run 13 -- so splitting them is two more import-table entries and
still no patched code.

## 14. Run 16: the pump is `PeekMessageA`, and it is the host

| | | |
| --- | ---: | ---: |
| `EWindow::ProcessMessages` | 8,395 ms | 6,798 calls |
| — `PeekMessageA` | **8,297 ms** | 11,367 calls — **98.8% of the pump** |
| — `DispatchMessageA` | 72 ms | 4,569 calls |
| — the loop's own nine instructions | 27 ms | |

The worst frames are **212 ms in two peeks**.  Not a message flood -- the
median frame peeks twice and the maximum is nineteen.  Not the game's window
procedure -- dispatch averages 15.7 µs and totals 72 ms in a session.

**`PeekMessageA` averages 730 µs per call on this machine, against 0.67
dispatched messages per frame.**  The queue is empty almost every time, and
polling an empty queue costs three quarters of a millisecond, with excursions
to 200.  That is 8.2% of wall clock and **1.22 ms of every average frame**,
before the spikes.

### Three checks, because this is a conclusion about the host

- **The instrument is not creating it.**  Run 15 measured the pump at 7,974 ms
  with no hook inside it; run 16 measures 8,395 ms with two.  The split did not
  inflate what it split.
- **It is not the mod's or the game's I/O starving the host.**  Frames with a
  peek over 20 ms and frames with a peek under 2 ms have the *same* median
  archive I/O -- zero.  The three worst non-load frames (143.0, 50.8, 46.5 ms
  of peek) have **no** archive activity within five frames either side.
- **It is not correlated with load.**  The heavy frames that do show I/O
  (2910, 2913) are the level-load frames where everything is heavy.

So the second class of hitch, unnamed since run 8, is `PeekMessageA` on an
empty queue taking a fifth of a second.  Under CrossOver that is a wineserver
round trip, and **it is not the game's problem and not reachable from this
mod.**  The mod owns the import slot and can measure it; there is nothing
useful to put in its place, because the work is the round trip itself.

The one lever is the host: how CrossOver is configured to synchronise (its
wineserver / esync-style options) directly changes what a round trip costs.
That is a setting to test, not code to write.

### Where the frame goes, complete

After seven instrumented runs, the frame is fully accounted for and the
remaining hitch time has one fixable owner:

| | session | frames over 50 ms |
| --- | ---: | ---: |
| `Engine::Render` | 57.2% | 51.0% |
| `Engine::PresentSurface` | 21.6% | 5.2% |
| `Engine::Update` | 10.2% | 8.8% |
| **`PeekMessageA`** | **8.2%** | **20.3%** |
| unexplained | 2.3% | 12.2% |
| everything else measured | <1% | <3% |

`Engine::Render` is the half that is ours to attack, and §8 already priced the
work inside it: 4.38 s a session in archive block inflates, 1.88 GiB inflated
to serve 1.03 GiB.  That is Stage 4.1, and it is the last item in this
investigation with both a number and a fix.

## 15. Run 17: Greece, and the peek split

A different act, different archives, different terrain, and the finding
holds -- but the split inside it is the opposite of what §14 predicted, and
the correction matters.

### The pump is route-independent

| | Eternal Embers (run 16) | Greece (run 17) |
| --- | ---: | ---: |
| session | 101,378 ms | 87,496 ms |
| `PeekMessageA` | 8,297 ms, **8.2%** of wall | 4,568 ms, **5.2%** of wall |
| mean per peek | 730 µs | 455 µs |
| worst frame | 212.0 ms | 165.2 ms |
| `DispatchMessageA` | 15.7 µs mean | 15.4 µs mean |
| pump's share of frames over 50 ms | 11.3% | 12.1% |

Greece is quieter in absolute terms and identical in shape.  Two completely
different regions produce the same behaviour at the same share of the hitch
time, which is what a property of the host looks like and is not what content
looks like.

### But the expensive peeks are the ones that find a message

§14 predicted the cost was in polling an empty queue.  It is not:

| | calls | total | mean |
| --- | ---: | ---: | ---: |
| peeks that returned a message | 4,193 | 3,440 ms | **820 µs** |
| peeks that found the queue empty | 5,844 | 1,129 ms | 193 µs |

Retrieving a message costs four times what finding nothing costs, and 75% of
all the peek time is in retrieval.

The per-frame numbers say what that means, and it is not queue depth.  The
median frame sees **one** message and 2,574 of 5,842 frames see none; the
maximum in any frame is six.  On the worst frames:

```
 frame       ms |   peek(empty)  peek(message) | #peek #empty #msg
  4633    188.7 |          0.0          165.2  |     4      1     3
  5360    141.7 |          0.0          126.3  |     2      1     1
  3451    147.1 |          0.0          125.2  |     2      1     1
  4858    131.5 |        118.9            0.0  |     1      1     0
```

**A single `PeekMessageA` retrieving a single message takes 126-165 ms.**  And
frame 4858 shows an empty poll doing the same thing, so both forms can block.

That is not a cost proportional to work; it is a round trip that sometimes
does not come back for a fifth of a second.  Retrieval is dearer on average
only because it must always ask, where an empty poll can sometimes be answered
without asking.  The conclusion of §14 survives -- the cost is the round trip,
not anything the game or its window does -- but the reasoning given for it was
wrong and is corrected here.

### Also from this run: Stage 4.1's case is stronger in the base game

| | blocks | inflated | requested | amplification |
| --- | ---: | ---: | ---: | ---: |
| Eternal Embers | 7,491 | 1,917,696 KiB | 1,070,055 KiB | 1.8x |
| **Greece** | 4,560 | 1,167,360 KiB | 505,729 KiB | **2.3x** |

Greece does about half the archive work and wastes a larger fraction of it:
2.3 bytes inflated for every byte asked for, at 467 µs a block.  The
single-slot cache is worse where the reads are smaller, which is what a
one-entry cache in front of a 2 GB file should do.

### A note on why the message histogram is missing from this run

It was written from `engineprobe::shutdown()`, which never runs.  Titan Quest
exits without unloading, so `reserved` is set at `DLL_PROCESS_DETACH` and
`src/fix.cpp` takes the branch that calls only `probe::flushOnExit()` -- the
reason that function exists at all.  The histogram now writes from the
`Engine::Render` bracket every 1,800 frames instead, so the last snapshot
covers nearly the whole session whether or not teardown is reached.

## 16. Run 18: what the messages are

The histogram, from the last snapshot of a 105-second Eternal Embers session:

| message | count | retrieved by a peek over 5 ms | slow rate |
| --- | ---: | ---: | ---: |
| `WM_MOUSEMOVE` | 2,158 | 28 | 1.3% |
| **`WM_TIMER`** | **1,495** | **90** | **6.0%** |
| `WM_LBUTTONDOWN` / `UP` | 53 | 0 | |
| `WM_MOUSEWHEEL` | 9 | 0 | |
| keyboard | 9 | 0 | |
| `WM_PAINT` | 1 | 0 | |

Nine kinds of message in a whole session, 3,725 in total.  **76% of the slow
retrievals (90 of 118) return `WM_TIMER`**, and a `WM_TIMER` retrieval is 4.6
times more likely to be slow than a mouse move.

That is a real asymmetry and it has a mechanical explanation.  `WM_TIMER` is
**synthesized, not queued**: nothing posts it, and `PeekMessage` manufactures
it when the queue is otherwise empty and a timer has expired.  Finding one
therefore *requires* asking the host, where a queued message may already be in
the client's own buffer.  So the messages most likely to be slow are exactly
the messages that cannot be answered without a round trip -- which is the same
explanation §14 and §15 arrived at, arriving a third time by a different road.

**The timer is the game's own.**  `TQ.exe` imports `SetTimer` and `KillTimer`
(`Engine.dll` and `Game.dll` import neither), and `WM_TIMER` arrives at 14.2 a
second against `WM_MOUSEMOVE`'s 20.5.

Run 18's peek numbers, for the record: 9,002 ms, 8.5% of wall clock;
retrievals 6,651 ms over 5,042 calls (1,319 µs mean); empty polls 2,351 ms
over 7,925 calls (297 µs mean).

### Correlation or cause, and the experiment that separates them

Two readings fit the asymmetry equally well so far:

- the timer *causes* the stalls, because servicing it forces a round trip
  fourteen times a second; or
- the timer merely *correlates*, because a peek that has to go to the host is
  both slow and the only kind that can come back with a `WM_TIMER`.

They differ in what would help.  `[performance] timer_period_ms` settles it:
`0` is the default and leaves the game's own period alone, byte for byte; any
other value replaces the period `TQ.exe` asks for.  If the stalls scale with
the timer rate the first reading is right; if lengthening the period changes
nothing, the second is, and the pump is closed as a host property with no
lever in it.

### Run 19: `SetTimer` is never called, and why that is informative

The hook installed (12 of 12 imports redirected) and logged **nothing**.
`TQ.exe` arms its timer during startup, before the renderer exists and long
before this module can be loaded -- so the call is already in the past by the
time anything here is watching.  The `timer_period_ms` switch as first written
could therefore never have fired.

But the timer identifies itself in every message it produces.  A `WM_TIMER`
carries `hwnd`, `wParam` = the timer id, and `lParam` = its `TIMERPROC` or
null, which is exactly what `SetTimer` needs to re-arm the same timer.  So the
experiment is rebuilt around the messages instead of the call: the first
`WM_TIMER` supplies the identity, and `SetTimer` on an `(hwnd, id)` pair that
already exists replaces the period and leaves everything else alone.  The
`TIMERPROC` is passed straight back rather than cleared -- passing null there
would turn a callback timer into a posted one and change *what* the game does
rather than only *when*.

The period itself is also derived from the messages: the **shortest** gap
between two `WM_TIMER` arrivals.  `WM_TIMER` is synthesized only when the
queue is otherwise empty, so every gap is inflated by coalescing and only the
minimum approaches the period actually asked for.  Run 18's 14.2 arrivals a
second is a lower bound on the timer's rate, not a measurement of it.

## 17. Run 20: the timer is not the game's, and the pump has no lever

```
Engine trace: game timer hwnd=00000000 id=32767 proc=FFFF0016
              shortest gap 2927 us over 1461 samples
```

Three things in one line, and together they end this.

**`hwnd = NULL` makes it a thread timer, not a window timer.**  That matters
mechanically rather than as a detail: `SetTimer` only re-periodises an
existing timer when it is given a *window* and a matching id.  For a thread
timer the id argument is ignored and a **new** timer is always created.  So
the re-arm designed in §16 would not have changed this timer's period at all;
it would have added a second timer beside it, and handed it the observed
`lParam` -- `0xFFFF0016`, which is not a code address in a 32-bit user
process -- as a `TIMERPROC` to call.  That is not an experiment, it is a
crash.  The path is now refused, and logs why, rather than being left as a
loaded footgun in the tree.

**And it is very unlikely to be the game's timer.**  `TQ.exe` imports
`SetTimer` and never calls it in a session -- run 19 hooked it and logged
nothing across a full route.  A timer with no window, an id of `0x7fff`, and
an `lParam` that is not a callback is not the shape of something a game
arms for itself.  It looks like something the host synthesizes, which if true
is the third independent line of evidence pointing at the same conclusion.

**The period is not what the arrival rate suggested, and neither number
supports the causal reading.**  The shortest gap between arrivals is 2.93 ms
and the mean is 70 ms.  If the timer really ran at ~3 ms it would be pending
at almost every pump call -- frames are 13 ms -- and `WM_TIMER` would arrive
about once a frame.  It arrives 0.19 times a frame.  So the 2.93 ms minimum is
an outlier, most likely two fires landing either side of a stall, and the
timer is not running fast enough for its rate to be the thing driving 8% of
wall clock.

### So the pump is closed

Four runs, four independent roads, one answer:

- §14: the cost is in `PeekMessageA`, 98.8% of the pump, one call per stall.
- §15: Greece reproduces it at the same share of hitch time on entirely
  different content, and a *single* retrieval of a *single* message takes
  126-165 ms.
- §16: the messages most likely to be slow are exactly the ones that cannot be
  answered without asking the host.
- §17: the timer behind those is not the game's to change, and could not be
  changed by id even if it were.

`PeekMessageA` costs 7.8-8.5% of wall clock and 12-20% of the hitch time on
this install, and **there is no lever in it from inside the process**.  The
mod owns the import slot, can measure it exactly, and has nothing useful to
put in its place, because the work is the round trip itself.  What remains is
a host question -- how CrossOver is configured to synchronise -- and a
comparison against a Windows machine, where the same build should report
`pump_peek_us` near zero.

That is the honest end of this line, and it is worth having reached rather
than assumed: three of the five stages this plan opened with were aimed at
mechanisms that turned out to cost single-digit milliseconds a session, and
this one was aimed at a mechanism that is real, large, and not ours.

## 18. Stage 4.1: the block routine, read all the way through

The archive block cache is the first change in this work that has to be
*right* rather than merely cheap.  Every other patch either measures something
or refuses a resource; this one answers a request for data with data of its
own, and if it ever answers with the wrong block the result is a corrupt
texture or a corrupt level, silently, on a path that runs thousands of times a
zone transition.  So `FUN_1011d0e0` was re-read end to end against the pinned
`Engine.dll` before a line of it was written, and none of what follows is
taken from §6, §7 or `arc-format.md` -- those agree with it, which is
reassuring, but they are not the source.

### The routine, and what its operands say

`FUN_1011d0e0` is `__thiscall` with three stack arguments and ends `RET 0xc`:

```
  this  = ecx                       Archive*
  [ebp+0x08] entry index    [ebp+0x0c] block index    [ebp+0x10] BlockBuffer*
```

Its first forty-seven bytes are the whole address derivation, and every offset
the cache needs is an immediate inside one of them:

```
1011d0ec  8b 41 2c        mov eax,[ecx+0x2c]      the archive's entry table
1011d0ef  c1 e2 04        shl edx,4               } entry * 0x11 ...
1011d0f2  03 55 08        add edx,[ebp+8]         }
1011d0fa  8d 34 90        lea esi,[eax+edx*4]     } ... * 4 = stride 0x44
1011d101  8b 46 20        mov eax,[esi+0x20]      this entry's descriptors
1011d104  8d 0c 5b        lea ecx,[ebx+ebx*2]     } block * 3 ...
1011d108  8d 3c 88        lea edi,[eax+ecx*4]     } ... * 4 = stride 0xc
```

Note that `entry+0x20` is a **pointer** at runtime where the on-disk file
record holds `firstBlockIndex`; the engine rewrites it when it opens the
archive.  Reading the on-disk layout as the runtime one would have produced a
key built from an integer treated as an address.

The remaining fields come out of the operands of the three calls that consume
them, which is a stronger statement than any structure diagram:

```
1011d12c  ff 37           push [edi]              descriptor[0], the offset
1011d12e  ff 70 0c        push [eax+0xc]          the open .arc HANDLE
1011d131  ff 15 ..        call SetFilePointerEx

1011d145  ff 70 04        push [eax+4]            descriptor[1], compressed
1011d14c  ff 77 04        push [edi+4]            BlockBuffer[1], the staging
1011d14f  ff 70 0c        push [eax+0xc]          the HANDLE again
1011d152  ff 15 ..        call ReadFile

1011d1c2  ff 71 04        push [ecx+4]            compressed -> sourceLen
1011d1c5  8b 41 08        mov eax,[ecx+8]         descriptor[2] -> destLen
1011d1c8  ff 77 04        push [edi+4]            BlockBuffer[1] -> source
1011d1cb  8b 4f 08        mov ecx,[edi+8]         BlockBuffer[2] -> dest
1011d1d6  e8 85 85 f4 ff  call 0x10065760         zlib uncompress
```

So `BlockBuffer` is the twelve bytes at `File+0x1c` -- `{cachedBlockIndex,
compressed scratch, decompressed scratch}` -- and `[edi+8]` is the buffer the
inflate writes, which is exactly the buffer a cache hit has to fill.

And the epilogue is the contract a hit has to reproduce, in full:

```
1011d230  89 1f           mov [edi],ebx           cachedBlockIndex = block
1011d234  b0 01           mov al,1                return true
1011d23a  c2 0c 00        ret 0xc                 three stack arguments
```

Nothing else in the function is observable from outside it.  The archive's
critical section at `archive+0x60` is taken and released around the seek and
the read only; the two error paths run only when a length disagrees; the
`GetCurrentThreadId` at the top is discarded.  A hit therefore has to write
one dword and return 1 -- and, as a side effect worth naming, takes the
archive lock zero times.

### Three things checked because assuming them would have been wrong

- **The uncompressed branch never reaches here.**  `Archive::ReadFromFile`
  tests `TEST byte [ecx],2 / JZ` at `1011d390` and takes a different path when
  the compressed bit is clear, so the 6,880 uncompressed `.mp3` entries
  `arc-format.md` found in `Audio/Dialog*.arc` never reach the block routine
  at all.  The cache cannot see them.
- **The block size is read, not assumed.**  `MOV [ESI+0x40],0x40000` at
  `1011ea94` is the only writer in the image, and the cache re-reads
  `archive[0x40]` at runtime and refuses to key anything whose archive
  disagrees -- which doubles as a cheap assertion that the pointer it was
  handed really is an `Archive`.
- **The relocated dwords name the syscalls.**  `Engine+0x2ac190` is
  `KERNEL32!SetFilePointerEx` and `Engine+0x2ac1a4` is `KERNEL32!ReadFile`, so
  the two windows above are not merely byte sequences that happen to match;
  they are the seek and the read.

### What is now machine-checked

`tools/verify-sites.py` reads all of the above back out of
`src/engine_probe.cpp` and compares it to the installed binaries.  Six windows
-- the forty-seven byte derivation, the seek, the read, the inflate, the
epilogue and the block-size writer -- plus a section that takes each offset
`src/arc_cache.cpp` dereferences with and checks it against the *operand* that
uses it: `kArchiveEntryTableOffset` against the `0x2c` in `mov eax,[ecx+0x2c]`,
`kArchiveEntryStride` against `((1 << 4) + 1) * 4`, `kSlotBytes` against the
immediate in the block-size store, and so on.  Perturbing any one constant
fails the tool.

The four windows past the prologue are required only when the cache is on:
the instrument needs the function to *be* the block routine, and the cache
additionally needs every offset it reads to be the offset the routine reads.

### Why the design is what it is

- **Key.**  `{archive, handle, offset, compressed size, decompressed size}`.
  The first three identify a block; the two sizes are carried because they are
  free and because they turn "a wrong hit is implausible" into "a wrong hit
  needs a recycled `Archive` *and* a recycled `HANDLE` *and* the same offset
  *and* both lengths".
- **Slab.**  A fixed `archive_cache_mb` MiB of 256 KiB slots with a clock
  victim, committed once.  The default is `0`, which allocates nothing.
- **Lock.**  Held across the lookup and the copy out, and across the insert --
  a slot released before it is copied could be evicted and rewritten
  underneath the copy.  Never held across the `ReadFile` or the inflate: a
  miss holds nothing while the engine does its own work.
- **Lookup.**  A linear scan of a tag array, deliberately.  At most 1,024
  slots is a 4 KiB scan, against a routine entered 7,491 times in a
  hundred-second session and an inflate that costs 582 microseconds.  A hash
  table would buy nothing here and would need tombstones to survive the clock.
- **`8verify`.**  Commits the slab and never serves from it.  Every block is
  read and inflated by the engine exactly as it would be otherwise, and then
  compared byte for byte against whatever the slab already holds for that key
  -- so every request that *would* have been a hit is instead a proof, at the
  cost of an uncached run.  This needs no second buffer and no thread-local
  anything, which is why it is shaped as a comparison on insert rather than as
  a shadow read.  A disagreement disables the cache for the rest of the
  session and writes the key that caused it to the log.

### And what it does not do

`archive_cache_mb` defaults to `0` and installs nothing at `0`.  When it is
set it is the one thing in `src/engine_probe.cpp` that installs with the
performance probe off, because it is a fix and not an instrument -- but it
opens no other gate: the module check, the export check and the byte checks
are unchanged, and a build that is not the audited `Engine.dll` still gets
nothing.

## 19. Run 21: the cache is correct, and the 1.8x may not be what we thought

`archive_cache_mb=8verify`, the same Eternal Embers route and save as runs
10-16 and 18-20, 94.8 seconds.  Two results, and the second one is the
important one.

### The route reproduced, which is what makes the rest readable

| | run 10 | run 21 |
| --- | ---: | ---: |
| `engine_arc_read` | 4,941 | 4,932 |
| `engine_arc_kib` | 1,083,143 | 1,075,574 |
| `engine_arc_blocks` | 7,527 | 7,513 |
| `engine_arc_inflate_us` | 4,380 ms | 4,310 ms |
| `engine_level_load_main_us` | 513 ms | 515 ms |

Every archive column within 1.6%, on a route walked by hand eleven runs
apart.  The mod's share is unmoved -- 9.5% against 9.7%, 9,456 ms against
9,215 ms absolute -- so the verify boot cost nothing measurable, as designed.
p50 went 9.0 to 10.0 ms, which is not the cache: the session was five seconds
shorter and `waiting on Present` fell from 12,294 ms to 9,393 ms, i.e. it
contained proportionally less GPU-bound standing around.  The cache's own work
in verify mode is one 256 KiB `memcmp` or `memcpy` per request, about 200 ms
over the session, and it lands outside the `engine_arc_inflate_us` window.

### The correctness result, which is unambiguous

**`arc_cache_bad` = 0.  286 blocks compared byte for byte against what the
engine produced for the same key, and every one agreed.**

**`arc_cache_skip` = 0, across 7,513 requests.**  That is the stronger of the
two, and it is worth spelling out why: `describeBlock` refuses a request when
the archive's `[0x40]` is not 0x40000, when any of the four dereferences is
unreadable, or when the descriptor's decompressed size is not a sane block.
It never once refused.  So the structure offsets recovered in §18 -- `0x2c`,
the `0x44` stride, `0x20`, the 12-byte descriptor, `0xc`, `0x40` -- are right
at *runtime*, on every archive the session touched, and not merely right in
the byte tables.

### The result that changes the question

| | |
| --- | ---: |
| requests | 7,513 |
| would-be hits (`arc_cache_verify`) | **286  (3.8%)** |
| stores | 7,227  (96.2%) |
| evictions | **7,195  (99.6% of stores)** |

Two things to take from that, and they pull in opposite directions.

**The 3.8% is not a measurement of reuse.**  99.6% of stores evicted a live
block.  Thirty-two slots against a working set of several thousand is a
revolving door: the hand made about 225 full sweeps, so a block whose revisit
is more than 32 block-reads away is gone before the revisit arrives.  What
3.8% measures is how much reuse survives 32 slots, which is a much less
interesting number than how much reuse exists.

**But there is a second reading of the 1.8x amplification that this repo has
never separated out, and it is not the flattering one.**  §8 recorded

> 1.88 GiB inflated to serve 1.03 GiB requested: a **1.8x amplification**,
> which is the single-slot block cache of R1 re-inflating what it has already
> inflated.

The number is right; the clause after the colon is an assumption.
Inflated-over-served is two columns divided, and it comes out the same under
either of two mechanisms:

- **(a) Re-inflation.** A block is inflated, lost by the engine's one-slot
  cache, and inflated again later.  A block cache removes this.
- **(b) Partial consumption.** The average read this session was 218 KiB
  (1,075,574 KiB over 4,932 `ReadFromFile` calls) beginning at an arbitrary
  offset, so it straddles 1.52 blocks (7,513 over 4,932) and leaves the head
  of the first and the tail of the last unused.  Those bytes are inflated and
  discarded, and a cache recovers them **only if some later read comes back
  for them**.

Mechanism (b) alone reproduces the whole 1.79x, because it is the same two
columns.  Note also that (a) is already partly handled by the engine: a
sequential walk through one `File` hits `cachedBlockIndex` on the block it
straddled into, so the one-slot cache is not as useless as R1's framing
suggests.  What is left for a mod-side cache is reuse *across* `File` objects
and across re-opens of the same entry -- which for `Levels.arc`, opened
repeatedly for one 2 GB entry, is exactly where it ought to be.  Whether it
is actually there is the open question.

`arc_cache_verify` is the only instrument that can answer it, and at 32 slots
its answer is confounded.

### Why the next run is another verify run, at the maximum size

Run 22 is `256verify` -- 1,024 slots -- and still does not serve a block.

Verify mode turns out to be a **faithful simulator of a serving cache**, and
not by accident: it inserts on a miss and refreshes the reference bit on a
hit, exactly as serving does, and the stream of requests reaching it is
identical either way -- because a hit writes the engine's own one-slot cache
before returning, so `FUN_1011d240` decides what to ask for the same way in
both cases.  **`arc_cache_verify` at size N is precisely the hit count a
serving cache of size N would achieve.**  So the ceiling can be measured at
zero risk and with no behaviour change at all, which is a better first
experiment than switching serving on and hoping.

Going straight to the clamp rather than stepping is deliberate: 1,024 slots is
the most this build will ever hold, so if the ceiling there is still 3-6%, no
smaller size can beat it and **Stage 4.1 is a negative result** -- the 1.8x is
mechanism (b), no block cache recovers it, and the item should be switched off
and reported rather than tuned.  If instead it comes back at 20% or better,
the reuse was real and hidden by capacity, and the knee is then found
*downward* from 256.

Either answer is worth having, and the second-order value is the same in both
cases: 1,024 slots exercises thirty-two times as many distinct keys as run 21
did, so it is also a far wider correctness test than the boot that was
designed to be one.

## 20. Run 22: the ceiling, and what the 1.8x actually is

`archive_cache_mb=256verify` -- 1,024 slots, 256 MiB, still serving nothing --
on the same route.  106 seconds, 8,157 frames, 7,673 blocks.  This was meant
to separate the two readings of the amplification that §19 could not, and it
does, unambiguously.

### The ceiling is 8.2%, and 32 slots already has most of what matters

| | 8 MiB / 32 slots (run 21) | 256 MiB / 1,024 slots (run 22) |
| --- | ---: | ---: |
| would-be hits, session | 286 / 7,513 = **3.8%** | 628 / 7,673 = **8.2%** |
| would-be hits, frames > 200 ms | 258 / 1,934 = **13.3%** | 297 / 2,064 = **14.4%** |
| would-be hits, heaviest frame | 254 / 1,470 = **17.3%** | 285 / 1,468 = **19.4%** |
| evictions | 99.6% of stores | 85.4% of stores |
| `arc_cache_bad` | 0 | 0 |
| `arc_cache_skip` | 0 | 0 |

**Thirty-two times the address space buys 4.4 points of session hit rate and
2.1 points where it matters.**  That is the whole answer, and it settles §19's
question against mechanism (a):

- **The amplification is not recoverable re-inflation.**  With a quarter of a
  gigabyte of cache -- 1,024 blocks resident, against a largest-entry block
  count of 7,646 -- **91.8% of block requests are still for a block nothing
  has seen before.**  The engine inflates a 256 KiB block, consumes part of
  it, and is never asked for it again.  §8's "the single-slot block cache
  re-inflating what it has already inflated" is now measured false, and §8
  carries a notice saying so.
- **The reuse that does exist is burst-local.**  It is concentrated in exactly
  the frames that hurt -- 14.4% in frames over 200 ms and 19.4% in the single
  archive-heaviest frame, against 8.2% overall -- and it is nearly all within
  a window of a few dozen blocks, which is why 32 slots gets 17.3% of the
  worst frame and 1,024 slots gets 19.4%.  Whatever the game does inside one
  big level load, it comes back to blocks it read moments ago and to nothing
  older.

The slab is still saturated at 1,024 slots -- 85% of stores evict -- so the
working set genuinely exceeds 256 MiB.  There is simply nothing worth having
out in that tail.  Extrapolating the two points, twenty percent session hit
rate would want tens of thousands of slots, i.e. several gigabytes, in a
32-bit process.

### What Stage 4.1 is therefore worth

Arithmetic, not measurement, and the distinction matters because **no block
has ever actually been served**: both runs were `verify`.  At 562 microseconds
a block (4,309 ms over 7,673) and a 256 KiB `memcpy` costing on the order of
25, a hit saves roughly 535 microseconds.

| | session | heaviest frame |
| --- | ---: | ---: |
| 8 MiB | ~155 ms of 106 s (**0.15%**) | ~136 ms off 1,488 ms (**9%**) |
| 256 MiB | ~336 ms of 106 s (**0.32%**) | ~152 ms off 1,392 ms (**11%**) |

So: about a ninth of the worst frame of a session, for 8 MiB, and almost
nothing extra for thirty-two times that.  The worst frame stays over 1.3
seconds either way, because what dominates it is `Region::LoadLevel` (505 ms
in run 10) and the ~950 ms no column has ever named.

`proc_avail_va_mib` never fell below **3,458 MiB** with 256 MiB committed, so
address space was never the constraint -- worth recording, because it means
the size clamp was never what limited this.  What limited it is that the
locality is not there.

### Correctness, twice over

Across runs 21 and 22: **914 blocks compared byte for byte, 0 mismatches, 0
skips over 15,186 requests.**  Run 22 exercised thirty-two times as many
distinct keys as run 21.  The key and the structure offsets of §18 are right,
and `describeBlock` never once refused a request -- so `[0x40]` was `0x40000`
and all four dereferences were readable on every archive the session touched.

Whatever is decided about shipping it, that part is not in doubt, and the
`verify` mode is the reason it can be said at all rather than argued.

### Where this leaves the archive path

`engine_arc_inflate_us` brackets the **whole** block routine -- the
`SetFilePointerEx`, the `ReadFile` and the `uncompress` -- so its 4,309 ms is
read *and* inflate, and nothing here says which. That is now the only open
question on this path, and it decides between the two remaining archive items:

- if the 562 microseconds a block is mostly **zlib**, the lever is 4.3
  (libdeflate) and 4.2 buys little;
- if it is mostly the **two syscalls**, the lever is 4.2 (one `ReadFile` over
  a run of contiguous blocks) and 4.3 buys little.

The second would be entirely in character for this install: §14-§17 spent four
runs establishing that a single `PeekMessageA` round trip to the host costs
126-212 ms, and a seek/read pair per 256 KiB is 7,673 pairs a session of
exactly the kind of call CrossOver makes expensive.

**And the instrument for it already exists as a verified site.**  P8 in §7 --
the `E8` at `0x1011d1d6` to `FUN_10065760` -- is the call to zlib, it sits at
offset 20 of `kArchiveInflateWindowBytes`, and it is already byte-checked by
`verify-sites.py`.  A `detour::patchCall` on it gives `engine_arc_zlib_us`,
and read+seek is then the subtraction.  One CallPatch, one column pair, no new
reverse engineering, and it is the cheapest remaining question in the plan.

## 21. Run 23: the cache serves, and the freeze frame is anatomised

`archive_cache_mb=8`, serving for the first time.  Same route.

### What serving actually bought, measured rather than computed

| | run 21 `8verify` | run 23 `8` serving |
| --- | ---: | ---: |
| `engine_arc_blocks` | 7,513 | 7,498 |
| `engine_arc_inflate_us` | 4,310 ms | **4,180 ms** |
| hits | 286 would-be | **291 served** |
| `arc_cache_hit_us` | -- | **420 us total** |
| `arc_cache_bad` / `arc_cache_skip` | 0 / 0 | **0 / 0** |

130 ms off the session, against the 155 ms §20 computed -- the difference is
route variance (15 fewer blocks).  §20's arithmetic was sound.

**One number was badly wrong, in the cache's favour: a hit costs 1.4
microseconds, not the 25 assumed.**  420 us over 291 hits.  A miss costs 580.
So a hit avoids essentially the whole cost of a block rather than 95% of it,
and the `memcpy`-bound estimate was pessimistic by a factor of fifteen.  (The
true figure is between 1.4 and about 2.4 us: `microsecondsSince` truncates, so
sub-microsecond hits record zero.)

That does not change the verdict -- the ceiling is the hit *rate*, and §20
measured that at 8.2% with a quarter-gigabyte slab -- but it does mean the
cache is as close to free as an intervention on this path can be.  Three runs,
**1,205 blocks compared byte for byte, 0 mismatches, 0 skips over 22,684
requests.**

### The freeze frame, fully broken down for the first time

Frame 4311, **1,310.2 ms**, the zone transition.  Every column that reads
above noise:

```
engine_render               1,303.9 ms    99.5% of the frame
  |- Region::LoadLevel        508.8 ms    38.8%   (100% main thread, 5 calls)
  |    |- LoadResource        366.1 ms            (764 calls, all main thread)
  |         |- read+inflate   259.8 ms            (1,468 blocks, 254 cache hits)
  |- texture_create            25.2 ms            (715 textures)
  |- buffer_create              2.8 ms
  |- shader_create              1.6 ms
  +- UNNAMED                  795.1 ms    60.7% of the frame
```

Counts: `arc_open` 1,299, `engine_arc_read` 1,282, `engine_arc_kib` 82,342,
`engine_res_enqueued` 1,012, `proc_avail_va_mib` 3,799.

Three things follow, and the third is the important one.

**The whole archive path is 260 ms of a 1,310 ms frame.**  Every remaining
archive item in the plan -- 4.1 which is done, 4.2's prefetch, 4.3's
libdeflate -- is competing for a slice of 20% of this frame.  A perfect
archive layer that returned every block instantaneously would leave 1,050 ms.

**The level load is 509 ms and it is entirely on the main thread**, which is
Stage 5's target and is worth more than the whole archive path.  Note it is
only 143 ms wider than the resource loading nested inside it.

**And 795 ms -- 61% of the frame -- is inside `Engine::Render` and is named by
nothing.**  It is not the level load, not resource loading, not the archive,
not texture or buffer or shader creation, not the mod (every mod phase is
under 2 ms), not the GPU (`gpu_frame` spans the stall but its child passes are
idle), and not the message pump (0.67 ms).  It has been the largest single
unexplained cost in this project since run 8 and it has never had a candidate.

### It now has a candidate, and it is verified in the disassembly

`FUN_1014d020`, the archive `File` constructor called by
`FileSourceArchive::OpenFile`:

```
1014d05e  c7039c712f10   MOV [EBX],0x102f719c        the archive File vtable
1014d094  f6450002       TEST byte [EBP],0x2         compressed?
1014d098  7441           JZ  0x1014d0db              if not, no buffers
1014d09a  8b790c         MOV EDI,[ECX+0xc]           -> Archive*
1014d0a1  8b7f40         MOV EDI,[EDI+0x40]          -> blockSize, 256 KiB
1014d0a4  397d08         CMP [EBP+8],EDI             min(compressedSize, 256K)
1014d0ae  0f42c8         CMOVC ECX,EAX
1014d0b5  ff31           PUSH [ECX]
1014d0b7  ff1518c32a10   CALL [0x102ac318]           operator new[]
1014d0bd  894320         MOV [EBX+0x20],EAX
1014d0c0  397d0c         CMP [EBP+0xc],EDI           ... and again for
                                                     min(decompressedSize, 256K)
```

`0x102ac318` is `MSVCR110!??_U@YAPAXI@Z` -- `operator new[]` -- and
`0x102ac304` is `??_V@YAXPAX@Z`, the matching `delete[]` that
`Archive::FreeFileBuffer` and the File destructor call.  Both were read out of
Engine.dll's import table this session.

So **every compressed archive file the game opens allocates two buffers of up
to 256 KiB each, and frees them when it closes**.  Frame 4311 opened 1,299 of
them.  That is **up to 649 MiB of `operator new[]` / `delete[]` traffic in a
single frame**, roughly 2,600 allocation/free pairs, in a 32-bit MSVC heap --
under Wine, which is not the Windows heap.  `Lock` additionally grows the
`+0x18` scratch to the requested size, adding the 80 MiB the frame actually
read.

The plan already names this and declines it:

> Pooling the two 256 KiB `operator new[]` scratch buffers the archive `File`
> constructor allocates per compressed entry -- 512 KiB of heap churn per
> opened file in a fragmenting 32-bit MSVC heap -- is cheap and tempting but
> needs its own evidence that fragmentation is biting.

**Frame 4311 is that evidence's shape, and the instrument for it is four
bytes.**  `detour::patchImport` on Engine.dll's two IAT slots gives
`engine_heap_alloc` / `engine_heap_alloc_us` / `engine_heap_free` /
`engine_heap_free_us`, scoped to Engine.dll, patching no code, exactly as the
twelve import instruments added after `b161e08` are.  If those columns read
hundreds of milliseconds on frame 4311, the 795 ms has a name and a fix
(pooling the buffers), and it is larger than everything else left in the plan
combined.  If they read single-digit milliseconds, the hypothesis dies for the
price of one boot and the search continues.

That is the next thing to build, ahead of 4.2 and 4.3 -- which are arguing
over 260 ms while this is 795.

### And a second, cheaper instrument to build alongside it

`engine_arc_inflate_us` brackets the whole block routine, so its 580 us a
block is `SetFilePointerEx` + `ReadFile` + `uncompress` together and nothing
says which.  **P8** -- the `E8` at `0x1011d1d6`, already at offset 20 of
`kArchiveInflateWindowBytes` and already byte-checked by `verify-sites.py` --
splits it with one `detour::patchCall`.  Mostly zlib points at 4.3; mostly
syscall points at 4.2, and given §14-§17 the second would be in character.

Both instruments fit in one boot.

## 22. Why 16 MiB is not worth a boot, and why 4.2 and 4.3 are not worth building yet

Two questions asked directly, answered from the data already on disk rather
than by running more.

### The cache size is bracketed, and 16 MiB sits inside the bracket

Runs 21 and 22 measured two points, and the frame that matters is the third
column:

| | session | frames > 200 ms | heaviest frame |
| --- | ---: | ---: | ---: |
| 8 MiB, 32 slots | 3.8% | 13.3% | **17.3%** |
| 256 MiB, 1,024 slots | 8.2% | 14.4% | **19.4%** |

**19.4% is the ceiling** -- 1,024 resident blocks against a working set that
still evicts 85% of the time, so more slots is not the constraint.  32 slots
already captures **89% of the achievable** on the freeze frame.  The whole
gap between 8 MiB and 256 MiB is 31 hits on that frame, which at 580
microseconds a block is **18 milliseconds**.

16 MiB is 64 slots.  It sits between two measured points 18 ms apart, so it
can buy at most single-digit milliseconds over 8 MiB, on one frame, once a
session.  That is not worth a boot, and it is not worth changing the
documented value either: 8 MiB is the number that was measured and the reason
larger values are pointless is now recorded.

The general finding is the useful part: **the reuse is burst-local.**  Inside
one big level load the game returns to blocks it read moments ago and to
nothing older, which is why the curve is flat from 32 slots onward.  Any
future design that hopes to exploit archive locality should be sized against
tens of blocks, not thousands.

### 4.2 and 4.3 should not be built yet, and one of them may never be

Run 23's frame anatomy (§21) reprices both of them:

```
frame 4311, 1,310.2 ms
  the entire archive path      260 ms      20% of the frame
  Region::LoadLevel            509 ms      39%   (Stage 5)
  UNNAMED                      795 ms      61%   (nothing in the plan)
```

**4.2 and 4.3 are competing for slices of 260 ms, and they overlap.** Both
attack the same 580 microseconds a block: 4.2 removes syscalls, 4.3 removes
zlib time.  Whichever half of that 580 is small, the corresponding item is
worth nothing -- and **nobody knows which half is which**, because
`engine_arc_inflate_us` brackets the seek, the read *and* the `uncompress`
together.  Building either one now is a coin flip on a 260 ms prize.

4.3 is additionally the riskiest item in the whole plan.  It needs a
hand-emitted caller-pop thunk on `FUN_10065760`'s non-standard convention
(`ECX`/`EDX` plus two caller-popped stack arguments) on a path entered
thousands of times a second, plus a vendored decompressor that has to agree
with zlib bit for bit on 122,302 blocks.  That is a great deal of exposure for
at most a fraction of 20% of one frame.

So the order is:

1. **Measure, do not build** -- run 24, below.  Two import redirects.
2. **Whatever the heap columns say**, because 795 ms beats 260 ms and 509 ms
   both.
3. **Stage 5.1** -- retarget P1/P2 to `Region::BackgroundLoadLevel` -- and
   *then* widen the game's preload distance.  509 ms, and see the note below
   on why the ordering is the whole trick.
4. **4.2 only if the split says the syscalls dominate.**
5. **4.3 last, or never.**

### Run 24: the two instruments, and why they are cheap

Both are `detour::patchImport` -- four bytes of a data table, no code patched,
scoped to `Engine.dll` -- which makes them the same class of instrument as the
twelve added in runs 13-20 rather than anything new.

**`engine_trace` bit 2048, the heap.**  `??_U@YAPAXI@Z` and `??_V@YAXPAX@Z`
at Engine's IAT slots `0x2ac318` and `0x2ac304`, both verified by
`verify-sites.py` to be MSVCR110's `operator new[]` and `operator delete[]`.
Columns `engine_heap_alloc` / `_us` / `_kib`, `engine_heap_big` / `_big_us`
(the subset at or above 64 KiB, which is what the block scratch buffers are),
`engine_heap_free` / `_us`.  Hundreds of milliseconds on the freeze frame
names the 795 ms; single-digit milliseconds kills the hypothesis.

**`engine_trace` bit 4096, the read/inflate split.**  `SetFilePointerEx`
(`0x2ac190`) and `ReadFile` (`0x2ac1a4`), also verified.  Columns
`engine_io_seek` / `_us` and `engine_io_read` / `_us` / `_kib`; the inflate is
`engine_arc_inflate_us` minus the two.  Engine.dll's other callers of both are
all inside the same archive module -- `FUN_1011bfd0` through
`Archive::AddFileFromMemory` -- so comparing `engine_io_read`'s count against
`engine_arc_blocks` is what says the attribution holds.

**The one thing to watch.**  This is the first instrument on a function the
engine may call at a far higher rate than anything hooked so far, and two
`QueryPerformanceCounter` calls per allocation could cost real time.  The
precedent is reassuring -- a detour on `ResourceLoader::EnqueueResource` at 12
million calls a session moved neither p50 nor the mod's share -- but
`operator new[]` could be busier still.  p50 and the mod's share against run
23 (9.0 ms, 9.5%) are the check, and the mask bisects it: `engine_trace=4096`
installs the split alone, `2048` the heap alone.

### Turning the cache on outside a measurement run

Asked directly: the cache is validated, so should it come back on?  For normal
play, yes -- but not for run 24, and the reasons are worth separating.

**Not for run 24.**  Its numbers are the stock baseline any later fix is
judged against, and one variable per run is how every run in this project has
been designed.  There is a second, smaller reason that is specific to what run
24 measures: a cache hit is by definition a block that was read recently, so
its file pages are the ones most likely to still be in the host's page cache.
Serving them removes the *cheapest* reads from the sample and biases the
read-versus-inflate ratio upward on the read side.  It is only ~4% of the
sample, but that ratio is the entire purpose of the boot.

**For play, yes, and `verify` is the way in.**  `archive_cache_mb=8verify`
with `[debug] trace=1` and `performance_trace` left at 0 installs the cache
and nothing else -- which is what its third install gate exists for -- never
serves a block, and compares every one byte for byte while the game is played
normally.  All three measurement runs were ~100-second sessions on one Eternal
Embers route; a few hours of ordinary play exercises far more archives, some
reopened, and many more distinct keys, and this extends the proof to that for
the price of one `memcmp` per block.

**One thing had to be fixed to make that viable.**  `tq::hdr::log` appends
into a fixed 64 KiB buffer that never resets, and run 23's log was 15 KB for
a hundred seconds.  A fixed report cadence of 1,024 requests would have filled
the buffer inside an hour of play -- and once it is full, **a mismatch late in
a long session would be silently dropped**, which is the one line that must
never be lost.  The cadence now backs off after the eighth report to every
8,192 requests, so a measurement run gets the same eight it always did and a
multi-hour session writes about a kilobyte.

`cache/runs/play-with-cache-verify.ini` and `play-with-cache.ini` are
those two configurations, each two lines from the reporter's own.

### A note on preloading, since it was asked

The proposal was to preload adjacent zones on a worker, possibly in a separate
64-bit process serving decompressed blocks over IPC, having found that the
game's own preload-distance setting made hitches *earlier* rather than
smaller.

**That last observation is exactly right and now has a mechanism.**  Both
`AddElementsInBox` overloads call `Region::LoadLevel(region, false)` --
synchronous, and §8 measured it at 100% main thread across 203,419 calls.
Widening the radius widens the set of regions force-loaded *on the render
thread*, so the setting can only move the hitch.  The engine already has the
async path (`Region::BackgroundLoadLevel`, `0x1020be60`) and both call sites
are already shaped for it: the two instructions after the call are
`CMP byte [EDI+0x74],0` / `JNZ epilogue`, i.e. skip this region while a load
is in flight.  So async alone gives pop-in, widening alone is worse, and
**async plus widening is the preload idea with no pop-in.**  The ordering is
the trick, and it is Stage 5.1 followed by a setting change.

**The separate process is the part to decline**, on this project's own
evidence.  `Levels.arc` is one entry -- 1.87 GiB decompressed, 626 MiB
compressed -- so it fits a 4 GB helper.  But the transfer would have to be
shared memory: anything with a host round trip per block is disqualified by
§14-§17, where a *single* `PeekMessageA` round trip measured 126-212 ms, and
frame 4311 wanted 1,468 blocks.  With shared memory the design collapses back
into mapping a window into the 32-bit process and copying out of it, which is
what §18's cache already does at 1.4 microseconds a block.  The helper's only
real advantage is address space, and address space was never the constraint:
`proc_avail_va_mib` never fell below 3,458 MiB with 256 MiB committed, and
read **3,799 MiB during frame 4311 itself**.

Also worth stating, because it is a point in the proposal's favour: **run 22's
8.2% ceiling does not bound a preloading design.**  It bounds a *demand*
cache, which needs reuse.  A preload cache needs prediction, and would serve
first-touch blocks -- 91.8% of requests.  The reason not to build it is not
that it cannot hit; it is that the entire archive path is 260 ms of a 1,310 ms
frame, so even a perfect one leaves 1,050 ms of freeze.  If the read half
turns out to dominate, the cheap version of the same idea is to map
`Levels.arc` and let block reads come out of memory -- no helper, no IPC, no
prediction.

## 23. Run 24: the heap is innocent, 4.2 is dead, and I had the split backwards

Two import instruments, four slots, 17 hooks installed, `engine_trace=1`.  Both
answered.  One killed a hypothesis, the other reversed a prediction.

### The heap hypothesis is dead, and cleanly

| | session (99.1 s) | freeze frame (1,534.8 ms) |
| --- | ---: | ---: |
| `engine_heap_alloc` | 77,261 calls, **87 ms** | 7,324 calls, **1.6 ms** |
| `engine_heap_free` | 73,183 calls, **86 ms** | 6,471 calls, **2.0 ms** |
| `engine_heap_big` (>= 64 KiB) | 5,842 calls, 62 ms | 330 calls, 1.2 ms |
| `engine_heap_alloc_kib` | 2,188,634 | 153,162 |

**173 ms over a whole session, and 3.6 ms on the frame it was supposed to
explain.**  The archive `File` constructor really does allocate two buffers per
compressed entry and the freeze frame really does open 1,299 files and churn
150 MiB -- and it costs three and a half milliseconds.  Wine's heap is simply
not slow at this.

So §21's candidate for the 795 ms is gone.  That is the instrument doing its
job: the hypothesis was verified in the disassembly, plausible in shape, and
wrong in magnitude, and one boot settled it.  **Pooling the two scratch
buffers is now struck from the plan** -- it would recover single-digit
milliseconds a session.

(Note in passing: only 5,842 of a possible ~10,000 allocations were >= 64 KiB,
because the constructor asks for `min(size, blockSize)` and most archive
entries are smaller than 64 KiB.  The 256 KiB pair is the exception, not the
rule.)

### The read/inflate split, and it is the opposite of what I predicted

I expected the syscalls to dominate, on the strength of §14-§17 -- where a
single `PeekMessageA` round trip to the host measured 126-212 ms.  That
reasoning does not transfer, and the numbers say so plainly:

| | session | per block | share |
| --- | ---: | ---: | ---: |
| the whole block routine | 4,543 ms | 594 us | 100% |
| **zlib `uncompress`** | **3,494 ms** | **457 us** | **76.9%** |
| `ReadFile` | 1,043 ms | 136 us | 23.0% |
| `SetFilePointerEx` | 5 ms | 1 us | 0.1% |

`engine_io_read` counted 7,761 calls against `engine_arc_blocks`' 7,646, which
is the attribution check: Engine.dll's other callers of `ReadFile` are all in
the same archive module, and the ~115 extra calls are theirs.

**File I/O under CrossOver is not the pathology the message pump is.**  The
seek is genuinely free at a microsecond.  `ReadFile` moved 626 MiB in 1,043 ms
-- about 600 MB/s -- so it is paying for *bytes*, not for round trips.

Two consequences, and the first is a deletion.

**4.2, the bounded compressed prefetch, is dead.**  Its whole pitch was "86
syscall pairs and 86 acquisitions of `archive+0x60` for one texture".  The
syscall half of that is worth 5 ms a session.  Collapsing many reads into
fewer, larger ones transfers the same bytes at the same throughput -- and 4.2
deliberately *over-reads* past the requested block, so it would move strictly
more.  There is nothing for it to win.  Struck.

**4.3, libdeflate, is now the only archive item left with a number.**  3,494 ms
of zlib a session; 189 ms of it inside the freeze frame.  libdeflate is
typically two to three times faster at decompression, so 1,700-2,300 ms a
session and perhaps 110 ms of that frame.  It remains the riskiest item in the
plan -- a hand-emitted caller-pop thunk on `FUN_10065760`'s non-standard
convention, plus a vendored decompressor that must agree with zlib on 122,302
blocks -- but it is no longer a coin flip: the number it attacks is now
measured and it is the larger half.

### And the frame is *more* unexplained than before, not less

```
frame 1906, 1,534.8 ms
  engine_render                1,529.0 ms   99.6% of the frame
    |- Region::LoadLevel         505.7 ms   (100% main thread)
    |    |- LoadResource         404.7 ms
    |         |- read + inflate  311.2 ms   (122 read, 189 zlib)
    |- texture_create             23.4 ms   (715 textures)
    |- heap alloc + free           3.6 ms
    +- STILL UNNAMED             996.4 ms   64.9% of the frame
```

A second frame from the same run, 908.0 ms, is a useful contrast: its
`Region::LoadLevel` is **0.01 ms** -- no level load at all -- and yet it has
388 ms of main-thread resource loading, 64 ms of `Engine::Update`, 75 ms of
texture creation, and **758 ms unnamed**.  So the missing time is not a
side-effect of level loading; it appears with and without one.

### What has never been measured, and it is the obvious thing

Every instrument in this project times the main thread *doing* something.
**Nothing has ever timed it waiting.**

- The region lock was measured at **zero** contention -- but only at three
  render-path call sites (§8).
- The loader fence was measured at 1.6 ms -- but only at one call site.
- **Engine.dll's archive lock, `archive+0x60`, is taken and released around
  every one of the 7,646 block reads a session, and has never been
  instrumented at all.**

`FUN_1011d0e0` enters that section, seeks, reads, and leaves it -- so the
loader thread holds it for the 136 microseconds of every `ReadFile`.  If the
render thread force-loads a level while the loader thread is inside that
window, the render thread blocks, and no column in any run to date would show
it.  That is the right shape and roughly the right size for 996 ms.

So run 25 adds `engine_trace` bit **8192**, four more import redirects on
`Engine.dll`:

| import | columns | note |
| --- | --- | --- |
| `EnterCriticalSection` (`0x2ac17c`) | `engine_cs_wait` / `_us` | **contended acquisitions only** -- `TryEnterCriticalSection` first, timestamp only on failure, so an uncontended lock records nothing. Covers every critical section in the module, the archive's included. |
| `WaitForSingleObject` (`0x2ac188`) | `engine_obj_wait` / `_us` | every wait outside the one fence call site |
| `WaitForMultipleObjects` (`0x2ac154`) | `engine_obj_wait` / `_us` | |
| `Sleep` (`0x2ac108`) | `engine_sleep` / `_us` | |

These are **disjoint from `engine_region_lock_*` and `engine_fence_wait_*` by
construction**: `patchCall` repointed those four call sites at a mod-owned
cell, so they no longer read the import slot.  Nothing is double counted.

The group installs **last**, and that ordering is required rather than tidy:
the region-lock and fence groups verify these same slots still hold kernel32's
exports before they patch their call sites, and would refuse if this group had
already redirected them.

Either answer is worth the boot.  Hundreds of milliseconds of `engine_cs_wait`
on the freeze frame names the 996 ms and points at a real fix -- the loader
thread should not hold the archive lock across an inflate, and Stage 5.1 would
stop the render thread being there to block on it.  Near zero says the main
thread is *working*, not waiting, and that the remaining time is CPU inside
`Engine::Render` that no import and no exported function reaches -- which
would mean instrumenting that function's internals, a bigger job than anything
attempted so far.

## 24. Run 25: the archive lock is innocent, and a poll loop shows up instead

`engine_trace=1`, 18 hooks, 4/4 blocking imports redirected, 96.3 seconds on
the same route.  One hypothesis died, one instrument was built wrong, and the
wrong instrument found something anyway.

### Critical-section contention is not the missing time

| | session | freeze frame (1,505.3 ms) |
| --- | ---: | ---: |
| `engine_cs_wait` | 3,159 contended | **0** |
| `engine_cs_wait_us` | **222 ms** | **0.00 ms** |
| `engine_region_lock_hits` (3 sites) | 0 | 0 |
| `engine_fence_wait_us` (1 site) | 1 ms | -- |

**Zero contended acquisitions on the freeze frame**, and 222 ms across a whole
session across every critical section in `Engine.dll` -- the archive's own
`archive+0x60` included, which is held across every one of 7,646 block reads
and 136 microseconds of `ReadFile` each.  §23's hypothesis was that the render
thread blocks there while the loader thread reads.  It does not.  Dead.

That also generalises §8's region-lock result properly: it was measured at
three call sites and now the whole module agrees with it.  **Lock contention is
not a mechanism in this game on this machine.**

### The instrument was wrong for the question, and that is my error

`engine_obj_wait_us` read **178.7 seconds** and `engine_sleep_us` **165.5
seconds** over a 96.3-second session.  Both larger than wall clock, because
both sum across every thread in the process and most of it is background
threads sitting idle.  One frame shows 31,288 ms of object wait.

Unattributed, those two columns cannot answer anything.  The level and
resource load columns have had a `_main` split since run 10 -- compared against
the engine's own recorded thread id at `Engine+0x41a5dc` -- for precisely this
reason, and these were built without one.

### But the wrong instrument found a poll loop

| | frames | `Sleep` calls per frame |
| --- | ---: | ---: |
| under 20 ms | 5,591 | **5.8** |
| over 200 ms | 9 | **250.2** |

A **43x** rate difference, and the four worst frames of the run carry 406,
653, 436 and 319 `Sleep` calls each.  Background idle does not do that.
Something polls with `Sleep` during the hitch.

Set that against the freeze frame with everything runs 24 and 25 named
subtracted out:

```
frame 1863, 1,505.3 ms
  engine_render                1,498.9 ms
    |- Region::LoadLevel         513.8 ms   (100% main thread)
    |- texture_create             23.2 ms
    |- heap alloc + free           3.6 ms
    |- critical sections           0.0 ms
    +- UNNAMED                   958.3 ms
```

**406 `Sleep` calls account for all 958 ms at 2.4 milliseconds apiece.**  That
is the arithmetic of a `Sleep(1)` poll loop on a host whose sleep granularity
is not one millisecond, and it is the first candidate for this time that is
both the right size and the right shape.

### Run 26 adds the split, and the requested-versus-actual pair decides it

Seven columns: `engine_cs_wait_main` / `_us`, `engine_obj_wait_main` / `_us`,
and `engine_sleep_main` / `_us` / **`_req_us`**.  The last is the same shape as
the `loop_sleep_req_us` pair added in run 13 -- what the main thread *asked*
for beside what it *got* -- and it splits three ways:

- **actual large, requested small** (say 400 ms requested, 950 ms returned):
  the game polls with `Sleep(1)` and the host hands back two to fifteen
  milliseconds each time.  That is a granularity problem, it is why this
  reproduces under CrossOver and would not on Windows, and **it is reachable
  from here**: `timeBeginPeriod` is a `winmm` export, and `winmm.dll` is the
  library this mod *is*.  It would be the first real lever found since the
  loose-texture cap.
- **both large**: the game really does mean to sleep for most of a second
  while loading.  Its own poll loop, and the fix is Stage 5.1 -- make the
  level load asynchronous so the main thread is not there to poll at all.
- **near zero**: the sleeping is all worker threads, and the 958 ms is CPU
  inside `Engine::Render` that no import and no exported function reaches.
  That is the expensive answer, and it means instrumenting that function's
  internals.

All three are decisive, which is what makes the boot worth taking.  Nothing
about the game changes: the same four import redirects as run 25, attributed
by thread.

### Standing correction to §8 and to my own reasoning in §23

§8 closed the region lock on three call sites and I generalised that in §23
into an argument that the *archive* lock might be the exception.  Run 25 says
no exception exists.  Two conclusions worth carrying forward:

1. **Every lock hypothesis in this project is now closed by measurement**, at
   whole-module scope rather than per-site.
2. **The pattern in my own errors is worth naming**: §23 predicted syscalls
   over zlib and was wrong; §21 predicted heap churn and was wrong; §23
   predicted lock contention and was wrong.  All three were mechanisms
   verified in the disassembly and plausible in shape, and all three were
   wrong about *magnitude*.  The instrument-first discipline is what has
   caught each one for the price of a single boot, and it is the reason to
   keep resisting the urge to build the fix before the measurement.

## 25. Run 26: the main thread does poll, and the freeze is not one mechanism

The `_main` split §24 should have had from the start.  97.5 seconds, 7,046
frames, same route.  Three results, and the third is the one that reorganises
the remaining work.

### The main thread polls with `Sleep`, and it is the game's own loop

| | all threads | **main thread** |
| --- | ---: | ---: |
| `engine_cs_wait` | 2,968 / 209 ms | 1,532 / **88 ms** |
| `engine_obj_wait` | 12,229 / 183,036 ms | 8,388 / **158 ms** |
| `engine_sleep` | 63,027 / 164,060 ms | 418 / **518 ms** |

The unattributed columns really were background idle: 183 seconds of object
wait over a 97-second session collapses to **158 milliseconds** once the main
thread is separated out.  Same for `Sleep`: 164 seconds becomes 518
milliseconds.

And on the freeze frame the poll loop is unmistakable:

```
frame 1911, 1,376.9 ms
  engine_render                1,371.1 ms
    |- Region::LoadLevel         512.6 ms   (100% main thread)
    |- main-thread Sleep         434.9 ms   350 calls, 350 ms requested
    |- texture_create             23.5 ms
    |- heap alloc + free           3.6 ms
    |- critical sections           0.0 ms
    |- object waits                0.0 ms
```

**350 `Sleep(1)` calls on the main thread in one frame.**  Against 418 for the
whole session -- so 84% of the session's main-thread sleeping happens in that
single frame.  This is the game waiting for its loader thread, one millisecond
at a time.

### The granularity lever is small, and that is worth knowing before building it

| | |
| --- | ---: |
| main-thread `Sleep` calls | 418 |
| requested | 418 ms |
| actual | 518 ms |
| **ratio** | **1.24x**, 1.24 ms per `Sleep(1)` |

§24 hoped for two to fifteen milliseconds a call, which would have made
`timeBeginPeriod` -- a `winmm` export, in the library this mod *is* -- a real
lever.  It is 1.24.  Perfect granularity would recover **100 ms a session and
about 85 ms of the freeze frame**.  Worth a switch eventually because it is
nearly free, but it is a rounding error against the 435 ms the loop costs, and
it is not the fix.

**The requested 350 ms is the point.**  The game asks to sleep for a third of
a second in that frame.  This is its own poll loop, not the host's
granularity, and the answer is to stop the main thread being in the loop at
all -- which is Stage 5.1.

### The freeze frames are three different mechanisms, and I have been treating them as one

This is the finding that reorganises the plan.  The three worst frames of run
26 have almost nothing in common:

| | frame 1911 | frame 3168 | frame 6914 |
| --- | ---: | ---: | ---: |
| frame | 1,376.9 ms | 1,113.3 ms | 437.2 ms |
| `engine_render` | 1,371.1 ms | 1,040.3 ms | **52.5 ms** |
| `Region::LoadLevel` main | 512.6 ms | **0.02 ms** | 0.00 ms |
| main-thread `Sleep` | **434.9 ms** | 0.00 ms | 0.00 ms |
| main-thread `EnterCriticalSection` | 0.00 ms | **50.2 ms** | 0.00 ms |
| main-thread object wait | 0.00 ms | 0.05 ms | **56.2 ms** |
| `texture_create` | 23.5 ms | **193.3 ms** | 1.9 ms |
| archive inflate | 262.8 ms | 275.8 ms | 4.4 ms |
| `arc_open` | 1,299 | 199 | -- |

- **Frame 1911 is the zone transition**: a forced synchronous level load that
  is 85% a `Sleep(1)` poll loop.  Stage 5.1 addresses this one and only this
  one.
- **Frame 3168 has no level load at all** -- 0.02 ms -- and still spends 1,040
  ms in `Engine::Render`, with 193 ms of texture creation, 276 ms of archive
  inflate on some thread, 50 ms of main-thread lock contention, and roughly
  790 ms nothing accounts for.  A different mechanism entirely.
- **Frame 6914 is not in `Engine::Render` at all** -- 52.5 ms of render inside
  a 437 ms frame.  That is the §13-§17 class, the window message pump, closed
  as a host question.

So "the freeze frame" has been three things wearing the same number.  Every
attribution in §21 through §24 was computed against whichever frame happened
to be slowest in that run, and those were not the same frame.  **Any future
claim about "the worst frame" has to say which class it means.**

### Where that leaves the work

**Stage 5.1 is now the best-founded item in the plan**, and better founded
than when it was written:

- its premise is measured three times over -- `Region::LoadLevel` is 100%
  main-thread, and cost 505-514 ms on the worst frame of runs 10, 23, 24, 25
  and 26;
- the engine's own asynchronous entry point exists and both call sites are
  already shaped for it;
- and run 26 adds the part that was not known: **the load is mostly waiting.**
  435 ms of the 513 ms is a `Sleep(1)` poll.  A load that is 85% idle is
  nearly finished by the time the renderer is told to skip the region, so the
  pop-in Stage 5 trades for should be brief rather than a visible hole.

It is worth ~513 ms of a ~1,380 ms frame, on the zone-transition class only.
It does nothing for frames 3168 or 6914.

**And the remaining unknowns are now properly separated:**

| class | biggest unexplained piece | next step |
| --- | ---: | --- |
| zone transition (1911) | ~390 ms after the load and the poll | Stage 5.1 first, then re-measure what is left |
| no-load render hitch (3168) | ~790 ms | needs its own instrument; 193 ms of texture creation and 50 ms of main-thread lock contention are the only footholds |
| pump (6914) | -- | closed, §17. Host question. |

## 26. Stage 5.1 built: what re-verification added to the record

Everything §25 and the handoff recorded about the two call sites and
`Region::BackgroundLoadLevel` was re-read against the pinned `Engine.dll`
(SHA-256 `0aedbb18...f694f6`) before a byte was written, and all of it held.
Three things turned up that were *not* in the record, and two of them are
load-bearing.

### The renderer's skip test and the asynchronous path use the same byte

This is the claim the whole change rests on, and neither document stated it as
one thing. The call site ends:

```
  80 7f 74 00            cmp byte [edi+0x74],0
  c7 47 6c 00 00 00 00   mov dword [edi+0x6c],0
  0f 85 <rel32>          jnz the epilogue
```

and `BackgroundLoadLevel`, on the `[0x50] == 0` path -- the only path the
thunk routes to it -- ends:

```
1020be87  83 79 50 00    cmp dword [ecx+0x50],0
1020be8b  74 06          jz  0x1020be93
1020be93  c6 41 74 01    mov byte [ecx+0x74],1
```

`0x74` in both. **Deferring is not something bolted onto these call sites;
they were already written to skip a region that is still loading, and the flag
they read is the flag the asynchronous path raises.** If those two offsets
ever disagreed the renderer would draw a region whose level had not loaded,
silently, so `verify-sites.py` now reads both out of the operands and asserts
they agree.

Note also that the branch at `1020be8b` is *always taken* on the thunk's path,
so it is `[0x74]` and not `[0x75]` that gets set. Had the thunk routed the
resident case here it would have set `[0x75]`, which the call sites do not
test -- another way of saying the `region[0x50]` test is required rather than
defensive.

### The two windows are adjacent to the region-lock windows, exactly

The forced-load window is thirty-four bytes, not the thirty the handoff
recorded -- the handoff's count stopped before the trailing `JNZ`'s
displacement. And:

```
  0x167847 + 34 == 0x167869      the kLockSites window for the same owner
  0x17d8b7 + 34 == 0x17d8d9      likewise
```

Each table ends exactly where the other begins. That is an address cross-check
on both groups at once, it is free, and `verify-sites.py` now makes it.

### BackgroundLoadLevel is live code, and the game itself pairs it with LoadLevel

A full `.text` scan finds **zero** callers of `?BackgroundLoadLevel@Region@
GAME@@QAEX_N0@Z` inside `Engine.dll`, which on its own would be worrying. It
is called from `Game.dll`, once, and the site is the best evidence in the
project that the two functions are interchangeable:

```
101732a9  80 7c 24 40 00    cmp byte [esp+0x40],0
101732ae  8b 0e             mov ecx,[esi]
101732b0  8b 09             mov ecx,[ecx]
101732b2  74 0c             jz  0x101732c0
101732b4  6a 00             push 0
101732b6  6a 01             push 1
101732b8  ff 15 ..          call [BackgroundLoadLevel]
101732be  eb 08             jmp past
101732c0  6a 01             push 1
101732c2  ff 15 ..          call [LoadLevel]
```

`EndlessModeManager::ChangeArena` chooses between them at runtime, ten bytes
apart, on a flag. So the asynchronous path is shipped code the game runs, not
a vestige -- and the engine's own `Region::Update` sets the same `[0x74]` and
`[0x75]` pair in the same shape at `+0x372`, `+0x3d9` and `+0x777`. Nothing
about the deferred state is new to the engine.

The one thing this site does *not* settle is the flag: the game passes `true`
there, both call sites pass `false`. On the thunk's path the flag never
reaches a branch inside `BackgroundLoadLevel` -- the `jz` at `1020be6c` skips
the test -- but it *is* stored into the queued work item at `[esp+8]`, so it
is forwarded rather than hardcoded. Both sites push `0` today; forwarding is
what keeps that a fact about the sites rather than an assumption in the thunk.

### And one that only narrows the claim

`GraphicsSceneRenderer::AddElementsInBox`, the third of the three
`AddElementsInBox` overrides, does not call `Region::LoadLevel` at all. Of the
thirty-eight `E8` sites in `Engine.dll` that reach it, exactly the two the
handoff named are inside a renderer. There is no third site to miss.

### What is now machine-checked

`verify-sites.py` grew a section: both call-site windows byte for byte at
their RVAs, each site's owner resolving to an `AddElementsInBox` export named
in the source, `E8` at offset 12 with its displacement re-derived and required
to land on `Region::LoadLevel`, the adjacency above, and six assertions about
`BackgroundLoadLevel`'s behaviour rather than its identity -- the trap, the
re-entry guard, the flag store, and the `RET 8` that is the ABI the thunk
calls with. 120 checks pass; perturbing `kRegionLoadingOffset`,
`kRegionLevelOffset`, `kForceLoadCallOffset`, `kBackgroundLoadLevelRva`, a
call displacement, an owner RVA, or the count of sites each fails it.

**Still unmeasured, and only a run can settle it:** whether the deferred load
actually completes promptly, and how visible the pop-in is. `RegionLoader`
exports `Update`, `GetIsDone` and `GetAreLevelsLoaded`, so there is a service
loop; that it drains fast enough on this host is what runs 27 and 28 are for.

## 27. Runs 27 and 28: Stage 5.1 works, and it is aimed at the wrong call sites

The pair was run as designed: run 27 with `async_level_load=0`, run 28 with
`=1`, same DLL, same route, only the switch between them.

### Run 27 did its job

The build changed nothing. Frame 1943 is a near-exact replay of run 26's 1911:

| | run 26 f1911 | run 27 f1943 |
| --- | ---: | ---: |
| frame | 1,376.9 ms | 1,381.0 ms |
| `Region::LoadLevel` main | 512.6 ms | 514.9 ms |
| main-thread `Sleep` | 434.9 ms / 350 | 434.5 ms / 350 |
| `arc_open` | 1,299 | 1,299 |

p50 identical at 9.1 ms, both async columns zero session-wide. A good baseline,
and worth the boot for what it lets run 28 be compared against.

### Run 28 installed, ran, and deferred nothing at all

```
Async level load: 2/2 forced loads retargeted at Region::BackgroundLoadLevel
Engine trace: on, mask=0x1, cache off, async load requested, hooks=19
```

| | run 28 |
| --- | ---: |
| `engine_async_sync` (fell through, level resident) | **2,849** |
| `engine_async_load` (deferred) | **0** |
| frames the thunk was reached on | 1,284 |

**Not one region was ever deferred.** Every one of 2,849 calls through the two
`AddElementsInBox` sites found `region[0x50]` non-null -- the level already
resident -- and went to the original, which answers that case out of its own
first four instructions. The reporter noticed no pop-in, and that is not
evidence the trade is cheap: there was no trade, because nothing was deferred.

### And on the freeze frame the thunk was not called at all

This is the finding.

```
frame 1818, 1,534.3 ms          the zone-transition class
  engine_render                 1,528.6 ms
    |- Region::LoadLevel main     513.9 ms   in FIVE calls
    |- main-thread Sleep          434.7 ms   350 calls
    |- archive inflate            271.2 ms
    |- arc_open                     1,299
    |- engine_async_load                0
    +- engine_async_sync                0    <-- the thunk never ran
```

Five `Region::LoadLevel` calls cost 513.9 ms on that frame, and **none of them
came through either patched call site**. Across the whole session those five
calls are **99.7% of all main-thread level-load time** (515 ms of 515 ms; only
one frame in 6,881 has more than a millisecond of it).

So the premise Stage 5.1 was built on is wrong. "The renderer forces a
synchronous level load" is true; "it does so from `AddElementsInBox`" was never
measured. It was inferred from two facts that are both true and do not combine:
the load happens inside `Engine::Render`, and `AddElementsInBox` contains a
`Region::LoadLevel` call. `Engine::Render` reaches `Region::LoadLevel` by some
other route, and the `AddElementsInBox` sites are a cheap already-resident
check 2,849 times out of 2,849.

`Region::LoadLevel` and `Region::BackgroundLoadLevel` are structurally the same
function in this respect -- both bail immediately on `[0x50] != 0` with a false
flag -- which is why the thunk's fall-through was correct and why it cost
nothing. It was correct and it was pointed at nothing.

### What that leaves

**The mechanism is proven and the aim is not.** The thunk, the byte
verification, the switch, the counters and the two-run protocol all worked, and
the change is provably inert at both settings. What is missing is one fact
nobody has ever measured: **which of the thirty-eight `Region::LoadLevel` call
sites produces those five calls.**

That is cheap to get. `hookLoadLevel` is a trampoline detour, so its own return
address is the caller's; recording it when a call exceeds a millisecond costs
one comparison per call and produces about five log lines a session. Candidates
worth naming in advance, from the same `.text` scan:

- `?GuaranteedGetLevel@Region@GAME@@QBEPAVLevel@2@_N@Z+0xc` (`0x1020e7bc`) --
  the name is "get the level, loading it if it is not there"
- `?AddToScene@Region@GAME@@QAEXAAVGraphicsSceneRenderer@2@...+0x17`
  (`0x1020e707`) -- also a renderer path
- `?Update@Region@GAME@@QAEXPAV12@...+0x39c` (`0x1020aebc`)

Guessing between them is what produced this section. The next boot should
measure it.

**`async_level_load` stays in, defaulting off.** It is verified, inert, and it
becomes the fix the moment the right call site is known -- the thunk does not
change, only `kForceLoadSites`.

## 28. Run 29: the forced load comes from Region::GuaranteedGetLevel

One line answers it:

```
Engine trace: slow LoadLevel from Engine+0x20e7c1  x2 (2 main) total 515481 us worst 402394 us
```

`0x20e7c1` is a return address, so the call is at **`0x1020e7bc`** --
`?GuaranteedGetLevel@Region@GAME@@QBEPAVLevel@2@_N@Z` (`0x20e7b0`) **+0xc**.
Two calls, both on the main thread, 515.5 ms between them, the worse of the
two **402.4 ms** on its own. Nothing else in the session crossed a
millisecond, and the total matches run 28's session figure to within 2 ms.

Both landed on one frame, and the brackets say what kind of frame it was:

```
frame 1703, 1,540.7 ms          the ZONE-TRANSITION class
  engine_render                 1,535.2 ms
    |- Region::LoadLevel main     515.5 ms   two calls
    |- main-thread Sleep          434.9 ms   350 calls
    |- archive inflate            266.0 ms
    |- arc_open                     1,299
  engine_update                     0.0 ms
  game_update                       0.0 ms   <- no simulation on this frame
```

So the chain is `Engine::Render` -> ... -> `Region::GuaranteedGetLevel` ->
`Region::LoadLevel`, and the 350 `Sleep(1)` calls are inside that.

### The call site is the same shape as the two already patched

```
1020e7b0  56              push esi
1020e7b1  8b f1           mov esi,ecx
1020e7b3  57              push edi
1020e7b4  85 f6           test esi,esi
1020e7b6  74 42           jz  the null return
1020e7b8  ff 74 24 0c     push [esp+0xc]          the bool argument
1020e7bc  e8 ff d6 ff ff  call Region::LoadLevel        <- offset 12
1020e7c1  80 7e 74 00     cmp byte [esi+0x74],0         the loading flag
1020e7c5  c7 46 6c 00..   mov dword [esi+0x6c],0        unload countdown
1020e7cc  74 09           jz  0x1020e7d7                loaded -> go on
1020e7ce  33 ff           xor edi,edi                   still loading -> NULL
1020e7d0  8b c7           mov eax,edi
1020e7d4  c2 04 00        ret 4
```

`test reg,reg / jz`, `push <flag>`, `call` at **offset 12**, `cmp byte
[reg+0x74],0`, `mov dword [reg+0x6c],0`, branch on the flag. That is the
`kForceLoadSites` shape exactly, down to the call offset -- so the thunk, the
verification and the switch all work here unchanged, and pointing Stage 5.1 at
it is an entry in that table and nothing else.

**And despite the name, it already returns NULL when the region is loading.**
`[0x74]` set means "still loading", and the function answers that with
`xor edi,edi`. The deferral does not invent a state; it uses one the function
already has -- the same argument that made the `AddElementsInBox` sites safe.

### But the callers are not all rendering, and that is the new risk

Seventeen call sites in `Engine.dll`, none in `Game.dll` or `TQ.exe`:

| caller | what deferring would mean |
| --- | --- |
| `Portal::GetBackToFrontCoords+0x2b`, `GetFrontToBackCoords+0x2b`, `GetConnectedPortal+0x18` | rendering. A pop-in trade. |
| `World::GuaranteedGetRegionLevel+0x1e` | depends on *its* callers |
| `World::SetCoords` (x2), `WorldVec3::PutOnFloor`, `TranslateToFloor`, `TranslateUsingBoxToFloor`, `Region::AddEntity` | **entity placement.** A NULL level here is a gameplay question, not a visual one. |
| seven inside the path-mesh code | pathfinding |

On the `AddElementsInBox` sites the deferral could only ever cost a frame of
drawing. Here it reaches code that positions entities and builds paths, and
`GuaranteedGetLevel` is called from all seventeen through one body -- there is
no way to defer for the renderer and not for the rest by patching that call.

**So the next fact needed is which of the seventeen produced those two calls.**
`Engine::Update` and `GameEngine::Update` both read zero on frame 1703, which
points hard at the three `Portal` sites and away from placement and
pathfinding -- but "points hard at" is the reasoning that produced §27, and it
is measurable by exactly the trick that produced this section.

`Region::GuaranteedGetLevel` is exported, and its first six bytes
(`56 8b f1 57 85 f6`) contain no relative branch, so it takes an ordinary
trampoline detour and its return address is its caller's. One more boot names
the caller; if it is the portal renderer, Stage 5.1 becomes a one-entry change
and the trade is pop-in again.

## 29. Run 30: the caller is TranslateToFloor, and the chain keeps going

```
Engine trace: slow GuaranteedGetLevel from Engine+0x27faf4  x1 (1 main) total 403549 us worst 403549 us
Engine trace: slow GuaranteedGetLevel from Engine+0x275323  x1 (1 main) total 120138 us worst 120138 us
```

Minus five for the call instruction, and both land well inside their exports
with the next export starting after them:

| call | function | cost |
| --- | --- | ---: |
| `0x1027faef` | `?TranslateToFloor@WorldVec3@GAME@@QAE_NABVVec3@2@@Z` **+0x5f** | **403.5 ms** |
| `0x1027531e` | `?GuaranteedGetRegionLevel@World@GAME@@QBEPAVLevel@2@H@Z` **+0x1e** | 120.1 ms |

Both on frame 5312 -- 1,666.3 ms, `engine_render` 1,659.9 ms, `engine_update`
and `game_update` both **zero**. So the whole chain runs inside
`Engine::Render` on a frame with no simulation at all, and it is:

```
Engine::Render -> ? -> WorldVec3::TranslateToFloor
                        -> Region::GuaranteedGetLevel -> Region::LoadLevel
```

This session was longer than 27-29 (11,055 frames, 134 s) and caught a third
event the others missed: `slow LoadLevel from Engine+0x117a99`, 105.7 ms, on
frame 7085 -- which is an `Engine::Update` frame (108.1 ms update, 3.0 ms
render), not the zone-transition class. A fourth freeze class, an order of
magnitude smaller, worth naming only so it is not confused with 1911/1943/1703/5312.

### Why this does not finish the job

`TranslateToFloor` has seven direct callers in `Engine.dll`, and is imported by
`Game.dll` and `TQ.exe` besides:

| caller | kind |
| --- | --- |
| `RenderGroupManager::InitialUpdate+0x3e` | **rendering** |
| `World::PlaceDecalOnGround+0x38` | **rendering-adjacent** |
| `PathPE::SetCurrentSegment+0x680` | pathfinding |
| four sites in the path-mesh code | pathfinding |
| `Game.dll`, `TQ.exe` via import | unknown |

And `World::GuaranteedGetRegionLevel` has **zero** direct `E8` callers in
`Engine.dll` -- it is reached through `TQ.exe`'s import table.

So patching the `TranslateToFloor -> GuaranteedGetLevel` call at `0x1027faef`
would defer for pathfinding too, which is the same objection §28 raised one
level down. **Every level up multiplies the call sites rather than narrowing
them**, and two of the seven being rendering is a lead, not an answer.

The lead is a good one: a zone transition inside `Engine::Render` with no
simulation running fits `RenderGroupManager::InitialUpdate` -- a render group
being set up for a region for the first time -- and fits pathfinding badly.
But that is the third time this project has had a good-looking inference about
this call chain, and §27 is what the first two were worth.

### The technique has to change, not the target

Hooking one function per boot resolves one link per boot, and the chain is at
least four deep. What answers it in one is a **bounded stack scan at the slow
call**: from the hook's frame, walk a few hundred dwords upward, keep every
value that lands inside `Engine.dll`'s `.text` *and* is preceded by a call
instruction -- `E8 rel32` five bytes back, or `FF 15` six back -- and log the
first several in order.

The game's code does not keep frame pointers (`GuaranteedGetLevel` opens
`push esi; mov esi,ecx; push edi`), so a proper frame walk is not available;
the call-preceded filter is what makes the scan trustworthy instead. Stale
slots can survive on the stack, so the output is a superset -- but the true
chain appears in it as a subsequence in increasing stack-address order, and
three events a session makes it readable by eye.

Read-only, bounded, and it runs only on calls already costing a millisecond.

## 30. Run 31: the stack scan works, and the chain leaves Engine.dll

Two chains, both on frame 1542 (1,507.4 ms, `engine_render` 1,501.5 ms,
`engine_update` and `game_update` zero), 20 and 21 frames each and sharing a
tail. Reading the raw RVAs against the export table is nearly useless in a
stripped binary; **decoding the call instruction in front of each return
address and naming its target** is what made them readable, and that is the
technique note worth keeping:

```
  +0x27faf4   E8 -> Region::GuaranteedGetLevel        TranslateToFloor+0x64
  +0x20e7c1   E8 -> Region::LoadLevel                 GuaranteedGetLevel+0x11
  +0x275323   E8 -> Region::GuaranteedGetLevel        GuaranteedGetRegionLevel+0x23
  ...
  +0x144027   E8 -> Display::Update                   Engine::Render+0x47
```

### What is established

- `Engine::Render+0x47` calls `Display::Update`. A verified edge, and the top
  of the chain.
- `TranslateToFloor+0x64` and `GuaranteedGetRegionLevel+0x23` both call
  `GuaranteedGetLevel`, which calls `LoadLevel`. The bottom, confirming runs
  29 and 30 from a second direction.

### What is stale, and how the scan says so itself

The group `+0x11f3a7 +0x1447ad +0x1447cd` -- `LeaveCriticalSection`, then two
call sites inside `Engine::Update` -- **appears twice in each chain**, and
`engine_update_us` reads **zero** on that frame. Those are leftovers from an
earlier frame's `Engine::Update` at the same stack depth. A repeated group is
the tell, and the per-frame columns are the arbiter. The superset warning in
§29 was not theoretical.

### Why it does not close, and this is the useful part

**Nothing in either chain calls `TranslateToFloor`.** No entry's decoded edge
targets `0x27fa90`. The scan bridged `Engine::Render` to `Display::Update` and
`TranslateToFloor` to `LoadLevel`, and found nothing between them -- because
the chain leaves the module:

| | imports `TranslateToFloor` | imports `GuaranteedGetRegionLevel` |
| --- | --- | --- |
| `Game.dll` | yes (also `PutOnFloor`, `PlaceDecalOnGround`) | -- |
| `TQ.exe` | yes (also `PutOnFloor`) | **yes** |

Both slow call sites are functions that `TQ.exe` and `Game.dll` import
directly, so their immediate callers are in those modules -- and the filter
keeps only addresses inside `Engine.dll`'s `.text`. Every frame between
`Display::Update` and `TranslateToFloor` was dropped by construction.

`TQ.exe` also imports `?Update@DisplayWidget@GAME@@UAEXXZ`, which fits the
`FF10 call [reg]` at `Display::Update+0x22`: a widget list dispatched
virtually, with the widget implemented in `TQ.exe`.

### The fix is one adjustment, not a new technique

Accept `Game.dll` and `TQ.exe` `.text` alongside `Engine.dll`'s, and label
each entry with the module it came from. The `E8` filter already re-derives
its destination, so it stays as strong; the indirect forms stay as they are.
`auditedImage` already knows both other modules' `SizeOfImage`, so nothing new
has to be trusted.

That is the same boot again with a wider filter, and on the evidence above it
should close the gap in one.

## 31. Run 32 closes the chain, and the zone-transition freeze is a level load

All three modules admitted, both chains 32 frames, and the gap filled:

```
 0  E+0x27faf4  TranslateToFloor+0x64            E8   -> Region::GuaranteedGetLevel
 1  E+0x20e7c1  GuaranteedGetLevel+0x11          E8   -> Region::LoadLevel
 4  T+0x46411   TQ.exe sub_45437+0xfda           FF15 -> Engine!TranslateToFloor
...
28  E+0x144027  Engine::Render+0x47              E8   -> Display::Update
29  T+0x4eeac   TQ.exe                           FF15 -> Engine!Engine::Render
```

and chain 0's differing frame is `T+0x463bf`, `FF15 -> Engine!World::
GuaranteedGetRegionLevel`, in **the same TQ.exe function** -- `sub_45437`,
`+0xf88` and `+0xfda`, eighty-two bytes apart.

### What that function is

It is not a renderer and it is not pathfinding. Its imports name it outright:

```
+0x12c   Engine::InitializeMod          +0x601   World::Load
+0x15b   GameEngine::Reload             +0x646   SoundManager::EnableDistanceCheck
+0x2d8   Engine::RemoveWidget           +0x8fa   GameEngine::SetPlayer
+0x3b1   IOAtomicRead ctor              +0x425   IOStreamRead::StreamPropertyEx
```

and the twenty instructions around the two slow calls settle it completely:

```
+0xf03   Character::GetSpawnPoint
+0xf14   World::SetPlayerSpawnPoint
+0xf27   IOStreamRead::Shutdown              the save file is finished with
+0xf6f   PlayerManagerClient::SetMainPlayer
+0xf82   World::GuaranteedGetRegionLevel     <-- 114.5 ms
+0xf8e   Character::GetSpawnPoint
+0xfd4   WorldVec3::TranslateToFloor         <-- 402.4 ms
+0xff4   World::AddEntity                    the player is placed
+0x1051  GameInfo::SetLevelName
```

**This is the game loading a world and spawning the player into it.** It reads
the save, calls `World::Load`, finds the spawn point, forces the spawn
region's level resident, snaps the spawn position to the floor, and adds the
player entity. It runs under `Display::Update` inside `Engine::Render` --
which is why `engine_update` and `game_update` read zero on every one of these
frames across runs 26-32. The game is not simulating. It is loading, and it
paints from the display update so the screen is not dead while it does.

### Stage 5.1 cannot be applied here, and this is the end of that line

Deferring the load at either site means `GuaranteedGetLevel` returns null,
which means `TranslateToFloor` cannot find the floor, which means
`World::AddEntity` places the player at an unsnapped position. That is not
pop-in. It is spawning the player into a world that is not there.

So the answer to §28's question -- rendering or placement -- is **placement**,
and of the most consequential kind. `async_level_load` stays in the build:
verified, inert, default `0`, and correct if a genuinely deferrable site is
ever found. But it does nothing for this frame class and it never could.

### What the frame actually is, and what is left in it

```
frame 1803, 1,470.2 ms          run 32; the same shape in runs 26-31
  engine_render                 1,464.5 ms
    |- Region::LoadLevel main     516.8 ms   the spawn region, mandatory
    |    |- main-thread Sleep     435.3 ms   350 calls, 350 ms REQUESTED
    |- archive inflate            270.5 ms   on the loader thread
    |- arc_open                     1,299
  engine_update                     0.0 ms
  game_update                       0.0 ms
```

The main thread asks to sleep 350 ms and gets 435. It is polling a loader
thread that is genuinely busy -- 1,299 archive entries opened and 270 ms of
zlib on that frame. So the wait is real work elsewhere, not idleness, and the
two levers on it are both already in the plan:

- **4.3, libdeflate.** 270 ms of zlib on this frame; typically 2-3x faster, so
  perhaps 110-140 ms off it. The largest remaining item, and its
  justification is now much stronger than when it was written -- it attacks
  the thing the main thread is actually waiting for.
- **`timeBeginPeriod`.** 350 requested against 435 actual is 85 ms of host
  granularity on 350 `Sleep(1)` calls, exactly as §25 measured. Nearly free.

Everything else on the frame is the game loading a level, and no mod can make
that not happen.

### It IS the initial load, and the discriminator is game_collisions

This section said "start-up world load"; §32's first draft then called that
wrong and relabelled it a mid-play level change; this settles it back, on a
column neither reading had used.

`game_collisions` counts `InterpenetrationManager::FixupCharacterCollisions`,
which cannot run without a character in a world:

| | run 32 | run 33 |
| --- | ---: | ---: |
| the big frame | 1803 | 2704 |
| `game_collisions` before it | **0** | **0** |
| after it | 4,765 | 4,874 |
| first non-zero frame | 2156 | 2828 |
| `grass_draw` first non-zero | 3182 | 3474 |

The player is not in the world until after that frame. **It is the initial
load of the save from the menu**, once a session, before play starts.

What produced the wrong correction was reading `game_update_us > 0` from frame
631/1367 as "gameplay has started". It has not: `GameEngine::Update` ticks the
**main menu** too. The menu runs at several hundred frames a second, so
twenty-five seconds of menu and character select is 2,700 frames, which is why
the frame index looked far too late to be a start-up load.

**And the reporter's model of traversal is right** -- crossing a level
boundary during play loads the next area without re-spawning anybody. That
mechanism is real and it is measured: it is the 1,700-3,700 sub-millisecond
loads a session in §32's table. It is simply not what this frame is.

So the conclusion stands as first written, for the right reason now: this
frame is a loading pause the player waits through once, `sub_45437` is the
menu's load-game routine, and nothing in it can be deferred because the player
is placed on a floor that has to exist.

## 32. Traversal is already asynchronous — with exactly one exception

The reporter asked the obvious follow-up to §31: the first world load has to be
synchronous, but why should walking from region to region be? The frame data
answers it without another boot, and the answer is that **it already is.**

Main-thread `Region::LoadLevel`, bucketed by how much a frame spent in it:

| forced load on the frame | run 28 | run 30 | run 32 |
| --- | ---: | ---: | ---: |
| >0 and under 1 ms | 1,195 frames | 3,697 | 1,739 |
| 1-5 ms | **0** | **0** | **0** |
| 5-20 ms | **0** | **0** | **0** |
| 20-100 ms | **0** | **0** | **0** |
| over 100 ms | 1 frame | 2 | 1 |
| calls / total | 205,564 / 515.4 ms | 207,442 / 635.8 ms | 200,204 / 519.4 ms |

**The distribution is bimodal with nothing whatever in the middle.** Two
hundred thousand calls a session cost two to six milliseconds in total once the
one or two big frames are set aside. There is no such thing as a
medium-sized level load on this route.

The renderer says the same thing from the other side: with `async_level_load=1`
the two `AddElementsInBox` sites were reached 2,849 times and found the region
already resident 2,849 times. `Region::PreLoad` and `RegionLoader` appear in
the run 32 chain, and they are keeping ahead of the player. Nothing to make
asynchronous, because nothing is synchronous.

### The exception, and it is the case the question meant

Run 30's longer session (11,055 frames) caught a second big frame that is not
the world load: `Engine+0x117a94`, 105.7 ms, on frame 7085 -- `engine_update`
108.1 ms, `engine_render` 3.0 ms. **An Engine::Update frame, so this one is
during play.** Its containing function, `sub_117980`, is unmistakable:

```
+0x8c    World::GetRegionsInFrustum
+0xc2    WorldFrustum::GetRelativeFrustum
+0x105   Portal::GetConnectedRegion
+0x114   Region::LoadLevel            <-- 105.7 ms
+0x148   Region::GetPortal
+0x158   Portal::GetChokePoint
```

Portal traversal: find the regions in view, walk the portals, take the region
on the far side, and force its level resident. That is exactly "walking from
one area to the next", and it is the only synchronous level load that happens
during play.

### And it is deferrable, on a stronger argument than the sites already patched

```
10117a8c  85 f6              test esi,esi
10117a8e  74 7b              jz  past
10117a90  6a 00              push 0
10117a92  8b ce              mov ecx,esi
10117a94  e8 <rel32>         call Region::LoadLevel      <- offset 8
10117a99  80 7e 74 00        cmp byte [esi+0x74],0
10117a9d  c7 46 6c 00..      mov dword [esi+0x6c],0
10117aa4  75 19              jnz 0x10117abf              still loading -> skip
...
10117ac8  e8 <rel32>         call Region::GetPortal
10117acd  85 c0              test eax,eax
10117acf  74 3a              jz  0x10117b0b              NULL-CHECKED
```

The same shape as the two sites already in `kForceLoadSites` -- with the call
at **offset 8** rather than 12, because the `jz` is the two-byte form here --
already branching on `region+0x74`, **and** the one call it makes on that
region afterwards has its result null-checked. That last part is what §28
could not say about `GuaranteedGetLevel` and is why this site is a better
candidate than either of the two already patched.

### What that is worth, honestly

105.7 ms, once in the longest session measured, on a frame the player is
playing through rather than waiting on -- and run 33 then measured how rare
that is. See §33.

So §31's "Stage 5.1 is dead" was too broad. It is dead for the zone-transition
*load* frame, where deferring would spawn the player into a world that is not
there. It has exactly one live target during play, and pointing it there is a
third `kForceLoadSites` entry plus a per-site call offset.

## 33. Run 33: all three sites measured, and Stage 5 is finished

`Async level load: 3/3 forced loads retargeted`, 8,130 frames, 120 seconds.

| | calls | deferred |
| --- | ---: | ---: |
| the two renderers (`engine_async_*`) | 2,836 | **0** |
| portal traversal (`engine_portal_async_*`) | **1,355** | **0** |

**The portal site was exercised 1,355 times and never once had to load.** The
reporter thought portals had gone untested because they took no teleport
portal -- but `Portal` in this engine is a region *connection*, a doorway, and
`sub_117980` walks them every update. The path ran constantly; the region on
the far side was already resident every single time.

So all three sites are now measured on the same route, and all three defer
nothing. The engine's own `RegionLoader` keeps ahead of the player everywhere
this mod can reach. Run 30's 105.7 ms event was one miss in five sessions and
about 40,000 opportunities.

A third slow caller also appeared, and it is the same story a fourth time:
`slow GuaranteedGetLevel from Engine+0x27b026` -- `World::SetCoords+0x51` --
**1.2 ms**, once.

### Where that leaves async_level_load

It stays: verified byte for byte, inert, default `0`, three sites, its own
counters. It costs nothing and it is correct. It is simply worth nothing on
this route, and the measurement that says so is now four sessions deep rather
than an argument.

**Stage 5 is finished.** Every synchronous `Region::LoadLevel` in the game has
been found, attributed to its call site, and priced:

| | cost | can it be deferred? |
| --- | ---: | --- |
| menu load-game, spawning the player | 516 ms, once a session | no -- the floor has to exist |
| the two renderer sites | 0 | nothing to defer |
| portal traversal | 105.7 ms once in five sessions | yes, and it is built |
| `World::SetCoords` | 1.2 ms | not worth it |
| everything else (~200k calls) | 2-6 ms a session total | already asynchronous |

### And the freeze the reporter actually feels is still unexplained

The 1,470-1,666 ms frame is the initial load and only 516 ms of it is
`Region::LoadLevel`. **Some 950 ms of it is the rest of `World::Load` and has
never been instrumented** -- and that is a loading pause, so it is the least
interesting large number in the project.

The in-play stutters remain frame 3168's class -- a render hitch with no level
load, ~790 ms unaccounted, 193 ms of texture creation and 50 ms of main-thread
lock contention the only footholds -- and frame 6914's, the message pump,
closed as a host question in §17. 3168's class has been the honest next target
since §25 and nothing since has displaced it.

## 34. A fresh-eyes review, once "the player is in the world" became knowable

`game_collisions` -- `InterpenetrationManager::FixupCharacterCollisions`, which
cannot run without a character in a world -- turns out to be a clean marker for
when play starts. Applying it to all nineteen recorded runs changes more than
§33 did, and two of the three things it changes are mistakes this project has
been carrying for a long time.

### 1. Every headline number has included the menu

| | whole session | in-game only |
| --- | ---: | ---: |
| p50 | 8.3-10.0 ms | **12.1-13.4 ms** |
| menu share of frames | 25-47% | -- |
| in-game frames | -- | 4,879-5,792, near-constant |

**Menu frames are a quarter to nearly half of every session**, and they are
cheap and short, so they have been dragging p50 down by about 40%. Worse, the
menu's *length varied wildly between runs* -- 1,719 frames in run 31, 5,454 in
run 30 -- while the in-game frame count barely moved. So run-to-run p50 and
"the mod's share" comparisons have been differencing a varying amount of menu.
Nothing in the plan turned on those comparisons, but they were being read.

**`tools/frames.py` should split at the first frame with `game_collisions`
non-zero and report the two halves.** That is a small change and every future
run benefits from it.

### 2. In almost every run, "the worst frame" was in the menu

The load-game frame is bigger than anything that happens in play, so it won
`max()` in fourteen of nineteen runs. Everything §21-§33 chased was that frame.
The worst *in-game* frame is a different and much more consistent animal:

| | worst in-game frame |
| --- | --- |
| when | 940-1,670 frames after the player enters the world |
| size | 578-1,938 ms |
| `Region::LoadLevel` | **0.0 ms** |
| main-thread `Sleep` | **0.0 ms** |
| `arc_open` / `texture_create` | ~200 / ~100 |
| reproduced in | **16 of 19 runs** |

Same shape every time, at nearly the same point on the route. This is §25's
frame 3168 class, and it is the only large thing in this project that happens
while the player is playing.

### 3. It was never unaccounted, and that is the finding

§25 anatomised frame 3168 as `Region::LoadLevel` 0.02 ms, texture creation
193.3 ms, inflate 275.8 ms, lock contention 50.2 ms, and **"roughly 790 ms
nothing accounts for"**. That table omitted a column that was in the same CSV
row:

```
frame 3168, 1,113.3 ms
  engine_render                          1,040.3 ms
    |- ResourceLoader::LoadResource, MAIN THREAD  616.7 ms   <-- was in the row
    |    |- archive inflate                       275.8 ms
    |    |- off-thread texture creation           189.9 ms
    |- texture_create (the mod's own phase)       193.3 ms
    |- main-thread lock contention                 50.2 ms
```

`engine_res_load_main_us` has existed since run 10 and has had a `_main` split
the whole time. Across the sixteen runs of this class it names **33% to 68% of
`Engine::Render`, median 44%**:

| run | frame | ms | render | res_load main | share |
| --- | ---: | ---: | ---: | ---: | ---: |
| 20 | 3590 | 577.8 | 501.1 | 164.3 | 33% |
| 26 | 3168 | 1,113.3 | 1,040.3 | 616.7 | 59% |
| 19 | 2981 | 1,453.7 | 1,382.3 | 816.3 | 59% |
| 30 | 7124 | 1,938.1 | 1,855.0 | 1,265.4 | **68%** |

**The in-game stutter is the main thread synchronously loading resources
inside `Engine::Render`** -- not levels, resources: textures and models, with
the archive inflate and the texture creation nested inside. Stage 5 spent
runs 27-33 on `Region::LoadLevel`, which is 0.0 ms on every one of these
frames.

This also re-founds **4.3 (libdeflate)** on something better than a session
total: 106-366 ms of the inflate sits *inside* that main-thread resource load,
on the frame the player actually feels.

### 4. And there is a five-second frame nobody has ever looked at

Runs 16 and 33 each contain one in-game frame of **5,016.9 ms** and
**5,024.9 ms**, with `engine_render` of 5,007.5 and 5,007.9 ms and essentially
nothing else named -- no resource load, no level load, no lock wait, no object
wait, 33-228 archive opens.

Two independent runs landing within 0.4 ms of each other, at 5.007 seconds,
is not a coincidence: **that is a five-second timeout somewhere inside
`Engine::Render`**, and it is the largest single event in the entire dataset.
It is five times the in-game stutter and three times the load frame. Nothing
in this project has ever mentioned it, because `max()` over the whole session
usually found the load frame instead and these two runs were never compared.

### The corrected taxonomy

| class | size | when | status |
| --- | ---: | --- | --- |
| **A** menu load-game | ~1.5 s | once, before play | a loading pause; ~516 ms is `LoadLevel`, ~950 ms is the rest of `World::Load`, uninstrumented |
| **B** in-game resource load | 578-1,938 ms | ~1,000-1,600 frames in, 16/19 runs | **the real target.** Main-thread `LoadResource`, 33-68% of `Engine::Render` |
| **C** the five-second stall | ~5,007 ms | twice in 19 runs | **never investigated.** Looks like a timeout |
| **D** the message pump | ~1,500 ms | rare | closed as a host question, §17 |

§25 said to name the class whenever making a claim about "the worst frame".
That was right, and the classes it named were nearly right -- but it put
class A and class B in the same list as if they were comparable, and it
mis-anatomised B by leaving out the column that explains it.

## Cross-references worth acting on

1. `hookArchiveUnmap` (`src/visual.cpp:617-650`) binds to `FileDirectory`, not to
   archive entries.  If the audited install serves textures from `.arc`, the
   progressive uploader never runs and its cost is pure overhead; if it serves
   them loose, the uploader is holding `MapViewOfFile` views open across frames
   and taking their page faults on the present thread.  *(Answered since:
   `arc-format.md` establishes that this install serves everything from `.arc`,
   so the first branch is the one that holds and the uploader has never run.
   The `upload_src_*` columns confirm it per run.)*

   **This item's implied fix — bind to the archive `File` class as well — is
   the wrong shape, and §5 below says why.**  The `"TEX"` container branch of
   `GraphicsTexture::Initialize` walks one buffer across several
   `CreateTexture2D` calls, so a design that takes ownership of that buffer at
   the first call is a use-after-free.  The uploader must copy the mips it
   retains, which then makes both `File` classes identical from its point of
   view and removes the need to bind to either.
2. The renderer can force a synchronous level load (§1a).  A probe counter on
   `Region::LoadLevel` entries during a frame would separate "the game loaded a
   level inside our render pass" from every mod-side suspect, which is exactly
   what Stage 3 run 7 needs to interpret its residual hitches.
3. The widened `shadow_split` does not pull regions in, but it does hold them in
   (§2), and it enlarges the set of regions a render-path call can trip a
   synchronous load on.  Reverting the split is not a fix for the "new area"
   hitch; it is a reduction in how often the game's own hazard is reached.
