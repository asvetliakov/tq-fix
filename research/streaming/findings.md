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
