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

## 35. `game_collisions` is not "in the world", and the stutter is somewhere else

§34 was right that every headline number had included the menu, right that the
worst in-game frame was a different animal from the load-game frame, and right
that `engine_res_load_main_us` had been sitting unread. It was wrong about
where the boundary is, and that moves both classes it named.

### The marker, and the ten to fourteen seconds it hides

`game_collisions` fires when a character exists in a world. It does not fire
when the player can see one. Across runs 14-33 the game simulates collisions
for another **646-1,670 frames -- 8.8 to 14.4 seconds** -- while `draw_indexed`
sits at **1** a frame and nothing is on screen but a loading screen.

`draw_indexed` separates them and does it with enormous margin:

| | draw_indexed |
| --- | ---: |
| loading screen | **1** |
| menu, character preview | 77-80 |
| first frame that draws the world | **1,753-1,802** |

Any threshold between 100 and 1,400 works. It is also the only marker of the
two that runs 9-13 can use, since `game_collisions` did not exist yet, and it
puts world entry in all twenty-four recorded runs.

### So the session has four parts, not two, and it is the same in 19 of 19 runs

```
menu                  1,719-5,454 frames    p50 7.3-8.2 ms
  load-game frame     1,302-1,666 ms        Region::LoadLevel ~513 ms   (class A)
loading screen          646-1,670 frames    8.8-14.4 s, one draw a frame
  first world frame     578-1,938 ms        3,811 ms in run 33          (was "class B")
play                  3,853-4,656 frames    p50 13.5-14.3 ms
```

The route is scripted and the shape is identical in every run: an outdoor
stretch, an indoor stretch, an outdoor stretch, then the exit.

### §34's class B is the frame that ends the loading screen

The 578-1,938 ms frame is not "~1,000-1,600 frames into play". **It is the
first frame of play**, in **19 of 19 runs, not 16** -- and it is the same frame
by identity, not by resemblance:

| | frame before | the frame |
| --- | ---: | ---: |
| `draw_indexed` | **1** | 1,753-1,802 |
| `buffer_create` | 0 | 1,275-1,325 |
| `shader_create` | 0 | 55-67 |
| `engine_render_us` | 1.6-4.1 ms | 501-3,682 ms |

That is the whole visible world being created at once. It is the last third of
a loading pause the player is already sitting through. From the load-game
frame to the first world-drawing frame inclusive is **11.5 to 19.2 seconds**,
and this frame is 3.4-22.1% of it. It belongs
with class A, and calling it "the only large thing that happens while the
player is playing" put the project back on a loading screen for a third time.

### The real in-game stutter is smaller, and far more consistent

Split at the first world-drawing frame and take the worst render-dominated
frame after it. It is there in **19 of 19 runs**, at the same point on the
route every time, and it is a different size from what §34 measured:

| | |
| --- | ---: |
| when | play + **3,245-3,527 frames**, median 3,326 |
| frame | **290.6-439.2 ms**, median 337.9 |
| `Engine::Render` | 283.9-421.4 ms -- **96-99% of the frame** |
| `Engine::Update` | 2.8-14.2 ms |
| `Region::LoadLevel` main | **0.0 ms** |
| main-thread `Sleep` | **0.0 ms** |
| main-thread `EnterCriticalSection` | **0.0 ms** |
| main-thread object wait | **0.0 ms** |
| `engine_res_load_main_us` | 146.6-221.1 ms, **42-69% of Render**, median 54% |
| `texture_create_ms` (nested in it) | 34.1-78.9 ms |
| archive inflate | 56.5-86.6 ms |
| archive read | 16.5-22.1 MiB |
| `draw_indexed` | 1,456-1,648, against **211-217 the frame before** |

So §34's mechanism survives its own relocation: the main thread does
synchronously run `ResourceLoader::LoadResource` inside `Engine::Render`, and
it is the largest named thing on the frame. It just does it on a 340 ms frame
during play rather than on a 1,100 ms frame on a loading screen.

`draw_indexed` going 213 -> 1,500 **and staying there** says what the frame is:
the player steps from an indoor stretch back outdoors, and the outdoor scene is
created and drawn in one frame. It is the loading-screen frame again, one order
of magnitude down and with the player in control.

### One correction inside the anatomy: the nesting

§34's tree hung "off-thread texture creation" under `LoadResource, MAIN THREAD`
and put `texture_create` beside it. Both are backwards, and the columns settle
it without a boot:

- `engine_tex_create_off_us` is `CreateTexture2D` **not** on the render thread
  (`visual.cpp:1281`). It cannot be inside a main-thread call.
- `texture_create_ms` **is** the render thread, and it nests: from world entry
  on it exceeds `engine_res_load_main_us` on 14-20 frames a run and never by
  more than 0.67 ms. (It exceeds it by 141 ms on frame **0** of run 26 -- the
  device creating its own resources before any load exists. That is the only
  large exception in any run, and it is not a counterexample.) It is a child,
  not a sibling.
- archive inflate does **not** nest -- it exceeds the main-thread load on
  779-990 frames a run, because it runs on the loader thread.

### What the remaining share is made of

Subtract the main-thread resource load and everything else named on the frame
-- grass 3.0-11.7 ms, buffer and shader creation 2.6-5.6 ms, `PresentSurface`
0.1-2.9 ms, and lock and object waits which are exactly zero -- and
**87-230 ms is left, 29-54% of `Engine::Render`, median 43%.**

The frame after it says what that is, and it says it in all nineteen runs:

| | frame N | **N+1** | N+2 |
| --- | ---: | ---: | ---: |
| frame | 290-439 ms | **120-218 ms** | 18-57 ms |
| `Engine::Render` | 284-421 ms | **101-194 ms** | 12-31 ms |
| `engine_res_load_main_us` | 147-221 ms | **0.0 ms** | 0-17 ms |
| `arc_open` | 65-92 | **0-8** | 0-14 |
| `texture_create` | 50-70 | **0** | 0-4 |
| `buffer_create` | 134-356 | **0-36** | 4-47 |
| `draw_indexed` | 1,456-1,648 | 1,437-1,630 | 1,467-1,686 |
| `gpu_frame_ms` | 398-583 | **19.7-25.6** | 19.6-56.4 |

**N+1 draws the same scene as N+2, loads nothing, creates nothing, and the GPU
finishes it in the normal 20 ms -- and it still costs 89-179 ms more
`Engine::Render` than N+2, median 129 ms.**

That is not the game's loader and it is not the GPU. It is CPU time inside
`Engine::Render`, charged to the first frames that *use* the 134-356 buffers
and 50-70 textures created on frame N. Every other candidate is already
excluded by a column that reads zero: no level load, no sleep, no lock wait, no
object wait, no heap, no I/O, and creation itself is timed at 2.6-5.6 ms for
all of it.

**Two candidates remain and the data cannot separate them.** Either the time is
*inside* the D3D11 calls -- the layer below doing first-use work, which under
CrossOver is DXMT rather than the DXVK an earlier draft of this section named
-- or it is the *game's own code between* the calls, per-object setup the
engine does the first time a batch of objects comes into view.

**The reporter's evidence weighs against the first.** This stutter is well
known on Titan Quest on native Windows, where there is no translation layer at
all. That does not eliminate a translation-layer contribution, but it makes the
engine's own first-use work at least as likely, and it is the reason not to
write this section as if the answer were already known. The instrument settles
it either way, because it brackets the driver call and nothing else.

### Two costs, not one: what the correlations say

Across the nineteen in-play stutter frames, and the route has two variants
(~1,460 draws with 134-170 buffers, and ~1,630 draws with 338-356), so draws,
maps, buffers and SRV binds all move together and cannot be told apart here:

| | frame N residual (87-230 ms) | frame N+1 excess (89-179 ms) |
| --- | ---: | ---: |
| `buffer_create` | **r = +0.81** | +0.17 |
| `draw_indexed` | **r = +0.81** | +0.07 |
| `map` | **r = +0.81** | +0.12 |
| `ps_set_srv` | **r = +0.80** | -0.00 |
| `texture_create` | -0.10 | -0.03 |
| `engine_arc_kib` | -0.38 | -0.13 |
| `engine_res_load_main_us` | -0.16 | +0.22 |

**Frame N's residual scales with scene size and not at all with textures
created or bytes inflated. Frame N+1's excess correlates with nothing** -- it
is a flat ~129 ms whatever the scene size. So there are two costs here, one
per-object and one fixed, and they need separating before either can be
attacked. A fixed cost that is independent of object count is what a fixed set
of shader or material state being prepared once looks like; `shader_create` is
0 on frame N, so whatever is being prepared, it is not the game creating D3D
shaders.

### Does it need a new hook? No new patch site -- two phase counters. Built.

`hookDrawIndexed`, `hookDraw` and `hookMap` already exist as device-context
vtable patches (`visual.cpp:2776`, `2825`, `2843`) and are already installed on
every run. They counted and did not time.

Built as `[debug] draw_timing`, default `0`, adding two phases:
`draw_submit_ms` around the game's own `Draw`/`DrawIndexed` and
`map_resource_ms` around its `Map` -- **the driver call only**, not the
surrounding hook, so the mod's own SMAA, bloom and grass-cross work does not
nest inside them. No new bytes, no new import, nothing added to
`verify-sites.py`, which stays at 139 checks.

Three things the build had to get right, and one it caught:

- **The columns are the game's time, not the mod's.** `tools/frames.py` now
  reports them in their own bucket beside `present_call`; charging them to the
  mod's share would invert the one number that script exists to print.
- **The switch cannot arm itself.** `draw_timing=1` with the probe off does
  nothing -- there is no frame record for a phase to land in -- and the probe
  alone leaves it off. Both are self-test assertions.
- **A zero column has to be readable.** The CSV carries a `# draw_timing=`
  header line, so `draw_submit_ms = 0` can be told from nobody holding a
  stopwatch.
- **The names-array assertions were tautological.** `kPhaseNames[PhaseCount]`
  makes `sizeof / sizeof` the declared bound rather than the initializer count,
  so the existing `static_assert` on `kCounterNames` would have held however
  many names were written, and a phase added without one would have shipped an
  empty CSV column. All four arrays now declare no bound and all four assert
  their count; removing a name fails the build, which was checked.

The cost is the remaining objection and `hookUnmap` already states it: a clock
pair on a hook that runs 1,500-2,700 times a frame is not free. **Run 34
validates itself against it**: in-play p50 across runs 14-33 is 13.5-14.3 ms,
and if the run lands inside that band the instrument is cheap enough and its
numbers stand. If it lands above, arm the timers only on the frame after a long
frame -- N+1 is the cleaner measurement anyway, since it loads nothing.

### And the five-second frames are not in play at all

The reporter says they do not remember a five-second stutter in game. They are
right, and this is the third thing the marker moves.

| | run 16 | run 33 |
| --- | ---: | ---: |
| frame | 2856 | 3369 |
| frame time | 5,016.9 ms | 5,024.9 ms |
| `engine_render_us` | 5,007.5 ms | 5,007.9 ms |
| where | **loading screen** | **loading screen** |
| frames before the world is drawn | 55 | 105 |
| `draw_indexed` | 1 | 1 |

Both sit inside the loading screen, near its end, with nothing on screen. They
are `game_collisions`-positive, which is exactly why §34 read them as in-game.
So class C is a five-second addition to a loading pause the player is already
waiting through, in 2 of 19 runs -- unpleasant, but not the freeze anyone
feels, and it drops below the 340 ms in-play stutter in priority.

What it *is* remains open, and run 33 carries the one clue the mod recorded:

```
frame 3369, 5,024.9 ms
  engine_render                       5,007.9 ms   main thread
    |- everything hooked inside it          0.0 ms
  engine_obj_wait_main_us                  0.024 ms
  engine_cs_wait_main_us                   0.032 ms
  engine_obj_wait_us (all threads)      5,023.6 ms   <-- one non-main thread
  engine_sleep_us    (all threads)      9,343.0 ms   3,582 calls
```

A 5,023.6 ms object wait on a loader thread is a `WaitForSingleObject` with a
5,000 ms timeout expiring. The main thread's own 5.007 s is *not* any of the
four blocking primitives -- `Sleep`, `WaitForSingleObject`,
`WaitForMultipleObjects`, `EnterCriticalSection` are all hooked and all read
zero on the main thread. But they are hooked **in `Engine.dll`'s import table
only**, so a wait issued from `Game.dll`, from `TQ.exe`, or from inside
d3d11/DXMT is invisible. That is where to look, and `patchImport` on a second
module is the cheap way to look.

### What this leaves

| class | size | when | status |
| --- | ---: | --- | --- |
| **A** menu load-game | 1.30-1.67 s | once | loading pause; ~513 ms `LoadLevel`, rest of `World::Load` uninstrumented |
| **A2** loading screen | 8.8-14.4 s | once, straight after A | never measured as a unit; the largest number in the project |
| **A3** first world frame | 0.58-3.81 s | once, ends the loading screen | §34's "class B". Creates the whole scene: ~1,780 draws, ~1,300 buffers |
| **B** the in-play stutter | **0.29-0.44 s** | play + ~3,300 frames, **19/19** | **the real target.** 42-69% main-thread `LoadResource`; 29-54% in the D3D11 path |
| **C** the five-second stall | ~5.007 s | in the **loading screen**, 2/19 | a 5-second timeout; the main thread's wait is outside `Engine.dll`'s IAT |
| **D** the message pump | ~0.3-1.5 s | scattered, and at exit | closed as a host question, §17 |

### And what it costs the plan

**4.3 (libdeflate) shrinks.** §34 re-founded it on 106-366 ms of inflate inside
the main-thread load. On the real in-play frame the whole archive inflate,
across all threads, is **56.5-86.6 ms**, median 71.9. At 2-3x that is 35-50 ms
off a 340 ms frame, against a rewrite of the decompressor. It is still the
largest lever on class A3 and on the loading screen, and those are loading
pauses.

**`timeBeginPeriod` shrinks to nothing on class B.** Main-thread `Sleep` is
0.0 ms on all nineteen in-play stutter frames.

**The 87-230 ms inside `Engine::Render` is now the largest unattributed thing
that happens while the player is playing**, and it is the cheapest thing left
to measure. Run 34 measures it.

## 36. Run 34: the residual is the game's own `DrawIndexed`, and it is 13-15x

> **Read §37 before acting on this section.** The measurement here holds --
> `draw_submit` really is 31% of the stutter frame and 92.6% of the frame after
> -- but the *attribution* below, that it is a per-draw first-use cost in the
> driver, is **withdrawn**. Run 35's control and run 34's own `gpu_frame`
> column show it is GPU backpressure surfacing inside `DrawIndexed`, and that
> the GPU work is the mod's enhanced shadows and grass.

`draw_timing=1`, 6,892 frames, 97.3 s, same route: 1,948 menu frames, a
951-frame loading screen, 3,993 frames of play -- all three inside the bands
runs 14-33 set, so this run is comparable to them.

### First, the instrument is affordable

In-play p50 **13.7 ms**, against 13.5-14.3 ms across runs 14-33. The clock
pairs on 1,500-2,700 calls a frame do not move the median out of the band, so
the numbers below stand as measured and the conditional-arming fallback is not
needed.

### The residual was `DrawIndexed`, and there is essentially nothing left

Frame 6285, world+3,386 -- the in-play stutter, in the 3,245-3,527 band:

| | ms | of `Engine::Render` |
| --- | ---: | ---: |
| `Engine::Render` | 404.7 | 100% |
| `engine_res_load_main_us` | 256.8 | 63.5% |
| **`draw_submit_ms`** | **127.1** | **31.4%** |
| `map_resource_ms` | 1.7 | 0.4% |
| everything else named | 8.9 | 2.2% |
| **still unaccounted** | **10.1** | **2.5%** |

**The 87-230 ms (29-54%) §35 could not account for is 10 ms (2.5%).** And the
frame after is the cleaner statement of it, because it loads nothing at all:

| frame 6286 | ms | of `Engine::Render` |
| --- | ---: | ---: |
| `Engine::Render` | 154.8 | 100% |
| `engine_res_load_main_us` | **0.0** | 0% |
| **`draw_submit_ms`** | **143.4** | **92.6%** |

§35 predicted a flat ~129 ms on N+1 that correlated with nothing. It is
143.4 ms of the game's own `DrawIndexed` calls.

### `Map` is innocent, and that kills half the hypothesis outright

`map_resource_ms` is **1.7 ms** on the stutter frame and 1.6 ms on the frame
after, against 2,217 and 1,983 `Map` calls. Session-wide it is 1,549 ms against
`draw_submit`'s 25,115. The 2,200-2,750 dynamic-buffer maps a frame that §35
listed as a candidate cost nothing. `hookUnmap`'s standing comment -- that a
clock pair on these would cost more than the work it measured -- was right
about `Map` and wrong about the draws.

### It is a per-draw first-use cost, it is 13-15x, and it lasts exactly two frames

Steady state over the 120 full-scene frames after the event: `Engine::Render`
17.1 ms, `draw_submit` 9.8 ms (57% of render), 1,608 draws -- **6.1 us a
draw**. That 57% is not pathological; it is what a 1,600-draw-call frame costs.

| frame | per draw | vs steady | `buffer_create` | `texture_create` |
| --- | ---: | ---: | ---: | ---: |
| 6284 | 0.6 us | 0.1x | 0 | 0 |
| **6285** | **79.6 us** | **13.1x** | 162 | 79 |
| **6286** | **90.7 us** | **14.9x** | **7** | **0** |
| 6287 | 3.4 us | 0.6x | 16 | 0 |
| 6288 | 5.2 us | 0.9x | 24 | 0 |

**Two frames, no decay, straight back to normal.** And the highest per-draw
cost is on the frame that creates almost nothing -- 7 buffers, 0 textures. So
the cost is not creation and it is not proportional to what is created on the
frame: **it is charged to the first draws that *use* what the previous frame
created**, and it takes two frames to work through. Total excess over steady
state across N..N+2 is **247 ms**.

The first world frame, class A3, is the same animal one size up: frame 2899,
825.6 ms, `Engine::Render` 748.5 -- `res_load_main` 418.1 (55.9%),
`draw_submit` **175.3 (23.4%)**, after creating 1,297 buffers and 98 textures.

### What this does and does not say about the layer below

`draw_submit` brackets `g_drawIndexed` and nothing else, so the 127 and
143 ms are **inside the D3D11 call**, not in the game's code between calls.
That was one of §35's two candidates and it is the one that held.

**It does not make this a CrossOver problem.** The reporter's point stands and
this measurement is consistent with it: D3D11 defers state validation and
pipeline construction to draw time on *every* implementation, native Windows
included, so "the first draws after a batch of new resources are 13-15x" is
exactly the shape the well-known Titan Quest new-area hitch would have on
native hardware too. DXMT may make the constant worse. Nothing here shows that
it invented the effect, and §35 should not have implied it.

### So what is actually left, and the one question worth a run

The in-play stutter is now fully attributed:

| | frame 6285 | frame 6286 |
| --- | ---: | ---: |
| the game loading resources synchronously | 256.8 ms | 0 |
| the game's first draws with them | 127.1 ms | 143.4 ms |
| unaccounted | 10.1 ms | ~0 |

Neither half is the mod's. **The one thing that could be** is whether the
mod's own state changes multiply the number of distinct pipeline states the
driver has to build on first use: `enhanceShadowPcf` rewrites pixel shaders at
creation, `bindRegionalCompositeShader` swaps a pixel shader on some draws, and
the shadow-map scaling changes texture dimensions. More distinct states means
more first-use work on exactly these two frames.

That is one control run, and it is the discipline the README already states for
`Map` and `Unmap`: **price it by turning the feature off in the INI.** Same
route with the visual enhancements off, and compare the 247 ms excess. If it
does not move, the mod contributes nothing to this and the in-play stutter is
the game's, end to end -- which would close it the way §17 closed the pump, but
with an attribution rather than a shrug.

## 37. Run 35: the control lands, and §36's reading of `draw_submit` was wrong

Same route with the visual enhancements off. 8,986 frames, 95.3 s, 1,976 menu,
a 1,010-frame loading screen, 6,000 frames of play.

**One defect in the control, and it is mine.** The ini set `aa=off`, but
`visual.cpp:328` reads `g_options.smaa = _wcsicmp(value, L"fxaa") != 0` -- only
the literal `fxaa` disables it. **SMAA stayed on for run 35.** What the run
actually turned off is enhanced shadows, enhanced grass, enhanced bloom, and
the FP16/AgX HDR path. The conclusions below are about those four.

### The mod's visual work owns essentially all of `draw_submit`

| | run 34, all on | run 35, four off |
| --- | ---: | ---: |
| in-play p50 | 13.7 ms | **9.1 ms** |
| steady in-play frame | 25.4 ms | **10.9 ms** |
| steady `Engine::Render` | 20.3 ms | 5.6 ms |
| steady `draw_submit` | 11.9 ms | **0.3 ms** |
| steady us per draw | 5.7 | **0.2** |
| stutter frame `draw_submit` | 127.1 ms | **0.4 ms** |
| N+1 `draw_submit` | 143.4 ms | **0.2 ms** |
| excess over N..N+2 | 247 ms | **0 ms** |

### But it is not a first-use cost. It is GPU backpressure.

§36 read the 13-15x per-draw spike as the driver doing first-use pipeline work.
The control says otherwise and so does run 34's own GPU column, which was in
the same rows:

| run 34, steady in-play (median of 1,051 frames) | |
| --- | ---: |
| CPU frame | 25.39 ms |
| **`gpu_frame_ms`** | **25.39 ms** |
| `present_call_ms` | **0.0 ms** |

**The GPU frame time equals the CPU frame time exactly, and the frame does not
wait in Present.** So the backpressure surfaces where the driver applies it --
inside the draw calls. `r(draw_submit, gpu_frame)` is **+0.51** in run 34 and
**+0.08** in run 35, where the GPU is no longer the limit.

The two-frame shape follows without any appeal to pipeline compilation:

| frame | CPU | `draw_submit` | `gpu_frame` | `gpu_shadow_dir` |
| --- | ---: | ---: | ---: | ---: |
| 6285 | 421.2 ms | 127.1 ms | **563.5 ms** | **351.6 ms** |
| 6286 | 181.9 ms | **143.4 ms** | 20.6 ms | 6.6 ms |
| 6287 | 22.9 ms | 5.5 ms | 23.4 ms | 7.5 ms |

Frame 6285 submits **563 ms of GPU work**, of which **351.6 ms is the enhanced
directional shadow pass** re-rendering a newly visible outdoor scene into its
cascades at `shadow_map_scale=4`. Frame 6286 then blocks 143 ms inside
`DrawIndexed` draining that backlog while its *own* GPU span is 20.6 ms -- which
is exactly why §35 found N+1's cost correlated with nothing on N+1. It was
never about N+1. It was the previous frame's queue.

**So §36's "13-15x per-draw first-use cost" is withdrawn.** The measurement was
right, the attribution was not: `draw_submit` is where a GPU-bound frame waits,
not where a pipeline gets built. The instrument earned its keep anyway -- it is
what made the residual visible, and the control is what corrected it.

### Where the mod's GPU time goes

Median over 1,051 steady full-scene in-play frames, run 34:

| | ms | of the 25.39 ms GPU frame |
| --- | ---: | ---: |
| **`gpu_shadow_dir`** | **8.09** | **32%** |
| **`gpu_grass`** | **4.47** | **18%** |
| `gpu_smaa` | 0.74 | 3% |
| `gpu_bloom` | 0.37 | 1% |

With those off the GPU frame is 11.25 ms and `gpu_shadow_dir` is 0.00. **The
enhanced shadows and grass roughly double the GPU frame at 5120x1440 and turn
a new-area transition into a 421 ms frame followed by a 182 ms one.**

That is the reporter's stutter, or a large part of it, and it is the mod's --
which is the first time in this project that has been true of anything.

### And what remains once they are off, because the reporter still feels it

The reporter ran run 35 and reported stutters remaining. They are right, and
they are a different class. Worst in-play frames in run 35, excluding the first
world frame:

```
frame 5303   234.8 ms   Engine::Render 3.2 ms   Engine::Update 2.2 ms
   loop_pump_us        223,614 us      <-- 95% of the frame
   pump_peek_us        221,250 us      in THREE PeekMessageA calls
   engine_sleep_us     453,308 us      196 calls, background threads
   draw_submit             0.2 ms
```

Frames 5303, 5542, 6873, 7921, 8578 and 7557 are all this shape: 160-235 ms
with `Engine::Render` under 7 ms. **That is class D, the message pump, §13-§17**
-- and with the mod's GPU cost removed it is now the largest in-play stutter
left. It was closed as a host question in §17 on the grounds that the timer is
not the game's and the pump has no lever. That closure was made when the pump
was one class among three; it is worth reopening now that it is the last one.

The other survivor is the first world frame: 533.6 ms, `res_load_main` 336.4 ms,
`draw_submit` 0.9 ms. That is class A3, the game's own loading, unchanged.

### What to do

**`shadow_map_scale` is the lever and it is one line of INI.** It defaults to
4 (`visual.cpp:375`) and its own comment says 2 is the cheaper setting that
visibly softens the shadows. Shadow-map cost goes as the square of the scale,
so 4 -> 2 should take `gpu_shadow_dir` from 8.09 ms toward ~2 ms steady and cut
the 351.6 ms transition spike hard, while keeping enhanced shadows. That is run
36, single variable, everything else at the reporter's normal settings.

## 38. Run 36: halving the shadow map barely helps, so the pass is not fill-bound

`shadow_map_scale` 4 -> 2, single variable, everything else the reporter's
normal configuration. 4,013 in-play frames against run 34's 3,993, so the runs
are directly comparable.

| steady in-play, median | run 34 scale=4 | **run 36 scale=2** | run 35 visuals off |
| --- | ---: | ---: | ---: |
| frame | 25.4 ms | **24.5 ms** | 10.9 ms |
| `gpu_frame` | 25.39 | **24.36** | 11.25 |
| **`gpu_shadow_dir`** | 8.09 | **5.06** | 0.00 |
| `gpu_grass` | 4.47 | 4.43 | 0.00 |
| `draw_submit` | 11.9 | 10.6 | 0.3 |
| in-play p50 | 13.7 | **12.4** | 9.1 |

| the outdoor transition | run 34 | **run 36** |
| --- | ---: | ---: |
| frame N | 421.2 ms | **350.4 ms** |
| its `gpu_frame` | 563.5 | 453.5 |
| its `gpu_shadow_dir` | 351.6 | **243.2** |
| frame N+1 | 181.9 ms | 144.2 ms |

**Quartering the shadow-map area cut the directional pass by 37%, not 75%.**
`gpu_grass` is unchanged at 4.43 against 4.47, which confirms the run really
did move one variable.

### What that rules out, and where it points

If the pass were fill-rate bound, halving each dimension would have taken 8.09
to about 2 ms. It took it to 5.06. **So the directional shadow pass is bound by
the geometry going into it, not by the resolution it rasterises at** -- the same
meshes are submitted to the same cascades whatever the map size, and only the
per-texel work shrank.

The geometry knob is `shadow_split`, and the mod already widens it:

```
shadow_fix.cpp:23   kNativeSplit  = 0.325f     what the game uses
shadow_fix.cpp:24   kDefaultSplit = 0.450f     what this mod uses
README:197          coverage scales as split^1.90
```

`(0.45 / 0.325)^1.90 = 1.86`. **The mod's directional shadow map covers 1.86x
the area the game's does, so it takes nearly double the geometry**, which is
exactly the quantity run 36 says the cost is proportional to. The
cross-references already suspected the widened split of enlarging the region
set (§2, item 3); this prices it.

That makes `shadow_split=0.325` the next single variable -- native coverage,
keeping the mod's filtering, stabilisation and map scale. Expect
`gpu_shadow_dir` toward 8.09 / 1.86 = ~4.3 ms at scale 4, on the same axis run
36 could not move, and the two levers multiply if both hold.

### What did not move, in any of the three runs

- **The first world frame**: 825.6 / 771.4 / 533.6 ms. The game's own resource
  loading, class A3.
- **The pump frames**: in-play frames over 100 ms are 10, 12 and 20 across runs
  34, 36 and 35 -- 0.25%, 0.30% and 0.33% of play. `Engine::Render` under 7 ms
  on all of them. Nothing on the GPU axis touches them, as expected, and they
  are still the class to reopen once the GPU cost is down.

## 39. Run 37: the split is the transition lever, and the two axes are independent

`shadow_split` 0.45 -> 0.325 (the game's native value), `shadow_map_scale` back
at its default 4. One variable from the live config.

### The number that matters is the transition, and it moved a long way

| N + N+1, the outdoor transition | |
| --- | ---: |
| run 34, stock | 421 + 182 = **603 ms** |
| run 36, `shadow_map_scale=2` | 350 + 144 = **495 ms** |
| **run 37, `shadow_split=0.325`** | 263 + 86 = **349 ms** |
| run 35, visual enhancements off | 118 + 24 = 142 ms |

`gpu_shadow_dir` on the transition frame: 351.6 -> 243.2 -> **131.8** -> 0.0.
**The split takes 62% off the shadow spike where halving the map took 31%**, and
`gpu_grass` held at 4.39 against 4.47 and 4.43, so the run moved one variable.

**42% off the stutter the reporter actually feels, from one line of INI.**

### Steady state improves, and the earlier p50 comparison was the wrong one

Whole in-play p50 read 13.7 / 12.4 / **14.4** ms across runs 34 / 36 / 37, which
looks like a regression and is an artefact: play alternates between full-scene
outdoor stretches and much cheaper indoor ones, and the proportion of each
varies with how the route is walked. Compared on the frames that are actually
comparable -- full-scene in-play frames under 60 ms:

| | run 34 | run 36 | run 37 |
| --- | ---: | ---: | ---: |
| frames | 1,051 | 1,085 | 1,102 |
| p25 | 22.1 ms | 20.9 | **20.4** |
| p50 | 25.4 ms | 24.5 | **24.1** |
| p75 | 27.8 ms | 27.7 | **27.2** |

Monotone improvement at every quartile. **Whole-session in-play p50 is not a
usable comparator between these runs and should not be quoted as one** -- the
same trap §34 found with menu frames, one level down.

### The first world frame did not regress; it loaded twice as much

Run 37's first world frame reads 1,370.5 ms against 825.6 and 771.4, which is
not the split:

| run | frame | `res_load_main` | `engine_arc_kib` |
| --- | ---: | ---: | ---: |
| 34 | 825.6 ms | 418.1 ms | 34,204 |
| 36 | 771.4 ms | 328.8 ms | 28,480 |
| **37** | **1,370.5 ms** | **792.4 ms** | **63,293** |
| 35 | 533.6 ms | 336.4 ms | 28,185 |

**It inflated 63 MiB where the others inflated 28-34.** That is class A3
streaming variance -- the same frame ranged 578-3,811 ms across runs 14-33 with
no setting changed at all -- and `gpu_shadow_dir` on it is 40.1 ms against run
34's 40.3, so the shadow pass is not what moved.

### The two axes are independent, so combine them

Split and scale act on different quantities, which is why each is weak where the
other is strong:

| | steady `gpu_shadow_dir` | transition spike |
| --- | ---: | ---: |
| stock | 8.09 ms | 351.6 ms |
| `scale=2` (quarter the texels) | **5.06 ms** | 243.2 ms |
| `split=0.325` (54% of the area) | 6.23 ms | **131.8 ms** |

Scale wins the steady-state number, split wins the transition by a mile.
Nothing couples them: one changes texel density, the other changes how much
geometry is in the frustum. Run 38 sets both -- expect steady `gpu_shadow_dir`
near 3.9 ms and a transition spike near 90 ms.

## 40. Run 39: the pump filter does nothing, and §17 was right

`pump_timer_min_ms=50` -- an unfiltered `PeekMessageA` allowed through at most
once every 50 ms, every other poll asking for everything below `WM_TIMER` and
then everything above it. The mod owns `TQ.exe`'s import slot, so this is the
one lever §17 never tried.

**The filter engaged exactly as designed**: 1,047 unfiltered peeks against
4,914 split ones over 65 seconds of play -- 16 a second, matching the 50 ms
floor, against a couple of hundred a second before. **82% of the peeks that
could synthesize a `WM_TIMER` were removed.**

**The stalls did not follow.**

| | stalls/min | ms/min | `pump_peek` % of play |
| --- | ---: | ---: | ---: |
| run 34 stock | 7.1 | 1,329 | 9.06% |
| run 36 `scale=2` | 10.1 | 1,713 | -- |
| **run 39 `pump_timer_min_ms=50`** | **11.1** | **1,734** | **9.83%** |
| run 37 `split=0.325` | 16.5 | 3,066 | 10.31% |
| run 35 visuals off | 20.9 | 3,211 | 18.33% |

11.1 a minute sits in the middle of a 7.1-20.9 spread that four runs produced
with no pump change at all. **The run-to-run variance is larger than the effect
being tested**, which is itself the most informative number here.

### And the premise the filter was built on is false

Two of the twelve stall frames settle it, and they were in the same rows:

```
frame 3721   176.5 ms   pump_peek 1   pump_peek_miss 1   pump_peek_miss_us 154,594
frame 5289   198.9 ms   pump_peek 1   pump_peek_miss 1   pump_peek_miss_us 185,129
```

**The peek that came back EMPTY cost 154 and 185 milliseconds.** Run 35's frame
5303 -- one empty poll at 1 us, two retrievals at 221,249 us -- was not the
rule, and §39's build was designed around it as if it were. Four of the twelve
stall frames took no unfiltered peek at all, so their stall happened on the
filtered path, **which cannot synthesize a `WM_TIMER`.**

So `PeekMessageA` sometimes simply blocks, whether or not it has anything to
return, and whether or not a timer is in range. `WM_TIMER` was a correlate --
§16 offered exactly that reading as one of its two, and this is the run that
chooses between them. It chose the other one.

### The pump is closed, for the second time and now on a test rather than an argument

§17 closed it on four converging lines of evidence and the judgement that the
mod "has nothing useful to put in its place, because the work is the round trip
itself". That judgement was about re-arming the timer. This run tried it on the
peek itself, which was the untested part, and the answer is the same. **Nothing
inside the process moves this.** It is 1.3-3.2 seconds of stall per minute of
play, 7-21 events a minute, 60-460 ms each, and it is the host.

`pump_timer_min_ms` stays in the build the way `async_level_load` does:
verified, inert, default `0`, its own counters, and correct if a use is ever
found. It is worth nothing on this install and the measurement that says so is
in the table above.

### What is actually true about this install, after seven runs

| what | size | whose | can the mod help? |
| --- | ---: | --- | --- |
| the message pump in play | **1.3-3.2 s per minute of play** | the host | **no.** Two closures, §17 and this one |
| the mod's GPU cost | doubles the GPU frame; 603 ms on the outdoor transition | **the mod's** | **yes**, and it is the one thing here that is ours |
| the game's synchronous resource load | 147-336 ms on the transition frame | the game | not without a rewrite |
| the loading screen and first world frame | 11.5-19.2 s once a session | the game | no |

**The only lever this project owns is the mod's own GPU cost**, and the honest
version of that is: `shadow_map_scale=2` takes the outdoor transition from
603 ms to 495 ms and the steady GPU frame from 25.4 to 24.4 ms without touching
shadow distance. `shadow_split` would be worth more and is not available -- it
exists to fix shadow distance, which is the feature.

The reporter has reported stutters after every one of these runs, and the
stalls that remain are the pump.

### And the host reading is probably wrong too

This section first ended by proposing a Windows comparison, on the theory that
`pump_peek_us` would read near zero there. **The reporter has since said that
Titan Quest stutters on Windows as well, without this mod at all**, and they
said the same thing earlier about the in-play hitch, where it was also right
and also corrected a section of mine (§35 -> §36).

That is testimony rather than a measurement in this dataset, but it is the only
evidence anyone has about the other platform, and it does not fit "CrossOver's
event path is the cause". Two readings survive it:

1. `PeekMessageA` blocking really is a Wine property, and it is a *different*
   thing from what the reporter feels -- in which case this project has spent
   four runs measuring an artefact of the host and calling it the stutter, and
   the felt stutter has never been isolated at all.
2. The pump blocking is where the game's own stall *surfaces* under Wine -- the
   main thread is waiting for something the game does, and `PeekMessage`
   happens to be the call it is inside -- in which case the cause is upstream
   of the pump and the pump columns have been a dead end pointing at a symptom.

**Nothing in the recorded data distinguishes these, and both make every
"whose time was it" conclusion in §14-§17 and §40 suspect.** That is the state
this line is actually in, and it is worth saying plainly rather than filing the
pump as closed for a second time: it is closed as a *lever*, twice tested, but
the attribution behind the closure no longer stands on the evidence available.

## 41. Run 40: the felt-stutter marker works, and its first implementation perturbed the run

This was the first run in the project that recorded a human observation rather
than selecting an event afterward with `max()`. The reporter pressed F12 after
felt stutters. All **15 markers** landed in play; none landed in the menu,
load-game frame, loading screen, or first world frame.

The reaction-time concern was real but not a problem. For the first ten clear
matches, the likely event ended 330-581 ms before the marker (apart from a
keypress that landed 7 ms after a 230 ms frame), and the event onset was
roughly 0.5-0.7 s before it. The marker is therefore an anchor for a backwards
window, not a label on the same row.

### What the presses appeared to select

| felt class in play | likely event frames | markers |
| --- | --- | ---: |
| `PeekMessageA` / pump | 2848, 4392, 4788, 5028 | 4 |
| outside every existing main-loop bracket | 3277, 3372, 3680, 4503, 4660, 5792, 6021, 6078, 6163/6175, 6417 | 10 |
| outdoor-transition render burst | 6035-6046 | 1 |

Two unmarked events after the final press -- frame 6517 in the pump and frame
6564 outside the brackets -- fit the reporter's immediate note that they had
probably missed one or two. That makes the observation method useful. It does
**not** make the apparent 10/4/1 class mix valid, because the instrument had an
observer effect.

### The marker probably created the new residual class

The first implementation called `GetAsyncKeyState(VK_F12)` once from
`probe::endFrame`. Run 40 then produced **12 play frames of at least 100 ms**
with `Engine::Render` under 60 ms and the pump under 10 ms. The same predicate
finds 0, 2, 0, 1 and 1 frames in runs 34, 35, 36, 37 and 39.

The frame clock makes the failure mode exact. `recordFrame()` samples `now`,
then calls `endFrame()`, where the extra input query ran, and only afterward
stores that earlier `now` as the next boundary. A slow `GetAsyncKeyState` call
is therefore charged to the following row while sitting outside every engine
and main-loop bracket. The only per-frame change in run 40 has exactly the
shape and location of the new stalls. Until a clean repeat removes them, this
is a strong causal inference rather than a completed control.

Run 40 is consequently **withdrawn as a baseline**. Its F12 rows remain valid
records of when the reporter reacted, and they show that pump stalls can be
felt, but most of its newly dominant residual stalls were probably made by the
marker itself.

### Corrected marker and the next run

The per-frame input poll is gone. The marker now recognizes
`WM_KEYDOWN`/`WM_SYSKEYDOWN` for F12 in the result of the game's existing
`PeekMessageA` import hook, ignores autorepeat, and adds no second Win32 input
call. With `engine_trace=0` it installs only that existing import wrapper; with
the performance probe off it installs nothing.

Run 41 repeats the observation, not a mitigation A/B. If the residual events
return to the 0-2/run baseline, it confirms the observer effect and the new
marker can answer which *normal* class is felt. Only after that is it worth
adding the sent-message instrument: `PeekMessageA` can run inter-thread
`SendMessage` handlers inline before it checks the posted queue, so an empty
154-185 ms peek still does not identify what consumed the time.

### What the old 3x pump rate means now

The 7.1-20.9/minute number is a one-minute tail count, not a stable rate.
Runs 34, 36, 37 and 39 spend a much narrower 5.5-6.4 seconds/minute in the
pump, while the count moves when a heavy-tailed sample crosses a hard 60 ms
line. Run 35 also raises exposure from about 111 to 150 peeks/second because
turning four visual features off raises frame rate; its 20.1 events of at
least 50 ms per minute are 22.4 per 10,000 peeks, versus 9.4 in run 34.
Exposure explains part, not all, of the range; the remaining arrivals are
bursty and the sample is short. This explains why the tail count cannot measure
a modest A/B. It does **not** explain what work runs inside a slow peek, and it
does not restore the host attribution withdrawn in §40.

## 42. Run 41: the passive marker is clean, and the frequent felt class is the pump

**Corrected forward by §45:** the passive key path is clean relative to run
40's polling query, but `performance_trace=full` was not clean. Its worker
opened and closed the CSV after nearly every emitted frame, creating
wineserver contention with the pump it was measuring. The F12 presses are
real reactions; the claim that their pump candidates describe the unobserved
game is withdrawn.

Same live configuration and the same F12 workflow as run 40, with one change:
the marker no longer calls `GetAsyncKeyState`. It recognizes F12 when the
game's existing `PeekMessageA` retrieves the key event.

### The observer effect is confirmed

| play predicate | runs 34, 35, 36, 37, 39 | run 40 polling marker | run 41 passive marker |
| --- | --- | ---: | ---: |
| frame >=100 ms, render <60 ms, pump <10 ms | 0, 2, 0, 1, 1 | **12** | **0** |

The new residual class vanished completely. The inference in §41 is now a
completed control: the per-frame `GetAsyncKeyState` query created those stalls,
and the event-based marker does not.

### What the reporter actually marked

Run 41 has 8 markers, all in the **play** part of the session. Seven follow
166-209 ms frames that spend 140-196 ms in `PeekMessageA`:

| event frame | frame | `pump_peek` | event onset to F12 |
| ---: | ---: | ---: | ---: |
| 3174 | 191.0 ms | 165.5 ms | 613 ms |
| 3428 | 166.2 ms | 139.7 ms | 760 ms |
| 3994 | 168.2 ms | 159.7 ms | 702 ms |
| 4241 | 202.5 ms | 194.0 ms | 698 ms |
| 4394 | 174.5 ms | 162.2 ms | 642 ms |
| 5209 | 209.4 ms | 195.7 ms | 554 ms |
| 5718 | 195.2 ms | 176.5 ms | 539 ms |

The eighth marker follows the outdoor-transition render burst: frame 6319 is
368.0 ms with 359.9 ms in `Engine::Render`, followed by a 144.4 ms render
frame. The marker comes 636 ms after that pair ends. This is class B from §35,
not the frequent scattered complaint.

There were plausible misses, as the reporter expected: pump frames 4161 and
4314, a mixed update/pump frame at 6508, and a 251.6 ms render frame at 6721
whose GPU grass span is 230.3 ms. The 984.9 ms maximum nominally inside the
tool's `play` tail is not ordinary play: on frame 6833 `game_collisions` drops
to zero, and all 147 remaining frames draw at most 47 indexed primitives. It
is the frame that leaves the world and has no marker.

### This settles relevance, not cause

The pump is now tied to what the reporter feels, rather than selected by a CSV
maximum: 7 of 8 captured reactions point to it. But `pump_peek_us` still wraps
two different things. `PeekMessageA` first dispatches pending nonqueued
messages sent to the thread and only then checks the posted queue. A 150 ms
call returning empty can therefore contain a 150 ms window procedure; the
return value says nothing about that hidden work.

Run 42 adds the missing split without changing pump behaviour. A
thread-specific `WH_CALLWNDPROC` / `WH_CALLWNDPROCRET` pair runs only while
the game's existing `PeekMessageA` call is on the stack. The CSV adds
`pump_sent_wndproc`, `pump_sent_wndproc_us`, and a one-time
`pump_sent_hook=1` installation proof. If `pump_sent_wndproc_us` owns the
marked pump frames, the cause is a synchronous sent-message handler on native
Windows and Wine alike. If it stays near zero, the time is below the window
procedure and the sent-message hypothesis is rejected.

## 43. Run 42: the felt pump stalls are not sent-message window procedures

**Corrected forward by §44:** the proposed per-peek `GetThreadTimes` split is
not a valid instrument under CrossOver. Its own server calls captured the
stalls and moved them out of `pump_peek_us`.

**Corrected forward again by §45:** the sent-window-procedure split still
rejects that handler as the owner of the observed time, but the observed pump
tail is contaminated by the full-trace CSV writer. It cannot be called the
normal game's felt class.

The installation proof is present exactly once and the two thread hooks saw
5,484 window-procedure entries, so zero cannot mean that the instrument failed
to arm. They measured 2,588.0 ms across the complete session and 1,791.5 ms in
the **play** part. Inline sent-message handling is real and common here.

It does not own the slow tail. Run 42 has 13 F12 markers, all in **play**.
Nine have a likely pump event in the same reaction-time window established by
run 41:

| event frame | frame | `pump_peek` | sent wndproc | event onset to F12 |
| ---: | ---: | ---: | ---: | ---: |
| 3663 | 170.8 ms | 164.3 ms | 2.1 ms | 515 ms |
| 3883 | 108.6 ms | 103.6 ms | 0.0 ms | 649 ms |
| 4427 | 201.9 ms | 189.4 ms | 2.6 ms | 516 ms |
| 4804 | 196.3 ms | 183.8 ms | 1.0 ms | 450 ms |
| 4886 | 141.8 ms | 130.0 ms | 2.1 ms | 726 ms |
| 5084 | 159.0 ms | 143.7 ms | 1.2 ms | 513 ms |
| 5192 | 160.3 ms | 146.3 ms | 0.0 ms | 541 ms |
| 5565 | 130.3 ms | 123.0 ms | 3.7 ms | 568 ms |
| 5902 | 146.4 ms | 138.6 ms | 1.8 ms | 712 ms |

Those nine frames spend 1,322.8 ms in `PeekMessageA` and only 14.4 ms, **1.1%**,
in sent window procedures. The maximum sent-handler span among them is 3.7 ms.
The other 98.9% is below those handlers. Curiously, sent handlers are 22.0% of
all play-part peek time, so the ordinary cheap path contains much more of this
work proportionally than the felt stalls do. The proposed explanation is
therefore rejected, not merely unobserved.

The other four markers anchor an early-play render/Present cluster, a 108 ms
frame outside the existing render and pump brackets, a 49 ms render candidate,
and the 423 + 196 ms outdoor-transition render pair. The last is class B from
§35. The 1,127 ms maximum in the nominal play tail is frame 6414, where
`game_collisions` becomes zero and the following frames draw only the menu/UI;
it is the world-exit transition, not ordinary play, and has no marker.

### The instrument did not recreate run 40

Run 42's **play** part makes 107.5 peeks/s and spends 7.46 s/min in them, close
to run 41's 111.5 peeks/s and 7.23 s/min. The run-40 residual predicate --
frame at least 100 ms, render below 60 ms and pump below 10 ms -- finds two
frames, inside the 0-2 range of runs 34-39 rather than run 40's twelve. The
thread hooks did not produce a new stall class visible at this resolution.

### What is actually known now

The frequent felt class is wall time accrued while the main thread is inside
`PeekMessageA`. It is not explicit `DispatchMessageA`, not an inline sent
window procedure, and run 39 proves it can occur on a call that returns empty
while this run proves it can dominate frames whose final empty poll is only
0-12 us. That is a stack location, not yet a cause. It still does not justify
calling the stall Wine, CrossOver, USER32, or the host; the same symptom is
reported on native Windows and no measurement here attributes the time to one
of those implementations.

The next split is active versus off-CPU time, not another pump mitigation.
Run 43 brackets each measured peek with `GetThreadTimes` and records the game
thread's user and kernel execution time. `pump_thread_sample` must equal
`pump_peek`, and `pump_thread_query_us` records the instrument's own overhead.
High wall time with comparable thread CPU means active work below the window
procedure hooks; high wall time with little thread CPU means the thread waited
or was descheduled while the API remained on its stack.

## 44. Run 43: the stall moves to any added CrossOver server call

**Corrected forward by §45:** the shared server call did not encounter an
unexplained external delay. Run 44 found a concrete source of server
contention inside this mod: the full-trace CSV writer opened and closed the
file after almost every frame.

Run 43 did not measure the planned active/off-CPU split. It found a more basic
instrumentation failure, with complete coverage: `pump_thread_sample` is
12,966 for exactly 12,966 peeks over the **complete session**. During the
**play** part it is 7,400 for 7,400. The API worked every time.

Its two timing queries around each peek cost 5,369.0 ms during 67.154 seconds
of **play**, or **4.80 seconds per minute**. Eleven play frames put at least
50 ms in `pump_thread_query_us`, including nine at 111-219 ms. At the same
time there were **zero** play frames with 50 ms in `pump_peek_us`; its total
fell to 3.02 seconds per minute. The long wall-time samples moved from the API
being measured to the API added to measure it.

Eight of the nine F12 markers in **play** line up with those query stalls. The
ninth lines up with the 372 + 154 ms outdoor-transition render pair. One event
makes the observer effect especially direct: the reporter reacted to a 218 ms
query stall at frame 4861; retrieving that F12 press at frame 4880 was followed
by another 187 ms query stall in the same frame; the next marker then follows
frame 4880. The diagnostic generated the event that generated the next mark.

This is the same failure shape as run 40's per-frame `GetAsyncKeyState`, but
run 43 names the time instead of leaving it residual. The CPU-time columns do
not answer the intended question: the expensive query is outside the interval
whose thread CPU it reports.

### What moved, and what did not

The exact CrossOver Preview 27.0.0.40921 binaries used by the live bottle were
checked after the run. `GetThreadTimes` calls
`NtQueryInformationThread(ThreadTimes)`, whose Unix implementation enters
`_wine_server_call`. `NtUserPeekMessage` also enters `_wine_server_call` while
retrieving messages. That shared boundary explains why adding the nominally
unrelated timing query can capture the same 100-200 ms wall-time class here.

It does **not** explain why Titan Quest stutters on native Windows. Therefore
the defensible conclusion is narrower: on this CrossOver run,
`pump_peek_us` named whichever synchronous server call happened to encounter
the delay, not work intrinsic to `PeekMessageA`. The CrossOver manifestation
and the reported Windows manifestation may share an upstream game event, or
may be different mechanisms with the same feel. This dataset cannot choose.
Calling the overall stutter a Wine bug would repeat the attribution error in
§40.

The per-peek CPU queries are removed. `QueryThreadCycleTime` is not a valid
substitute: it would add another synchronous OS query on the same hot path and
repeat the experiment under a different API name. Any next causal observation
must run outside the game thread. A macOS stack sample of the game process and
`wineserver-x86` during the clean passive-marker route is the least invasive
available next step; it can show whether the server is active or idle while
the client is stuck without inserting another server request into every frame.

## 45. Run 44: the full-trace writer was manufacturing the pump tail

Run 44 kept the passive F12 marker and removed both the invalid thread-time
queries and the completed sent-window-procedure hooks. It simultaneously ran
macOS `sample` against the actual CrossOver Preview 27.0.0.40921 game task and
the ARM64 wineserver that held this run's installed `winmm.dll`. The archived
CSV is `tqflicker-frames.run44.csv`, SHA-256
`f9ebfc24ad25ca3e63a8567e0e5773c2cfd740b9f54ee86e2809a03ffad3c90b`.

The five session parts are menu frames 0-16515 (163.163 s), load-game frame
16516 (1.831 s), loading screen frames 16517-17282 (10.290 s), first world
frame 17283 (0.917 s), and play frames 17284-21151 (65.900 s). All eight F12
markers are in play. Six have an unambiguous 98-209 ms pump candidate ending
362-574 ms before the reaction. One follows a 69.9 ms GPU-bound frame, itself
immediately after a 58.1 ms frame with 47.7 ms in the pump. The last follows
the 408 + 169 ms outdoor-transition render pair. This repeats the shape of
runs 41-42, but it does not validate their causal interpretation.

### The sampled thread is the probe's own writer

The aggregate reports cannot align an individual stack with an F12 marker;
`sample` condenses time order, and running two samplers was intrusive enough
that only 5,826 game samples and 8,544 server samples were collected during a
nominal 120 s at 10 ms. They nevertheless expose a structural bug that does
not depend on marker alignment:

- The game task's unnamed thread 4307584 spent 3,994 of 5,823 samples in
  `NtWaitForMultipleObjects`, then 1,726 in
  `cxcompatdb -> NtCreateFile -> wine_server_call`, with `NtWriteFile` and
  `NtClose` beside them.
- That is the exact control flow of `probe.cpp`'s logger: wait on its stop and
  flush events with a 250 ms timeout, call `CreateFileW(OPEN_ALWAYS,
  FILE_APPEND_DATA)`, `WriteFile`, and `CloseHandle`.
- `appendLogReserved` signalled the flush event after every appended row.
  Full mode emits every frame, so the nominally asynchronous writer executed
  an open/write/close cycle as fast as the render thread could produce rows.
- The matching wineserver spent 3,010 of 8,544 samples at the actual macOS
  `open()` inside its file-create request path. Its binary instruction at
  runtime `0x1002b5858` is a call to `_open`; the following sampled PC
  `0x1002b585c` stores its return value.
- The game main thread was sampled 369 times in
  `NtUserPeekMessage -> wine_server_call -> read`. Run 43 already showed that
  the long delay moves to whichever main-thread server call is added around
  Peek. The logger supplies the missing competing request.

The strongest reading is therefore that full tracing created or amplified
the CrossOver pump tail by queueing the main thread's final empty Peek behind
the probe worker's repeated file opens. This explains why a call returning no
message can be slow without a window procedure, and why run 43 moved the
delay into `GetThreadTimes`. It also withdraws the conclusion that runs 41-44
isolated Titan Quest's normal stutter. They isolated a stutter the reporter
really felt while the broken observer was active.

It does **not** explain the reporter's native-Windows stutter, and it is not a
claim that the uninstrumented game is smooth. That stutter remains
unisolated. The CrossOver-only queueing mechanism is an observer defect, not
the game's root cause.

### The threefold rate was exposure plus small, clustered counts

Recomputing only collision-active full-scene play, runs 34, 35, 36, 37, 39,
41, 42 and 44 contain respectively 7, 22, 11, 15, 9, 10, 12 and 7 frames with
at least 50 ms in Peek, out of 7,202, 9,590, 6,922, 6,562, 5,770, 7,299,
6,836 and 6,780 peeks. A common per-peek probability fits those counts:
16.33 events per 10,000 peeks, chi-squared 8.10 for seven degrees of freedom.
No run is more than 1.60 expected standard deviations from that model.

Run 35 raised two exposures at once. Restricting the comparison to
collision-active full-scene frames below 60 ms, visuals-off run 35 produced
96.2 logged frames/s versus run 34's 61.4, because full tracing writes one row
per frame; it also made 157.1 peeks/s versus 115.2. The remaining arrivals are
clustered along the route: ten-second bucket variance is 1.74 times the mean
across the eight runs. Multiplying different exposure by ordinary fluctuation
in only 7-22 threshold crossings produces the apparent 3x per-minute range.
It was never a stable effect estimate.

This is now a causal explanation for why the measured pump rate is unstable,
not an explanation of the uninstrumented game's stutter. The decisive control
is run 45: retain one CSV handle for the session, flush ordinary rows in 250 ms
batches, run the passive-marker route without an external sampler, and see
what the reporter marks when this source of wineserver contention is gone.

## 46. Run 45: removing the writer defect removes the felt micro-stutter class

Run 45 is the control specified in §45: normal graphics and performance
settings, passive F12 marker, full tracing through one retained CSV handle and
250 ms batches, and no external sampler. The archived CSV is
`tqflicker-frames.run45.csv`, SHA-256
`b1f7f67b00797386713e92cd8bc521ddf44cd6c0c57f05134191754672595d33`.
It contains every frame from 0 through 7331 with no dropped rows.

The reporter's judgement arrived before the CSV was read: **the frequent
micro-stutters felt gone, leaving only the loading burst-stutter**. That
qualitative result is the primary observation. The trace independently has
the same split.

The five session parts are menu frames 0-1874 (17.670 s), load-game frame 1875
(1.542 s), loading screen frames 1876-2967 (9.604 s), first world frame 2968
(0.721 s), and play frames 2969-7331 (66.345 s). Both F12 markers are in
**play** and follow the outdoor-transition render/resource/GPU burst:

- Marker 6737 follows frames 6713 (260.559 ms), 6714 (131.997 ms), and 6722
  (46.290 ms). Frame 6713 spends 251.922 ms in `Engine::Render`, including
  164.431 ms of main-thread resource loads and 67.623 ms of archive inflate;
  the resolved GPU interval is 353.809 ms, including 236.973 ms of directional
  shadows. The next frame spends another 115.604 ms in render.
- Marker 6851 follows frames 6809 (96.926 ms) and 6810 (73.736 ms). Frame 6809
  spends 86.098 ms in render, including 71.725 ms of main-thread resource
  loads and 48.810 ms of archive inflate; its resolved GPU interval is
  146.165 ms, including 125.352 ms of directional shadows. The next frame
  spends another 61.533 ms in render.

Those components overlap rather than add, and F12 records a delayed reaction,
so neither press identifies one exact causal frame. It does identify the felt
class: both presses follow a multi-frame outdoor loading/render burst, and
neither follows a pump stall.

The corrected writer removes the old slow-Peek population completely. There
is no frame with 50 ms in `PeekMessageA` anywhere in run 45. In the 2,475
collision-active full-scene play frames there are 4,994 Peek calls; the
largest aggregate Peek time in any such frame is 3.671 ms. The common-rate
model from §45 predicts 8.2 crossings of 50 ms in that exposure. The old eight
runs contained 7-22; run 45 contains zero. Even outside the comparison set,
the largest Peek aggregate is 40.701 ms in the **loading screen** and 11.832
ms in **play**.

This validates §45's causal diagnosis of the measured CrossOver pump tail:
the full-trace writer was manufacturing it by contending through wineserver,
and the user's reported micro-stutters were real reactions to that observer
defect. It also validates the retained-handle/batched-writer fix as necessary
measurement infrastructure. It still does **not** explain or claim to fix the
historical native-Windows stutter. On the present CrossOver route, however,
the only felt residue run 45 isolates is the already-known outdoor-transition
class: synchronous game resource work plus a simultaneous enhanced-shadow GPU
burst. Do not run another message-pump A/B unless that class reappears with the
corrected writer.

## 47. Run 46 design: measure the transition shadow call before scheduling it

The remaining felt class after run 45 is the **play outdoor-transition
burst**, not the menu, load-game frame, loading screen, first world frame, or
the removed observer-induced pump population. Its two F12 reactions follow
frames that contain both 71.7-164.4 ms of main-thread resource loading and
125.4-237.0 ms of directional-shadow GPU time. Those intervals overlap; the
existing columns do not establish that one causes the other or even that the
resource calls occur inside the shadow build.

A fresh read of the pinned Engine.dll rules out a naive cascade scheduler.
Titan Quest DX11 builds **one global directional shadow map**, fitting one
projection and rendering its caster scene every frame. There are no cascades
to rotate. Skipping only the inner draw would update the receiver matrix while
leaving the previous map behind, which is an invalid map/matrix pair. Skipping
the whole build could preserve the pair, but doing that on an inferred slow
frame would repeat the project's pattern of implementing an unpriced
mechanism.

Run 46 therefore adds attribution only. At RVA `0x1644bc`, the deferred
renderer directly calls the exported
`GraphicsShadowMapDx11::RenderDirectional` at RVA `0x18db80`. A
`detour::patchCall` wrapper changes only that E8's four-byte displacement after
verifying a 23-byte argument/call window. A separate 24-byte constructor
window at RVA `0x18d427` proves that the call object's region argument is
stored at `this+0x6c` before the wrapper reads it.

Five game-time columns are added:

- `engine_shadow_render` / `_us`: the whole directional build at this call;
- `engine_shadow_region_change`: a non-null region pointer differing from the
  previous call;
- `engine_shadow_res_load` / `_us`: main-thread `ResourceLoader::LoadResource`
  calls nested inside that directional build. This pair is a subset of
  `engine_res_load_main` / `_us`, not an amount to add to it.

The new engine group is `16384`; the default `engine_trace=1` selects it along
with the existing resource-load hook needed for the nested pair. It installs
only while the performance probe is active and changes no game behaviour.
`shadow_split`, the map size, the projection, and the render cadence are all
untouched. `verify-sites.py` now performs 149 checks, including re-deriving
the call destination, export identity, call offset, verification lengths, and
constructor field operand. Perturbing each new RVA, offset, byte table, or
export name makes the verifier fail.

The run is intentionally not an A/B. If the play transition's synchronous
resource time is nested in this shadow call and a region change precedes it,
that change is a grounded candidate for a one-frame whole-map reuse test. If
either condition is false, this route does not justify a shadow scheduler.

## 48. Run 46: the felt play burst loads resources inside the shadow build

Run 46 is archived as `tqflicker-frames.run46.csv`, SHA-256
`bf1c5aff030bc11e2ac0a7f3055c138bccef378f021bdcb7b3ae71faa272641b`, with
all 7,662 frames present. The five parts are menu frames 0-2169 (19.770 s),
load-game frame 2170 (1.410 s), loading screen frames 2171-3265 (9.508 s),
first world frame 3266 (0.843 s), and play frames 3267-7661 (66.866 s).

The one F12 press is in **play**, at frame 6970. Its reaction window contains
the same outdoor-transition sequence as run 45: frames 6939 (458.033 ms),
6940 (150.554 ms), and 6942 (40.190 ms). Frame 6939 is collision-active and
full-scene (`draw_indexed=1559`), so this is not the later exit transition.

The new bracket answers the question directly:

| frame 6939, play outdoor transition | time |
| --- | ---: |
| whole frame | 458.033 ms |
| `Engine::Render` | 441.658 ms |
| `GraphicsShadowMapDx11::RenderDirectional` CPU | **170.532 ms** |
| main-thread resource loads nested inside it | **167.799 ms** |
| all main-thread resource loads | 209.717 ms |
| directional-shadow GPU interval | **273.815 ms** |
| whole GPU interval | 564.960 ms |

Thus 98.4% of the shadow call's CPU duration is its 57 synchronous
`ResourceLoader::LoadResource` calls, and they are 80.0% of all main-thread
resource-load time on the frame. The CPU and GPU intervals overlap and must
not be added. This is nevertheless a causal localization: the game is loading
resources *inside the directional-shadow build* while that same build submits
the large caster pass. It is not a message-pump stall and not an unexplained
host pause.

The region pointer changes on frame 6939, before the wrapper invokes
`RenderDirectional`. That is early enough to choose reuse before the 167.8 ms
of loading or the 273.8 ms GPU pass begins. The trigger is not merely another
name for a slow frame: five other region changes in play cost 10.5-33.4 ms.
It identifies a transition, not its magnitude.

The burst is wider than one frame. Frame 6940 spends 121.473 ms in
`Engine::Render` but only 2.834 ms in the shadow call and no main-thread
resource load; this is compatible with the prior frame's GPU backlog, although
run 46 had draw timing disabled and does not re-prove that attribution.
Frame 6981, after the F12 press, is another 86.185 ms full-scene play frame:
66.990 of its 68.524 shadow CPU milliseconds are nested loads and its
directional GPU interval is 92.827 ms, but there is no region change. A
one-frame transition reuse can target the marked onset; it cannot be claimed
in advance to eliminate every later shadow-resource burst.

Across all play, 479.340 of 653.361 ms of main-thread resource loading occurs
inside the directional build. No play frame reaches 50 ms in the pump; its
maximum is 3.059 ms (Peek maximum 3.025 ms). The only 50 ms pump event is at
frame 0 in the **menu**, not in the felt play class. This is a second clean run
after the writer fix and another reason not to reopen the pump route.

The next A/B may now be specific: on a non-null region change with a valid
previous directional result, reuse the previous **whole depth map and its
matching 4x4 matrix for one frame**, then render normally. Do not skip only the
draw, do not alter `shadow_split`, and do not promise more than moving the
marked transition onset. The caller stores the output matrix in the same
directional light record whose `+0x14` target is the one global map; the fix
must explicitly cache and restore all 64 matrix bytes rather than assume that
record survived a frame.

## 49. Run 47 design: one-frame whole map/matrix reuse

Run 47 implements exactly the A/B §48 supports behind
`[performance] shadow_transition_reuse=1`, default `0`. It changes no split,
map size, shader, caster rule, or steady-state cadence. On a non-null change of
the constructor-verified `GraphicsShadowMapDx11+0x6c` region pointer, and only
when the render target matches the last successful directional call, it:

1. leaves the one global directional depth texture untouched;
2. copies the prior successful call's complete 64-byte world-to-shadow matrix
   into the current light record;
3. returns the prior successful result without invoking `RenderDirectional`;
4. forces the following call through normally, even if the region pointer
   changes again.

That is reuse of one matched pair, not skipping the inner draw after a new
matrix has been computed. The matrix size is re-derived from two new binary
windows: the seventh argument is saved from `[ebp+0x1c]`, and the function
later writes `16` dwords to it with `rep movsd`. `verify-sites.py` now performs
154 checks. Perturbing either new RVA, either byte table, or the 16-dword
constant makes it fail.

The switch is a fix rather than an instrument. It reaches `install()` with the
performance probe off, executes no probe clocks or counters in that mode, and
brings no trace group. With group `16384` active,
`engine_shadow_reuse` identifies each skipped build. The self-test covers the
default-off and trace-off gates, full 64-byte restoration, target identity,
and the no-consecutive-skip rule.

The expected result is deliberately narrow. Run 46's region-changing marked
onset is eligible; its later frame 6981 is not. Run 47 must be judged on the
collision-active full-scene **play** transition and F12 observation, plus
whether any one-frame stale-shadow flash is visible. No cross-run session or
in-play p50 is relevant.

## 50. Run 47: one-frame reuse defers the work and visibly flickers

Run 47 is archived as `tqflicker-frames.run47.csv`, SHA-256
`f48562aaedc130722bc98648e3adc0a2e575a0b5f099cc7b7d969db51bdf0a12`.
It contains 7,105 contiguous rows. The five session parts are: **menu** frames
0-1686 (15.947 s), **load-game frame** 1687 (1.542 s), **loading screen**
1688-2788 (10.142 s), **first world frame** 2789 (751.599 ms), and **play**
frames 2790-7104 (65.645 s).

Before the CSV was read, the reporter observed three whole-scene flickers:
the first few near the beginning, and the last approximately 3-4 seconds after
the felt stutter. That observation rejects the fix independently of its timing
result. The switch fired seven times: once on the **first world frame** 2789,
and in **play** on frames 3092, 3208, 4223, 6442, 6690, and 7024. Frame 7024
is after collision activity ended during the exit portion of play. The last
active-play reuse, frame 6690, begins 4.822 s after the F12 marker and is the
strongest match for the reported last flicker. There is no flicker marker, so
the individual visual events cannot be assigned more precisely, but the
timing and the exact count of visible discontinuities establish that stale
whole-map reuse is not visually safe.

The one F12 marker is frame 6468 in **play**. Its reaction window contains the
collision-active outdoor-transition onset at frame 6442. The switch worked as
implemented there: `engine_shadow_reuse=1`, zero shadow CPU time, and zero
directional-shadow GPU time. That frame nevertheless lasts 148.733 ms, with
142.103 ms in `Engine::Render` and 48.482 ms in 13 main-thread resource loads
outside the skipped shadow call. On the immediately following **play** frame
6443, the forced normal call lasts 119.249 ms, including 117.641 ms in 58
synchronous resource loads, beside a 217.272 ms directional GPU interval; the
whole frame lasts 146.314 ms. Frame 6444 then lasts 122.691 ms. The three-frame
burst is 417.738 ms. Run 46's corresponding first three frames totaled
631.550 ms, but their resource and GPU workloads differ, so that is not a
controlled magnitude claim. The supported conclusion is narrower: the felt
burst remained, and the work skipped on the trigger was paid on the next
frame.

The trigger is also incomplete. Later **play** frame 6511 has no region change
but spends 67.014 ms in the directional build, of which 65.528 ms is 23
synchronous loads, beside a 116.768 ms directional GPU interval. This repeats
run 46's later non-region shadow burst. A longer fixed reuse interval would
both prolong the now-observed stale-shadow discontinuity and still pay the
build when the interval expires. Do not test that variation.

The message pump remains unrelated to this class. The maximum `PeekMessageA`
time on any collision-active **play** frame is 8.124 ms; on frames 6442-6444
it is 0.748, 0.753, and 0.077 ms. Do not reopen the pump route.

`shadow_transition_reuse` therefore stays default-off and is rejected as a
fix. The binary-site verification and default-off implementation remain useful
as the exact experiment record, but it must not be enabled in the playable
configuration. A further shadow fix needs to make the caster resources ready
before the visible transition or genuinely distribute the build without
showing a stale map; merely postponing the same call is not such a fix.

## 51. Run 48 design: shadow-resource lifecycle, before choosing a fix

Run 47 makes the architectural question narrower, but does not answer it. On
the deferred **play** build, all 117.641 ms of main-thread resource loading is
inside `RenderDirectional`. That can still mean either that the loader worker
had already started those resources and the render thread joined unfinished
work, or that shadow caster traversal discovered unloaded resources and forced
their first load synchronously. The first supports resource priority/readiness
work; the second supports earlier caster discovery or omitting only unavailable
casters. Guessing between them would repeat the error this project has made.

Run 48 adds a raw, per-frame partition of every main-thread
`ResourceLoader::LoadResource` call nested inside the directional build. It
samples the resource immediately before calling the original function:

- `engine_shadow_res_state0` / `_us`: loaded state 0 and its complete call
  duration;
- `engine_shadow_res_state1` / `_us`: loaded state 1 and its duration;
- `engine_shadow_res_state2` / `_us`: loaded state 2, retained as a
  race/assumption check;
- `engine_shadow_res_state_other` / `_us`: every other raw value;
- `engine_shadow_res_in_queue` / `_us`: queue-link non-null and the same call
  duration, deliberately overlapping the state partition.

The meanings come from the engine rather than names invented for the columns.
`Resource::EnsureAvailable` calls `LoadResource` when state is not 2;
`LoadResource` waits on the resource lock when state is 1; and exported
`Resource::GetLoadedState` is exactly `mov eax,[ecx+0x30]; ret`. Exported
`Resource::GetInLoadingQueue` tests `[ecx+0x60]` against null. Two new 16-byte
windows and both exact exports are checked at runtime before either field is
read. `verify-sites.py` performs 162 checks after adding the same assertions.

This adds no new detour or call-site patch. The existing verified
`LoadResource` detour reads two fields only for the small population that is
both on the main thread and already inside the existing directional-shadow
bracket. Every engine duration remains `_us`; no game time is charged to the
mod. The switch rejected by run 47 is absent from the run INI, so no shadow or
resource behaviour changes.

`cache/runs/run48-shadow-resource-lifecycle.ini` differs from
`live-config.ini` only by `performance_trace=full` and the passive F12 marker.
Judge the marked collision-active full-scene **play** transition. State 1 and
the queue flag owning the time would support option 2, loader scheduling or
priority. State 0 owning it would support option 3 or shadow-specific early
residency. Neither result by itself authorizes a fix; it chooses which boundary
to reverse-engineer next.

## 52. Run 48: shadow traversal demands cold resources; the queue has none

Run 48 is archived as `tqflicker-frames.run48.csv`, SHA-256
`1c3a51470c485f21eb44fd18844d8cdf598ddf0435929051285eff9f3279609b`.
It contains 7,474 contiguous rows. The five session parts are: **menu** frames
0-1906 (17.885 s), **load-game frame** 1907 (1.536 s), **loading screen**
1908-3090 (10.434 s), **first world frame** 3091 (1.438 s), and **play**
frames 3092-7473 (67.253 s).

The lifecycle partition is exact, not a majority result. Across **play**, all
214 directional-shadow resource loads, totaling 451.588 ms, enter
`ResourceLoader::LoadResource` in raw loaded state 0. State 1, state 2,
state-other, and the non-null queue-link count are all zero. The state buckets
equal `engine_shadow_res_load` in both count and microseconds on every row, so
there is no missing or misaligned population. The one shadow load in the
**menu** and the one on the **first world frame** are also state 0; neither the
**load-game frame** nor the **loading screen** has a directional-shadow load.

The one F12 marker is frame 6764 in **play**. Its reaction window contains the
collision-active full-scene outdoor-transition onset on frame 6728: 318.412 ms
for the frame, 298.020 ms in `Engine::Render`, and 151.041 ms in the
directional-shadow call. All 65 resource loads nested there and all 147.135 ms
of their duration are state 0, with no queue link. The same call has a 230.289
ms directional GPU interval. These intervals overlap and must not be added.
The following **play** frame 6729 is 97.419 ms with no resource load. Its Peek
times are 0.308 and 0.132 ms respectively, so the marked class is again not
the message pump.

The later non-region event is also cold demand. **Play** frame 6804 has 31
state-0 shadow loads totaling 74.212 ms and a 131.852 ms directional GPU
interval, with no state-1 or queued resource and no region change. Frame 6805
then lasts 78.457 ms. This is the same limitation seen in runs 46-47: a region
change is neither necessary for a shadow-resource burst nor a complete
scheduling trigger.

This rules out the narrow loader-priority hypothesis for the felt class: there
is no already-queued or already-loading work for the main thread to prioritize
or wait out. It does not rule out explicit earlier residency, but that fix
would first have to discover the shadow-only caster resources the stock queue
has not seen. The supported next boundary is therefore the call path above
`Resource::EnsureAvailable` inside `RenderDirectional`: identify the resource
types and the per-caster selection/draw site, then choose between explicitly
preloading those resources earlier and omitting only state-0 casters until
resident. No whole-map reuse, longer skip, `shadow_split` reduction, message
pump change, or general resource-loader rewrite follows from this result.

## 53. Run 49 design: identify the resource class and exact caster boundary

A fresh export from the pinned `Engine.dll` resolves the indirect part of the
directional-shadow path that §52 left open. During **play**, each
`RenderDirectional` call obtains entities from `Region::GetEntitiesInFrustum`
or `World::GetEntitiesInFrustum`, calls each entity's virtual `AddToScene`, and
builds a temporary `GraphicsShadowMapRenderer`. `Actor::AddToScene` hands its
`GraphicsMeshInstance` to `GraphicsSceneRenderer::AddRenderable`. The shadow
renderer then walks those renderables in `FUN_1018c870`; its first virtual call
asks for the number of shadow render passes, before shader selection and before
the 0x88-byte draw record is constructed.

For `GraphicsMeshInstance`, that virtual target is the exported
`GetNumShadowRenderPasses` at RVA `0x173440`. Its complete 24 bytes say: load
`this+4`, null-check it, put it in ECX, call exported
`Resource::EnsureAvailable` at RVA `0x2130f0`, then return `[mesh+0x7c]` as the
pass count. A null mesh returns zero. The later `RenderPass` starts with
`GraphicsMesh::GetMeshRenderInfo`, which calls `EnsureAvailable` again, but a
mesh loaded at the pass-count query is resident by then. This makes the
pass-count query an exact per-mesh eligibility boundary: an option-3 fix could
return zero only for a state-0 mesh, before that caster has a shader or draw
record. It would omit that individual caster's shadow temporarily, not reuse,
skip, or invalidate the whole directional map.

That static ordering does not yet prove that run 48's measured population is
mesh resources. Run 49 therefore makes two non-behavioural partitions:

- `engine_shadow_res_mesh` / `_us`, `shader`, `texture`, and `type_other`
  partition every main-thread `LoadResource` nested in the directional build
  by the suffix returned from the engine's own `Resource::GetFileName`;
- `engine_shadow_mesh_cold` / `_us` counts state-0 `EnsureAvailable` calls at
  the exact `GraphicsMeshInstance::GetNumShadowRenderPasses` call site. It
  overlaps the mesh-load duration and must not be added to it.

The filename getter is an exported 16-byte function at RVA `0x2130e0`; its
instructions select the MSVC small-string storage at `Resource+0xc`. No name
is retained or written. The mesh-boundary instrument uses `detour::patchCall`
on the one `E8` inside the verified complete 24-byte function, changing only
its displacement. Resident mesh calls pay one state load and branch; only a
cold mesh pays a timer pair. This is materially narrower than detouring every
`EnsureAvailable` in the process.

If, on the marked collision-active full-scene **play** transition,
`engine_shadow_mesh_cold`, `engine_shadow_res_mesh`, and
`engine_shadow_res_load` agree in count and duration, option 3 is fully scoped.
Option 2 would have to discover the same off-camera shadow caster before this
shadow-frustum traversal; enqueueing at the pass-count query is already too
late. A shader, texture, other, or unmatched interval means the omission is
incomplete and that class has to be followed before changing behaviour.

`verify-sites.py` now performs 176 checks. It re-derives the patched call's
destination, the mesh pointer and pass-count operands, both exact exports, and
the filename getter. Perturbing either new RVA, either export name, either byte
table, the `EnsureAvailable` RVA/name, or the call offset makes it fail. The
required `doctor`, build, and off-game self-test pass; the GPU timestamp
retirement passed on the first completed self-test.

`cache/runs/run49-shadow-caster-boundary.ini` is built from the normal live
configuration and differs only by `performance_trace=full` and the passive F12
marker. It changes no game behaviour and leaves `shadow_split` untouched.

## 54. Run 49 result: DX11 reaches the boundary, but textures dominate later

The archived CSV is `tqflicker-frames.run49.csv`, SHA-256
`69e07412d10c77ad123b236763f1c8c34c38f93bf112c8179ab1c39e7628d76c`.
It contains 7,116 contiguous frames. The five session parts are: **menu**
frames 0--1735 (16.107 s), **load-game frame** 1736 (1.576 s), **loading
screen** 1737--2806 (9.525 s), **first world frame** 2807 (0.774 s), and
**play** 2808--7115 (64.653 s).

The concern that `GetNumShadowRenderPasses` might be stale DX9-only machinery
is resolved in the other direction by both the binary and the measurement.
`RenderDevice::activeAPI==0` selects `GraphicsForwardRenderer`, while value 1
selects `GraphicsDeferredRendererX`. Inside the shared
`GraphicsShadowMapRenderer::Render`, API 0 bypasses `FUN_1018c870`; API 1 calls
it. That function makes the virtual shadow-pass-count call. Run 49 then counted
ten state-0 mesh `EnsureAvailable` calls at the patched
`GraphicsMeshInstance::GetNumShadowRenderPasses` call site during **play**.
The path is therefore live in this DX11 session, not merely present in the
binary.

The important correction is that this is not the complete resource-readiness
boundary assumed by §53. Across **play**, all 218 directional-shadow resource
loads are still state 0 and none has a queue link, totaling 447.434 ms. Their
engine filename classes partition exactly as follows: 87 meshes take 83.858
ms, 5 shaders take 4.025 ms, 126 textures take 359.551 ms, and `other` is zero.
Only 10 cold meshes totaling 15.315 ms are encountered at the pass-count call.
The pass-count counter and load counters overlap, but their large mismatch
shows that later resource gates dominate.

The one F12 marker is frame 6552 in **play**. Its reaction window contains two
consecutive collision-active full-scene **play** candidates. Frame 6520 lasts
331.841 ms, including 315.203 ms in `Engine::Render` and 142.782 ms in
`GraphicsShadowMapDx11::RenderDirectional`; it is also a shadow-region change.
All 67 nested resource loads and all 142.026 ms enter in state 0. Textures are
37 loads / 104.701 ms, meshes 28 / 34.762 ms, and shaders 2 / 2.563 ms. The
exact pass-count site sees only 4 cold meshes / 9.093 ms. The same call has a
262.601 ms directional GPU interval. CPU load, enclosing shadow CPU, and GPU
intervals overlap and must not be added. Frame 6521 then lasts 156.865 ms with
142.500 ms in `Engine::Render`, but has no resource load and only 1.801 ms of
shadow CPU; its residual render time is not attributed by this run.

This rejects only the proposed **mesh-state-only** option 3. Returning zero
passes when the root mesh is state 0 would cover 9.093 ms of the marked load
interval, not the later 133 ms, and run 49 gives no evidence that it would
remove the texture-dominated burst. Per-caster omission may still be viable if
all required shader and texture dependencies can be checked without forcing
them available, but that safe predicate has not been found. Option 2 also
remains unscoped: preloading only the root mesh would miss the dominant texture
work. The next discriminating trace is cold `EnsureAvailable` caller
attribution within the directional build, especially the list-building shader
selection and `RenderPass`/shader-parameter paths. No behaviour A/B is
supported yet.

## 55. Run 50 prepared: required versus gratuitous shadow material textures

The first later texture boundary is now exact. In
`GraphicsMesh::SetShaderParameters` (`Engine.dll` RVA `0x169c40`), each type-7
material entry calls `GraphicsTexture::GetTexture` at RVA `0x169cab`. Only
after that potentially synchronous load does the code call the texture
parameter setter at RVA `0x169cc1`. The setter searches the active shader's
parameter table and returns without binding anything when the material Name is
absent. Thus the generic loop can load a texture before discovering that this
particular shadow shader cannot use it.

Run 50 wraps those two adjacent calls with `patchCall`; it does not detour the
shared functions. Only on the main thread, inside
`GraphicsShadowMapDx11::RenderDirectional`, and for a texture whose raw
resource state is 0, the getter wrapper times the complete call. The following
setter wrapper calls the verified exported
`GraphicsShader2::HasParameter`—only after verifying that shader state is 2—
and partitions the load into `engine_shadow_material_tex_used`, `unused`, or
`unknown`. Their `_us` columns are engine durations and the three buckets must
equal the material-texture total. They overlap the broader
`engine_shadow_res_texture` interval and must not be added to it.

This is the discriminating measurement for “load only the mesh.” A dominant
`unused` bucket supports a narrow fix that tests the active shadow shader
before calling `GetTexture`, avoiding only loads whose values the existing
setter discards. A dominant `used` bucket means texture-free shadow submission
would change behaviour: alpha-tested foliage and similar casters need a base
or opacity texture to decide which texels cast. It would then require an
explicit fallback representation or earlier residency, not a blind omission.

“Load the lowest mip and defer the rest” is already partly active on this
installation. On run 49's marked full-scene **play** frame 6520, 26 loose
textures started the progressive uploader and held 32 MiB of source mappings;
37 shadow texture resources were loaded in total. The uploader creates an
eligible large block-compressed texture from its small mips and sends the
withheld high mips over later frames. The burst nevertheless retained 104.701
ms of texture resource loading and 41.378 ms across 52 texture creates. That
does not reject a more invasive placeholder/streaming design, but it shows
that deferring high-mip upload alone is not the missing fix: synchronous file
access, container parsing, base texture/SRV creation, and ineligible textures
remain.

The new sites verify 16, 21, and 24 bytes respectively. `verify-sites.py` now
performs 198 checks: both call destinations, both call offsets, all three byte
windows, the owning mesh export, the texture getter export, the shader
parameter export, and the relevant operands are independently re-derived from
the pinned image. Fourteen one-constant perturbations were all rejected. The
required doctor, build, and off-game self-test pass, including GPU timestamp
retirement on its first completed run. `cache/runs/run50-shadow-material-textures.ini`
differs from the normal configuration only by full performance tracing and the
passive F12 marker. It changes no game behaviour and leaves `shadow_split`
untouched.

## 56. Run 50 result: half the material texture work is unnecessary to shadows

Run 50 is archived as `tqflicker-frames.run50.csv`, SHA-256
`86c1e6d6b26a42b9a69bf2bedf3096cf3dfec3011525c44c94b4baa7ddf37bd1`.
It contains 7,398 contiguous rows. The five session parts are: **menu** frames
0--1953 (17.712 s), **load-game frame** 1954 (1.577 s), **loading screen**
1955--3114 (10.097 s), **first world frame** 3115 (1.191 s), and **play**
3116--7397 (63.629 s).

The new partition is exact on every row and `unknown` is zero. Across
**play**, 105 directional-shadow texture loads cost 298.901 ms. The generic
material path accounts for 77 / 225.958 ms of them: 40 textures / 113.009 ms
have a matching parameter in the active shadow shader, while 37 / 112.949 ms
do not. The near one-for-one population is consistent with the audited caster
package: its transparent styles have the one alpha-test pixel shader that
samples base alpha, while lighting-only material maps have no shadow use.

The F12 marker is frame 6835 in **play**. Its reaction window is not unique:
frame 6834, only 17 ms before the marker, lasts 64.536 ms with 47.361 ms in
`Engine::Update` and no shadow resource load; the earlier collision-active
full-scene **play** transition begins at frame 6809, 850 ms onset-to-marker.
Frame 6809 lasts 228.246 ms with 220.043 ms in `Engine::Render`, 90.009 ms in
the directional-shadow call, and 43 nested resource loads / 87.313 ms. Its 26
textures / 68.174 ms include 17 material textures / 47.705 ms: 9 shader-used /
25.328 ms and 8 shader-unused / 22.377 ms. The call also has a 151.354 ms
directional GPU interval. CPU load, enclosing shadow CPU, and GPU intervals
overlap and must not be added. The next **play** frame, 6810, lasts 86.783 ms
with no resource load and only 1.786 ms of shadow CPU.

This supports a behavior-preserving optimization, but bounds it: test the
active shader for the material Name before `GetTexture` and omit the 8 unused
loads / 22.377 ms on the transition frame. That removes about half the cold
material interval, not the whole burst; 9 shader-used material textures /
25.328 ms, 9 other texture loads / 20.469 ms, and 17 meshes / 19.139 ms remain.

Temporarily omitting the shader-used class is a defensible performance/quality
trade rather than a correctness-preserving optimization. The caster package
proves what it means visually: transparent shadow styles use their sole pixel
shader to sample base alpha and discard below 0.5. Omitting such a caster until
its texture is resident removes only that caster's shadow; it does not remove
the scene object or require a shadow-system rewrite. It may produce local
foliage/fence shadow pop-in or a short change in shadow density, but is much
narrower than run 47's visible whole-map reuse.

The word “until” needs an active guarantee. If the shadow getter merely avoids
`GetTexture`, a shadow-only/off-camera resource can remain state 0 indefinitely;
if the color pass later needs it, that pass can pay the same synchronous load.
A valid experiment must explicitly enqueue a state-0 texture, continue omitting
that one caster while its texture is state 0 or 1, and restore the normal path
only at state 2. Before building it, re-verify the exported
`Resource::GetResourceLoader` and `ResourceLoader::EnqueueResource` path, its
priority/ownership contract, and the exact per-caster suppression boundary.
The experiment should report enqueue-to-state-2 latency so “a few frames” is
measured rather than assumed.

## 57. Run 51 installed: opaque shadows avoid unused textures; cold alpha casters defer

The first implementation pass was too narrow. It omitted cold alpha-tested
`GraphicsMeshInstance` caster/pass records but left opaque casters completely
unchanged. That would preserve their geometry, but it would also preserve the
run-50 bug: the generic material loop would still synchronously load texture
entries before discovering that the opaque shadow shader has no matching
parameter. Run 51 therefore combines the two parts that the evidence supports:

- an opaque caster still renders its shadow normally, but a material texture
  whose `Name` is absent from the active shadow shader returns null before
  `GraphicsTexture::GetTexture`; the adjacent original parameter setter then
  makes the same absent-parameter decision and binds nothing;
- an alpha-tested caster whose base `GraphicsTexture` Resource is in state 0
  is explicitly enqueued and its one caster/pass record is omitted; state 1
  stays omitted, and state 2 goes through the original path.

The normal colour pass is intentionally unchanged. A visible object needs its
material to render correctly; the shadow traversal instead supplies earlier
notice for off-camera casters, which are exactly the objects that can cast into
the map without entering the colour list. If the colour pass reaches the same
resource before the loader finishes, that residual synchronous load remains
visible in the trace rather than being hidden by this experiment. A colour-pass
fallback or small-mip representation would be a broader resource-streaming
change, not this A/B.

The suppression boundary is a direct `E8` at Engine RVA `0x18c8fe`, inside a
22-byte verified window beginning at `0x18c8f5`. Its arguments are the pass,
renderable entry, temporary output record, and renderer. The original target at
`0x18c650` returns a boolean that the caller immediately tests; false branches
around appending the 0x88-byte caster/pass record. The wrapper decides before
calling that helper, so it never constructs a temporary record that the caller
will not own. It repeats the helper's first virtual eligibility test only for a
cold-alpha candidate before enqueueing, avoiding loader work for a caster the
engine would reject anyway.

The class and style test is equally narrow. Virtual slot 2 must equal the
exported `GraphicsMeshInstance::GetShadowRenderStyle` at RVA `0x1733b0`.
Verified return blocks establish styles 0/1/2 as opaque static/skinned/foliage
and 3/4/5 as their alpha-tested counterparts. The exported
`GraphicsMeshInstance::GetTexture` at `0x1731a0` obtains the material's
`GraphicsTexture` Resource; its verified dependency call ensures the already
accepted owning mesh, while its verified return block reads the texture pointer from
the material entry at `+0x14`. It does not make that returned texture resident.

For state 0, exported `Resource::GetResourceLoader` at RVA `0x212dc0` returns
`Resource+0x24`, and the fix calls exported
`ResourceLoader::EnqueueResource` with `(resource, 1, true, false)`. Those are
not guessed arguments: the 20-byte window in
`BaseResourceManager::PreLoadResource` at RVA `0x120110` pushes the same tuple
and calls the same export. State and queue fields remain guarded by the
previously verified accessors at Resource `+0x30` and `+0x60`. No Resource
pointer is retained across calls or frames.

The unused-parameter filter stays at run 50's adjacent material getter call
site. A 32-bit naked adapter forwards the material `Name` already in ESI and
the active `GraphicsShader2*` at verified getter-entry `ESP+0x20`; its emitted
body is a push of `[esp+0x20]`, ESI, and ECX, one helper call, stack restore,
and return. The helper queries only an already-resident shader. If the Name is
absent it returns null without loading the material texture; otherwise it calls
the original getter unchanged. This keeps opaque shadow geometry while making
its texture-free status real rather than rhetorical.

`[performance] shadow_defer_cold_alpha=1` is off by default, reaches
`install()` with the performance probe off, and enables no trace group. The
combined change is atomic: failure to verify or patch either the record
decision or the material getter restores both. With tracing, run 51 adds
`engine_shadow_alpha_omitted`, state-0/state-1, enqueue/enqueue-failed, and
material skipped/skipped-cold counters. `enqueue_failed` must be zero;
state-1 occurrences in following frames measure persistence without unsafe
pointer tracking.

`research/streaming/tools/verify-sites.py` now performs 243 checks against the
installed binaries. Every new code window is 16--24 bytes, including the
original helper, all style-return paths, the mesh texture lookup/return, loader
accessor, and stock preload call. All eighteen newly introduced address,
offset, and stack constants were independently set to zero and every mutation
was rejected. The off-game self-test covers the default/off/no-trace contract,
the style/state predicate, counter/header alignment, and the audited-module
gate. `cache/runs/run51-shadow-defer-cold-alpha.ini` differs from
`live-config.ini` only by this behavior switch, full performance tracing, and
the passive F12 marker. `npm run doctor && npm run build && npm run selftest`
passes; GPU timestamp retirement passed on the first completed run. The
installed DLL is byte-identical to the release
build, SHA-256 `e3f21518f9229a4cf7b6fcad1ed9b14577968604adcd00357178b148c0951e12`;
the installed INI is byte-identical to the cache copy, SHA-256
`869e8e6b174a2cc5d3b6094eab19b263ebd21c5c94c6271db27584499232bc70`.
The stale live CSV was removed after run 50 had already been archived.
`shadow_split` remains untouched, and the game was not launched.

## 58. Run 51 result: visually safe, but base-texture deferral does not remove the burst

Run 51 is archived as `tqflicker-frames.run51.csv`, SHA-256
`cbf4f4e8feecfb6f7ce4b2d62058adaa66a99aa6cccea871f28673fbef3c4494`.
It contains 7,512 contiguous rows. The five session parts are: **menu** frames
0--1872 (17.531 s), **load-game frame** 1873 (1.517 s), **loading screen**
1874--3023 (9.962 s), **first world frame** 3024 (785.342 ms), and **play**
3025--7511 (68.607 s).

The visual-safety result is positive for this class and this session. The
reporter noticed no flicker and no shadow popping. During **play**, the switch
omitted 71 alpha-tested `GraphicsMeshInstance` caster/pass records: 68 while
the checked base `GraphicsTexture` Resource was state 0 and 3 while it was
state 1. Fifty-five state-0 calls made a new queue/state transition and
`engine_shadow_alpha_enqueue_failed` remained exactly zero. The remaining
state-0 occurrences already had a queue link; more than one caster/pass
attempt can name the same resource, so these are attempt counts, not unique
textures. The three state-1 attempts cannot establish residency latency
without resource identity.

The behavior-preserving opaque/material half also worked exactly as designed.
Across **play**, the active shadow shader rejected 980,873 material getter
attempts before `GetTexture`, including 7,121 attempts made while the named
texture was state 0. Those large numbers are repeated shader-parameter tests
on every directional build, not 980,873 resources or loads. Only 13 cold
material textures were actually loaded, taking 30.155 ms; all 13 and all
30.155 ms were shader-used, while the shader-unused and unknown buckets were
both exactly zero. For comparison, run 50 observed 37 shader-unused cold
material loads / 112.949 ms across **play**. Route composition varies, so that
cross-session total is not a magnitude A/B, but run 51's zero unused bucket
does prove that the discarded-value load path was removed.

The one F12 marker is frame 6742 in **play**, 387.614 ms after the end of frame
6723 and 540.244 ms after the end of frame 6722. These are consecutive,
collision-active, full-scene **play** frames, lasting 193.597 and 152.630 ms;
there is no competing 40 ms candidate in that reaction window. This therefore
ties the felt event to the two-frame transition rather than selecting a CSV
maximum after the fact.

The switch engaged at the transition but did not remove it. Frame 6722 spends
38.271 ms in `GraphicsShadowMapDx11::RenderDirectional`, including 28 cold
shadow resource loads / 37.111 ms. It omits 24 state-0 alpha records, makes 16
new enqueue transitions, and has a 111.856 ms directional GPU interval. Frame
6723 has no alpha omission, yet spends another 36.551 ms in the directional
build: 12 cold shadow textures take 34.388 ms, including 5 shader-used material
textures / 14.510 ms, beside a 41.871 ms directional GPU interval. CPU load,
the enclosing shadow call, and GPU intervals overlap and must not be added.

The corresponding run-50 transition frames 6809--6810 lasted 315.029 ms in
total; run 51's two frames total 346.227 ms. Their scene work differs, so this
does not prove a 31.198 ms regression. It does rule out claiming that run 51
fixed the felt burst. The narrower intended work did shrink: the two-frame
material interval is 18.337 ms versus 47.705 ms in run 50, and total nested
shadow-load time is 71.499 versus 87.313 ms. But after 24 omissions on the
first run-51 frame, the next frame has no omission and again has a large
shadow CPU/GPU interval plus new synchronous texture work. This is consistent
with the checked base texture becoming resident before other dependencies are
ready. The counters do not prove that those 12 textures belong to the same
returned casters, so “the load moved” remains a strong pattern, not an identity
claim.

The per-material parameter check has no visible millisecond-scale steady-play
penalty in this run. Restricting both runs to collision-active, full-scene
**play** frames below 60 ms, with no shadow resource load or region change,
mean directional-shadow CPU differs by -0.029, -0.148, +0.130, and +0.078 ms
in indexed-draw bands 500--999, 1000--1499, 1500--1999, and 2000-plus. The
mixed signs and different scene populations prevent a finer overhead claim,
but exclude a gross regression from the hundreds of `HasParameter` calls.

Run 51 therefore supports keeping the shader-unused texture filter and says
that temporary alpha-caster omission is visually acceptable; it does not yet
support shipping the base-texture-only deferral as the loading-burst fix. The
next discriminating run should be passive. For every remaining cold
shader-used material texture, it must identify the `GraphicsMeshInstance`
shadow style/pass and whether the loaded texture is the same base Resource
checked by the record gate; the remaining non-material shadow texture call
sites need their own partition. That distinguishes an incomplete dependency
list on a returning alpha caster from used textures on opaque or non-mesh
casters before any omission rule is widened. `shadow_split` remains untouched.


## 59. Run 52 prepared: identify the complete shadow-texture dependency before widening

**WITHDRAWN by §60.** The direct-caller stack partition is non-mutating, but
the material-context adapter mistook EBP for the pass after the original code
had converted it to a `MeshRenderInfo*`. Both run-52 attempts froze when the
first cold texture passed that pointer back as an index. Do not run this
implementation again.

Run 51 established that temporarily omitting this alpha-tested
`GraphicsMeshInstance` caster class caused no noticed flicker or shadow pop,
but it did not remove the marked loading burst. Collision-active full-scene
**play** frames 6722 and 6723 lasted 193.597 and 152.630 ms. The first omitted
24 state-0 alpha records and newly queued 16; the second omitted none but still
loaded 12 cold shadow textures / 34.388 ms, including 5 shader-used material
textures / 14.510 ms. That is evidence that checking only the record's base
texture is incomplete, not evidence that every remaining texture belongs to
the same alpha caster.

Run 52 is therefore passive attribution with the run-51 behavior held fixed.
At the verified `GraphicsMeshInstance::SetShaderParameters` call into
`GraphicsMesh::SetShaderParameters`, a call wrapper records the instance and
render-pass argument for the duration of the original call. Only when a cold
material texture is actually encountered does it resolve and cache that
call's shadow style and base `GraphicsTexture` Resource; ordinary material
setups pay neither lookup. Each cold shader-used material texture is then
classified independently by style 0--5, base-resource match/other, and pass
0/other. `context_unknown` is explicit rather than guessed. For every frame
the invariants are:

`used = sum(style0..style5) + context_unknown`

`used = base_match + base_other + context_unknown`

`used = pass0 + pass_other + context_unknown`

This context is call-scoped; no engine pointer survives the original call.
Separately, a bounded stack scan made only during an actual main-thread `.tex`
load inside `GraphicsShadowMapDx11::RenderDirectional` partitions every such
load by the nearest exact return address from a direct
`GraphicsTexture::GetTexture` call. The buckets distinguish mesh material,
billboard, forward renderer, line effect, PieOmatic, water, state-parameter,
three unnamed functions, and unresolved/indirect callers. Their counts and
durations must exactly sum to `engine_shadow_res_texture` and `_us`.

The caller set was not copied from a prior note. An exhaustive scan of the
pinned Engine.dll `.text` found ten direct calls to the exported getter: the
material site plus the nine attribution entries. The source verifier now has
268 checks: it proves the three new 17--22-byte windows, export identity, each
E8 target, exact source-table coverage of all ten direct calls, and the x86
adapter's argument contract. One-at-a-time perturbation of the five new
RVA/offset constants, nine caller RVAs, export name, and one byte in each new
window rejected all 18 changes. The emitted adapter was also independently
assembled and matched its intended stack transformation.

Run 52 keeps `shadow_defer_cold_alpha=1`, `loose_texture_max=4096`, full trace,
and F12 marking. It changes no behavior relative to run 51: the color pass,
opaque caster behavior, `shadow_split`, map size, culling, and resource policy
are unchanged. The useful outcomes are identity statements: whether the
remaining used loads are base matches or other dependencies of returning
alpha casters, another mesh style/pass, or a non-material caller. Only then is
there evidence for either expanding the dependency gate or choosing a
different class-specific boundary.


## 60. Run 52 froze twice; remove the re-entrant material lookup

Both attempts with run 52 froze where the prior loading transition/burst had
occurred. Only the second attempt's CSV survives because a new session creates
the live CSV afresh; it is archived as `tqflicker-frames.run52.csv`, SHA-256
`5f6e68493efd3c1bf2a2f4ea6ba785ccc624f99b9e8afa9797b344838c62916f`.
It contains 6,599 contiguous presented frames, 0--6598: menu frames 0--1809
(17.130 s), load-game frame 1810 (1.545 s), loading-screen frames 1811--3069
(10.752 s), first-world frame 3070 (895.7 ms), and play frames 3071--6598
(51.557 s). The final written play frame is ordinary, 8.248 ms; the freezing
frame never completed Present and therefore has no CSV row. No F12 marker was
written.

More importantly, every new texture-caller and material-context counter is
zero over the entire captured session. Thus the trace does not identify the
offending texture or caster. Together with two freezes precisely when the
first cold transition was expected, it localizes the regression to code first
exercised by that cold event, but it does not by itself distinguish a deadlock
from another non-returning engine path.

The exact adapter error is visible in the bytes. EBP receives arg3/pass at
function RVA `0x173494`, but immediately before the patched call the original
code executes `imul ebp,ebp,0x34; add ebp,[edi+0x1c]`. EBP is therefore a
`MeshRenderInfo*`, not the pass. Run 52 pushed EBP as the pass and, on the first
cold material texture, supplied that pointer as an index to
`GraphicsMeshInstance::GetShadowRenderStyle` and `GetTexture`. The freeze is
fully explained without a Wine-only hypothesis or an inferred lock cycle.

Run 53 removes both nested engine calls from the material path. The already
exercised run-51 `BuildShadowRecord` boundary records the exact
`GraphicsMeshInstance`, pass,
and style it has just queried, but only after the original helper accepts the
record. For alpha-tested styles 3--5 it also records the base texture pointer
that the run-51 gate already obtained; opaque styles 0--2 use an explicit
`base_unknown` bucket rather than performing a new lookup. A fixed 4,096-slot
generation-keyed table relates that identity to the later
`GraphicsMeshInstance::SetShaderParameters` call during the same
`GraphicsShadowMapDx11::RenderDirectional` invocation. A generation change
expires every entry in O(1), and stored pointers are compared only during that
same directional call; none is dereferenced from the material path. The
correct original pass is read from adapter stack offset `0xbc`, derived from
the 0x8c-byte local frame, four saved registers, two original call arguments,
the E8 return address, and the adapter's first two pushes. A new verified
16-byte argument window explicitly proves the instruction that destroys EBP's
pass value.

`verify-sites.py` now performs 275 checks. It includes the new 19-byte frame
and 16-byte argument windows, derives `0xbc` from the verified 0x8c local
allocation and stack layout, and requires the adapter immediate to match.
All 24 one-at-a-time perturbations of the context RVAs/offsets, direct-caller
table, export name, window bytes, and emitted adapter immediate were rejected.
The release DLL contains exactly one emitted adapter sequence, and its bytes
decode as the two original-argument pushes, `push [esp+0xbc]`, instance/mesh
pushes, helper call, `add esp,20`, and `ret 8`.

The corrected invariants are:

`used = sum(style0..style5) + context_unknown`

`used = base_match + base_other + base_unknown + context_unknown`

`used = pass0 + pass_other + context_unknown`

The independent bounded stack partition remains, because it performs one
`VirtualQuery` and read-only comparisons against exact return addresses only
after a texture load has already entered. Run 53 still makes no behavior
change relative to run 51. This correction is about the alpha-tested and
opaque `GraphicsMeshInstance` shadow-caster classes and other direct texture
callers inside the directional-shadow build; it says nothing new about the
color pass or the game's original loading burst. `shadow_split` remains
untouched.


## 61. Run 53 completed safely; the marked play window contains two different stalls

Run 53 is archived as `tqflicker-frames.run53.csv`, SHA-256
`5203a1c247deda8c59d8605c386d695930103df2d1d7f25cb99fbe04f68140b5`.
It contains 7,662 contiguous presented frames, 0--7661: menu 0--2110
(19.767 s), load-game frame 2111 (1.302 s), loading screen 2112--3358
(10.641 s), first-world frame 3359 (1.250 s), and play 3360--7661
(64.939 s). Unlike run 52, it completed through the transition; the invalid
EBP/pass adapter was the run-52 freeze regression.

The one F12 marker is play frame 7075. Its nearest collision-active,
full-scene **play** candidate is frame 7074: 59.411 ms, ending 16 ms before the
marker and beginning 75 ms before it. It has 41.787 ms in `Engine::Update`,
16.664 ms in `Engine::Render`, 0.108 ms in the message pump, and no resource
load. Its resolved GPU-frame interval is 54.653 ms, while directional shadow
GPU is only 5.833 ms. CPU and GPU intervals overlap. This is the strongest
reaction-time candidate, but it is not the loading transition.

The earlier loading burst is also inside the two-second reaction window.
Full-scene **play** frames 7041 and 7042 last 254.638 and 87.588 ms; they end
772 and 685 ms before the marker. Frame 7041 changes shadow region and spends
45.639 ms in the directional build. Its 24 nested loads / 40.828 ms are 23
meshes / 40.094 ms plus one shader / 0.734 ms; six meshes at the exact
`GraphicsMeshInstance::GetNumShadowRenderPasses` boundary cost 25.067 ms. Its
directional GPU interval is 103.536 ms. Frame 7042 then spends 29.405 ms in
the directional build and loads 12 textures / 27.086 ms: five direct mesh
material textures / 11.291 ms and seven indirect/unresolved textures /
15.795 ms. The material getter measures the same five shader-used loads as
11.314 ms. Again, nested load, enclosing CPU, and GPU intervals overlap.

Across all **play**, the new caller partition balances exactly: 27 shadow
textures / 82.381 ms are 12 direct mesh-material calls / 32.756 ms and 15
unresolved calls / 49.625 ms. All 12 cold material textures / 32.791 ms are
shader-used. The style/base/pass partitions also balance mathematically, but
all 12 are `context_unknown`, so run 53 does **not** identify their caster
class. The honest unknown result overturns §59's expectation that an accepted
record keyed only for the exact base-class vtable would necessarily match the
later base `GraphicsMeshInstance::SetShaderParameters` call.

The run-51 behavior remains visually and mechanically separate: during
**play**, 77 alpha-tested exact-class caster/pass attempts were omitted (76
state 0, one state 1), 58 newly enqueued, and enqueue failure was zero. No
visual observation was supplied with this run, so run 51 remains the visual
safety evidence. Comparing only collision-active full-scene **play** frames
below 60 ms with no shadow load or region change, run-53 minus run-51 mean
directional CPU is +0.056, +0.376, -0.022, and -0.024 ms in indexed-draw bands
500--999, 1000--1499, 1500--1999, and 2000-plus. The small and mixed values,
especially 43 samples in run 53's 1000--1499 band, exclude a gross table
overhead but do not support a fine effect estimate.

The marker therefore does not uniquely decide which event was felt. The
nearest candidate is the non-loading `Engine::Update` frame 7074; the large
mesh-then-texture transition ended about 0.7 seconds earlier. Do not relabel
one from the other without the reporter's timing recollection.

## 62. Run 54 prepared: explain the material-context miss without new engine calls

Run 54 keeps run 53's behavior and sites, but broadens only the in-memory
record identity table. Every accepted renderable is stored for the current
directional call. The exact base-class `GraphicsMeshInstance` entry retains
verified style and alpha-base identity; an accepted renderable whose virtual
style method differs is stored as `class_other`, without invoking it or
assuming its layout. On the rare cold shader-used material event, a failed
exact key lookup scans the fixed table once to distinguish `pass_mismatch`
from `instance_missing`. The scan is not performed on ordinary material work.
Table overflow has its own counter.

The four lookup categories -- exact base-class `GraphicsMeshInstance`, derived
or overriding class, same instance with a different pass, and no accepted
record -- exactly partition the used material count/time. Style, base, and
pass retain separate unknown buckets, so no dimension borrows another's
unknown value merely to balance. The verified adapter supplies the actual
material-call pass even if the accepted-record lookup misses; `pass_unknown`
therefore means the adapter context itself was absent. No stored pointer is
dereferenced, no engine method is added, and the table is generation-expired
at the next directional call. This is still passive attribution, not another
omission experiment. It is the minimum next measurement because run 53's
all-unknown result cannot support widening the alpha-tested exact-class
omission to opaque or derived casters. `shadow_split` remains untouched.

The verifier now performs 279 checks, adding source invariants for the
4,096-slot power-of-two table, engine-call-free miss scan, accepted-record
gate, and cold-miss-only fallback. The 24 prior one-at-a-time perturbations
remain covered; independently changing the new table-size constant is also
rejected, making the relevant mutation audit 25/25. The self-test additionally
fills all 4,096 slots and requires the next insertion to report exactly one
overflow rather than silently losing the join.

Doctor, the release build, and the full off-game self-test pass, including GPU
timestamp retirement on this attempt. The installed DLL is byte-identical to
the release build at SHA-256
`0c7da90d9aa7e35090c8832b102093e957461281b8e1907e9517bdf072116d8f`.
The installed INI is byte-identical to
`cache/runs/run54-shadow-context-miss.ini`, and the archived run-53 live CSV
was removed before run 54. The game was not launched.

## 63. Run 54: the marked event is the loading transition; the context patch was absent

Run 54 is archived as `tqflicker-frames.run54.csv`, SHA-256
`fbdd2e2a8ac9bd12e5d6bedb2f5d708b4fe534dd3a8524e1e3513991defd6c8e`.
It has 7,596 contiguous presented frames, 0--7595: menu 0--2067
(18.976 s), load-game frame 2068 (1.472 s), loading screen 2069--3191
(9.654 s), first-world frame 3192 (649.889 ms), and play 3193--7595
(66.495 s).

The reporter confirms that F12 at **play** frame 6913 followed the large
loading transition, not the smaller frames immediately beside the keypress.
The transition is full-scene, collision-active **play** frames 6897 and 6898,
251.589 and 147.926 ms (399.515 ms together), ending 508 and 361 ms before
the marker. This also resolves run 53: its roughly 0.7-second marker delay was
ordinary reaction time and identified that run's large loading pair, not its
later 59.411 ms `Engine::Update` frame.

Run 54 reproduces the same two-stage class. Frame 6897 changes shadow region
and spends 36.672 ms in `GraphicsShadowMapDx11::RenderDirectional`. It loads
27 meshes / 32.099 ms and one shader / 2.584 ms; five meshes at the exact
`GraphicsMeshInstance::GetNumShadowRenderPasses` boundary cost 11.745 ms. Its
resolved directional GPU interval is 140.789 ms. Frame 6898 then spends
34.260 ms in the directional build and loads 13 textures / 31.988 ms: six
direct mesh-material loads / 11.517 ms and seven unresolved loads / 20.471 ms.
The material getter measures the same six shader-used calls as 11.528 ms, and
the resolved directional GPU interval is 40.878 ms. CPU nested loads,
enclosing directional work, and GPU intervals overlap; they must not be added.

Across **play**, 127 shadow-nested resource loads / 186.523 ms partition into
89 meshes / 85.617 ms, six shaders / 5.217 ms, and 32 textures / 95.689 ms.
The texture-caller partition balances exactly: 14 direct mesh-material calls /
31.800 ms and 18 unresolved / 63.889 ms. All 14 cold material getters /
31.824 ms are shader-used. The retained alpha-tested exact-class mitigation
omits 83 caster/pass attempts (82 state 0, one state 1), newly queues 61, and
reports no enqueue failure. Context-table overflow is zero.

Run 54's intended join result needs a forward correction. All 14 used material
calls report `style/context_unknown`, `base_unknown`, and `pass_unknown`.
Because the corrected adapter supplies a pass before any table lookup,
`pass_unknown` proves that the base
`GraphicsMeshInstance::SetShaderParameters` adapter context was absent. The
simultaneous `lookup_exact=14` is impossible and is **withdrawn**: the lookup
enum's zero value was `Exact`, and `explainShadowRecordMiss` returned early
when the zero-initialized context had no instance. Thus run 54 does not show 14
successful joins; it shows 14 calls that never acquired adapter context and a
bad label on that missing-context state.

Run 55 corrects the zero-instance state to `instance_missing` and explains why
the adapter was absent without another behavior experiment. One of seven
status counters is emitted for every actual directional build: active,
dependency missing, frame/entry/context signature mismatch, call-patch
failure, or rollback after the material hook failed. Independently, each cold
shader-used material load is classified by whether the enclosing
`GraphicsMesh::SetShaderParameters` call returns to the one verified direct
base-`GraphicsMeshInstance` site or another site. The outer return address is
at getter-entry ESP+0x1c, derived from the verified eight local bytes, four
saved registers, and the inner E8 return. No retained pointer is dereferenced
and no engine method is called.

Two new 23/24-byte windows verify that stack contract. `verify-sites.py` now
performs 285 checks. All eight new one-at-a-time RVA, byte, offset, and adapter
mutations are rejected; together with the prior 25 this is 33/33 relevant
mutations. Doctor, release build, and the full off-game self-test pass,
including explicit tests for zero-instance relabeling, all seven patch-status
outcomes, caller partitioning, and GPU timestamp retirement. Run 55 remains
behavior-identical to run 51: it retains only
`shadow_defer_cold_alpha=1`, full tracing, and F12 marking. It does not change
the color pass, opaque-caster behavior, `shadow_split`, map size, culling, or
resource policy.

The run-55 DLL and INI are installed, and the archived run-54 live CSV was
removed. The installed DLL is byte-identical to the release build at SHA-256
`502ff9d2044196d94d104c995e8cca613dddea79134c2e43c2840dc805074a48`.
The game was not launched.

## 64. Run 55: the expected mesh-instance caller is live; one unrelocated operand disabled its context patch

Run 55 is archived as `tqflicker-frames.run55.csv`, SHA-256
`490297012fbddaa4ac5d7c36659c7395d18c77e107e589c5692df503a19d9ee0`.
It has 7,257 contiguous presented frames, 0--7256: menu 0--1932
(17.968 s), load-game frame 1933 (1.429 s), loading screen 1934--3053
(9.892 s), first world frame 3054 (672.733 ms), and play 3055--7256
(63.210 s).

F12 at **play** frame 6698 follows full-scene, collision-active **play** frames
6679 and 6680, 225.310 and 195.514 ms (420.824 ms together). They ended 553
and 357 ms before the marker; onset to marker is 778/553 ms. This is the same
felt loading-transition class the reporter identified in runs 53 and 54, not a
maximum selected without visual confirmation.

Frame 6679 changes shadow region and spends 33.337 ms in
`GraphicsShadowMapDx11::RenderDirectional`. Its 29 nested cold loads /
32.973 ms are 27 meshes / 31.177 ms and two shaders / 1.796 ms; eight meshes
at the exact `GraphicsMeshInstance::GetNumShadowRenderPasses` boundary cost
13.809 ms. Its resolved directional GPU interval is 128.058 ms. Frame 6680
then spends 51.954 ms in the directional call and loads 19 textures /
49.604 ms: eight direct mesh-material loads / 22.157 ms and eleven
indirect/unresolved loads / 27.447 ms. The material getter measures the same
eight shader-used loads as 22.179 ms, and the resolved directional GPU
interval is 59.598 ms. CPU nested loads, enclosing directional work, and GPU
intervals overlap and are not additive.

Across **play**, all 14 cold shader-used material calls return through the one
verified direct base `GraphicsMeshInstance::SetShaderParameters` site;
`outer_other_site` is zero. Thus the data do not support an alternate caller
or caster-class explanation for the absent adapter context. Instead, every one
of the 2,958 actual directional builds reports
`context_patch_frame_mismatch`; the other six patch-status buckets are zero.
All 14 material calls consequently remain `instance_missing` and
`pass_unknown`, as run 55 correctly labels them.

The mismatch has an exact byte-level cause. The verified 19-byte function
entry starts with `81 ec 8c 00 00 00`, followed by opcode `A1` at byte 6 and
the absolute preferred-base VA `44 b0 41 10` at bytes 7--10. That operand
targets Engine.dll RVA `0x41b044`. The runtime loader rebases it, but
`kShadowMeshParameterFrameBytes` previously had no relocation descriptor.
The file verifier sees the image at preferred base `0x10000000`, so the
literal happened to pass there; runtime `detour::matches` compared it against
the rebased module and necessarily rejected it. This reconciles the on-disk
verification with the run-55 status without invoking patch order or another
module.

Run 56 adds only `kShadowMeshParameterFrameRelocs = {{7, 0x41b044}}` to the
passive context signature. The existing `patchCall` remains the mutation; no
new engine method or rendering/resource behavior is introduced. The color
pass, opaque-caster behavior, run-51 alpha-tested `GraphicsMeshInstance`
deferral, `shadow_split`, shadow-map size, culling, and resource policy remain
unchanged. The exact first check is that `context_patch_active` equals
`engine_shadow_render` per frame and every other status is zero. Only then may
the joined style/pass/base result scope another behavior change.

`verify-sites.py` now performs 286 checks. Independently changing the new
relocation offset, target RVA, or preferred-base operand is rejected, bringing
the relevant one-at-a-time mutation audit to 36/36. Doctor, release build, and
the full off-game self-test pass, including GPU timestamp retirement. Run 56
is installed from `cache/runs/run56-shadow-context-relocation.ini`; its DLL is
byte-identical to the release build at SHA-256
`650e8dd5b69a9e397d7d21aa14de44a78e86db6f7e325e98fc30618ec21f8d9c`.
The installed INI is byte-identical to the cache copy, the archived run-55
live CSV was removed, and the game was not launched.

## 65. Run 56: exact style-3 context works; the base-only gate misses a second material dependency

Run 56 is archived as `tqflicker-frames.run56.csv`, SHA-256
`f287e5c90c897a18b801dd9d0d3accf779bcf668e9d2ef0b0fa1fa53581612b5`.
It has 7,436 contiguous presented frames, 0--7435: menu 0--2007
(18.606 s), load-game frame 2008 (1.543 s), loading screen 2009--3128
(9.760 s), first world frame 3129 (1.442 s), and play 3130--7435
(65.053 s).

The reporter warns that F12 at **play** frame 6860 may be late, so two
candidate classes remain. Full-scene, collision-active **play** frames 6831
and 6832 last 222.567 and 61.223 ms (283.790 ms together), ending 652 and
591 ms before the marker; onset to marker is 875/652 ms. A separate
full-scene, collision-active **play** frame 6857 lasts 60.787 ms, ends 56 ms
before the marker, and spends 42.866 ms in `Engine::Update`. It has no cold
resource load and only 1.703 ms of directional-shadow CPU. Proximity alone is
not evidence that the later frame is what the reporter felt; preserve both.

The run-55 relocation diagnosis is confirmed exactly. Across the whole
session, `engine_shadow_context_patch_active=4,245` equals
`engine_shadow_render=4,245`; in **play**, both equal 3,039. Every other patch
status is zero on every row. Style, base, pass, miss-reason, outer-caller, and
texture-caller partitions balance on every row, and the context table never
overflows.

All eight cold shader-used material textures / 15.176 ms in **play** join an
exact accepted record. Every record is pass 0, style 3, and the exact base
`GraphicsMeshInstance` implementation; there are no unknown, derived-class,
or pass-mismatch events. Critically, all eight are `base_other`: none is the
base texture whose residency the run-51 omission gate checks. The mechanism
is therefore exact. An alpha-tested `GraphicsMeshInstance` is omitted while
its base is in state 0/1, but returns to the directional list once that base
reaches state 2 even when another material texture required by the active
shadow shader remains cold.

This does not yet support a wider behavior change. In candidate frame 6831,
`GraphicsShadowMapDx11::RenderDirectional` takes 26.353 ms and loads fifteen
meshes / 23.934 ms; only two meshes / 10.162 ms are at
`GraphicsMeshInstance::GetNumShadowRenderPasses`. Its directional GPU interval
is 44.662 ms and full GPU frame is 227.531 ms. Frame 6832 then spends
17.007 ms in the directional call and loads five textures / 14.458 ms: two
direct material textures / 3.604 ms and three unresolved textures / 10.854 ms.
The material getter independently measures the two as 3.614 ms. Its
directional GPU interval is 21.761 ms. These nested CPU and GPU intervals
overlap and are not additive.

Across **play**, 95 directional-shadow loads / 135.581 ms divide into 73
meshes / 73.703 ms, three shaders / 1.467 ms, and 19 textures / 60.411 ms.
The textures divide into eight direct material calls / 15.157 ms and eleven
unresolved calls / 45.254 ms. Thus waiting for or omitting the newly proved
secondary material dependency addresses only one quarter of the observed
shadow texture time in this session and only 3.604 ms of the candidate second
frame. The larger unresolved class must be named first.

Run 56's outer-caller partition needs a forward semantic correction. All
eight events say `outer_other_site`, whereas run 55 placed all fourteen at the
verified direct `GraphicsMeshInstance` site. That does not describe a caller
change: in run 55 the context patch was absent, so the original E8 return was
on the material getter's stack. In run 56 the active context wrapper calls
`GraphicsMesh::SetShaderParameters` itself, necessarily replacing that return
address. The exact joined instance/pass is stronger evidence and proves why
the label flipped. Run 57 treats a live wrapper context as the same verified
site.

Run 57 remains behavior-identical to run 56 and adds no CSV columns. With
`trace=1`, it logs at most eight cold used material dependencies as a verified
`Name::Hash`, resource filename, exact `GraphicsMeshInstance` style/pass/base
relationship, and join result. It separately logs at most eight unresolved
directional-shadow texture filenames and their call-shaped stack candidates
across the audited Engine.dll, Game.dll, and TQ.exe images. Both paths use the
existing fixed eight-slot bound, and the logger flushes each event during the
session rather than relying on unload.

The new `Name::Hash` layout is re-derived from its 16-byte exported window;
its three meaningful bytes are `mov eax,[ecx]; ret`. `verify-sites.py` now
performs 293 checks. Seven one-at-a-time changes to the RVA, byte, export,
report bound, used-only gate, wrapper classification, and unresolved-only gate
are all rejected, making 43/43 cumulative relevant mutations. Doctor, release
build, and the full off-game self-test pass, including GPU timestamp
retirement. Run 57 is installed from
`cache/runs/run57-shadow-texture-identities.ini`; its DLL is byte-identical to
the release build at SHA-256
`d84aea5579e093fe4daad2b610af702870b7bd1ec1c98bcc0dc90ecd6bf27427`.
The installed INI is byte-identical to the cache copy, the archived run-56
live CSV and stale debug log were removed, and the game was not launched.


## 66. Run 57: the unresolved texture class is an unused instance bump override

Run 57 is archived as `tqflicker-frames.run57.csv`, SHA-256
`a23a7f5a2745fe0d82102a8bf1c1a7a6f0cb7daebe4df9ad0dbedcc9c3fcba05`;
its live-written debug log has SHA-256
`d41b9e9dee108e65d4b1b880755d4775feb7fd7a0fcca5a2fe0d2d30e03559a7`.
It has 7,780 contiguous presented frames, 0--7779. The five session parts are
**menu** 0--1782 (16.609 s), **load-game frame** 1783 (1.522 s), **loading
screen** 1784--3538 (15.000 s), **first world frame** 3539 (2.278 s), and
**play** 3540--7779 (64.538 s).

F12 is a reaction anchor at **play** frame 7213, not the event frame. The
loading pair is the full-scene, collision-active **play** frames 7196/7197 at
167.872/60.953 ms, ending 411/350 ms before the press (onset 579/411 ms before
it). Frame 7196 loads twenty directional-shadow meshes / 21.927 ms and one
shader / 0.648 ms inside a 23.896 ms directional CPU call, beside a 36.546 ms
directional GPU interval and 160.465 ms whole-frame GPU interval. Frame 7197
loads twelve shadow textures / 27.345 ms: six direct mesh-material loads /
13.058 ms and six previously unresolved loads / 14.287 ms, inside a 29.753 ms
directional CPU call and beside a 35.429 ms directional GPU interval. These
nested and GPU intervals overlap; they are not additive.

There is also a separate full-scene, collision-active **play** frame 7212 /
46.730 ms, ending 23 ms before F12. It spends 29.065 ms in `Engine::Update`,
has no resource load, and only 1.664 ms in the directional-shadow CPU call.
Preserve it as a separate near-marker candidate. Proximity alone does not turn
it into the reported loading transition.

The passive partitions remain exact. In **play**, all 3,051 directional builds
report the context patch active, every failure status and table overflow is
zero, and all material dimensions balance. Fourteen cold shader-used material
textures / 26.017 ms are exact `GraphicsMeshInstance` joins: ten style 3 and
four style 4; thirteen pass 0 and one other pass; all fourteen are
`base_other`. All eight bounded identity reports carry Name hash
`0xf5a35fef`. Recomputing the engine digest from the literal proves that this
is the little-endian first dword of `Name("baseTexture")`, not a generic
"secondary alpha texture" label.

The unresolved class is independently byte-named. Every one of its eight
bounded stack reports returns first through Engine.dll+`0x173b4d`. The exact
stock path at `GraphicsMeshInstance::SetShaderParameters` is:

1. read the optional texture Resource from instance+`0x18`;
2. call `Resource::EnsureAvailable` at Engine.dll+`0x173b48`;
3. select a resident render-texture value;
4. call the texture-parameter setter with the static
   `Name("bumpTexture")`.

The eight filenames independently agree: each is a `...BMP.tex`/`...bmp.tex`
bump map corresponding to the nearby base texture. More importantly, the
setter at Engine.dll+`0x35ea0` searches the active shader's parameter table
*after* the Resource was ensured. Its two verified missing-parameter branches
return success without reading the supplied texture value. The synchronous
load is therefore an engine ordering bug whenever the directional-shadow
shader has no `bumpTexture` input; it is not a requirement of alpha-tested
shadowing and not a Wine/CrossOver explanation. The same x86 path exists on
native Windows.

Across **play**, sixteen unresolved shadow textures / 54.775 ms exceed the
fourteen direct material textures / 25.968 ms. The eight-report bound proves
the first half of that unresolved population is the bump path, not necessarily
all sixteen. Run 58 therefore makes the narrow behavior-preserving change and
measures it directly: at only the verified bump `EnsureAvailable` E8, while
inside the directional-shadow build, ask the already verified active shader
whether it has `bumpTexture`. If absent, skip the ensure and count both all
skips and the state-0 subset; otherwise forward the original call unchanged.
The following verified setter discards the empty value on exactly that absent
path. The normal colour pass, any shadow shader that declares `bumpTexture`,
opaque geometry, alpha-caster base deferral, culling, map size, and
`shadow_split` remain unchanged.

This is incorporated into the existing
`[performance] shadow_defer_cold_alpha=1` fix rather than creating a second
knob for one ordering defect. It remains default-off, reaches `install()` with
the performance probe off, and brings no trace group. The new trace-only
columns are `engine_shadow_bump_tex_skipped` and
`engine_shadow_bump_tex_skipped_cold`; both are counts, not mod-duration
columns. Run 58 keeps full tracing and F12 only to validate the mechanism and
relate it to the reported **play** transition.

Five independently verified windows cover 22 bytes around the bump ensure,
24 around its setter, 20 around construction of the exact static Name, 23
around the setter's missing-parameter decisions, and 16 at their common return
target. Only the five-byte E8 displacement is changed. `verify-sites.py`
passes 312 checks. All eight new named RVA/offset constants, all four new
relocation descriptors, one byte in each of the five tables, and the
directional gate, `HasParameter` polarity, forwarding call, and atomic install
dependency were perturbed one at a time; all 21 are rejected (64/64 cumulative
relevant mutations). Doctor, release build, and the full off-game self-test
pass, including GPU timestamp retirement.

Run 58 is installed from `cache/runs/run58-shadow-bump-unused.ini`. The
installed DLL is byte-identical to the release build at SHA-256
`96a1fce0fb2b67fc50e8056a7536d8bf8125b1eb7e7a55755ea9e241f9459e46`;
the installed INI is byte-identical to the run file at SHA-256
`663c9b8a5665303f69add0e1f3cc1333012eba2cc2b8f354b6c0652122fc20eb`.
The archived run-57 CSV and debug log were byte-identical to their live names
before those two stale live files were removed. The game was not launched.


## 67. Run 58: the bump omission removes the unresolved class; only overridden base textures remain

Run 58 is archived as `tqflicker-frames.run58.csv`, SHA-256
`2a35a84d14cdb1ead167b50e73c87e82dd9b4b547c2f11685b546adcb8687bc7`;
its live-written debug log has SHA-256
`1e67d9c46d363113b6325e15a5920a293eec1fceac6757501d44b254d08b2e5f`.
It has 7,286 contiguous presented frames, 0--7285. The five session parts are
**menu** 0--2011 (18.673 s), **load-game frame** 2012 (1.566 s), **loading
screen** 2013--3111 (9.405 s), **first world frame** 3112 (864.484 ms), and
**play** 3113--7285 (62.946 s).

F12 at **play** frame 6705 is a reaction anchor. The full-scene,
collision-active **play** loading pair is frames 6687/6688 at 269.817/57.089
ms, ending 401/344 ms before the press (onset 671/401 ms before it). Frame
6687 loads 23 directional-shadow meshes / 30.392 ms and one shader / 0.882 ms
inside a 33.569 ms directional CPU call. Its directional GPU interval is
87.737 ms and whole-frame GPU interval is 275.310 ms. Frame 6688 loads seven
directional textures / 10.868 ms inside a 13.128 ms directional CPU call; its
directional GPU interval is 18.102 ms. These nested CPU and GPU intervals
overlap and are not additive.

The run-58 omission worked exactly. In **play**,
`engine_shadow_bump_tex_skipped=561,588`, including 2,958 state-0 Resources
that the stock path would have synchronously ensured. There is no unresolved
directional-shadow texture load anywhere in **play**. The marked second frame
has 456 skipped bump bindings, ten state-0, and likewise no unresolved load.
This confirms that the complete formerly unresolved load population in this
session was the `GraphicsMeshInstance+0x18` bump-override path; it did not move
to another shadow caller.

The remaining texture population is also exact. Across **play**, all fourteen
cold direct material textures / 26.215 ms are shader-used
`Name("baseTexture")` entries, exact accepted `GraphicsMeshInstance` joins,
and `base_other`: nine style 3 and five style 4; thirteen pass 0 and one other.
The marked frame contains seven / 10.899 ms: three style 3 and four style 4;
six pass 0 and one other. All context patch failures, unknown partitions, and
table overflow remain zero.

The reason `base_other` is safe to act on is now verified independently rather
than assumed. In `GraphicsMeshInstance::SetShaderParameters`, the generic
`GraphicsMesh::SetShaderParameters` call occurs first. The same live method
then reads instance+`0x14`; if non-null it ensures that Resource and binds it
to the exact static `Name("baseTexture")` before any draw. Run 59 carries the
enclosing instance through the already verified call adapter and omits a
generic getter only when all of these are true:

1. execution is on the main thread inside `RenderDirectional`;
2. the material's complete 16-byte Name equals `baseTexture`;
3. the live instance+`0x14` override is non-null; and
4. that override Resource pointer differs from the generic material Resource.

Returning null at that getter produces only a temporary null shader binding;
the verified stock instance block immediately replaces it with the override.
Every other getter forwards unchanged. The adapter exposes its global context
only on the main directional path, so a concurrent colour/worker invocation
cannot overwrite the live instance pointer. The normal colour pass, instances
without an override, identical Resource pointers, required bump inputs, alpha
base deferral, opaque geometry, culling, map size, and `shadow_split` remain
unchanged.

This is a narrow cleanup of the remaining texture half, not an explanation of
frame 6687's much larger mesh/GPU work. Run 59 measures the two new count
columns `engine_shadow_base_override_skipped` and
`engine_shadow_base_override_skipped_cold`; neither is a duration or charged
to the mod. The change stays within the existing default-off
`shadow_defer_cold_alpha=1` fix, reaches `install()` with the performance probe
off, and brings no trace group.

The complete verifier passes 328 checks. Twenty-five one-at-a-time mutations
of the new RVAs, offsets, verified bytes, relocations, Name identity, pointer
polarity, directional/main-thread scope, forwarding, status, rollback, and
atomic-install dependencies are all rejected: 25/25 for run 59 and 89/89
cumulative relevant mutations. `npm run doctor`, the release build, and the
full off-game self-test pass, including GPU timestamp retirement. Run 59 is
installed from `cache/runs/run59-shadow-base-override.ini`; installed and
source DLLs match at SHA-256
`2d1af22215fd48c5781603a77637cc59b645d9975385630456fb77add3823c20`,
and installed and source INIs match at SHA-256
`1511b3c95d46fdfdd3d1016b55f5d6bde64148e566e2ebbfef1ce21d511a619c`.
The run-58 live CSV and debug log were byte-identical to their archives before
the stale live names were removed. The game was not launched.


## 68. Run 59: all shadow texture loads are gone; cold meshes and GPU queueing remain

Run 59 is archived as `tqflicker-frames.run59.csv`, SHA-256
`e74d2a84ba1c27688b0c0f5e6e3f7d71ae1b99eb532238de530f0385d211807c`;
its live-written debug log has SHA-256
`1f4b413788a4a1de0399bd9bcb60f2d3dc999a7c138d3f965bbacfbde8bbbe01`.
Both archives are byte-identical to the current live names. The CSV contains
7,547 contiguous presented frames, 0--7546. The five session parts are
**menu** 0--1816 (16.947 s), **load-game frame** 1817 (1.485 s), **loading
screen** 1818--3077 (10.781 s), **first world frame** 3078 (934.609 ms), and
**play** 3079--7546 (69.075 s).

F12 at **play** frame 6710 is a reaction anchor. The full-scene,
collision-active **play** burst is frames 6692/6693 at 218.733/149.357 ms,
ending 488/339 ms before the press (onset 707/488 ms before it). Frame 6692
changes shadow region and spends 41.139 ms in
`GraphicsShadowMapDx11::RenderDirectional`. Its 30 nested state-0 loads /
41.062 ms are 29 meshes / 38.219 ms and one shader / 2.843 ms. Five cold root
meshes at the exact `GraphicsMeshInstance::GetNumShadowRenderPasses` boundary
cost 11.111 ms. Its directional GPU interval is 137.047 ms and whole-frame GPU
interval is 329.861 ms. CPU loads, enclosing shadow CPU, and GPU intervals
overlap and must not be added.

Frame 6693 has no resource load and only 1.928 ms of directional-shadow CPU,
yet spends 133.489 ms in `Engine::Render`; its own GPU-frame interval is only
19.067 ms. This is not a newly exposed loader cost. Frame 6692 completed on
the CPU in 218.733 ms while submitting 329.861 ms of GPU work, leaving about
111 ms queued. That remainder plus an ordinary following render accounts for
the 133 ms frame-6693 render interval and has the exact two-frame shape that
§37 directly localized to `DrawIndexed` backpressure. Enabling the existing
per-draw clocks again would remeasure a closed mechanism rather than choose a
new fix.

The run-59 omission itself is decisive. Across **play**,
`engine_shadow_base_override_skipped=737,873`, including 3,535 cold generic
base Resources. Directional-shadow texture loads are zero: direct material,
unresolved, and every other caller bucket are all zero. The existing unused
material and bump filters respectively skip 1,024,644 / 10,875 cold and
700,948 / 5,326 cold bindings. Thus the previous shadow texture classes did
not move elsewhere; all of them are absent from the measured load population.
All 3,321 directional builds report the context patch active, and every
context failure, enqueue failure, and table-overflow counter is zero.

There is no gross steady-scene overhead. Restricting runs 58 and 59 to
collision-active full-scene **play** frames under 60 ms, with no shadow load
or region change, run-59 minus run-58 mean directional CPU is -0.065, +0.049,
+0.183, and -0.004 ms in indexed-draw bands 500--999, 1000--1499,
1500--1999, and 2000-plus. The signs and route populations are mixed; this is
only an exclusion of a large regression, not a fine performance claim.

The remaining directional resource population in **play** is now 100 meshes /
100.932 ms and five shaders / 4.990 ms. The marked onset also retains the
large directional GPU interval that creates the following frame's queue
drain. The next supported behavior boundary is the root-mesh pass-count method
already verified in §53: when the exact `GraphicsMeshInstance` root mesh is in
state 0/1, explicitly enqueue state 0 and return zero passes until state 2.
This would omit one cold caster before its record, dependent resources, and
draws exist, rather than reusing or dropping the entire map. It can therefore
test both the 11.111 ms direct root wait and any downstream mesh/GPU work owned
by those casters. The normal colour pass, resident casters, map size, culling,
and `shadow_split` must remain unchanged. No visual observation accompanied
the completion message, so run 59 supplies no new artifact-safety claim.


## 108. Run 85 confirms the fix and proves self-arming without the reflection proxy

Run 85 is archived as `tqflicker-frames.run85.csv`, SHA-256
`61a90305a13a833f1ae08aabf1dd1e27cb1cd787da58f16534493fc53e8d4a97`,
and `tqflicker-debug.run85.log`, SHA-256
`1bddaaafd7f2c9c6ea7b81edba8cc4cf117dde1f3ec99bfef8341b6bc684ca67`.
Both archives were compared byte-for-byte with the completed live files. The
result-annotated Run-85 INI is SHA-256
`e352417f76d4859b2ccb7469564ffc68da472a8bcf689e364c97dc0d421b91e3`;
the exact launch-time INI hash is retained in section 107. The
CSV has 7,305 contiguous frames, 0--7304. Its five parts are **menu** 0--1902,
**load-game frame** 1903, **loading screen** 1904--2995, **first world frame**
2996, and **play** 2997--7304.

F12 is **play** frame 6583 at 22.240 ms. As in Run 84, this is a deliberate
old-location marker without a felt hitch, not a human-reaction marker. The
exact old transition class is **play** frame 6490, 1.917 s before F12: it
changes the directional-shadow region, creates 60 buffers in the whole frame,
and runs the second-manager / first-plane reflection `BuildScene`. The user
again reports that no old stutter was noticeable.

Frame 6490 is 40.117 ms CPU / 40.780 ms GPU. Exact reflection is 0.128 ms GPU,
directional shadow is 19.090 ms GPU, and terrain ground is 0.765 ms GPU. As a
same-run reference, 90 collision-active, full-scene **play** frames 6493--6582
under 60 ms in the 1,200--1,699 indexed-draw band have medians of 20.484 ms
CPU, 20.471 ms GPU, 6.971 ms directional GPU, 1.021 ms terrain-ground GPU,
and 0.315 ms exact reflection GPU. The transition surcharge is 19.633 ms CPU
and 20.309 ms GPU. This repeats Run 84's positive result using each run's
controlled local class rather than an across-run p50.

The self-arming behavior is exact. It records one trigger in the whole
session, on **first world frame** 2996: eight new identities are admitted and
449 pending directional draw calls are suppressed. The exact reflection
>=32-buffer counter is zero on that frame and zero for the entire session.
Although the frame also has a shadow-region change, Run 85's verified code no
longer reads that event as an admission trigger. Later region changes do not
increment the one-shot trigger counter.

At the marked **play** transition, four new reflection plus four new
directional-shadow identities consume the shared budget, and ten further
directional draw calls are suppressed. The next frame admits one reflection
plus seven shadow identities with zero suppression; frame 6492 admits one
further identity. Those frames are 32.247 and 20.943 ms CPU / 20.330 and
20.889 ms GPU, so the transition has no large postponed rebound. A separate
**play** population beginning at frame 6616 proves the generalized class in
the data: it has no region change and no reflection threshold, admits eight,
and suppresses 102 pending shadow calls. That population is spread through
frame 6631; none of those frames exceeds 59.033 ms. It has no felt marker and
is not claimed as a subjective event.

Across the session there are 85 newly admitted reflection identities and
1,336 newly admitted directional-shadow identities. The shared per-frame
admission count never exceeds eight, the identity table has zero overflow,
and the rejected mesh-only and whole-reflection omission counters remain
zero. The exact marked-area class plus the user's second positive report now
support keeping progressive secondary-pass admission and rejecting more trace
or another reflection omission. The next confirmation should use the same
budget eight with `performance_trace=0`: normal play must preserve the result
without the measurement load, and that boot exercises the newly independent
trace-off Draw-hook path.


## 107. Run 85 self-arms on the controlled population instead of transition proxies

Run 84 is the first positive subjective and measured result for the old
**play** transition: the user reports that it appears fixed or is no longer
visible, while the exact old event falls to 38.229 ms CPU / 31.758 ms GPU and
spreads 39 newly admitted secondary identities over five frames without a
one-frame rebound. Section 106 contains the five session parts and controlled
same-run comparison.

Run 85 changes only how default-off
`[performance] secondary_pass_admission_budget=8` begins controlling new
objects. The first eight previously unseen identities encountered across the
exact reflection and directional-shadow classes in a presented frame render
normally and become globally admitted. The ninth proves that the actual
controlled population exceeds the budget, records the one session arming
event, remains pending, and retries against the next presented frame's shared
budget. An identity already admitted through one secondary consumer spends no
slot in the other. Once armed, the state remains explicit, but the same budget
rule itself handles every later population without needing another trigger.

The >=32-buffer reflection build and a change between two non-null directional
shadow-region pointers no longer arm this behavior. They remain trace
telemetry and continue to drive only their older default-off experiments where
applicable. A trace-off progressive-admission boot therefore no longer
requests the D3D11 `CreateBuffer` vtable hook. This covers a scene transition
with no reflection, fewer than 32 created buffers, or no region-pointer change
without inventing a broader scene-transition proxy.

An install audit also closed a pre-existing fail-open gap. Progressive
admission now independently requests the mod's `Draw` and `DrawIndexed` vtable
wrappers even if every visual feature and trace group is disabled. Visual
installation publishes readiness only when both slots patch successfully;
the Engine-side reflection, directional, terrain, and mesh behavior becomes
active only with that readiness plus all three existing verified Engine
dependencies. Failure of either draw slot leaves all game draws stock. No new
Engine target, byte table, or detour is added.

The self-test now proves that two shared identities fit a synthetic budget of
two without arming, identity three self-arms and remains pending in both
secondary consumers, and that identity enters on the next presented frame.
It also proves that the progressive option no longer asks for the reflection
buffer signal. The verifier checks the pressure ordering, removal of both
proxy calls, CreateBuffer independence, draw-hook request/readiness ordering,
atomic Engine activation, and shutdown reset. All 784 site checks, doctor, the
766,464-byte release build, and the complete off-game self-test pass; GPU
timestamp retirement passes with one non-flushing poll and one post-flush
poll.

Run 85 is installed from
`cache/runs/run85-secondary-self-arming-budget8.ini`. Source and installed
DLLs are byte-identical at SHA-256
`d654f91f5f98e8c931501fb4cf27554a9ed0f251432e6727f34c6a84c6033569`;
launch-time source and installed INIs were byte-identical at SHA-256
`b9aec8863f2ed6bf27fc6bd54030c8e909bd6c7aaeb052f2e5a043ff3fca2f41`.
The archived INI header is annotated with the completed result in section 108.
No stale live CSV or debug log existed, and the game had not been launched.


## 106. Run 84 removes the marked-area burst; transition proxies are no longer the right admission gate

Run 84 is archived as `tqflicker-frames.run84.csv`, SHA-256
`ef3a9372e7d03b8cedfc234424dcf183a8456621a0317b165aea54f7e201f8e9`,
and `tqflicker-debug.run84.log`, SHA-256
`91814fc42cd9ef8118fa0ecac44166d6622cbbe81c91937d3f879a75497fb149`.
Both archives were compared byte-for-byte with the completed live files. The
result-annotated Run-84 INI is SHA-256
`0aa75236405ade7ad422fb727cb859b2a064e5becffb09c245431d963b57c46c`;
the exact launch-time INI hash is retained in section 105. The
CSV has 7,310 contiguous frames, 0--7309. Its five parts are **menu** 0--2141,
**load-game frame** 2142, **loading screen** 2143--3205, **first world frame**
3206, and **play** 3207--7309.

F12 is **play** frame 6827 at 22.249 ms. This press was deliberately made at
the old route location even though no hitch was apparent, not as a reaction
to a felt event, so the marker has no reaction-window candidate. The exact
old transition class is nevertheless unambiguous: **play** frame 6764, 1.320
s before F12, changes the directional-shadow region, creates 93 buffers, and
runs the second-manager / first-plane reflection `BuildScene`. The user's
primary result is that the old stutter now appears fixed or is no longer
visible.

Frame 6764 is 38.229 ms CPU / 31.758 ms GPU. Exact reflection is 0.762 ms GPU,
directional shadow is 15.168 ms GPU, and terrain ground is 2.147 ms GPU. As a
same-run reference, 58 collision-active, full-scene **play** frames 6769--6826
under 60 ms in the 1,300--1,699 indexed-draw band have medians of 20.681 ms
CPU, 20.646 ms GPU, 7.062 ms directional GPU, 1.113 ms terrain-ground GPU,
and 0.328 ms exact reflection GPU. The transition surcharge is therefore
17.548 ms CPU and 11.112 ms GPU. Run 83's old marked transition candidate had
a 75.367 ms same-run CPU surcharge and a 79.260 ms same-run GPU surcharge;
this comparison uses each run's controlled local reference, not a p50 across
routes.

The behavior fired and spread rather than merely moving the work once. At
frame 6764 it admits eight new identities and suppresses 55 pending secondary
draw calls. Frames 6765--6768 admit 8, 8, 8, and 7 further identities; they
are 28.645, 21.397, 24.939, and 24.502 ms. Pending draw suppression falls to
28, 20, 8, and zero. Thus 39 identities are admitted over five frames with no
later rebound in this transition. The shared eight-object limit is never
exceeded anywhere in the CSV, and the 8,192-slot identity table reports zero
overflow.

The current arming condition is less restrictive in practice than its
reflection wording suggests. A directional-region change arms the behavior
on **first world frame** 3206, after which it remains armed for the session.
The marked frame reports two trigger events because it has both another
directional-region change and a >=32-buffer reflection build, but neither is
needed to re-arm it. **Play** frame 4545 demonstrates the non-reflection case:
its directional-region change has no reflection call, and the already-armed
budget spreads its pending directional population over frames 4545--4549.

The user's general objection is still correct: neither a 32-buffer reflection
build nor a change between two non-null shadow-region pointers is a universal
definition of a scene-admission burst. The next implementation should stop
trying to name the transition. In either exact reflection or directional-
shadow context, count previously unseen shared renderable identities in the
current presented frame; allow the first `N` stock, and self-arm when identity
`N+1` proves that an actual backlog exists. Subsequent identities are pending
under the existing shared budget. This directly measures the work being
controlled, covers transitions with no reflection and no region-pointer
change, and can remove the CreateBuffer dependency from a trace-off behavior
install. Region changes and reflection buffer counts can remain trace
telemetry but should no longer control the fix.

Do not combine either rejected reflection-defer experiment with this result.
Runs 81 and 82 moved one reflection consumer's complete work to a later
consumer/frame and produced no subjective improvement. Run 84 instead leaves
Resource/material preparation in place and reduces exact reflection at the
marked transition to 0.762 ms GPU by budgeting the actual draws. Skipping the
mesh reflection or whole reflection once would now duplicate that treatment,
delay useful preparation, and add one-frame stale reflection for no measured
benefit.


## 105. Run 83 rejects raw first-seen count and prepares coordinated progressive secondary-pass admission

Run 83 is archived as `tqflicker-frames.run83.csv`, SHA-256
`91a3336771bc1e18ab03850fcaa9edaf120899deafc46ccfaf25d5a0a2758658`,
and `tqflicker-debug.run83.log`, SHA-256
`765278ffba1089e26bd1fcedd79c2c3d11c2d871e2ed3caff5c304df1d3eb2f9`.
Both archives were compared byte-for-byte with the completed live files. The
CSV has 6,885 contiguous frames, 0--6884. Its five parts are **menu** 0--1650,
**load-game frame** 1651, **loading screen** 1652--2790, **first world frame**
2791, and **play** 2792--6884.

F12 is **play** frame 6316 at 35.349 ms. Frame 6315 is 80.927 ms but ends only
35 ms before the key is retrieved; its onset is only 116 ms before the marker,
so it cannot be selected as the user's reaction target. The plausible felt
event is **play** frame 6293 at 97.060 ms, ending 604 ms before F12. The user
reports that the session still feels the same. Run 83 is passive, so that
subjective result is a baseline rather than a behavior rejection.

The new identity trace is complete at that event and has zero table overflow.
Exact second-manager / first-plane reflection contains 288 draws and sees 70
`TerrainPlug` calls, 24 `TerrainBlock` calls, and 144
`GraphicsMeshInstance` calls. Their first-in-this-exact-consumer counts are
12, 4, and 118: 134 previously unseen reflection renderables arrive on one
frame. Exact directional shadow contains 762 mesh calls but only 15 first
visits. Both exact second-owner geometry consumers contain zero first visits;
the scene child has 42 unclassified draws. Thus the marked population is not
a large wave entering all four consumers together.

Raw first-seen count is not itself a cost model. Another **play** region
change, frame 4144, admits 125 first-seen meshes to exact second-owner geometry
setup and 40 to directional shadow but is only 20.344 ms CPU / 25.846 ms GPU.
Frame 6360 admits 100 first-seen directional meshes in 22.638 ms CPU /
23.807 ms GPU. The candidate's distinguishing fact is first GPU/resource use
of a particular cold population and pass state, not merely the number of new
object pointers.

At **play** frame 6293, exact reflection `BuildScene` is 19.322 ms CPU and
creates 176 buffers. Its following exact `RenderLightStyle` is 41.786 ms CPU /
54.860 ms GPU. Sixteen nested main-thread Resources take 34.934 ms, including
four meshes and twelve textures; fourteen texture creations take 23.445 ms.
The retained names include new NPC/debris meshes and textures plus the two
known Gadir rocky-pebbles terrain textures. Reflection's 20-draw GPU bins are
not uniform: bins 1, 7, and 8 cost 25.064, 13.128, and 7.334 ms. The first is
all `GraphicsMeshInstance`; the seventh is eighteen mesh calls; the eighth is
eight `TerrainPlug` calls. Six fresh buffers are observed in both reflection
and directional shadow on the same frame, but no large shared-buffer
population reappears.

Fourteen nearby collision-active, full-scene **play** frames 6300--6314 under
60 ms, with 1,400--1,699 indexed draws, no main Resource load, no region
change, and no off-main texture creation, average 21.693 ms total, 16.988 ms
exact `Engine::Render`, 6.604 ms game draw submission, 21.974 ms whole GPU,
7.877 ms directional GPU, 1.055 ms terrain-ground GPU, 0.970/0.941 ms exact
second-owner geometry-setup/scene GPU, and 0.260 ms exact reflection GPU.
Relative to this same-run local class, frame 6293 adds 75.367 ms total, 76.812
ms render, 11.817 ms draw submission, 79.260 ms whole GPU, 15.640 ms
directional GPU, 21.904 ms terrain-ground GPU, and 54.619 ms reflection GPU.
These nested/queued intervals are not additive.

The fresh correction is that no single named renderer is “the stutter.” The
native engine makes a region's cold GPU population eligible without a frame
budget. CPU Resource realization, reflection, terrain ground, and directional
shadow therefore submit together; later game draws become queue-drain points.
Which exact pass owns the longest timestamp changes across identical routes
because the immediate D3D11 queue serializes the burst differently. This
mechanism applies to native Windows D3D11 as well as Wine/CrossOver and does
not require a host-round-trip explanation.

Runs 81--82 did not test progressive GPU admission. They omitted one
reflection consumer and returned all postponed work together on the next
consumer/frame, where the felt burst remained. Run 84 instead adds default-off
`[performance] secondary_pass_admission_budget=8`. After the existing exact
>=32-buffer reflection signal or a directional-region change, reflection and
directional shadow share one budget of eight newly encountered renderable
identities per frame. `TerrainPlug`, `TerrainBlock`, and
`GraphicsMeshInstance::RenderPass` still execute Resource/material setup, but
their game `Draw`/`DrawIndexed` calls are suppressed while an identity is
pending. Already admitted objects and the normal colour class stay stock. A
pending identity competes again on the next frame, so 134 objects are spread
over roughly seventeen visible frames rather than all returning one frame
later.

This adds no Engine byte target. It reuses the verified reflection
`BuildScene`/`RenderLightStyle` `patchCall` sites, the exact directional-shadow
wrapper, the existing 19-byte `TerrainPlug`/`TerrainBlock` entries with six
bytes stolen, the 24-byte `GraphicsMeshInstance::RenderPass` entry with six
bytes stolen, and the mod-owned D3D11 Draw vtable hooks. The behavior reaches
`install()` with the performance probe off and brings no trace group. Six new
CSV fields are counts only. The existing 8,192-slot / 16-probe identity table
also carries pending/admitted state and fails open to stock rendering on
overflow. A separate Present-driven frame serial keeps the budget functional
when `performance_trace=0`.

The extended verifier passes 784 checks. Changing the new budget upper bound
from 64 to 65 fails it. Doctor, the 766,464-byte release build, and the complete
off-game self-test pass, including GPU timestamp retirement and the synthetic
two-object shared budget / next-frame pending admission. Run 84 is installed
from `cache/runs/run84-secondary-pass-admission-budget8.ini`. Source and
installed DLLs are byte-identical at SHA-256
`dad27c4f84f667f7577a540f88519869993b187a1b64a26c43c3360a1f0e6dd5`;
launch-time source and installed INIs were byte-identical at SHA-256
`dd1e2661c5c9e650c00c9183ae53287568becabbadc774c59f3357f1a8e51b69`.
Run 83's live CSV/debug log matched their archives immediately before both
stale live names were removed. The game has not been launched.


## 104. Run 82 rejects whole-reflection omission; Run 83 traces first-seen renderables across every consumer

Run 82 is archived as `tqflicker-frames.run82.csv`, SHA-256
`dd4c8083f39b0574851819795c39d9d48321fec5a5e91da65d26cb9dbe66f235`,
and `tqflicker-debug.run82.log`, SHA-256
`d786d6e79c95a3cd1413a547cea9befd24540ad44f5b5995c1d3e14da1daef1c`.
Both archives were compared byte-for-byte with the completed live files. The
CSV has 7,004 contiguous frames, 0--7003. Its five parts are **menu** 0--1704
(16.308 s), **load-game frame** 1705 (1,269.2 ms), **loading screen**
1706--2873 (9.800 s), **first world frame** 2874 (588.7 ms), and **play**
2875--7003 (63.294 s).

F12 is **play** frame 6436 at 25.336 ms. The plausible felt pair is **play**
frames 6417/6418 at 79.752/165.151 ms, ending 569/404 ms before the marker.
The user again reports that the stutter **feels the same**. This subjective
failure is the primary result.

The whole-child behavior fired exactly once at the reaction candidate. Play
frame 6417 creates 98 buffers, 87 inside exact second-manager / first-plane
reflection `BuildScene`, crosses the fixed 32-buffer gate, and records
`engine_reflection_admission_all_deferred=1`. The complete following
`RenderLightStyle` is omitted: its wrapper takes 37 us and its exact GPU
interval is only 0.042 ms. Thus Run 82 is a valid rejection of complete
one-frame reflection omission, not another missed trigger.

The removed work migrates to later consumers. On play frame 6417, the exact
second deferred owner's geometry-setup scene-list class costs 39.381 ms GPU,
terrain ground costs 20.157 ms GPU, and exact
`GraphicsShadowMapDx11::RenderDirectional` costs 114.014 ms GPU across 755
draws. The whole frame is 79.752 ms CPU / 203.690 ms GPU and spends 49.020 ms
in game draw submission. On frame 6418, whole GPU work has fallen to 26.780
ms, but the exact second-owner geometry-scene class spends 125.166 ms CPU,
including 123.574 ms in its game draw calls; total game draw submission is
125.098 ms. This is first-use production followed by queue backpressure even
with reflection absent.

Thirteen nearby collision-active, full-scene **play** frames 6421--6435 under
60 ms, with 1,400--1,699 indexed draws, no main Resource load, no region
change, and no off-main texture creation, average 22.327 ms total, 17.877 ms
exact `Engine::Render`, 7.068 ms game draw submission, 22.447 ms whole GPU,
7.999 ms directional GPU, 1.072 ms terrain-ground GPU, 0.998 ms exact second-
owner geometry-setup GPU, 1.042 ms exact second-owner geometry-scene GPU, and
0.271 ms exact reflection GPU. This is a within-run post-admission class, not
a cross-run p50.

Texture realization again follows rather than causes onset. The skipped
reflection child performs no main-thread Resource load on frame 6417. On frame
6418, when reflection returns, its two Gadir terrain Resources cost 10.145 ms,
including 7.940 ms texture creation. From frames 6418--6436, 50 off-main
texture creations take 67.988 ms of overlapping loader-thread time. Neither
population can explain the already completed frame-6417 GPU producer.

The correction is architectural but does not yet justify a renderer rewrite.
The >=32-buffer reflection `BuildScene` event is a reliable observation point
for a wider scene-admission transition; it is not the unique owner of the
cost. Omitting the first consumer merely transfers cold GPU work to the exact
second-owner scene list, terrain ground, and directional shadow. A further
reflection-only A/B, texture throttle, archive prefetch, or lower-mip change
would therefore test the wrong boundary. The next decision is whether newly
seen directional casters can be admitted progressively, or whether first use
is broad enough to require staging the whole scene.

Run 83 is passive and answers that decision in one route. The existing exact
`TerrainPlug`, `TerrainBlock`, and `GraphicsMeshInstance::RenderPass` wrappers
now count their calls and object identities independently in four exact
consumers: second-manager / first-plane reflection, second-owner geometry
setup, second-owner geometry scene, and
`GraphicsShadowMapDx11::RenderDirectional`. Each `_first` count means that
object has never previously reached that exact consumer in the session.
Per-consumer draw counts expose unnamed remainder. The bounded 8,192-entry,
16-probe identity table reports overflow and adds no clock, D3D getter, GPU
query, Engine patch, or behavior change. Header and rows now share one audited
32-KiB line bound. Independent perturbations of 8,192, 16, the identity hash
salt, and 32,768 are all rejected by the verifier.

The extended verifier passes. Doctor, the 763,904-byte release build, and the
complete off-game self-test pass, including GPU timestamp retirement and
first-once-per-class/per-consumer identity behavior. Run 83 is installed from
`cache/runs/run83-admission-consumer-identities.ini`. Source and installed
DLLs are byte-identical at SHA-256
`5ba7abeaa4b2f66527f9420697e26d09dafbcb3d94f22f7a94fae43e1e7a71e8`;
source and installed INIs are byte-identical at SHA-256
`a5a356546fd025482f3ad659a9ec1deb6f04b57669e1fd14f30322314feec8b0`.
The completed Run-82 live outputs matched their archives before both stale
live names were removed. The game has not been launched.


## 103. Run 81 rejects mesh-only reflection staging; Run 82 skips the complete admitted reflection once

Run 81 is archived as `tqflicker-frames.run81.csv`, SHA-256
`801c30ef033205f4be9de082b26aa08769f1b27b583f6dc123242c81f762ccce`,
and `tqflicker-debug.run81.log`, SHA-256
`5ce41f83503a603a1b90fb43161ab98382104bc64f8518271d00794208604692`.
Both archives were compared byte-for-byte with the completed live files. The
CSV has 6,939 contiguous frames, 0--6938. Its five parts are **menu** 0--1933
(18.441 s), **load-game frame** 1934 (1,517.0 ms), **loading screen**
1935--3024 (9.525 s), **first world frame** 3025 (761.5 ms), and **play**
3026--6938 (61.942 s).

F12 is **play** frame 6409 at 24.416 ms. The plausible felt event is the
**play** pair 6388/6389 at 115.778/138.573 ms, ending 579/440 ms before the
marker. The user reports **no perceptible change in the stutter**. That report
is the primary result; neither a maximum nor the nearest frame supersedes it.

The behavior fired exactly once, on play frame 6388. Its reflection
`BuildScene` created 91 of the frame's 105 buffers and crossed the fixed
32-buffer gate. The following exact reflection `RenderLightStyle` omitted all
87 `GraphicsMeshInstance` calls. Its retained class totals prove zero mesh
calls/draws, 50 `TerrainPlug` calls/draws, and 18 `TerrainBlock` calls/draws.
This is therefore a valid rejection of the mesh-only treatment, not a missed
trigger or a different route event.

What survived is larger than the omitted class. Exact second-manager / first-
plane reflection still costs 41.959 ms GPU; its six populated chunks total
41.920 ms, all from the terrain-only range. The first three chunks cost
26.862, 8.739, and 5.816 ms. One `TerrainBlock` call spends 9.699 ms in two
Resource loads, including 6.711 ms of texture creation, but that nested CPU
work must not be added to the GPU chunks. Directional shadow is a separate
81.536 ms GPU producer. The later exact second-owner geometry-scene class
spends 52.994 ms CPU, including 51.871 ms in game draw submission, for only
13.910 ms GPU. Frame 6388 is 115.778 ms CPU / 214.312 ms GPU; frame 6389 then
blocks 113.445 ms in game draw submission. This is again producer followed by
queue backpressure, now with the mesh-reflection population proved absent.

Twelve nearby collision-active, full-scene **play** frames 6391--6408 under
60 ms, with 1,400--1,699 indexed draws, no main Resource load, no region
change, and no off-main texture creation, average 21.348 ms total, 16.889 ms
exact `Engine::Render`, 7.085 ms game draw submission, 21.360 ms whole GPU,
7.591 ms directional GPU, 0.237 ms exact reflection GPU, and 0.820 ms exact
second-owner geometry-scene GPU. They are a within-run post-admission class,
not a cross-run p50. Against it, play frame 6388 adds 94.430 ms total,
46.079 ms game draw submission, 192.952 ms whole GPU, 73.945 ms directional
GPU, and 41.722 ms exact reflection GPU.

The texture tail remains secondary to onset. No off-main texture creation is
charged to frame 6388. From play frames 6389--6407, 49 off-main initial-data
texture creations take 66.054 ms of overlapping loader-thread time. The
during-session debug file reaches its bounded output size while reporting
later retained chunks, but it contains the complete frame-6388 class report;
the CSV counters and timings are complete.

Run 82 adds default-off
`[performance] reflection_defer_admission_all=1`. It uses the same exact
successful-buffer count in reflection `BuildScene`, but at 32 skips the one
immediately following whole reflection `RenderLightStyle` call. Terrain and
mesh reflection return on the next frame. Directional shadows, the main
colour pass, resource loading, culling, and `shadow_split` are untouched. A
one-frame stale water reflection is the explicit visual trade-off to report.
With tracing off this behavior needs only the already verified `patchCall`
sites at Engine RVAs `0x186501` and `0x18694d`; it does not install the mesh
entry detour or any trace group. The new CSV count
`engine_reflection_admission_all_deferred` is not a duration.

The extended verifier passes, including the conditional mesh-detour
dependency and the exact shared 32-buffer boundary. Doctor, the 761,856-byte
release build, and the complete off-game self-test pass, including GPU
timestamp retirement. Run 82 is installed from
`cache/runs/run82-reflection-admission-all.ini`. Source and installed DLLs are
byte-identical at SHA-256
`85e1b1e48c2f616446f9e51dd767c1bb441d59ce51ca2187549a87ecdb758a7d`;
source and installed INIs are byte-identical at SHA-256
`9d9b9f2e33d0fd44bc3f2a763797dd9600b5d69a36a3b73a17a5a398cba90ac2`.
The completed Run-81 live outputs matched their archives before both stale
live names were removed. The game has not been launched.


## 102. Run 80 separates the reflection onset from the texture tail; Run 81 stages mesh reflection

Run 80 is archived as `tqflicker-frames.run80.csv`, SHA-256
`68ca3bd58f89dde72a64b31fbbe337acb070cc8d552ccf9c75d0e6f6c88c9b19`,
and `tqflicker-debug.run80.log`, SHA-256
`6fd82332ad539ad94cdf7d437ea97b9cd311cc0527ec4aadb9eeabfe099bb365`.
Both archives were compared byte-for-byte with the completed live files. The
CSV has 7,495 contiguous presented frames, 0--7494. Its five parts are
**menu** 0--2191 (20.183 s), **load-game frame** 2192 (1,492.219 ms),
**loading screen** 2193--3337 (9.811 s), **first world frame** 3338
(680.377 ms), and **play** 3339--7494 (63.023 s).

F12 is **play** frame 6919 at 22.061 ms. The probable felt event is the
full-scene, collision-active **play** pair 6895/6896 at 80.635/41.034 ms,
ending 534/493 ms before F12. Their onset-to-marker distances are 614/534 ms.
The user reported only completion, so this remains a reaction-window
identification rather than a claim that the marker proves the pair.

The onset is now exact. **Play** frame 6895 changes shadow region, jumps from
214 to 1,445 indexed draws, and creates 104 buffers / 3,550,400 bytes. The
second-manager/first-plane reflection `BuildScene` owns 95 of those creations
and takes 8.896 ms CPU. Its following exact `RenderLightStyle` takes
10.479 ms CPU / 35.892 ms GPU across 211 draws. The later exact second-owner
deferred geometry-scene class then blocks for 44.927 ms CPU, including
43.473 ms in game `Draw` / `DrawIndexed`, while producing only 2.175 ms GPU.
That is the same-frame submission drain after the reflection producer, not a
second GPU producer. The whole frame is 80.635 ms CPU / 83.652 ms GPU;
directional shadow separately contributes 24.561 ms GPU.

The new class partition overturns the remaining texture-proximity inference.
Two Gadir terrain texture Resources cost 7.865 ms and four nested texture
creations cost 6.115 ms inside one hot `TerrainBlock` call at reflection draw
173. But the 20-draw GPU bin containing that call costs only 0.495 ms. The
large reflection ranges occur earlier: four all-`GraphicsMeshInstance` bins
covering draws 21--100 cost 3.065, 8.094, 4.339, and 1.347 ms, and the next
bin, containing eighteen `TerrainPlug` and two `GraphicsMeshInstance` calls,
costs 14.451 ms. Therefore cold terrain texture creation is a real CPU cost
but does not own this 35.892 ms reflection GPU interval.

The off-main descriptor trace names a second, later mechanism. No off-main
texture creation starts on frame 6895. From frames 6896--6901, one loader
thread makes 45 successful, initial-data, fully-mipped compressed textures in
64.082 ms of overlapping worker time: 37 `DXGI_FORMAT_BC3_UNORM`, six BC1,
and two BC2. Their mip-chain payload is about 168.668 MiB, including three
4096-square textures. Thus texture realization can amplify the multi-frame
tail, but it cannot cause the first 80.635 ms frame. A texture throttle alone
would miss the principal onset; lower-mip staging remains a separate, more
invasive tail experiment rather than the first fix.

The within-run reference is the nine collision-active, full-scene **play**
frames 6908--6916, all under 60 ms, with 1,400--1,699 indexed draws, no main
Resource load, no region change, and no off-main texture creation. Their means
are 20.423 ms total, 15.622 ms exact `Engine::Render`, 8.256 ms game draw
submission, 20.735 ms whole GPU, 6.900 ms directional GPU, and 0.269 ms exact
second-plane reflection GPU. Frame 6895 is therefore +60.212 ms total,
+36.213 ms game draw submission, +62.917 ms whole GPU, and +35.662 ms exact
reflection GPU relative to that same-scene class. These nested and queued
scopes are not added.

The buffer count supplies a stable, machine-independent behavior boundary.
For the primary transition frame in Runs 73--80, exact second-plane reflection
created 69, 64, 132, 63, 172, 80, 87, and 95 buffers. The largest neighboring
Run-80 population was 30. Seven of those eight transition frames had
24.751--63.225 ms of exact reflection GPU work; Run 79's 0.440 ms occurrence
is the measured exception. A threshold of 32 therefore selects the repeated
admission class without using elapsed time or ordinary draw count.

Run 81 adds default-off
`[performance] reflection_defer_admission_mesh=1`. The already verified exact
reflection `BuildScene` call counts successful D3D buffer creations through
the existing device vtable proxy. At 32, only `GraphicsMeshInstance` calls in
the immediately following exact reflection `RenderLightStyle` are omitted.
Terrain reflection remains, the normal colour pass later in the same recursive
branch is untouched, and mesh reflection returns on the next frame. This is a
narrow one-frame colour-first staging A/B, not whole-reflection reuse, culling,
or a loader rewrite. It specifically attacks the four proved hot mesh bins;
it does not claim to remove the independent TerrainPlug range or the later
off-main texture tail.

The behavior reuses the exact `patchCall` sites at Engine RVAs `0x186501` and
`0x18694d` plus the already verified `GraphicsMeshInstance::RenderPass` entry
at `0x172dd0`. That shared six-byte prologue is verified across 24 bytes with
its relocation and only six complete bytes are stolen. With tracing off the
manager/plane trace calls are not patched, no clock or GPU query runs, and the
fix installs only its behavior dependencies. New CSV fields
`engine_reflection_admission_deferred` and
`engine_reflection_admission_mesh_deferred` are counts, not durations.

The extended verifier passes 771 checks. Independently changing the new
32-buffer constant to 33 fails the exact behavior-boundary check. Doctor, the
760,320-byte release build, and the complete off-game self-test pass, including
GPU timestamp retirement. Run 81 is installed from
`cache/runs/run81-reflection-admission-mesh.ini`. Source and installed DLLs are
byte-identical at SHA-256
`d1856797e4e9d0870ccf955c930ead8823a11ce5b0babff997f1d79201eb9545`;
source and installed INIs are byte-identical at SHA-256
`2890b546c468625797ab01af0f906a8f6e1d39a22487d15ed660de435cd7dfa3`.
Relative to Run 80's measurement settings, only
`reflection_defer_admission_mesh=1` changes behavior. Relative to the normal
live config, full trace, draw timing, and F12 are the explicit measurement
settings. The completed Run-80 live files matched their archives before both
stale live names were removed. The game has not been launched.


## 101. Run 79 rejects a reflection-renderable fix for this burst; Run 80 captures texture realization

Run 79 is archived as `tqflicker-frames.run79.csv`, SHA-256
`594a45b9b4946ec5fa53f35ea310d302f3333043c312114fab7c8d6014713745`,
and `tqflicker-debug.run79.log`, SHA-256
`3fc45ff8c63cf2f51e08ccea2092f38ccb187850e65198756662bf5d81b84b43`.
Both archives were compared byte-for-byte with the completed live files. The
CSV has 6,900 contiguous frames, 0--6899. Its five parts are **menu**
0--1786, **load-game frame** 1787, **loading screen** 1788--2907, **first
world frame** 2908, and **play** 2909--6899.

F12 is **play** frame 6416 at 19.364 ms. **Play** frame 6415 is 53.202 ms but
ended only 19 ms before the marker, so it cannot be a human-reaction target.
The probable felt event is the eight-frame **play** burst 6382--6389 at
34.525, 29.895, 33.586, 29.922, 22.919, 22.811, 22.405, and 35.452 ms. It
ends 790 through 593 ms before F12. The reporter supplied only completion, so
this is a reaction-window identification, not a claim that the marker proves
which frame was felt.

This occurrence is a scene-admission transition. Immediately before it,
**play** frame 6381 has 213 indexed draws, 0.111 ms in the game's draw-submit
class, and an 8.245 ms whole-frame GPU interval. Frame 6382 changes region and
jumps to 1,436 indexed draws. It creates 98 buffers / 3,382,400 bytes, though
the `CreateBuffer` calls themselves cost only 0.834 ms. Two exact Gadir
terrain textures load synchronously on the main render thread for 5.595 ms,
including 2.951 ms of nested texture creation. Its game-owned draw-submit
class rises to 8.909 ms and its whole GPU interval to 29.628 ms, including
13.720 ms in the exact directional-shadow class.

The consolidated trace rejects the proposed reflection-renderable deferral
for this occurrence. The exact second-manager/first-plane
`GraphicsForwardRenderer::BuildScene` selector costs 8.010 ms on frame 6382,
and its following exact `GraphicsForwardRenderer::RenderLightStyle` costs
7.438 ms CPU because it contains the two cold Gadir loads. But all ten
populated 20-draw bins total about 0.392 ms GPU, agreeing with the whole exact
child's 0.391 ms GPU interval. The nearby texture loads therefore do not own
the GPU burst in this run. Deferring that reflection child would at most move
its synchronous CPU load; it cannot remove the measured multi-frame producer.

The admission continues asynchronously. **Play** frames 6384--6389 contain
51 off-main `CreateTexture2D` calls totaling 80.306 ms of worker-side time,
while progressive loose-texture upload advances one bounded step per frame.
Across burst frames 6382--6389, game draw submission is 3.566--14.045 ms and
whole-frame GPU time is 21.251--33.690 ms. Those worker intervals overlap the
render frames and are not added to the CPU or GPU totals. They are nevertheless
the only measured transition-specific Resource activity that persists across
the same six-frame tail, so texture realization/upload pressure is now the
leading fix route, not yet a proved cause.

The within-run reference is 24 collision-active, full-scene **play** frames
6390--6414 under 60 ms, with 1,400--1,699 indexed draws, no main-thread
Resource load, and no region change. Their means are 20.822 ms total,
16.112 ms exact `Engine::Render`, 8.323 ms in game draw submission,
20.895 ms whole-frame GPU, and 7.241 ms directional-shadow GPU. This is a
same-run, same-scene-class reference rather than a cross-run p50. It shows
that most of the post-transition 20 ms cost is the newly visible full scene;
the removable target is the extra 22--35 ms burst while resources become
resident, not the permanent scene price.

Run 79's debug report has one explicit instrumentation failure. It retained
the two qualifying reflection events at frames 6382 and 6392, but emitted the
newer event first. Its 224 per-renderable lines filled the 65,491-byte session
log at call 167, before the older probable reaction event was written. Missing
frame-6382 renderable identities are therefore missing output, not zero work,
and frame 6392's cheap population must not be substituted for them.

Run 80 corrects that output path and adds the one missing admission dimension.
Qualifying reflection events are emitted oldest-first. Every draw bin gains
exact `TerrainPlug` / `TerrainBlock` / `GraphicsMeshInstance` call-and-draw
counts; complete class totals remain, while individual lines are emitted only
for calls with nested work or at least 250 us CPU. A separate lock-free
512-record ring retains successful off-main texture creations with start/end
frame, loader thread, duration, dimensions, mip count, DXGI format, bind/misc
flags, and whether initial data was supplied. F12 emits chronological records
from the preceding 120 frames, bounded to 192 lines with an explicit omission
count. The record is published only after all fields are written and never
inherits a main-thread reflection or deferred owner.

This is passive instrumentation, not a texture throttle and not an archive
prefetch experiment. If the burst is dominated by large/mipped initial-data
textures, the next A/B is a bounded texture-realization budget (or the more
involved archive lower-mip staging path). If it is many small textures, staging
newly visible shadow/color participation is the narrower route. The trace
does not reopen the message pump, archive block cache, Stage 4.2 prefetch,
buffer pooling, locks, sleep, Stage 5, or libdeflate, and leaves
`shadow_split` and all accepted fixes unchanged.

The extended verifier passes 769 checks. Independently perturbing each of the
four new ring/horizon/report constants is rejected, 4/4. The off-game
self-test covers exact descriptor/frame publication and passes, including GPU
timestamp retirement.

Doctor and the 759,296-byte release build also pass. Run 80 is installed from
`cache/runs/run80-admission-texture-descriptors.ini`; the game has not been
launched. Source and installed DLLs are byte-identical at SHA-256
`31ab51813d674b75407b12ede791c72b6b8b1c2e0a5e5359b1f4ffd874131356`;
source and installed INIs are byte-identical at SHA-256
`9704d1ea0859376300314bd45728d475f44fb8cdc0fb9408923e5dc620ebc16f`.
The completed Run-79 live CSV/debug log matched their archives before both
stale live names were removed.


## 100. Run 78 result and Run 79 preparation: cover the whole reflection child and name its renderable class

Run 78 is archived as `tqflicker-frames.run78.csv`, SHA-256
`9da2aa0ef4d278bc21372cb76fb5f40c1b45f55371c4e15af5eaacb23ed85561`,
and `tqflicker-debug.run78.log`, SHA-256
`f647970f9a14de5bcd266f0f22c042e96ee65e5eefd669c225cc9efcd48f8be6`.
Both archives were compared byte-for-byte with the completed live files. The
CSV has 7,271 contiguous presented frames, 0--7270. Its five parts are
**menu** 0--1848, **load-game frame** 1849, **loading screen** 1850--2983,
**first world frame** 2984, and **play** 2985--7270.

F12 is on **play** frame 6728 at 22.443 ms. **Play** frame 6723 is 52.428 ms
but ended only 107 ms before the press, too soon to be a confident human
reaction candidate. The probable felt sequence is **play** frames 6702/6703
at 75.266/41.243 ms, ending 609/568 ms before F12. The user reported only
completion, so this remains a reaction-window identification rather than a
claim that the marker proves which frame was felt.

On **play** frame 6702, the exact second-manager/first-plane
`GraphicsForwardRenderer::RenderLightStyle` class is 9.660 ms CPU / 31.101 ms
GPU after a 7.987 ms `BuildScene`. Two main-thread state-0 Gadir terrain
textures load for 6.111 ms inside that exact reflection cell, including 4.332
ms in four nested texture creations. These clocks nest and must not be added.
The same frame's whole directional-shadow class is independently 31.046 ms
GPU and has a region change; this run does not subdivide it.

The following exact second `GraphicsDeferredRendererX::Render` geometry-scene
class blocks for 36.172 ms CPU, with 35.342 ms inside the game's own draw
submissions, while its GPU interval is only 2.913 ms. Its four largest draws
are 19.122, 9.170, 4.067, and 2.142 ms. **Play** frame 6703 repeats that
shape: 29.778 ms CPU / 28.824 ms in game draws / 1.284 ms GPU, led by a
15.752 ms draw, while reflection is only 0.695 ms CPU / 0.167 ms GPU. This is
evidence for a downstream queue drain after prior GPU production, not for the
deferred-color scene as the producer or fix boundary.

Run 78 also invalidates the moving-window assumption. The selected reflection
child on **play** frame 6702 issues exactly 192 draws. Its Run-78 query window
starts at draw 193, so it opens no populated bin and retains no terrain call.
Later selected **play** frames 6707 and 6711 reach 220 and 276 draws, but their
post-192 bins total only 0.041 and 0.092 ms GPU; their exact late
`TerrainBlock`/`TerrainPlug` calls contain no nested work. Together with Run
77's cheap draws 65--192 and Run 76's different expensive draws 65--169, this
shows that fixed ordinal slices do not follow a stable population. It does
not establish that the unmeasured draws 1--64 are expensive on frame 6702.

Run 79 is therefore one consolidated passive trace before a behavior A/B. It
keeps the same sixteen query pairs and covers exact selected
`RenderLightStyle` draws 1--320 continuously in 20-draw bins. It retains up to
256 exact renderable calls across the same window, classifying the existing
unexported `TerrainPlug` and `TerrainBlock` overrides plus the exported
`GraphicsMeshInstance::RenderPass` override. Each record carries class,
object, draw range, CPU duration, and nested Resource/texture/buffer creation
totals. Existing exact downstream deferred-color slow-draw reporting and the
whole directional-shadow GPU interval remain active, so the same run observes
the reflection producer, the downstream drain, and the separate shadow
producer.

The sorted scene-list executor invokes virtual slot `+0x28`, so there is no
direct `E8` for the mesh override. The installed `Engine.dll` was re-read
before writing the table: the decorated export resolves to RVA `0x172dd0`;
its 24-byte opening is
`55 8b ec 83 e4 f8 81 ec fc 00 00 00 a1 00 f0 36 10 33 c4 89 84 24 f8 00`,
with the relocated security-cookie address at byte 13. The independent
23-byte tail at `0x173127` ends in `c2 10 00`, proving four explicit
arguments. The trace detour verifies all 24 entry bytes and steals only the
shared six-byte prologue. It installs atomically after the four existing
reflection `patchCall` sites and is detached first on rollback or shutdown.

The extended verifier passes 760 checks. All 57/57 one-at-a-time mutations of
the new or changed numeric bounds, every byte in both new tables, both
relocation fields, the decorated export, class order, and stolen length are
rejected. Doctor, the 756,224-byte release build, and the full off-game
self-test pass, including GPU timestamp retirement.

Run 79 is installed from
`cache/runs/run79-complete-reflection-renderables.ini`; the game has not been
launched. Source and installed DLLs are byte-identical at SHA-256
`3c3c178dd5cad41d1adfb87e8c6370ad0f2326dd97a91840ec2779de7dd6551d`;
source and installed INIs are byte-identical at SHA-256
`dc0cc89630226de801d03ceecabf909283045bc3773c737ca6dc2af6abb2242c`.
The completed Run-78 live CSV and debug log matched their archives before the
two stale live names were removed.

This trace is intended to choose a narrow behavior boundary, not to justify
skipping the whole reflection. Decompilation shows the plane helper creates,
binds, and clears its temporary 1024-by-1024 surface before
`RenderLightStyle`, then publishes that surface to reflection consumers; a
whole-child skip would therefore risk a blank reflection. If a hot GPU bin
overlaps cold `TerrainBlock`, `TerrainPlug`, or `GraphicsMeshInstance` work,
the next A/B can defer only that first-use renderable class for one frame while
queueing its Resources. If the hot bin is an unclassified gap, the result says
which remaining executor interval needs identity without pretending a nearby
cold texture caused it.


## 99. Run 77 result and Run 78 preparation: the marked reflection work moved beyond draw 192

Run 77 is archived as `tqflicker-frames.run77.csv`, SHA-256
`8afdff308c0402e6ecc59d45bb5bdb59dd3cb687c01bba90c95fd800917fd491`,
and `tqflicker-debug.run77.log`, SHA-256
`caedce37049ad80274176d1585dc55b124e4a623d44829805724e90f9fa5094a`.
Both archives were compared byte-for-byte with the completed live files. The
CSV has 7,443 contiguous presented frames, 0--7442. Its five parts are
**menu** 0--2046, **load-game frame** 2047, **loading screen** 2048--3194,
**first world frame** 3195, and **play** 3196--7442.

F12 is on **play** frame 6915 at 19.264 ms. **Play** frame 6914 is 62.928 ms
but ended only 19 ms before F12, too soon to be the event to which a person
reacted. The probable marker-associated event is **play** frame 6892 at
60.329 ms, ending 539 ms before F12 and beginning 599 ms before it. The user
reported only completion, so this remains a reaction-window identification,
not a claim that the marker proves which frame was felt.

On **play** frame 6892, exact second-manager/first-plane
`GraphicsForwardRenderer::RenderLightStyle` takes 27.003 ms CPU and 24.708 ms
GPU after a 13.522 ms `GraphicsForwardRenderer::BuildScene` selector. Its 16
main-thread state-0 Resource loads take 25.715 ms: four meshes and twelve
textures, with 17.769 ms in nested texture creation and 1.346 ms in nested
buffer creation. Those clocks nest and must not be added. Two of the textures
are the known Gadir `TerrainType` material-zero pair and take 2.744 ms in
Resource loading. That type's last semantic preload is **loading-screen**
frame 3155, while its runtime owner preloads through **play** frame 6891.

The new GPU subdivision excludes the range it measured. On **play** frame
6892, all sixteen eight-draw intervals for exact `RenderLightStyle` draws
65--192 total only 5.502 ms GPU; the largest is draws 161--168 at 2.161 ms.
The whole child is 24.708 ms GPU, leaving 19.206 ms outside that measured
range. The exact event reaches draw 193 and sets the explicit draw overflow.
Run 76's different marked **play** scene had only 169 draws and measured its
first 64 at 0.091 ms, which makes the unmeasured post-192 population the next
best target here, but does not prove it: the current event's first 64 were not
timestamped.

Run 77 also reveals an instrumentation-boundary error. It retained 37 exact
terrain calls through draw 193, all without nested Resource or creation work.
When draw 193 set the query-window overflow, `active.recording` became false;
the same flag then prevented later `GpuChunkTerrainCallScope` records. Thus
the trace stopped retaining terrain identity before the cold call it was
designed to name. `terrainCallOverflow=0` does not mean the selected child had
no later terrain calls; it means the separate 128-call storage limit was not
hit before draw-window recording stopped.

There is no gross ordinary-frame logging regression. Restricting Run 76 and
Run 77 to collision-active, full-scene **play** frames below 60 ms, with
1,000--1,499 indexed draws, no main-thread Resource load, and no shadow-region
change, their mean total times are 22.293/21.811 ms and their mean mod Present
classes are 0.079/0.052 ms over 46/37 frames. This only excludes a large
regression; the route populations are not identical and it is not a fine
effect estimate.

Run 78 corrects that boundary without adding load or rendering behavior. The
same sixteen query pairs move to exact `RenderLightStyle` draws 193--320 in
eight-draw intervals. Draws 1--192 retain ordinals but open no timestamp.
Terrain-call retention now begins at the same draw-window boundary, so calls
before draw 193 cannot consume the 128 fixed records needed by the late tail.
Directional shadow remains unqueried and its executor remains unpatched. No
new Engine site, D3D getter, query pair, trace group, accepted-fix change, or
`shadow_split` change is introduced.

The extended verifier passes 753 checks, including the new requirement that
the late-tail boundary is tested before a terrain-call slot is consumed. All
seven trace bounds were perturbed independently in memory and every mutation
was rejected. Doctor, the 754,688-byte release build, and the full off-game
self-test pass, including GPU timestamp retirement. Run 78 is installed from
`cache/runs/run78-reflection-late-tail-terrain-calls.ini`; rendering has not
been launched. The source/installed DLL SHA-256 is
`86880e95234db37420087447192537316e52b9a62b7c8ff5ef8667385d2a6a28`;
the source/installed INI SHA-256 is
`be46abecf6a32764ac50f477a73ece4b7392d6fc2de984554d6129194753bff1`.


## 98. Run 77 prepared: eight-draw reflection tail with exact terrain-call identity

Run 76's probable marked **play** event resolves the next boundary without
another shadow experiment. Exact second-manager/first-plane
`GraphicsForwardRenderer::RenderLightStyle` draws 65--169 own 38.647 ms, or
99.8%, of its 38.739 ms GPU interval. The exact second
`GraphicsDeferredRendererX::Render` geometry-scene class then blocks
48.151 ms in game draws before the later directional-shadow construction.
Run 77 is passive instrumentation only for that reflection tail. It changes no
rendering, scene admission, Resource state, accepted fix, or `shadow_split`.

The first 64 reflection draws are still counted so ordinals remain exact, but
they open no timestamp. Sixteen unique pairs now cover draws 65--192 in
eight-draw intervals. When one interval fills, its successor opens immediately
after the boundary rather than waiting for the next D3D Draw hook. Thus
Resource and creation commands issued between adjacent renderables belong to
the following draw range. A 193rd draw sets the existing explicit overflow and
disables the event's hot-path gate. Ordinary frames still read only the inline
false flag.

Directional setup and all sixteen directional-chunk GPU phase IDs and CSV
columns are removed, reducing per-frame query allocation/retirement from the
Run-76 schema. A region-changing exact
`GraphicsShadowMapDx11::RenderDirectional` call no longer arms this trace, and
the exact `GraphicsShadowMapRenderer` executor `E8` is no longer patched. Its
23-byte call window, 24-byte entry, and 21-byte `ret 0x0c` tail remain in the
static audit and are still re-read from the pinned binary; they are durable
disassembly evidence rather than a Run-77 write target.

No new Engine patch is added for renderable identity. While the sparse exact
second-manager/first-plane reflection event is active, the already verified
unexported `TerrainPlug` and `TerrainBlock` wrappers append to 128 fixed call
slots embedded in that event. Each record retains class, object identity,
start/end draw ordinal, CPU duration, the observed `TerrainType`/material, and
nested Resource, texture-creation, and buffer-creation counts/durations. F12
writes every retained record and an explicit overflow bit during the session.
The creation durations reuse the D3D hook's existing sample; no state getter
or additional Engine detour is introduced.

The reflection selector remains exact second-manager/first-plane
`GraphicsForwardRenderer::BuildScene >= 2,000 us`; it is a sparse selector,
not a causal attribution. Activation now requires the verified reflection
children, both exact terrain colour wrappers, the terrain/reflection trace
groups, and draw timing. The off-game self-test proves that draws 1--64 open
no bin, draws 65--73 partition 8+1, and an exact `TerrainBlock` record spanning
draws 65--66 retains its nested Resource and texture-creation totals.

`research/streaming/tools/verify-sites.py` passes 752 checks. All seven new or
changed numeric bounds were perturbed independently in memory; every mutation
was rejected. Doctor, the 754,688-byte release build, and the full off-game
self-test pass, including GPU timestamp retirement. Run 77 is installed from
`cache/runs/run77-reflection-tail-terrain-calls.ini`. The source and installed
DLLs are byte-identical at SHA-256
`ebbda98ba661a745d4206d39c1d9f9eeca4913490274e36be85457bfcb7aca91`;
the source and installed INIs are byte-identical at SHA-256
`2806e64c0de188d05ff38d628c03df67c0ecc2fa753320106f559313e5b75166`.
The Run-76 live CSV/debug log matched their archives before both stale live
names were removed. The game has not been launched.


## 97. Run 76 result: the marked drain follows reflection, not directional shadow

Run 76 is archived as `tqflicker-frames.run76.csv`, SHA-256
`dd4118f6896bf27c895e4e8c11adc97a394c096339912516bec7af2f93c623a8`,
and `tqflicker-debug.run76.log`, SHA-256
`d797b52321f0623637b6b501b98e0ad009b69c15f48f01969c91144f9627a71d`.
Both archives are byte-identical to the completed live files. The CSV has
7,310 contiguous presented frames, 0--7309. Its five parts are **menu**
0--1818, **load-game frame** 1819, **loading screen** 1820--2954, **first
world frame** 2955, and **play** 2956--7309.

F12 is on **play** frame 6575 at 24.320 ms. The immediately preceding frame
ended too close to be a human reaction target. The route event is **play**
frame 6548 at 90.945 ms, ending 601 ms before F12 and beginning 692 ms before
it. The reporter supplied no additional subjective classification with the
completion message, so this is the probable marker-associated event, not a
claim that the marker proves which frame was felt. A separate 423.042 ms
**play** frame 7171 occurs after F12 and is not substituted merely because it
is the session maximum.

The corrected reflection trace captures the whole exact second-manager,
first-plane `GraphicsForwardRenderer::RenderLightStyle` on frame 6548. The
preceding exact `GraphicsForwardRenderer::BuildScene` takes 5.961 ms CPU and
selects the event; it is still only a selector. `RenderLightStyle` takes
8.888 ms CPU and 38.739 ms GPU across 169 indexed draws. Draws 1--64 cost
0.091 ms GPU, draws 65--128 cost 7.932 ms, and draws 129--169 cost 30.715 ms.
The latter two ranges account for 38.647 ms, 99.8% of the child interval.
Ordinary selected **play** events at frames 6557 and 6561 take only 0.211 and
0.282 ms GPU across 221 and 271 draws, so the frame-6548 cost is not explained
by draw count.

Two state-0 Gadir rocky-pebbles base/normal textures are synchronously ensured
from the exact unexported `TerrainBlock` colour-render class inside that
reflection child. Their Resource calls take 7.400 ms and four nested texture
creations take 6.041 ms. The exact `TerrainType` had one successful
`TerrainType::PreLoad(true)` during the **loading screen**, on frame 2858.
The enclosing runtime `TerrainRT::PreLoad` owner was then visited 3,747 times,
including **play** frame 6547 immediately before the event, but that stock
owner class does not call the layer type's semantic preload. Thus Run 67's
one-time post-`LoadTextures` preload did not keep these two Resources resident
until their first reflected use. This is evidence for a missing near-use
refresh, but it does not yet prove whether the 38.739 ms GPU interval is upload
work, the newly admitted terrain draws, or both.

The CPU wait point is ordered after that reflection producer. Frame 6548
spends 49.302 ms in the game's `Draw`/`DrawIndexed` submission class. Of that,
48.151 ms is inside the exact second `GraphicsDeferredRendererX::Render`
invocation's geometry-scene call at `0x166412`; the whole child is 48.893 ms
CPU but only 4.732 ms GPU. Individual calls block for 28.080, 9.091, 5.054,
2.976, and 2.102 ms. The static call order places reflection before the
deferred owner and places geometry scene `0x166412` before shadow-map
construction `0x166454`. Therefore the 48.151 ms geometry submission wait is
the same-frame drain after reflection and cannot have been caused by the
directional-shadow work that executes later. CPU and GPU scopes overlap and
must not be added.

The corrected exact `GraphicsShadowMapDx11::RenderDirectional` boundary on
that same **play** frame is 7.090 ms CPU / 18.195 ms GPU / 750 draws. Its
pre-executor setup is 9.017 ms GPU. The exact DX11
`GraphicsShadowMapRenderer` record executor's twelve chunks total 8.897 ms;
no individual 64-draw range exceeds 1.866 ms, and only 0.281 ms remains after
the executor. This overturns an interpretation of the old mixed first chunk:
the marked Run-76 submission drain is not a pathological shadow-record range.
It does not claim that Run 75's independently larger directional event never
happened.

The ordinary-path gate also addresses the reported logging concern. In the
allowed collision-active, full-scene **play** class below 60 ms, with
1,000--1,499 indexed draws, no main-thread Resource load, and no shadow-region
change, Run 75 has 40 frames and Run 76 has 46. Their means are 22.103 versus
22.293 ms total, 0.047 versus 0.079 ms in the mod Present class, 16.590 versus
17.321 ms in exact `Engine::Render`, 9.802 versus 9.971 ms in game draw calls,
and 22.235 versus 22.107 ms whole-frame GPU. These matched means exclude a
large broad regression but do not establish exact zero observer cost. No
cross-run p50 is used. Run 76 arms only four reflection and six directional
events in **play**; every overflow and collision counter is zero. Its F12 log
is written after the candidate.

The next passive trace should stop spending queries on directional shadow and
subdivide only the exceptional reflection tail. Keep the same sparse exact
second-manager/first-plane `BuildScene` selector, count the first 64 draws
without timestamps, then cover draws 65--192 in sixteen eight-draw GPU bins.
While that event is active, the already installed exact unexported
`TerrainPlug` and `TerrainBlock` wrappers should retain each call's start/end
draw ordinal, CPU duration, and nested Resource/create totals in a bounded F12
ring. This needs no new Engine patch and no ordinary-frame D3D getter. It will
show whether the 30.715 ms range is the exact cold `TerrainBlock` call or a
different resident renderable population before choosing between a narrowly
refreshed semantic preload and temporary reflection-only omission. Do not turn
the global runtime-owner preload into an all-layer-per-frame behavior on the
present evidence: the affected owner has 142 layers and was called on almost
every intervening frame.


## 96. Run 76 prepared: remove ordinary-draw overhead and put both GPU traces on the corrected boundaries

Run 75's passive subdivision did answer the directional clustering question,
but its implementation made two calls across the DLL boundary around every
game `Draw`/`DrawIndexed`, even when no event was active. Run 76 puts a single
inline volatile flag in that ordinary path. The flag is false outside an
armed exact class, so neither chunk helper is called. The existing setter
snapshots remain the source of VS/PS/SRV0/VB0/IB identity; there is still no
per-draw D3D getter. The off-game self-test checks the false/true/false gate
transition around both selected classes.

The directional selection remains a region change in exact
`GraphicsShadowMapDx11::RenderDirectional`, but its chunks no longer begin at
that outer class. The verified DX11-only chain is
`GraphicsShadowMapRenderer::Render` at `0x18ce70`, record construction
`0x18d04f -> 0x18c870`, and record execution
`0x18d05d -> 0x18c520`. Run 76 opens one separate
`gpu_chunk_shadow_setup_ms` interval at the outer region-changing call, ends
it in the wrapper around the exact executor `E8`, and enables the sixteen
64-draw chunks only for the executor. The wrapper closes the chunks before it
returns. The old-renderer/DX9 branch still uses `0x187360` and never reaches
this site. The new runtime tables verify the 23-byte call window at
`0x18d054`, the 24-byte executor entry, and the 21-byte tail at `0x18c631`;
that tail ends in `ret 0x0c`, proving the wrapper's three explicit arguments.
Only the exact `E8` is patched.

For reflection, decompilation confirms that exact
`GraphicsForwardRenderer::BuildScene` gathers visible regions and calls
`GraphicsSceneRenderer::AddRegionToScene`, but returns no admitted-object or
admitted-region count. Its duration can select a rare event, but cannot name
the cause. Run 76 therefore treats a second-manager/first-plane
`GraphicsForwardRenderer::BuildScene` duration of at least 2,000 us only as a
sparse arming signal, records that exact trigger duration in the F12 metadata,
and opens chunk zero before the immediately following whole exact
`GraphicsForwardRenderer::RenderLightStyle`. In Run 75's **play** part this
would select four frames: 6744 at 12,766 us, 6747 at 3,024 us, 6748 at 2,590
us, and 6915 at 3,274 us. Thus it includes both implicated **play** frames
6744/6747 while remaining sparse; it does not claim that `BuildScene` caused
their later GPU cost. The old state-0 `Resource` trigger and its draw-ordinal
bookkeeping are removed.

This remains instrumentation only. Rendering choices, resource state,
`shadow_split`, and all three accepted performance fixes are unchanged. The
GPU result remains non-blocking and is retired in later frames. F12 still
writes the bounded 32-event/120-frame identity ring during the session. The
CSV's engine durations retain `_us`; the new setup value is game GPU time and
therefore ends in `_ms` without being charged to the mod.

`research/streaming/tools/verify-sites.py` is extended for the new executor
entry/call/tail, exact call target and ABI, corrected class boundaries,
BuildScene trigger, inline draw gate, install dependencies, restoration, and
self-test assertions. It passes 750 checks against the installed binaries.
All five new numeric constants and all 68 bytes in the three new executor
tables were perturbed independently; every one of the 73 mutations is
rejected.
Doctor, the 755,200-byte release build, and the complete off-game self-test
pass, including GPU timestamp retirement. Run 76 is installed from
`cache/runs/run76-corrected-gpu-boundaries.ini`. The source and installed DLL
are byte-identical at SHA-256
`96a7347716e33e07edbd97535739e6a77b9897cbe99cff88758563be5a0ddede`;
the source and installed INI are byte-identical at SHA-256
`f9fc60731be367987bfd1b584f5bb23a64b41ed77a112f80354a5a06cf692ade`.
The stale live Run-75 CSV/debug log matched their archived hashes before both
live names were removed. The game has not been launched.


## 95. Run 75 result: directional work is clustered; the cold-triggered reflection trace starts too late

Run 75 is archived as `tqflicker-frames.run75.csv`, SHA-256
`be2e4bb57de660b15c70e94371a84f1c33adaaa8571adf246fac84d37c0abf3c`,
and `tqflicker-debug.run75.log`, SHA-256
`ab315f205f7f84d22c4ccfd458840c8468af507261dd35048961ab6d4be5b000`.
Both archives were byte-compared with the completed live files. The CSV has
7,343 contiguous presented frames, 0--7342. Its five parts are **menu**
0--1943, **load-game frame** 1944, **loading screen** 1945--3146,
**first world frame** 3147, and **play** 3148--7342.

F12 is on **play** frame 6760. Frame 6759 ends only 21 ms before it and cannot
be the human reaction target. The probable felt sequence is **play** frames
6744/6745 at 107.764/136.323 ms, ending 522/385 ms before F12. A 35.960 ms
frame follows, then frame 6747 is 45.498 ms and ends 304 ms before F12. The
reporter says this session felt as though it had “a little more delay,” and
specifically asks whether the trace caused it. That subjective comparison is
retained; it is not converted into a cross-run frame claim.

The steady-state evidence does not show a measurable trace regression. Using
the allowed comparison class—collision-active, full-scene **play** frames
below 60 ms, 1,000--1,499 indexed draws, no main-thread Resource load, and no
shadow-region change—Run 74 has 46 frames and Run 75 has 40. Their means are
22.137 versus 22.103 ms total, 0.048 versus 0.047 ms in the mod Present class,
17.740 versus 16.590 ms in exact `Engine::Render`, 10.620 versus 9.803 ms in
the game's Draw/DrawIndexed calls, and 22.206 versus 22.236 ms whole-frame
GPU. These are matched full-scene means, never cross-run p50s. They do not
prove zero cost, but they exclude an observable broad slowdown of this class.

The trace is not literally free. While armed, its first implementation makes
two short instrument calls around every game draw even when neither sparse
event is active; the per-frame GPU retirement loop also scans 32 additional
phase slots, and the full CSV rows are wider. Across **play**, actual chunk
queries opened on only seven frames: two reflection arms, six directional
arms, and 3,547 classified event draws. The 38 KiB debug log is written when
F12 is retrieved, after all candidate frames, so its synchronous report cannot
cause the pre-reaction sequence. The CSV uses the existing asynchronous
batched writer. Event-frame timestamp commands could perturb a particular GPU
queue slightly; this run has no counterfactual capable of proving their cost
is exactly zero.

The directional result is useful and internally complete. On **play** frame
6744, exact `GraphicsShadowMapDx11::RenderDirectional` costs 5.299 ms CPU and
84.599 ms GPU across 743 draws. Its twelve 64-draw chunks account for 84.278
ms. Five bounded ranges contain 79.282 ms (94.1%): chunk 0, which includes
pre-executor setup plus draws 1--64, is 42.968 ms; draws 129--192 are 6.251 ms;
193--256 are 12.407 ms; 449--512 are 14.699 ms; and 641--704 are 2.957 ms.
The other seven ranges total only 4.996 ms. Directional work is therefore not
uniform across the 8,192-square map and not explained by raw draw count. Chunk
zero still mixes map setup with records, so the already verified
`0x18d05d -> 0x18c520` executor call is the next exact boundary needed to
separate setup from draws 1--64 and retain record identity for those five
ranges.

The reflection result corrects Run 75's own design assumption. The exact
second-manager/first-plane `GraphicsForwardRenderer::RenderLightStyle` on
**play** frame 6744 costs 12.382 ms CPU / 63.180 ms GPU. Its two Gadir texture
Resources cost 9.378 ms CPU, but the first state-0 load is not encountered
until reflection draw 200. Only 39 draws remain, and the post-trigger GPU chunk
is 0.382 ms. Thus almost all of the exceptional immediate-context GPU interval
precedes the cold-Resource trigger; “arm on the cold load” cannot subdivide the
producer. Device-level texture creation may have ordering outside the
immediate-context timestamps, so this does not prove texture upload harmless,
but it does prove the trigger is too late for the requested whole
`RenderLightStyle` breakdown.

A second reflection event occurs on **play** frame 6747. Six cold Resources
cost 15.547 ms inside an 18.179 ms CPU / 17.503 ms GPU `RenderLightStyle`.
The first load occurs at draw 31; its four retained chunks cover draws 31--269
and total 9.700 ms. This event has material work on both sides of the trigger,
again requiring a whole-child subdivision rather than a post-load one.

The longer subjective tail has corresponding game work. On **play** frame
6744 the game's D3D calls block 65.324 ms, including 64.234 ms in the exact
second deferred owner's geometry-scene class. Frame 6745 has no Resource load
or region change and only 29.334 ms whole-frame GPU, but blocks 82.364 ms in
game draws: 33.476 ms inside the exact second reflection plane and 45.703 ms
inside the exact second deferred owner's geometry-scene class. This is a
multi-consumer queue drain. Frame 6747 then adds the separate six-Resource
reflection event. The data supports real additional transition work in this
session more strongly than logger overhead, while not claiming the trace has
mathematically zero event-frame cost.

Before another measurement, make the hot path honest: expose an inline active
event bit so ordinary Draw/DrawIndexed calls do not enter either chunk helper.
For directional shadow, begin record chunks at the verified executor call and
give pre-executor setup its own interval. For reflection, the next trace must
cover the whole exact second-manager/first-plane `RenderLightStyle`, not start
at a Resource encountered after draw 199. Because doing that every frame adds
queries, first recover a pre-child sparse signal—preferably exact new scene
admission recorded by the preceding `BuildScene`—or explicitly accept and
measure a very small whole-child query budget. Do not interpret another
post-cold reflection run as a whole-child result.


## 94. Run 75 prepared: subdivide both exceptional GPU producers at their actual triggers

Run 74 rejects the hypothesized large shared-buffer population, but leaves two
independent GPU producers on the probable **play** onset: the exact reflection
`GraphicsForwardRenderer::RenderLightStyle` child is 47.117 ms GPU after two
cold Gadir terrain texture Resources, and the exact
`GraphicsShadowMapDx11::RenderDirectional` class is 81.471 ms GPU after a
region change with zero nested Resource loads. Run 75 passively subdivides
both. It does not change reflection, shadow, culling, resource loading, scene
admission, color rendering, or `shadow_split`.

The directional record flow is now recovered rather than inferred from the
outer class. `GraphicsShadowMapDx11::RenderDirectional` gathers/adopts its
casters, then reaches `GraphicsShadowMapRenderer::Render` at Engine RVA
`0x18ce70`. Its verified DX11 branch calls the 0x88-byte record builder at
`0x18d04f -> 0x18c870`, then the sorted record executor at
`0x18d05d -> 0x18c520`. The executor's indirect call at `0x18c613` is virtual
slot `+0x28` on the accepted renderable. The other renderer branch calls a
different helper at `0x187360`; therefore this builder/executor chain is
specifically the DX11 shadow path, not stale DX9-only support. The reproducible
streaming audit remains 1,592 functions and now carries explicit address roots
for both unexported record helpers. Independent 20--24-byte windows cover the
owner, helpers, direct calls, and final dispatch in
`disassembly-targets.md` and `verify-sites.py`.

The instrument adds no Engine code patch. It reuses the verified
`RenderDirectional`, reflection-child, and `ResourceLoader::LoadResource`
wrappers plus the existing D3D11 Draw hooks and setter-maintained binding
snapshot. A directional event arms only after the wrapper proves its region
pointer changed, before the exact call begins. A reflection event arms only
before the first state-0 Resource load observed on the main thread inside the
exact per-plane `RenderLightStyle` child. That distinction matters: the first
reflection chunk includes the texture load/upload commands that can make later
draws expensive; the first directional chunk includes pre-draw map setup even
though the class has no cold Resource load.

Each class has sixteen unique non-blocking timestamp-query pairs, with 64 game
draws per chunk. The 166-draw reflection child observed in Run 74 therefore
uses about three chunks; the 748-draw directional class about twelve. A fixed
32-event ring retains events for the 120-frame F12 reaction window. Each chunk
records its draw ordinal range, draw/indexed counts, element total, null pixel
shader/SRV0 counts, binding-transition count, and first/last VS, PS, SRV0,
VB0, and IB identities. No COM identity is retained or dereferenced and no D3D
state getter or extra per-draw CPU clock is added. A second trigger for the
same class/frame is folded into the live event and counted as a collision;
more than 1,024 draws is an explicit overflow.

The interpretation is narrow. A single exceptional directional chunk points
to a bounded record/terrain/alpha population worth inspecting at the recovered
executor boundary; cost spread across all twelve chunks instead supports a
genuinely incremental map build. For reflection, an exceptional first chunk
ties the delay to cold texture admission/upload; an exceptional later chunk
instead identifies a draw/binding population after residency. The following
deferred-color wait remains a queue-drain class and is not added to either
producer.

`verify-sites.py` passes 743 checks. All four new numeric bounds reject an
independent one-at-a-time perturbation. `npm run doctor`, the 754,688-byte
release build, and the complete off-game self-test pass, including GPU
timestamp retirement and an exact synthetic 64+1 reflection-chunk partition.
Run 75 is installed from `cache/runs/run75-gpu-draw-chunks.ini`. Installed and
source DLLs are byte-identical at SHA-256
`45768956bd265ca16c49d8e8d83dcb4c57aa041d28cedacae72a9c5534b3fb76`;
installed and source INIs are byte-identical at SHA-256
`630b798d1c66d968193d6583be3ff56eb2350222803c88507411d5755a3e600a`.
Run 74's live CSV and debug log matched their archived SHA-256 identities
before their stale live names were removed. The game has not been launched.


## 93. Run 74 rejects a large shared-buffer population and splits the two GPU producers

Run 74 is archived as `tqflicker-frames.run74.csv`, SHA-256
`7d6da968ffd189a09efe382be9ae72dcb4cd6d63028f5b1114b72e788735f0f3`,
and `tqflicker-debug.run74.log`, SHA-256
`2f8860bab93dbe367486afc5554b5c7e606b20b4950cbbd745f57c94818b88cf`.
Both archives were byte-compared with their completed live files. The CSV has
7,304 contiguous presented frames, 0--7303, totaling 95.231 s. Its five parts
are **menu** 0--2027 (19.271 s), **load-game frame** 2028 (1,335.705 ms),
**loading screen** 2029--3162 (9.717 s), **first world frame** 3163
(1,163.347 ms), and **play** 3164--7303 (63.744 s).

F12 is on **play** frame 6724. The normal-route pair is **play** frames
6705/6706 at 92.269/165.636 ms, ending 551/386 ms before the marker. Their
onset-to-marker distances are 643/551 ms. The reporter supplied no subjective
classification beyond completing the run, so this is a probable
marker-associated pair, not a claim that either frame was felt.

The exact identity result rejects the large shared-buffer population proposed
after Run 73. Frame 6705 creates 82 buffers / 2,867,200 bytes, including 64 /
0.549 ms inside the exact second-manager/first-plane reflection class. It has
36 first uses of fresh buffers in deferred color and one in directional
shadow, but zero first uses in reflection. Only one 11,200-byte vertex buffer
joins shadow and deferred on that frame. Although it was created while the
reflection context was active, its first observed reflection draw is on frame
6706, after the exceptional frame-6705 reflection GPU interval has already
ended. It therefore cannot be the source of that 47.164 ms reflection work.

The F12 window retains 203 creations and reports exactly that one joined
identity, with no omitted record, index overflow, or recent ring eviction.
Across all **play**, 2,597 buffers are created and 1,114 fresh buffers first
appear in deferred color, but only three buffers ever join all three classes.
Those joins occur on frames 6706, 6938, and 7031. All-session index-overflow
and recent-eviction counters are zero. The measured answer is thus not “the
same newly created geometry is rendered by all three passes in one burst.”
The proposed generic color-first staging of a large shared buffer population
has no measured population to act on.

The new child brackets instead make the reflection producer exact. On **play**
frame 6705, the second manager's first plane costs 19.805 ms CPU and
47.164 ms GPU. `GraphicsForwardRenderer::BuildScene` accounts for 7.287 ms
CPU and no measurable GPU interval. The following exact
`GraphicsForwardRenderer::RenderLightStyle` call accounts for 10.851 ms CPU
and 47.117 ms GPU. The two known Gadir terrain textures load synchronously in
this `RenderLightStyle` chain for 7.820 ms; four texture creations take
5.635 ms. These clocks nest and must not be added. In **play**, the next
largest second-plane reflection GPU interval is only 0.371 ms and the next
largest `RenderLightStyle` GPU interval is 0.325 ms.

Directional shadow is a separate producer on the same frame. The exact
`GraphicsShadowMapDx11::RenderDirectional` class costs 5.920 ms CPU and
81.471 ms GPU while issuing 748 draws, with zero nested Resource load. Its one
fresh shadow buffer is the lone identity described above; the other bound
buffers are not fresh within the verified 120-frame horizon. The directional
draw count is lower, not higher, than the 799.783-draw mean of the matched
reference population. The next largest directional-shadow GPU interval in
**play** is 11.800 ms. Thus neither raw draw count, synchronous cold resource
loading, nor a large population of newly created buffers explains this
directional spike.

The within-run reference is 46 collision-active, full-scene **play** frames
below 60 ms in the same 1,000--1,499 indexed-draw band, with no Resource load
or shadow-region change. Their means are 22.137 ms whole frame, 17.740 ms
`Engine::Render`, 10.620 ms game draw submission, 0.856 ms second-plane CPU,
0.206 ms `BuildScene` CPU, 0.585 ms `RenderLightStyle` CPU, 0.110 ms
second-plane GPU, 0.074 ms `RenderLightStyle` GPU, 2.478 ms directional CPU,
7.181 ms directional GPU, and 10.096 ms in second-owner geometry-scene game
draws. Relative to those same-run means, frame 6705 adds 18.949 ms
second-plane CPU, 47.054 ms second-plane GPU, 47.043 ms `RenderLightStyle`
GPU, 74.290 ms directional GPU, and 35.147 ms second-owner geometry-scene
game-draw time. These scopes overlap through submission and are not additive.

Frame 6706 is the queue drain. Whole-frame GPU is back to 22.467 ms,
second-plane reflection GPU is 0.059 ms, and directional GPU is 7.087 ms, but
the exact second-owner deferred geometry-scene class blocks 145.067 ms in the
game's draws. Its two worst `DrawIndexed` calls wait 63.183 and 55.072 ms.
Against the same matched means, the frame adds 134.971 ms in that exact
game-draw class. Seventeen off-main texture creations take 74.345 ms beside
the drain; that off-main clock and the preceding GPU work must not be added to
the color draw wait.

The corrected diagnosis is now two independent first-use producers followed
by one submission drain: terrain texture/resource admission inside the exact
reflection `RenderLightStyle` class, an exceptional resident-population
directional-shadow GPU build, then deferred-color draw backpressure. A
renderer, frustum, shadow-system, or resource-loader rewrite is still not
supported, but one generic cross-pass staging switch is not supported either.

Before another behavior A/B, the unexplained larger producer needs one more
narrow measurement. Statically recover the render-record execution inside
`GraphicsShadowMapDx11::RenderDirectional`, then bracket bounded draw-ordinal
chunks only on region-changing directional calls. Reuse the existing draw
hook and IA/shader/SRV snapshots; preserve per-chunk draw/index totals and a
bounded record identity, and retire non-blocking GPU timestamps later. The
candidate is a region change, so ordinary frames need no additional GPU
queries. This will distinguish one pathological terrain/alpha/caster record
range from a cost spread across the whole 8,192-square directional map. Only
then choose between per-record admission and a genuinely incremental map
update. Whole-map reuse remains rejected by §50 because it visibly flickered
and merely moved the work.


## 92. Run 74 prepared: prove or reject the shared first-use population

Run 73 established the order of the **play** transition but left one factual
gap: 69 buffers are created inside the exact second-manager/first-plane
reflection class, directional-shadow GPU work then becomes exceptional, and
the exact second deferred owner's geometry-scene draws block, but pointer
identity was not retained between those classes. Run 74 is a passive identity
trace for that gap. It changes no culling, scene admission, resource loading,
reflection, shadow, or color-render decision.

Every successful main-thread D3D11 buffer creation is retained in a fixed
4,096-record ring for 120 frames with byte width, bind flags, and its exact
creation context. An 8,192-slot index bounds each draw-time lookup to 16
pointer probes; index saturation has its own counter. The existing IA setter
snapshot already holds four vertex buffers and the index buffer. After each
already-timed game draw, Run 74
deduplicates those five identities and classifies their use as one exact
reflection manager/plane, the exact
`GraphicsShadowMapDx11::RenderDirectional` class, or one exact
`GraphicsDeferredRendererX::Render` owner/pass/site. Directional use takes
precedence over its enclosing deferred owner. Per-frame counters record the
first use of each fresh buffer in each family and the exact moment it first
joins reflection+shadow, reflection+deferred, shadow+deferred, or all three.
The 4,096-entry ring reports any overwrite still inside the 120-frame horizon.
F12 writes up to 128 joined identities during the session, including creation
and first-use frames and exact reflection/deferred contexts, and explicitly
reports omitted output and recent ring eviction.

This adds no per-draw clock read, GPU query, or D3D state getter. It reuses the
single Draw/DrawIndexed duration sample and state-setter snapshot already
required by Runs 71--73. The sole additional GPU query pairs are coarse child
scopes around the already verified per-plane `BuildScene` and
`RenderLightStyle` calls. Their unique `E8` sites are Engine RVAs `0x186501`
and `0x18694d`. Both use `detour::patchCall`; before either write, the runtime
verifies each 16--24-byte caller window, relocation-aware exported entry, and
independent callee-cleanup tail. `BuildScene` at `0x17d9d0` ends in `ret 0x04`
and `RenderLightStyle` at `0x179a40` ends in `ret 0x10`. The four reflection
manager/plane cells each receive separate child count/CPU-`_us`/GPU fields.
Directional draws receive a count under the already verified directional
scope.

The trace activates only if the reflection, child, directional, deferred-owner
and D3D binding dependencies are all live. Group 131072 owns it and pulls in
the two exact scope dependencies when selected alone; `engine_trace=1` already
selects everything. Failure leaves the cross-pass columns at zero and logs the
trace unavailable rather than emitting a partial class. All retained state is
bounded and pointer-only; no COM object is dereferenced or retained.

The decision after the run is binary. If the candidate **play** frame gains a
large `engine_crosspass_join_all_three`, the supported fix boundary is staged
scene participation: make newly admitted renderables visible to the normal
color class first, then admit those same identities to reflection and
directional shadows over later frames. If the join stays zero without a recent
eviction, the shared-population explanation is rejected and the child brackets
separate reflection `BuildScene` budgeting from resident-caster directional
shadow budgeting. Either result is more specific than a renderer, frustum,
shadow-system, or resource-loader rewrite.

The verifier passes 712 checks. A clean-baseline one-at-a-time audit rejects
all 150/150 new byte, RVA, relocation, ABI, and retention-bound mutations.
`npm run doctor`, the 748,032-byte release build, and the complete off-game
self-test pass; the documented GPU timestamp-retirement check failed once
with `stamps=0` and passed on the required immediate rerun with `stamps=1`.
The installed/source DLL SHA-256 is
`a419046fb76d351d6f5d2f7fc10021b3b36edaaa7473054e848873265d7577d9` and the
installed/source Run-74 INI SHA-256 is
`f2534e7ee34207f93ec43761706276627f46bab4a26e6ddd383776ac0309113d`.
Both pairs are byte-identical; the stale live CSV is absent and the game was
not launched.

## 91. Run 73: reflection and directional shadows jointly produce the play burst

Run 73 is archived as `tqflicker-frames.run73.csv`, SHA-256
`7f78a2f4dd18e5d4d75b1b771bde17e93de08445be61a7867103a5a347a6d948`,
and `tqflicker-debug.run73.log`, SHA-256
`7008890e94d2726c6b33c62891e197f851a0700435ebe38c644997d8b36efba1`.
Both archives were byte-compared with their completed live files. The CSV has
7,356 contiguous presented frames, 0--7355. Its five parts are **menu**
0--2083, **load-game frame** 2084, **loading screen** 2085--3156, **first
world frame** 3157, and **play** 3158--7355.

F12 is on **play** frame 6775. Frame 6774 ends only 18 ms before the marker;
it cannot be selected as a human reaction target and is a separate 44.949 ms
frame with 25.166 ms in the exact `Engine::Update` class. The normal-route
transition is **play** frames 6752/6753 at 101.205/136.010 ms, ending 635/499
ms before F12. The reporter did not describe the subjective size, so this is
the probable marker-associated pair rather than an assertion that either
frame was felt.

Frame 6752 proves that reflection is a real producer, but not the only one.
The first manager invocation has no reflection plane and costs 0.002 ms. The
second recursive DX11 branch's first water-plane forward renderer costs
20.826 ms CPU and 53.770 ms GPU; its enclosing exact
`GraphicsReflectionManager::RenderReflections` class costs 20.918 ms CPU and
the same 53.770 ms GPU. Inside that plane are two main-thread terrain texture
loads / 9.545 ms, four texture creations / 7.878 ms, and 69 buffer creations /
0.584 ms. The load and creation clocks nest inside the plane and must not be
added. The Resource identities are
`gadir_rockypebbles01.tex` / 5.229 ms and
`gadir_rockypebbles01normal.tex` / 4.316 ms, both in the exact
`TerrainBlock -> GraphicsForwardRenderer::RenderLightStyle` reflection chain.

The exact `GraphicsShadowMapDx11::RenderDirectional` class on the same
**play** frame costs only 6.162 ms CPU but 84.698 ms GPU. It performs zero
nested Resource load. The accepted exact `GraphicsMeshInstance` mitigation
successfully defers 38 cold Actor-root poses and omits 38 cold root meshes
(37 state 0, one state 1), so this is not the synchronous cold-root class that
Runs 68--69 removed. Whole-frame GPU reaches 202.410 ms. The second
`GraphicsDeferredRendererX::Render` geometry-scene class then takes 60.647 ms
CPU, including 60.178 ms blocked in game `Draw`/`DrawIndexed`; one draw waits
44.066 ms. These classes execute in order and overlap through GPU submission;
their durations are not additive.

Frame 6753 is the drain. Its exact second reflection-plane class is back to
0.924 ms CPU / 0.161 ms GPU, directional GPU is 7.870 ms, and whole-frame GPU
is 23.160 ms. Nevertheless `Engine::Render` costs 129.589 ms and game draw
submission costs 120.461 ms. The exact second-owner deferred geometry-scene
class contains 119.964 ms of it. Its two worst `DrawIndexed` calls wait 67.111
and 36.786 ms. Seventeen off-main texture creations / 15.338 ms happen beside
the drain; their clocks are not assigned to the earlier main-thread reflection
scope.

This is not a raw draw-count spike: frames 6752 and 6753 issue 1,421 and 1,418
indexed draws. The matched same-run reference is 45 collision-active,
full-scene **play** frames below 60 ms in the same 1,000--1,499 indexed-draw
band, with no Resource load or shadow region change. Their means are 22.030 ms
total, 17.585 ms `Engine::Render`, 10.994 ms game draw submission, 0.715 ms
reflection-manager CPU, 0.127 ms second-plane reflection GPU, 6.953 ms
directional-shadow GPU, and 10.524 ms in second-owner geometry-scene game
draws.

Relative to those matched means, frame 6752 adds 79.175 ms total, 77.568 ms
`Engine::Render`, 50.139 ms game draw submission, 20.203 ms reflection CPU,
53.643 ms reflection GPU, 77.745 ms directional-shadow GPU, and 49.654 ms in
the exact second-owner geometry-scene game-draw class. Frame 6753 adds only
0.034 ms second-plane reflection GPU and 0.917 ms directional-shadow GPU, but
adds 109.467 ms game draw submission and 109.440 ms in that exact deferred
geometry-scene draw class. These are within-run matched-class means, never
cross-run p50s.

The exceptional GPU attribution is not a normal fluctuation. Reflection GPU
on frame 6752 is the largest **play** value at 53.770 ms; the next is 2.753 ms.
Directional-shadow GPU is likewise the largest at 84.698 ms; the next is
13.266 ms. Both candidate frames contain exactly two manager invocations, only
the second has one plane, and both overflow counters are zero. Across all
**play**, 83 other frames expose a third manager and plane through the explicit
overflow counters, but that bounded limitation does not affect this pair.

The corrected diagnosis is a first-use multi-pass admission burst: reflection
forward rendering and directional shadows both produce exceptional GPU work
on frame 6752, then second-owner deferred color draws absorb the queue drain
on frames 6752--6753. The static order and 69 reflection-contained buffer
creations make it plausible that newly admitted geometry is consumed by all
three passes, but Run 73 does not preserve those buffer identities across the
later shadow and color draws. That overlap remains an inference, not a
measurement.

A wholesale shadow, frustum, resource-loader, or renderer rewrite is not
supported. The next passive run should retain a bounded cross-frame table of
main-thread buffer creations, reuse the already tracked IA bindings at each
game draw, and classify use as exact reflection plane, exact
`GraphicsShadowMapDx11::RenderDirectional`, or exact deferred owner/site. It
should also bracket the already verified reflection `BuildScene` and
`RenderLightStyle` calls and count directional draws. It needs no per-draw GPU
queries, state getters, or additional clock reads. If the same fresh buffers
cross all three classes, stage newly admitted renderables so the normal color
pass consumes them first and reflection/shadow participation follows over
later frames. If they do not, the child brackets separate reflection budgeting
from a resident-caster directional-shadow budget. This does not reopen any
closed pump, archive-cache, generic-prefetch, pooling, lock, sleep, Stage 5, or
libdeflate route, and `shadow_split` remains untouched.


## 90. Run 73 prepared: isolate recursive branch reflections without changing rendering

Run 72 and the verified static return chain identify reflection forward
rendering as the immediate owner of its eight terrain texture loads, but they
do not yet say which recursive DX11 portal/region branch or water plane owns
the CPU, draw-submission, and GPU producer. Run 73 is the narrow passive trace
for that question. It restores the desired `Graphics` enhanced-grass mode;
Run 72 rejected original grass as a solution because the **play** transition
remained, although it showed that enhanced grass amplifies steady rendering
cost. No shadow, terrain, culling, resource-loading, or reflection decision is
changed.

The trace patches two unique E8 sites, not either shared function prologue.
The branch call at `0x17f2d3` reaches exported
`GraphicsReflectionManager::RenderReflections` at `0x187270`; its 24-byte
caller window, 21-byte entry, and independent `ret 0x08` tail verify before a
write. The manager call at `0x1872bb` reaches the per-plane forward-render
helper at `0x1861d0`; its 20-byte caller window, relocation-aware 20-byte
entry, and independent `ret 0x0c` tail verify. The two patches install
atomically and restore in reverse. These independently prove the manager's
`thiscall` receiver plus two explicit arguments and the plane helper's three
stack arguments.

The first two manager invocations in a frame and the first two planes in each
manager receive exact CPU and game-draw `_us` counters plus non-blocking GPU
intervals. Existing `ResourceLoader::LoadResource`, `CreateTexture2D`, and
`CreateBuffer` samples are tagged in the same four plane cells; whole-manager
totals retain work outside a plane. A third manager or plane is visible in an
explicit overflow counter rather than folded into the bounded identities.
F12's retained Resource records also carry the exact manager/plane numbers.
Manager and plane intervals nest and must not be added. All reflection context
is main-thread context: off-main texture creation stays
unassigned rather than borrowing a stale scope, and must be correlated by
frame timing.

This run can distinguish the exact `GraphicsReflectionManager` / reflection
forward-render class from the following
`GraphicsDeferredRendererX::Render` class during the reporter-selected
**play** transition. It does not assume that the largest frame or the frame
immediately before F12 was felt. The five session parts must again be reported
as **menu**, **load-game frame**, **loading screen**, **first world frame**,
and **play**, and any comparison remains limited to matched full-scene
**play** frames below 60 ms.

The final verifier passes 681 checks. A dedicated mutation audit changes every
byte in the six new caller/entry/tail tables one at a time, both call offsets,
all RVA and relocation constants, the decorated export, every CSV/GPU name and
enum mapping, installation/rollback routes, the main-thread gate, and the
self-test oracle; all 280/280 mutations are rejected. `npm run doctor`, the
738,816-byte release build, and the complete corrected off-game self-test pass,
including GPU timestamp retirement on the first valid run. The self-test first
caught a missing neutral reflection argument in an existing test helper, then
caught the new oracle reading the preceding frame; both test-only defects were
fixed before this pass.

Run 73 is installed. The source and game-directory DLLs are byte-identical at
SHA-256
`e4a0195a7c5bb04a5062e1b231fc3b2897ac2223176ea1e6faf9c5ab9d8cc4b7`;
the source and installed INIs are byte-identical at SHA-256
`6c3b1e7a9eb7779914da43ec8daa52af3422b5e4e664c2011a89f583c059f08d`.
Run 72's live CSV and debug log matched their archived files before deletion.
The Run-73 live CSV and debug log are absent, and the game was not launched.


## 89. Run 72 plus the complete flow map place terrain first-use work in reflection rendering

Run 72 is archived as `tqflicker-frames.run72.csv`, SHA-256
`8039a9243cd88555c222581e9cc599092e74e0c17f8459d84b2eb98096eb102d`,
and `tqflicker-debug.run72.log`, SHA-256
`ccae2b8d088bbc4f6989edebeaf873fe025c24fb2c8d4b2a0157edff1320d9a7`.
Both archives were byte-compared with their completed live files. The CSV has
8,102 contiguous presented frames, 0--8101. Its five parts are **menu**
0--1979, **load-game frame** 1980, **loading screen** 1981--3098, **first
world frame** 3099, and **play** 3100--8101.

F12 is on **play** frame 7318. Frame 7317 ended only 14 ms before the marker,
so it cannot be selected as a human reaction; it is a separate 62.312 ms
frame with 48.462 ms in the exact `Engine::Update` class. The normal-route
transition is the **play** triplet 7295/7296/7297 at
46.065/117.857/64.646 ms, ending 575/457/392 ms before F12. The reporter only
said that the run was done, so this is a probable marker-associated triplet,
not a claim about which frame was felt or its subjective size.

Changing only `Graphics` enhanced grass to original grass did not remove the
transition. It changed where the driver queue drained. On **play** frame 7296,
the second `GraphicsDeferredRendererX::Render` invocation's so-called geometry
setup call at `0x1663a8` takes 26.799 ms, including 25.748 ms blocked in the
game's `Draw`/`DrawIndexed` calls, while its exact GPU interval is 4.825 ms.
One retained `DrawIndexed` accounts for 25.275 ms. The adjacent second-owner
geometry scene takes only 0.487 ms / 0.204 ms game draw, with a 0.073 ms GPU
interval. Thus Run 71's enhanced-grass scene was an amplifier and drain
location, not the necessary producer of the native transition.

The producer begins one **play** frame earlier. Frame 7295 performs 66 buffer
creations / 0.648 ms, ten main-thread texture creations / 4.980 ms, and nine
`ResourceLoader::LoadResource` calls / 21.465 ms. Eight exact terrain texture
loads / 20.226 ms occur in `Engine::Render` outside every active
`GraphicsDeferredRendererX::Render` owner. It changes the directional-shadow
region, but the exact `GraphicsShadowMapDx11::RenderDirectional` class takes
only 4.038 ms and performs zero Resource loads. Its whole-frame GPU interval
is 196.066 ms, including 54.590 ms in that directional-shadow class and
10.990 ms in the `TerrainRenderInterfaceRT::RenderGround` class; nested GPU
intervals must not be added.

Frame 7296 then has a 117.857 ms total and 98.157 ms in `Engine::Render`. The
game's draw calls block for 26.475 ms, 33 off-main `CreateTexture2D` calls take
37.104 ms, progressive upload work takes 13.487 ms, and the enhanced-bloom
class takes 53.466 ms CPU while the whole-frame GPU interval is only
22.501 ms. These clocks overlap through submission and backpressure. Frame
7297 is the tail: 64.646 ms total, with 32.706 ms in the game's `Present`
call and only 16.764 ms in its whole-frame GPU interval. This is the same
producer-then-drain shape as earlier runs, with different API calls absorbing
the waits.

Within Run 72, the matched reference is 58 collision-active, full-scene
**play** frames below 60 ms in the 1,000--1,499 indexed-draw band, with no
Resource call or region change. Their means are 16.736 ms total,
12.413 ms `Engine::Render`, 2.393 ms game draw submission, 2.844/1.355 ms in
the exact deferred geometry classes' CPU/draw clocks, and 2.161/1.073 ms in
the second-owner geometry-setup CPU/draw clocks. Relative to those same-run
means, frame 7296 adds 101.121 ms total, 85.744 ms `Engine::Render`,
24.082 ms game draw submission, and 24.638/24.675 ms in that exact
second-owner geometry-setup CPU/draw class.

Original grass does reduce steady full-scene **play** cost, although the two
routes are not composition-identical. For collision-active full-scene
**play** frames below 60 ms, Run 72 minus Run 71 means in the 1,000--1,499
indexed-draw band (43 / 58 rows) are -5.947 ms total, -5.352 ms
`Engine::Render`, -8.529 ms game draw submission, and -5.120 ms whole-frame
GPU. In the 1,500--1,999 band (768 / 1,235 rows), they are -8.396, -8.035,
-8.387, and -8.405 ms in those same exact classes. In the 2,000-plus band
(239 / 347 rows), they are -9.644, -9.301, -11.204, and -9.526 ms.

These are matched-class means, never cross-run p50s. Different route and
indoor/outdoor shares prevent treating the deltas as a precise grass price;
they are enough to show a steady cost and not enough to explain away a
117.857 ms **play** transition that persists without enhanced grass.

The fresh static reconstruction supplies the missing owner. The generated
resource audit was broad but had not been interpreted end to end. It is now
regenerated from 205 explicit roots as a 1,592-function closure, and
`research/streaming/disassembly-targets.md` records the verified chain:
`GraphicsPortalRenderer::Render` recursively visits visible portal/region
branches; every DX11 branch calls
`GraphicsReflectionManager::RenderReflections` before region admission and
before its `GraphicsDeferredRendererX::Render`; each visible water reflection
uses a `GraphicsForwardRenderer`, builds a reflected scene, and executes the
shared sorted render-list helper.

The eight frame-7295 terrain stacks prove this is not merely a plausible
caller. From `TerrainType::SetShaderParams`, they return through the
renderable virtual at `0x1885b4`, the shared list at `0x17ab1b`,
`GraphicsForwardRenderer::RenderLightStyle` at `0x179ba6`, the reflection
forward helper at `0x186952`, the per-plane loop at `0x1872c0`, and the branch
reflection call at `0x17f2d8`. Therefore those exact **play** terrain loads
belong to reflection forward rendering, not to either deferred owner. The
second correction is that deferred “geometry setup” callee `0x1653a0` also
builds and executes a sorted scene list; its historical label must not be read
as state setup only.

No wholesale renderer rewrite is justified. The next run should be passive
attribution, not another behavior A/B: patch the unique reflection-manager
call at `0x17f2d3` and its per-plane call at `0x1872bb`, tag existing Resource,
D3D-creation, and game-draw samples as reflection versus deferred branch, and
capture bounded CPU/draw/GPU totals per branch and reflection plane. Restore
the desired enhanced-grass mode for that trace; its cost is real, but Run 72
rejects it as the necessary cause. If the 196.066 ms **play** GPU producer is
inside reflection rendering, the workable fix boundary is reflected-scene
admission or reflection update budgeting. If it is outside, the new bracket
will exclude that class before any further decompilation or behavior change.

The expanded verifier checks all 18 new flow windows across 16--24 bytes,
their eight direct-call destinations, the renderable virtual slot, and every
documented RVA. It passes 647 checks, and 72/72 one-at-a-time perturbations of
the new window RVAs, byte strings, call sites, call targets, dispatch, and
documented RVAs are rejected. No runtime table or installed binary was changed
for this static reconstruction. `npm run doctor`, the release build, and the
complete off-game self-test pass; the game was not launched.


## 88. Run 71 isolates the felt transition's wait point in second-owner geometry/grass submission

Run 71 is archived as `tqflicker-frames.run71.csv`, SHA-256
`16a4c1c7c117aa8a24e47ba4a77f96703425b2665036bf700be5b93674bdceee`,
and `tqflicker-debug.run71.log`, SHA-256
`8a26253fb3cc524066f201b76fb5920ac115f419ca6763a735f2ab93e7c4abcf`.
Both archives were byte-compared with their completed live files. The CSV has
7,269 contiguous presented frames, 0--7268. Its five parts are **menu**
0--1975, **load-game frame** 1976, **loading screen** 1977--3121, **first
world frame** 3122, and **play** 3123--7268.

The reporter felt a little stutter and pressed F12 on **play** frame 6703.
Frame 6702 ended only 21 ms before the marker, so it cannot be selected as a
human reaction; it is a separate 80.524 ms frame with 61.601 ms in the exact
`Engine::Update` class. The normal-route region transition is **play** frames
6679/6680 at 82.592/51.919 ms. They ended 600/548 ms before F12 and are
therefore the probable felt pair, preserving the reporter-derived
qualification rather than claiming the marker proves it.

The first transition frame isolates the render-thread wait point. On **play**
frame 6679, `Engine::Render` takes 74.693 ms. The two exact
`GraphicsDeferredRendererX::Render` owners are observed with no overflow. Its
geometry children take 43.901 ms, including 42.098 ms in the game's
`Draw`/`DrawIndexed` calls. Specifically, second-owner geometry scene at call
site `0x166412` takes 42.545 ms and contains 42.073 ms of that draw blocking;
first-owner setup is 0.465/0.006 ms CPU/draw and second-owner setup is
0.891/0.019 ms. Thus 99.9% of the geometry draw wait and 96.9% of geometry CPU
are in one exact invocation/site.

This is not 42 ms of GPU execution by that scene child. Its exact non-blocking
GPU interval is 6.554 ms. The whole-frame GPU interval is 95.113 ms, while the
narrower `GraphicsShadowMapDx11::RenderDirectional`, terrain-ground, and
enhanced-grass intervals are 35.622, 13.456, and 6.554 ms respectively. These
intervals are not additive where nested. The second-owner geometry-scene and
`TerrainRenderInterfaceRT::RenderGrass` GPU intervals are identical on this
frame, locating the scene child around that live grass class.

The carry-over frame makes the queue interpretation stronger. **Play** frame
6680 has no main-thread Resource load and no buffer creation, yet second-owner
geometry scene still takes 24.116 ms and blocks 23.687 ms in the game's draw
calls. Its GPU interval is only 1.411 ms, identical to the nested enhanced-grass
interval, and its whole-frame GPU interval is 24.096 ms. Four off-main texture
creations take 4.334 ms; they cannot explain the main thread entering a single
game draw for 9.574 ms. The driver calls are acting as drain/backpressure
points for work admitted earlier, not pricing only the draw whose call happens
to wait.

Against 43 collision-active, full-scene **play** rows under 60 ms in the same
1,000--1,499 indexed-draw band, with no Resource call or region change, the
mean is 22.683 ms frame, 17.765 ms `Engine::Render`, 12.076 ms geometry CPU,
and 10.429 ms geometry draw. Frame 6679 is +31.825/+31.669 ms in the latter
two exact classes; frame 6680 is +13.592/+13.292 ms. The second-owner scene
alone accounts for those increases. This is a within-run matched full-scene
comparison, not an across-run median.

The retained draws show concentration, not an intrinsically slow mesh. On
frame 6679, one second-owner scene `DrawIndexed` call waits 26.765 ms and the
next waits 7.526 ms: together 34.291 ms, 81.5% of the exact scene draw wait.
They use the same index buffer, vertex shader, pixel shader, and pixel resource,
with different two-stream vertex buffers. The worst draw's vertex buffer
appears again on frame 6680 and waits only 0.944 ms. That large variance for
the same binding identity is further evidence that the recorded call is where
the queue drains, not that its 198 indices inherently cost 26.765 ms.

First-use admission still supplies the upstream burst. Frame 6679 performs
two exact colour-terrain Resource loads, the Gadir rocky-pebbles base and
normal textures, totaling 8.523 ms; four main-thread texture creations take
6.496 ms, and 91 buffer creations take only 0.801 ms. All occur outside an
active `GraphicsDeferredRendererX::Render` owner. In the same frame the
enhanced-grass class adopts 35 new grass streams, creates 35 twins, and issues
38 cross draws alongside 39 original grass draws; its measured mod CPU costs
are only 3.327 ms fill and 0.090 ms cross submission, but the extra draws and
overdraw can deepen the GPU/driver queue before a later original game draw
blocks. This can amplify a native-game transition without being a Wine-only
cause; the base game still admits the terrain and original grass scene.

One limitation is corrected forward. The Run-71 creation ring intentionally
accepted only creations inside an owner. Because this transition's 91 buffers
and four main-thread textures were all created before/outside the owner, every
slow draw reports `new -1`; that means “not found in the owner-only ring,” not
“old resource.” The trace successfully names the consumer/wait point but does
not yet map the outside-owner producer buffers to it.

The cheapest discriminating next run is an A/B with only the optional
`Graphics` enhanced-grass behavior changed to original, while retaining the
same trace and all accepted shadow/terrain behavior. This is not a proposal to
remove enhanced grass permanently. If the probable **play** transition falls
substantially, the proper fix is to budget activation of newly adopted grass
twins/cross draws across later frames, leaving original grass visible
immediately. If it does not, restore enhanced grass and extend identity capture
to all main-thread same-frame buffer creation plus SRV-to-texture mapping
outside the deferred owner. Neither result calls for rewriting shadows,
frustum culling, or resource loading wholesale.

Run 72 is installed for that A/B. Its settings are byte-for-byte Run 71 except
for `grass=enhanced -> grass=original`; the accepted shadow/terrain behavior
and full owner-exact trace remain enabled. The unchanged installed/source DLL
is 732,160 bytes, SHA-256
`bf1e4691c0a897b40acf17aa259a3ad4daa24dec3c9a8051c01e1ab5c60ace40`.
The installed/source `run72-grass-original-ab.ini` SHA-256 is
`d655f091ca5d0be4ffd287598a3446f915843a3c21960c0e9c9b07b96c2f294e`.
Run 71 was archived and byte-compared before its live CSV was removed; the
Run-72 live CSV is absent and the game was not launched.


## 87. Run 71 setup: split the two deferred owners and retain the slow geometry draw identities

Run 70 did not justify another behavior A/B. The reporter identifies **play**
frame 7264 as probably felt; its exact `GraphicsDeferredRendererX` geometry
children spend 50.893 ms blocked in the game's `Draw` / `DrawIndexed` calls,
but the first trace merges the two `GraphicsDeferredRendererX::Render`
invocations in its GPU spans and retains no identity for the slow draws. Run
71 is therefore passive instrumentation only. It changes no Resource state,
scene admission, shadow eligibility, frustum, draw, or upload behavior.

The owner is not detoured at its shared six-byte prologue. The complete static
callgraph has one direct caller, unexported `FUN_1017ead0`; its call at RVA
`0x17fc9b` is changed with `patchCall`. A 24-byte window beginning at
`0x17fc8b` proves the call and its surrounding argument setup, while a separate
20-byte tail at `GraphicsDeferredRendererX::Render+0x49f` (RVA `0x1665cf`)
ends in `ret 0x1c` and proves seven explicit stack arguments. The exported
owner entry remains independently verified across 24 bytes. The owner call,
all fourteen direct-child calls, and all thirteen unique callee ABI tails are
verified before the first write; any failed child patch restores the owner and
all prior children.

Each frame numbers owner invocations one and two and exposes overflow rather
than silently folding a third call into either. The exact geometry setup site
at `0x1663a8` and scene-list site at `0x166412` now have separate CPU,
game-owned draw, and non-blocking GPU fields for each invocation. These four
GPU query pairs begin and end within one direct child call, correcting Run
70's overlapping six-span design. The older six CPU/draw totals remain for
continuity; the old six GPU names are removed because their values were not
separable pass costs.

Synchronous `ResourceLoader::LoadResource`, `CreateTexture2D`, and
`CreateBuffer` work on the main render thread is partitioned into six
locations: setup, scene, or other owner work in invocation one or two. All
game durations end in `_us`. D3D creation reuses the timing sample already
taken by its hook. Off-main texture creation cannot be assigned to a main
owner stack and remains in its existing off-main class rather than receiving a
false invocation.

For the reaction window, Run 71 retains only the twelve slowest geometry draws
per frame in a fixed 128-frame ring. F12 selects at most eight frames from the
preceding 120 whose geometry-draw sum is at least 15 ms. Each retained draw
records invocation, exact setup/scene site, draw kind and arguments, the first
four vertex-buffer bindings, index buffer, vertex and pixel shader, and first
eight pixel shader resources. Those identities are maintained by the game's
existing D3D setter calls; `Draw` and `DrawIndexed` issue no state getter, new
clock read, or GPU query. A fixed 4,096-entry successful-creation ring can
associate the bound vertex/index buffer with its creation frame and descriptor;
the F12 report also lists at most 32 texture creations on each selected frame.
No formatting or file write occurs on the candidate frame itself.

The exact-site verifier now passes 602 checks. It re-reads both new on-disk
windows and checks the owner ABI, patch order and rollback, invocation/site
mappings, all CSV names and units, setter vtable slots, reuse of existing
timing samples, and every retention bound. A fresh mutation audit rejects all
190/190 one-at-a-time Run-71 byte, bound, name, mapping, and route changes;
the prior Run-70 audit remains 716/716. Doctor, the 732,160-byte release build,
and two complete off-game self-test runs pass, including GPU timestamp
retirement on both runs.

The installed/source Run-71 DLLs are byte-identical at 732,160 bytes and
SHA-256
`bf1e4691c0a897b40acf17aa259a3ad4daa24dec3c9a8051c01e1ab5c60ace40`.
The installed/source `run71-deferred-geometry-identity.ini` files are
byte-identical at SHA-256
`dd063f9164acbdfee266356e20c25187e353517cb8d469d9e78029c22a5c4868`.
Run 70's live CSV matched its archived SHA-256 before removal; the stale live
CSV is absent and the game was not launched.

Run the same five-part route and press F12 after the felt **play** transition.
The result should say whether invocation one or two and setup or scene owns the
block, whether a few repeated draw/resource identities dominate, and whether
their buffers or textures were created on that frame. Only then choose among
targeted warming, bounded colour-scene admission, or brief deferral of exact
cold colour renderables. This does not reopen the message pump, buffer pooling,
or the rejected broad prefetch routes.


## 86. Run 70 ties the probably felt transition to geometry submission, not the nearest marker candidate

Run 70 is archived as `tqflicker-frames.run70.csv`, SHA-256
`4f8dac182590173fd9de4dd5c017dbe2d00c7a8619f23e85716174b178c18509`,
and `tqflicker-debug.run70.log`, SHA-256
`55bf83319b31272decec52255b0249539f5263a495927f5244313ba37b329c52`.
Both archives were byte-compared with the during-session files. The CSV has
7,883 contiguous presented frames, 0--7882. Its five parts are **menu**
0--2572, **load-game frame** 2573, **loading screen** 2574--3677, **first
world frame** 3678, and **play** 3679--7882.

F12 is on **play** frame 7283. The automatic nearest candidate, frame 7282,
ends only 17 ms before the marker, which the reporter correctly rejects as an
impossible human reaction interval. The reporter identifies the earlier
region-transition frame 7264 as **probably** the stutter and says it felt
smaller in this run. Frame 7264 begins 583 ms before the marker and ends
461 ms before it. This is the first user-selected, instrumented instance of
the normal-route transition class; retain the reporter's uncertainty rather
than converting “probably” into proof.

Frame 7264 is a 122.155 ms **play** frame. `Engine::Render` takes 114.700 ms,
`Engine::Update` 4.390 ms, and the `Game::PumpMessages` class 0.934 ms. The
exact `GraphicsDeferredRendererX` direct-child partition assigns 52.406 ms to
the geometry class, including 50.893 ms inside the game's own `Draw` /
`DrawIndexed` calls. Shadow-map construction takes 7.656 ms; light
accumulation 0.066 ms; resolve/AO 0.442 ms; later scene lists 1.032 ms; and
post/fog/composite 0.297 ms. The six child classes total 61.899 ms, leaving
52.801 ms elsewhere in `Engine::Render`; that remainder is not an owner-only
routine and must not be presented as a new component. The frame's complete
game-owned D3D timing is 52.243 ms in `Draw` / `DrawIndexed` and 2.450 ms in
`Map`. The geometry children contain 97.4% of the draw block.

The same frame changes from 219 to 1,555 indexed draws and admits 221 buffers;
the 221 `CreateBuffer` calls themselves take only 1.765 ms. Sixteen
main-thread, colour-render Resource calls take 30.218 ms: four meshes /
2.913 ms and twelve textures / 27.305 ms. Fourteen main-thread
`CreateTexture2D` calls take 22.610 ms. These clocks may nest inside the
renderer partition and are not additive. The exact
`GraphicsShadowMapDx11::RenderDirectional` class takes 7.442 ms, performs no
synchronous directional Resource load, and defers 26 cold Actor-root
`GraphicsMeshInstance` poses, confirming 19 new queue insertions. Thus the
accepted shadow deferral is still operating while the probably felt event is
dominated by the colour/deferred geometry submission class.

For a matched reference, Run 70 has 774 collision-active, full-scene **play**
frames under 60 ms with 1,500--1,999 indexed draws, no Resource call, and no
region change. Their means are 23.926 ms frame, 18.812 ms `Engine::Render`,
13.144 ms geometry-child CPU, and 10.356 ms geometry-child draw time. Frame
7264 is therefore +39.262 ms in the exact geometry-child class and +40.537 ms
in its game-owned draw calls. This is class-local evidence, not a comparison
of route-dependent medians.

The new GPU columns need one correction forward from §85. The engine invokes
`GraphicsDeferredRendererX::Render` twice on these frames. Run 70 owns only
one timestamp pair per group per frame, so each pair opens at the first
group occurrence in the first owner invocation and closes at the last group
occurrence in the second invocation. The reported 97.880, 116.898, 116.968,
117.027, 118.037, and 118.478 ms spans consequently overlap and are **not six
separable pass costs**. Do not sum them or use them to assign GPU work. The CPU
and draw partitions remain exact sums of their scoped direct child calls.

Frame 7282 remains real but is a separate, apparently unperceived **play**
event: 73.550 ms total, with 54.816 ms in `Engine::Update`, while
`Engine::Render` is 17.055 ms and draw submission 10.448 ms. It has no
Resource load, region change, or main object wait. Its 68.261 ms whole-GPU
interval can include an idle/submission gap while the CPU is stopped and does
not prove 68 ms of active GPU work. Do not use this nearest frame to replace
the reporter-selected transition.

There is no gross steady-scene trace regression. Comparing only
collision-active, full-scene **play** frames under 60 ms with no Resource call
or region change, Run 70 minus Run 69 mean frame/GPU differences are
+0.189/+0.186 ms at 500--999 indexed draws (1,110 versus 1,159 frames),
-0.158/-0.156 ms at 1,500--1,999 (774 versus 715), and +0.056/+0.013 ms at
2,000-plus (232 versus 234). The 1,000--1,499 samples are only 39 and 42 and
are not used. This excludes a large instrumentation cost; the mixed signs do
not establish a fine change.

The next passive trace should split the two geometry direct-call sites and
the first/second `GraphicsDeferredRendererX::Render` invocations. It should
also retain only slow `Draw` / `DrawIndexed` records, using the QPC sample the
existing draw hook already takes, with draw kind, ordinal/index count, exact
geometry call site, and owner invocation. Cheap tracking of the bound
vertex/index buffers, shaders, and shader resources should be attached to
those slow records; it should not issue per-draw GPU queries or state getters.
GPU timestamps must be per invocation, preferably narrowed to the two geometry
sites. Resource loads and D3D creation should likewise be tagged with the
active owner invocation/site or owner gap. This will distinguish a small set
of first-use draws that can be warmed or deferred from general queue
backpressure.

A bounded lower-mip-first archive path remains a plausible contained fix for
the twelve colour textures and later worker uploads, but it cannot by itself
explain the 221 buffer admissions or the 50.893 ms geometry draw block.
`CreateBuffer` allocation is only 1.765 ms, so this result does not reopen the
closed buffer-pooling route. The evidence supports a narrow colour-scene
admission/warmup fix if the next trace identifies stable slow draws; it does
not support rewriting the shadow system, frustum culling, or deferred
renderer.


## 85. Run 70 setup: partition first-use submission before changing texture admission

Run 69's remaining **play** transition is not evidence that all 121 buffer
creations and 23 off-main texture creations themselves cost 55.814 ms. The
23 texture creations and their 55.814 ms belong to frame 6798; that frame also
blocks 97.651 ms inside the game's `Draw` / `DrawIndexed` calls but has only a
28.044 ms whole-frame GPU interval. The preceding frame 6797 has a 168.095 ms
GPU interval, of which directional shadows, terrain ground, and enhanced grass
name 54.441, 10.236, and 4.798 ms. The remaining 97.007 ms is merely unnamed,
not yet proved to be texture upload, buffer admission, a particular deferred
pass, or work submitted earlier. Those nested and adjacent intervals must not
be added into a fabricated component total.

Run 70 therefore adds a passive six-class partition of the exact
`GraphicsDeferredRendererX::Render` class: geometry, shadow-map construction,
light accumulation, resolve/AO, later scene lists, and post/fog/composite.
Fourteen verified direct `E8` calls are changed with `patchCall`. Each group
gets a whole-child-call count and engine duration ending in `_us`, the share of
the existing game-owned `Draw` / `DrawIndexed` timing that occurs inside it,
and one non-blocking GPU timestamp pair. A group with multiple calls measures
GPU time from its first entry to last exit, so that GPU interval admits the
small owner-code gaps it spans; its CPU and draw totals include only the
direct calls. This is six GPU pairs per frame, not per-draw timestamping. The
existing narrower `GraphicsShadowMapDx11::RenderDirectional`, terrain-ground,
resource, archive, and off-main creation fields remain available for the same
frame.

The owner is the decorated export at Engine RVA `0x166130`, independently
verified across 24 bytes. Every call verifies an original 16-byte window and
its resolved destination before the first write. Every unique destination
also verifies a separate 16-byte epilogue whose `ret` immediate agrees with
the wrapper's explicit x86 argument count. Installation is atomic and rolls
back all earlier calls if any later call fails. The exact sites, targets, and
ABI tails are indexed in `disassembly-targets.md`. Trace group `65536` owns the
instrument. It is selected by the usual `engine_trace=1`, and cannot install
when the performance probe is off. No fix option, resource decision, shadow
decision, or `shadow_split` behavior changes.

The proposed lower-mip-first direction is technically coherent but is not the
same experiment as rejected Stage 4.2. Stage 4.2 reduced archive read syscall
count while leaving the same compressed blocks to read, inflate, and consume;
it could not remove the first-use D3D resource realization seen here. A
lower-mip-first texture path could reduce initial allocation/upload work and
defer higher mip uploads. The existing progressive path cannot simply be
switched on for archive textures, however: it retains a loose file's mapped
bytes while later mip uploads consume them. Archive reads return decompressed
data with a different owner and lifetime. Supporting them requires a bounded
staging owner for copied or retained decompressed mip payloads, a resource
ready-state rule, cancellation/eviction handling, and a later upload schedule.
That is a contained archive-texture staging feature, not a complete shadow,
frustum-culling, or renderer rewrite. It is worth prototyping only if Run 70
assigns the transition to texture realization or its downstream geometry
submission; otherwise it would again optimize a correlated count rather than
the blocking producer.

The verifier passes 583 checks, and all 716 one-at-a-time mutations of the new
signature bytes, RVAs, relocations, routing tables, ABI counts, pass mappings,
CSV names, and timing routes are rejected. Doctor and the release build pass.
The complete off-game self-test passes, including GPU timestamp retirement;
that one documented host-flaky assertion failed on the first two final runs
and passed on the required rerun, while every new partition test passed on all
three. Run 70 is built from
`cache/runs/run70-deferred-pass-partition.ini`. Relative to the normal live
configuration it keeps the three accepted behavior settings and enables the
same trace, F12 marker, and draw timing as Run 69; the experimental delta from
Run 69 is only the passive binary partition above. The installed/source DLLs
match at 719,872 bytes, SHA-256
`c069f5a0ca48e01c6ff1fc64b24c5de741c46d5751b3cbed6723cb40fff6d017`.
The installed/source INIs match, SHA-256
`f0af10a5f7c5e680ecf04f593008155edb95bbb4470fceef674e9f5956dea550`.
Run 69's live CSV and debug log matched their archives before removal; both
live names are absent, and the game was not launched.

## 84. Post-Run-69 safety correction: an unconfirmed queue must take the stock pose path

Run 69 proves the successful transition deferral, but its 165 consecutive
enqueue-failure counters prove that the original failure policy was too broad.
The behavior no longer equates "we called `EnqueueResource`" with "this root
may safely be omitted." After the call it re-reads both verified fields. It
skips `Actor::UpdateMeshInstance` only if the root is in loader state 1 or is
state 0 with a non-null queue link. If the root has already reached state 2,
it runs the stock pose update immediately; if it remains state 0 without a
queue link, it records the failure and also runs the stock update. A resident
caster therefore cannot be admitted with stale pose data, and an unqueueable
caster cannot remain missing for many directional builds.

This correction changes no byte site, offset, exported identity, enqueue
tuple, or INI setting. The independently verified 23-byte
`Actor::AddToScene` caller window and 24-byte `Actor::UpdateMeshInstance`
callee window remain the complete binary surface. The source verifier now
also requires both stock-forward branches before a deferred event can be
counted and passes 456 checks. The mutation audit rejects all 91/91 relevant
one-at-a-time changes, including the queue predicate and both stock fallbacks.
The off-game self-test covers the exact queue
postcondition for states 0, 1, and 2. `npm run doctor`, the release build, and
the complete self-test pass, including GPU timestamp retirement. The resulting
710,656-byte checkpoint DLL is SHA-256
`8cd457072f927455b1f8a4bc4a2af3f399f6501235d268a2cd6c21935ad8ffba`.
It is built but not installed; the game directory still contains the tested
Run 69 DLL/configuration and its live files.


## 83. Run 69 removes the synchronous directional mesh class; the remaining burst is submission/queue work

Run 69 is archived as `tqflicker-frames.run69.csv`, SHA-256
`38f95a3cd21c499e9057432f9447da1f15124e148210889382b4ed48c05a79ae`,
and `tqflicker-debug.run69.log`, SHA-256
`2b7d1ff73c04fc87926218f0b23a1681d89e7a115fc222086a64bd0ef23fa3ae`.
Both archives were byte-compared with the live files. The CSV contains 7,406
contiguous presented frames, 0--7405, totaling 95.087 s. Its five parts are
**menu** 0--2013 (18.275 s), **load-game frame** 2014 (1,513.717 ms),
**loading screen** 2015--3168 (10.125 s), **first world frame** 3169
(915.749 ms), and **play** 3170--7405 (64.257 s).

F12 at **play** frame 6826 again leaves two candidates that must not be
collapsed. The nearest is frame 6825 at 65.694 ms, ending 17 ms before the
marker; it has no Resource load and spends 46.309 ms in the `Engine::Update`
class. The normal-route loading transition is the earlier contiguous pair
6797/6798 at 95.290/120.984 ms, beginning 850 ms and ending 633 ms before the
marker. It totals 216.274 ms. The marker does not by itself prove which the
reporter felt.

The new behavior does exactly the narrow operation it was built to test on
frame 6797. In the `GraphicsShadowMapDx11::RenderDirectional` class it finds
30 state-0 `GraphicsMeshInstance` Actor roots before pose work, confirms 23
new queue insertions, and skips all 30 `Actor::UpdateMeshInstance` calls. The
later exact-class gate omits all 30 roots. There is no enqueue failure on this
frame. Directional synchronous Resource loads are **zero**, versus 21 calls /
17.382 ms on Run 68's corresponding onset; that previous population was 20
meshes / 16.419 ms and one shader / 0.963 ms. The directional CPU call falls
from 19.318 to 5.146 ms. This is direct evidence that the earlier gate removes
the targeted class, independently of total-frame variation.

The only main-thread Resource work left on frame 6797 is two colour-terrain
textures / 4.174 ms outside directional shadow, again the Gadir rocky-pebbles
base/normal pair. One additional 0.977 ms Resource call is non-main. Frame
6798 has no main-thread Resource load. Thus the deferred meshes did not merely
reappear as synchronous directional work on the next frame.

The pair is not fixed. Run 69 totals 159.309 ms in the game's `Draw` /
`DrawIndexed` calls and 2.885 ms in `Map`, or 162.194 ms of the 216.274 ms
pair. The two directional CPU calls total only 8.827 ms. Frame 6797 submits a
168.095 ms whole-GPU interval, including 54.441 ms directional shadow,
10.236 ms terrain ground, and 4.798 ms enhanced grass; the currently named GPU
classes leave 97.007 ms unclassified. Frame 6798 then spends 97.651 ms in
draw submission while its own GPU interval is only 28.044 ms. It also records
23 off-main texture creations / 55.814 ms and 38.827 ms of archive inflate;
those worker intervals overlap the frame and are not amounts to add. This is
the established first-use GPU submission/backpressure shape, now with the
cold directional mesh wait removed. It exists in D3D11 on native Windows as
well as under CrossOver; the present run does not support a host-only cause.

For scale only, the corresponding Run 68 pair was 242.708 ms. The observed
Run-69 difference is -26.434 ms, but prior nominally identical transitions
vary too widely to assign that whole delta to the behavior. The class-local
zero and the 14.172 ms directional-CPU reduction are the stronger evidence.
In collision-active, full-scene **play** frames under 60 ms with no Resource
load or region change, Run 69 minus Run 68 mean frame/GPU differences are
-0.008/-0.008 ms at 500--999 indexed draws (1,159 versus 1,141 frames),
+0.296/+0.281 ms at 1,500--1,999 (715 versus 831), and +0.259/+0.330 ms at
2,000-plus (234 versus 222). The 1,000--1,499 population is only 42 versus 43
frames and is not used. This excludes a gross steady-scene regression; it is
not a fine improvement claim and no across-run p50 is used.

One safety result needs correction before this becomes an accepted default.
Across **play**, the Actor gate sees 255 state-0 roots and no state-1 roots.
It confirms 76 new queue insertions; 14 were already linked to a queue. On
frames 6920--7084, exactly one state-0 attempt per frame fails to establish
either a nonzero state or a queue link, producing 165 consecutive
`engine_shadow_actor_pose_enqueue_failed` events. The trace does not retain
the Resource identity, so calling all 165 one object remains an inference,
but the run proves the current code can keep omitting an unconfirmed root for
several seconds. The safe policy is narrower: skip pose work only for state 1,
an existing queue link, or a state-0 enqueue whose postcondition is confirmed;
on failure, forward the stock `Actor::UpdateMeshInstance` call. That fallback
does not affect frame 6797, where every attempted enqueue succeeds.

Visual acceptance is still separate evidence. The completion message did not
say whether a missing actor or shadow pop was seen, so this run does not claim
that result. After the failure fallback and visual answer, the next useful
instrument is a GPU-pass breakdown of the 97.007 ms unclassified interval,
not another mesh/resource A/B and not another message-pump experiment.


## 82. Run 69 setup: defer cold Actor roots before directional pose work

Run 68 identifies an earlier exact boundary rather than a reason to rewrite
the shadow system. All 20 cold mesh Resources on the marked **play** transition
frame are synchronously ensured by `GraphicsMeshInstance::UpdatePose`, called
from `Actor::UpdateMeshInstance`, called from `Actor::AddToScene`, while the
verified `GraphicsShadowMapDx11::RenderDirectional` bracket is live. The
accepted `GraphicsMeshInstance::GetNumShadowRenderPasses` root gate comes
later; these roots have already reached state 2 by then and escape it.

Run 69 adds `[performance] shadow_defer_cold_actor_pose=1`. It retargets only
the direct `Actor::AddToScene -> Actor::UpdateMeshInstance` `E8` at RVA
`0x111fd5`, inside a 23-byte verified caller window. An independent 24-byte
callee window verifies the shared six-byte prologue, exported
`Actor::UpdateMeshInstance` identity at `0x112060`, and the mesh instance at
`Actor+0x184`. The wrapper follows that exact instance to its root Resource at
`GraphicsMeshInstance+0x4`. On the main thread and inside the DX11 directional
class only, state 0 is handed to the stock loader with the established
`(priority=1, notify=true, immediate=false)` tuple and state 0/1 skips this one
pose update. `Actor::AddToScene` still adds the renderable; the already
accepted exact-class pass-count gate then returns zero until the root reaches
state 2. Resident actors, the color class, point shadows, workers, and every
other `Actor::UpdateMeshInstance` caller retain stock behavior.

This test is not justified by 16.419 ms alone. Keeping those 20 roots cold at
the later gate also omits their shadow draws from the transition frame, so it
can affect part of Run 68's 101.390 ms directional GPU interval and the next
frame's 84.977 ms `DrawIndexed` drain. The visual trade remains temporary
missing local casters while the ordinary background request completes; check
explicitly for shadow popping or missing actors. `shadow_split` is untouched.

The switch defaults off, reaches `install()` with the performance probe off,
and brings no trace group. Enabling it implies the complete later
`shadow_defer_cold_alpha` patch set because skipping pose work without the
later root rejection would be unsafe. With tracing already enabled, five new
count-only `engine_shadow_actor_pose_*` columns record state and enqueue
outcome; no engine duration is named `_ms` or charged to the mod.

The exact caller/callee windows, field, call destination, export, option,
directional/state predicates, enqueue tuple, activation dependency, rollback,
and CSV identity are independently checked. `verify-sites.py` passes 454
checks. Doctor, the release build, and the complete off-game self-test pass,
including GPU timestamp retirement and the new default-off/no-trace and
counter-partition tests. The mutation audit rejects all 87 one-at-a-time
changes, including all 16 added for this boundary.

Run 69 is built from `cache/runs/run69-defer-cold-actor-pose.ini`. Relative to
Run 68, the only behavior variable changed is
`shadow_defer_cold_actor_pose: 0 -> 1`; both accepted behaviors and the same
four instruments remain enabled. The 710,656-byte installed DLL is
byte-identical to the build at SHA-256
`29f0f725734c3412e609e1b3b4afb69919c66a0e64d876fedfbd616cda6a6dd5`.
The installed/source INIs are byte-identical at SHA-256
`2f75bdc85228220473aa389d7cb9b2dd7769397110a91657676dce9e1776d6a7`.
Run 68's live CSV/log matched their archives before removal and both stale live
names are absent. The game was not launched. Run the same normal route, press
F12 after the **play** transition, and report any visible missing actor or
shadow pop separately from the transition's felt duration.


## 81. Run 68 resolves the remaining cold meshes to Actor pose work before caster eligibility

Run 68 completed with 7,293 contiguous presented frames, 0--7292, totaling
96.435 s. Its five session parts are **menu** 0--1897 (17.840 s), **load-game
frame** 1898 (1,511.398 ms), **loading screen** 1899--2967 (9.492 s), **first
world frame** 2968 (828.877 ms), and **play** 2969--7292 (66.763 s).

F12 at **play** frame 6654 is still a reaction anchor, not proof that the
nearest candidate is the felt event. The nearest candidate is frame 6644 at
51.206 ms, ending 219 ms before the press; it has no Resource load and spends
31.117 ms in the `Engine::Update` class. The normal-route loading transition
is the earlier contiguous pair 6631/6632 at 108.875/133.833 ms, beginning
740 ms and ending 498 ms before the press. That pair totals 242.708 ms. It is
the same resource-plus-submission shape as the old marked transition, but the
marker cannot by itself prove whether the reporter reacted to the pair or the
nearer update frame. Neither is selected by whole-file `max()`.

On frame 6631, 23 main-thread Resource calls / 26.846 ms partition exactly
into 21 / 17.382 ms inside the `GraphicsShadowMapDx11::RenderDirectional`
class and two / 9.464 ms outside it. The directional population is 20 `.msh`
Resources / 16.419 ms plus one `.ssh` Resource / 0.963 ms. The complete mesh
call durations include the mesh loader's own parsing, decompression, and
buffer work, but not the two separately observed terrain `.tex` loads and not
an unobserved bundle of mesh textures. There are zero directional `.tex`
loads. The two outside calls are again the Gadir rocky-pebbles base/normal
terrain pair; the exact `TerrainType` received `PreLoad(true)` in the
**loading screen** at frame 2045 and its runtime owner was preloaded through
frame 6630, yet both enter color material use in state 0. Run 67's reduction
from thirteen onset textures to two therefore repeats, while the remaining
pair again shows that one early queue request is not a residency guarantee.

The retained caller evidence is complete: F12 emits 21 records from its
120-frame **play** window, with no ring truncation. Twenty occur on frame 6631
and one on frame 6641; every one entered state 0 with no pre-existing queue
flag and has the same verified deepest subsequence:

```
Resource::EnsureAvailable return                 E+0x213137
GraphicsMeshInstance::UpdatePose return          E+0x1765a2
Actor::UpdateMeshInstance return                 E+0x112133
Actor::AddToScene return                         E+0x111fda
GraphicsShadowMapDx11::RenderDirectional virtual E+0x18ee1b
```

Static bytes select that sequence from the deliberately over-complete raw
stack candidates. `0x1765a2` follows the `E8` from `UpdatePose` to
`Resource::EnsureAvailable`; `0x112133` follows the `E8` from
`Actor::UpdateMeshInstance` to `UpdatePose`; `0x111fda` follows the `E8` from
`Actor::AddToScene` to `UpdateMeshInstance`; and `0x18ee1b` follows the
directional renderer's virtual scene-add call. Other repeated candidates,
including the earlier `RenderDirectional+0x113a`, are a raw-stack superset and
are not promoted to callers merely because they look call-shaped. This proves
the remaining meshes are admitted during the DX11 directional scene gather,
before `GetNumShadowRenderPasses`; it does not support a stale-DX9-only label.

The resource time is not the dominant bound. Frame 6631 spends 102.641 ms in
the `Engine::Render` class and 52.733 ms in the game's `DrawIndexed` calls
while submitting a 200.097 ms whole-GPU interval, including 101.390 ms in the
directional-shadow class and 18.741 ms in the enhanced-grass class. Frame
6632 has no Resource load but spends 84.977 ms in `DrawIndexed` and 9.459 ms
in `Map`, producing a 117.300 ms `Engine::Render` interval. The CPU resource,
CPU render, GPU, and following submission intervals overlap or queue behind
one another and must not be added as independent costs.

Run 67's unusually long **first world frame** did not repeat: this run is
828.877 ms rather than 1,352.9 ms. It is a different session part and is not
evidence for the **play** behavior. The passive caller retention also has no
measurable steady-state regression. For collision-active, full-scene **play**
frames under 60 ms with no Resource load or shadow-region change, Run 68 minus
Run 67 mean frame differences are +0.104 ms at 500--999 indexed draws (1,155
versus 1,168 frames), -0.265 ms at 1,500--1,999 (842 versus 751), and +0.051
ms at 2,000-plus (223 versus 227). The corresponding GPU differences are
+0.100, -0.231, and +0.021 ms. No across-run p50 is used.

The supported next test is not a mesh-system rewrite or broad preload. Move
the existing cold-root decision to the exact `Actor::AddToScene` call before
`UpdateMeshInstance`, queue state 0, and let the later exact-class gate omit
the still-cold caster. That isolates both the measured 16.419 ms CPU load and
any shadow draws those newly admitted meshes would have added. It leaves the
color scene and all resident actors unchanged.

The archived CSV is `tqflicker-frames.run68.csv`, SHA-256
`f68a640d69949ffd4adc9e1ec346ca14939d6eb342754a5bd3a30df33542fec9`.
The during-session log is `tqflicker-debug.run68.log`, SHA-256
`0af78f91699c3da0791f3294a321d114e262f27e25cf338967fb3f3e81ceaba9`;
the live files were byte-identical to those archives at capture.


## 80. Run 68 setup: retain exact caller chains for the remaining directional cold meshes

Run 67 leaves a specific class, not a reason to broaden the current shadow
omission blindly: its marked **play** frame 6974 synchronously loads 26
state-0 meshes / 23.583 ms inside
`GraphicsShadowMapDx11::RenderDirectional`, after the exact base
`GraphicsMeshInstance` root behavior has already omitted twelve other casters.
The next question is which verified caller/dependency owns those 26 meshes.

Run 68 adds a bounded delayed report to the already installed Resource-load
and directional-shadow trace. Only a main-thread Resource whose pre-call state
is 0, whose engine filename class is mesh, and whose call occurs inside the
verified directional bracket is retained. Each record carries the filename,
pre-call queue flag, frame, duration, immediate return address only when it is
call-shaped inside a verified module, and at most 24 likewise verified
call-shaped upstream stack candidates. A fixed 128-record ring covers the 120
frames before F12 and explicitly reports truncation. The existing F12
retrieval writes the records before placing its CSV marker.

This is passive and low-frequency. The candidate frame performs a bounded
copy and stack scan only on a cold mesh load; filename/caller formatting and
log locking occur later at F12. There is no new GPU query, per-draw hook,
Resource operation, behavior setting, culling change, shadow change, or
`shadow_split` change. Both accepted behaviors, including
`terrain_preload_layers=1`, remain unchanged.

`verify-sites.py` passes 439 checks. All 71 relevant one-at-a-time mutations
are rejected, including the ring size, marker horizon, exact state/type/shadow
predicates, F12 emission, dependency activation, every terrain-layer behavior
constant, and both forbidden high-frequency colour GPU scopes. Doctor, release
build, and the full off-game self-test pass, including the directional-mesh
window boundary/overwrite tests and GPU timestamp retirement.

Run 68 is installed from `cache/runs/run68-directional-mesh-callers.ini`.
The 709,120-byte installed DLL is byte-identical to the build at SHA-256
`bec80cafbdc9fae6845ba24b1653b9b7cced2a6d86bc6d55a07d28f863f1ebc7`.
The installed INI is byte-identical to the run record at SHA-256
`d92bfd247b6edd7eeeff4595f2dc3d13b1c145dc644e060f56d878593376e12a`.
Run 67's live CSV/log matched their archives before removal; both stale live
names are absent. The game was not launched.

Run the same normal route and press F12 after the reduced **play** transition.
Also report whether the **first world frame** loading burst again feels
unusually long. The primary Run 68 result is the exact directional mesh
resource/caller population at the marker; do not choose a frame by maximum.


## 79. Run 67 removes eleven of thirteen marked colour-terrain loads; the remaining burst is directional cold meshes plus GPU submission

Run 67 completed with 7,605 contiguous frames, 0--7604, totaling 98.045 s.
Its five session parts are **menu** 0--2074 (19.422 s), **load-game frame**
2075 (1,386.5 ms), **loading screen** 2076--3441 (11.686 s), **first world
frame** 3442 (1,352.9 ms), and **play** 3443--7604 (64.199 s). The user ran
the same normal route used more than fifty times and reported that the old
stutter felt much smaller.

The behavior executed at the intended point. During the **loading screen**,
144 exact `TerrainRT::LoadRenderData -> TerrainType::LoadTextures` calls are
paired one-for-one with 144 semantic `TerrainType::PreLoad(true)` calls and
zero false calls. Another 150/150 pair occurs as new terrain is admitted in
**play**. The save loads and no visual fault was reported.

F12 at **play** frame 7013 anchors a two-frame transition. Frame 6974 is
73.472 ms and ends 887 ms before the marker; frame 6975 is 83.720 ms and ends
803 ms before it. The first frame has 29 main-thread Resource loads /
29.284 ms. Only two / 5.094 ms are outside directional shadow, both render-
phase terrain textures. Run 66's corresponding marked colour-terrain class
had 13 / 47.359 ms on its onset and another nine textures / 29.075 ms two
frames later. No such second colour-load frame follows Run 67's onset. This
does not make the submitted scenes identical, but the exact mechanism and the
reporter's observation agree: the new stock preload removed eleven of the
thirteen onset loads and roughly 42 ms from that nested colour class.

The two survivors are
`XPack3\\terraintextures\\gadir\\gadir_rockypebbles01.tex` and its
`normal.tex`, together costing 5.094 ms. Both belong to the same exact
`TerrainType*`. Its runtime layer was attached in **loading screen** frame
2598; `LoadTextures` and the new `PreLoad(true)` both completed at frame 2631;
runtime-owner `TerrainRT::PreLoad` then enumerated it 4,397 times through
**play** frame 6973. Nevertheless both Resources enter ordinary colour use in
state 0. This proves one early semantic call is not a lifetime residency
guarantee, but the current trace cannot distinguish never serviced from loaded
then evicted. Periodically walking every layer would be a broad, high-volume
response to a remaining 5.094 ms and is not supported.

The dominant remainder is already separate. Frame 6974 performs 27 state-0
directional-shadow loads / 24.190 ms: 26 meshes / 23.583 ms and one shader /
0.607 ms, inside a 26.166 ms `GraphicsShadowMapDx11::RenderDirectional` CPU
class. Twelve exact base `GraphicsMeshInstance` root casters are omitted and
eight state-0 roots are newly queued, so these 26 loads are reached through
some other shadow dependency/class. The directional GPU interval is 59.807 ms
and the whole-frame GPU interval is 104.808 ms. Frame 6975 has no Resource
load, but spends 50.607 ms in the game's `DrawIndexed` submission after that
work; its whole frame is 83.720 ms. These nested/adjacent numbers must not be
added as independent costs.

Removing the high-frequency colour GPU scopes also fixes the reported general
slowness. Against Run 63, using only collision-active full-scene **play**
frames under 60 ms with no Resource load or shadow-region change, Run 67 minus
Run 63 mean frame time is -0.109 ms at 500--999 indexed draws (1,168 versus
1,177 frames), +0.078 ms at 1,500--1,999 (751 versus 751), and -0.002 ms at
2,000-plus (227 versus 240). Corresponding GPU differences are -0.108,
+0.095, and -0.037 ms. No across-run p50 is used. Run 66's +8.266 and
+13.184 ms regressions in the latter two bands were observer cost.

One caution remains in a different session part. Run 67's **first world
frame** is 1,352.9 ms versus Run 66's 581.6 ms. Main-thread Resource counts
are similar (148 versus 143), but their durations are 810.286 versus
215.114 ms, texture creation is 352.090 versus 73.707 ms, and whole-GPU time
is 1,339.844 versus 560.602 ms. One frame in each run cannot attribute that
variance to the new preload, and it is not the reported **play** transition,
but a later acceptance run must not silently ignore the loading regression if
it repeats.

The next passive trace should retain only state-0 mesh Resource loads inside
`GraphicsShadowMapDx11::RenderDirectional` for the preceding F12 window, with
their engine filename, immediate verified caller, and bounded call-shaped
upstream candidates. That directly identifies the 26-load class before any
wider caster omission or mesh-prefetch behavior. Re-running terrain preload
or adding more high-frequency GPU scopes is not justified.

The archived CSV is `tqflicker-frames.run67.csv`, SHA-256
`b146a9a0238514d15fddb2bbec61c3d843f79aac103d8c9d4e93a21ee4604f47`.
The during-session log is `tqflicker-debug.run67.log`, SHA-256
`1f5cc7b4b6c8ffc1e1bb13f5a54289909125b8f7dcd0598e91341d915043be5a`;
the live files were byte-identical to those archives at capture.


## 78. Run 67: queue runtime layer textures at the exact post-LoadTextures boundary

Run 66 does not point to a terrain, shadow, culling, or resource-loader
rewrite. Its exact lifecycle says the layer `TerrainType` and its base, bump,
and grass texture Resources already exist during the **loading screen**; the
missing operation is the stock semantic preload that queues those Resources.
Run 67 adds only that operation.

`[performance] terrain_preload_layers=1` retargets the already verified E8 in
runtime `TerrainRT::LoadRenderData` with `detour::patchCall`. The wrapper first
calls the exact original `TerrainType::LoadTextures`, then calls the verified
exported `TerrainType::PreLoad(true)` on the same `this`. The latter's machine
body walks the base and bump vectors plus grass Resource and reaches the
game's existing `ResourceLoader::EnqueueResource` path. It does not wait,
construct a custom loader, omit a colour or shadow draw, alter frustum
culling, or change `shadow_split`.

The behavior defaults off. It reaches `engineprobe::install()` with the
performance probe off and, by itself, installs only the exact call patch; it
cannot open trace group 32768 or any other instrument. With the terrain trace
active, the behavior deliberately enters the exported `PreLoad` entry rather
than its trampoline, so the existing true/false counters prove the call ran.
The behavior becomes active only after the call site, original target, export
RVA, complete 24-byte signature, and relocation have all verified. Shutdown
restores the call and clears both behavior and diagnostic state.

The two high-frequency TerrainPlug/TerrainBlock GPU phases and CSV fields are
removed before this run. Their CPU call counts and engine `_us` spans remain.
This corrects Run 66's demonstrated observer overhead; it is not a game or fix
effect and steady-state Run 66 timing must not be used as the baseline.

`verify-sites.py` passes 429 checks. All 64 one-at-a-time terrain mutations are
rejected, including every runtime/preload RVA, signature byte and relocation,
the option name and default, the `true` argument, the post-LoadTextures order,
the activation endpoint, independent install route, and reintroduction of a
GPU scope into either colour wrapper. Doctor, release build, and the full
off-game self-test pass, including GPU timestamp retirement.

Run 67 is installed from `cache/runs/run67-terrain-layer-preload.ini`. The
705,024-byte installed DLL is byte-identical to the build at SHA-256
`3171d5301076962d41c67659444d50e1e873eb2bae5d730f6c45510d51c5dd07`.
The installed INI is byte-identical to the run record at SHA-256
`e57b0537009088cf6b061322da2b118d4f94a75871e45a619672d194be99fd52`.
Before removal, both Run 66 live files were byte-identical to their archives;
the stale live CSV/log are absent. The game was not launched.

For the result, use the same normal route and press F12 after the old **play**
loading transition. The primary outcome is not an across-run maximum: require
the fix's `TerrainType::PreLoad(true)` calls during loading, then inspect the
F12-anchored transition for the 13 outside-directional colour-terrain texture
loads that cost 47.359 ms in Run 66. A load freeze rejects thread-context
safety; missing or visibly incorrect terrain rejects behavior even if those
loads disappear.


## 77. Run 66 loads successfully and exposes missing runtime-layer preload; its high-frequency GPU scopes were intrusive

Run 66 validates §76's ABI correction. The CSV has 7,047 contiguous frames,
0--7046, totaling 94.061 s. Its five session parts are **menu** 0--1936
(17.383 s), **load-game frame** 1937 (1,302.6 ms), **loading screen**
1938--3208 (10.932 s), **first world frame** 3209 (581.6 ms), and **play**
3210--7046 (63.862 s). The save loaded, the character appeared, and the normal
route completed. Therefore the two five-explicit-argument colour wrappers,
not the game or a loader-thread GPU query, caused the Run 64/65 freeze.

F12 marks **play** frame 6618. Frame 6601 is the old loading-transition class:
109.223 ms, beginning 642 ms before the key was retrieved and ending 533 ms
before it. Frame 6617 is a separate 61.014 ms `Engine::Update` class only 29 ms
before the marker and has no Resource load. Do not substitute the closer frame
for the reported transition.

The transition frame spends 102.921 ms in the `Engine::Render` class. It has
36 main-thread Resource loads / 62.355 ms: 23 / 14.996 ms in one 16.679 ms
`GraphicsShadowMapDx11::RenderDirectional` class and 13 / 47.359 ms outside
directional shadow. All 13 outside-directional loads are textures in the
render phase. Exact colour-terrain CPU classes are 137 TerrainPlug calls /
45.531 ms and 59 TerrainBlock calls / 17.627 ms. These CPU spans and Resource
loads are nested and must not be added.

The whole-frame GPU interval is 87.644 ms. Nested first-entry-to-last-exit GPU
classes include 12.579 ms directional shadow, 15.689 ms enhanced grass,
21.809 ms DX11 `TerrainRenderInterfaceRT::RenderGround`, 76.484 ms TerrainPlug,
and 76.624 ms TerrainBlock. TerrainPlug and TerrainBlock overlap and cannot be
added. Frame 6602 has no Resource load and falls to 32.304 ms. Frame 6603 is
54.590 ms with ten outside-directional loads / 30.042 ms, including nine
textures / 29.075 ms.

The exact per-type lifecycle resolves the missing link. The affected
`TerrainType` records were attached by runtime `TerrainRT::Load` during the
**loading screen**, and their `TerrainType::LoadTextures` calls completed
during the **loading screen**. Runtime-owner `TerrainRT::PreLoad` subsequently
visited those owners and enumerated their layers through **play** frame 6600,
immediately before the transition. Nevertheless, exact semantic
`TerrainType::PreLoad` counts are zero for both `true` and `false`. The
Resources therefore existed long before frame 6601, but the runtime preload
path never queued the layer base, bump, and grass textures; ordinary colour
rendering synchronously first-used them. This is a missing runtime-layer
preload call, not a need to rewrite terrain, frustum culling, shadows, or the
resource system.

The user also reported general slowness and confirmed this was the same normal
route used for prior runs. A controlled comparison against Run 63 uses only
collision-active, full-scene **play** frames under 60 ms, excludes Resource
loads and shadow-region changes, and bands by indexed-draw count. Run 66 minus
Run 63 mean frame time is +0.574 ms for 500--999 draws (1,089 versus 1,177
frames), +8.266 ms for 1,500--1,999 (512 versus 751), and +13.184 ms for
2,000-plus (152 versus 240). The corresponding GPU deltas are +0.570,
+8.279, and +13.141 ms; `present_call` changes by only +0.013, +0.005, and
-0.009 ms. No across-run p50 is used.

This scaling identifies observer cost. `gpuBegin` records the first entry for
a phase, but `gpuEnd` deliberately records every exit so its timestamp becomes
the last exit. In Run 66 that meant about 221 immediate-context `End(query)`
calls per frame in the 1,500--1,999 band and 377 above 2,000 draws from the
new TerrainPlug and TerrainBlock scopes alone. Their diagnostic result is now
captured, so the scopes and their CSV columns are removed. The CPU call counts
and `_us` spans remain. The verifier rejects either high-frequency GPU scope
being restored.

The next supported behavior is narrow: immediately after the already verified
`TerrainRT::LoadRenderData -> TerrainType::LoadTextures` call completes, invoke
the stock `TerrainType::PreLoad(true)` on that exact layer type. Its disassembly
walks base, bump, and grass Resources and enqueues them without waiting. This
uses the game's existing queue at the point Resources first exist, during the
**loading screen**, rather than inventing a parallel loader or deferring colour
correctness. It must be a default-off behavior switch that installs with the
performance probe off; the trace only verifies that frame-6601-class colour
loads disappear.

The archived CSV is `tqflicker-frames.run66.csv`, SHA-256
`515cdb29ffd35d1a7fa35f014da6a4b3c2586ad6c6f93386712515e11eec8951`.
The during-session log is `tqflicker-debug.run66.log`, SHA-256
`91a46af9bd67d423a46f10ca126d3605db41d0de5c9bf1c3b137f9eacd585686`;
both live files were byte-identical to their archives.


## 76. Run 65 rejects the GPU-thread diagnosis; both colour-terrain wrappers had the wrong x86 argument count

Run 65 repeated the failure after the GPU-context guard. It is archived as
`tqflicker-frames.run65.csv`, SHA-256
`8132aab1179af8b96df72ca510ac4e8e983a0a6464b878f8239ae3cafce55b27`;
the during-session log is `tqflicker-debug.run65.log`, SHA-256
`43074682b369e49851a7b2f1039132fdad7ecf55629239b7f57e6a50dadb12f4`.
The CSV has 3,200 contiguous completed frames, 0--3199, totaling 30.260581 s.
Every completed row is **menu**. The **load-game frame** again never closed,
so Run 65 has no **loading screen**, **first world frame**, or **play** data.

The new partition is decisive about §75's first diagnosis. All 17 completed
calls to the exact runtime `TerrainRT::LoadRenderData` class were in the
non-main class: 17 / 37,318 us in
`engine_terrain_rt_load_render_other` / `_other_us`, zero in the main class.
The corrected shared accessor returned null there, so none of those calls
issued a GPU timestamp. Save loading nevertheless froze. Cross-thread GPU
timestamping was a real unsafe defect in Run 64, but it was not the cause of
this reproducible load-game freeze. Do not carry §75's causal hypothesis
forward.

The fresh ABI audit then found a second, concrete defect in the two unexported
colour-terrain hooks. Ghidra decompiled both functions with five parameters,
and the Run 64/65 `TerrainColourRenderFn` mistakenly treated all five as
explicit stack arguments in addition to `this`. The decompiler's count already
included the implicit `this` pointer. Machine code resolves the ambiguity:
both *TerrainPlug colour render* at Engine RVA `0x236240` and *TerrainBlock
colour render* at `0x23e1e0` end in `c2 10 00` (`ret 0x10`). Each therefore
has exactly four explicit stack arguments.

The bad fastcall wrappers accepted and forwarded five explicit arguments and
would return by popping 20 bytes where the engine caller provides 16. The
first call corrupts the caller's stack. Both classes have zero completed calls
in the Run 65 **menu** rows, which is consistent with the first colour-terrain
call occurring in the unfinished **load-game frame**; because that frame never
reached `Present`, its counters cannot appear in the CSV. This makes the ABI
defect a substantially stronger freeze explanation than the rejected GPU
hypothesis, but the causal link remains a prediction until the corrected DLL
successfully reaches the **loading screen**.

Run 66 changes only those two wrapper declarations and forwards: `this` plus
four explicit arguments, matching both `ret 0x10` bodies. It changes no game
rendering, loading, resource, terrain, culling, or shadow behavior. The durable
disassembly map now states the implicit/explicit distinction so a later reader
cannot repeat the prototype-count error.

`verify-sites.py` now passes 424 checks. It reads both concrete return sites
from the pinned Engine binary, requires `c2 10 00` at each, and requires both
wrapper types and forwards to carry exactly four explicit arguments. All 56
one-at-a-time terrain-chain mutations are rejected, including the typedef and
each individual forward call. Doctor, release build, and the complete off-game
self-test pass, including GPU timestamp retirement.

The Run 66 DLL is installed: source and installed copies are byte-identical,
704,000 bytes, SHA-256
`97149c6b2d1caeb1f131511c9d1279e63fee2364dc15b8500858bc6982f9ce28`.
Per the user's instruction, the INI was not recopied; the already installed
Run 65 INI remains byte-identical to its source at SHA-256
`28bb8420cd82d25a8367fe915713a1cbfea1f533d92760aa21ea217c86725b1c`
and requests the same trace settings recorded in
`cache/runs/run66-terrain-colour-abi.ini`. The Run 65 live CSV/log were
verified against their archives before the stale live names were removed. The
game was not launched.


## 75. Run 64 aborted in the load-game frame: the first GPU-thread diagnosis is withdrawn by §76

Run 64 is not a performance run and does not revise Run 63's game diagnosis.
It wrote 2,162 contiguous presented frames, 0--2161, totaling 21.607161 s,
before the user selected a save. Those completed rows are all **menu**. The
game then froze while the next **load-game frame** was in progress; that frame
never reached `Present` and therefore never entered the CSV. There is no Run
64 **loading screen**, **first world frame**, or **play** data. In particular,
no maximum from the partial file is a candidate for the reported freeze.

The partial CSV is archived as `tqflicker-frames.run64.csv`, SHA-256
`c5016c2057b254636624b04e78fde08101e8b7e9a4b0b6d84d954005bd817419`;
the during-session debug log is `tqflicker-debug.run64.log`, SHA-256
`75b82e5b9be13d3eb3f7935d7207e5ad903a1baba703d32414b6545380297cec`.
All ten runtime-terrain sites reported installed. In the completed **menu**
rows, exact runtime `TerrainRT::Load` ran 18 times / 163,193 us,
`TerrainRT::LoadRenderData` ran 17 times / 23,921 us, its exact calls to
`TerrainType::LoadTextures` ran 134 times / 545 us, and
`TerrainRT::PreLoad` ran 6,454 times / 24,195 us while enumerating 51,848
layer associations. These values establish only that the new ABIs and hooks
were live before the freeze; they do not classify the unfinished load-game
frame or support a game-performance conclusion.

Fresh review found one unsafe operation in the instrument. The Run 64
`TerrainRT::LoadRenderData` hook unconditionally constructed a GPU timestamp
scope using the active frame's D3D11 immediate context. The active timestamp
slot and immediate context belong to the thread that opened the render frame,
but `LoadRenderData` can be reached by save loading. If that call is on a
loader thread, the hook issues `ID3D11DeviceContext::End` concurrently with
the render thread and can deadlock the device. The successful **menu** calls
do not clear the bug: they can have been main-thread calls, while the first
save-load call takes the different thread path. The aborted trace did not yet
record a main/other split, so this is the leading instrumentation diagnosis,
not a claim that Run 64 directly proved the thread identity.

This failure is specific to the mod's new instrumentation. It is not an
explanation of the game's native Windows stutter, does not make the host the
cause, and is not evidence that `TerrainRT::LoadRenderData` itself hangs. A
trace-off abort configuration was installed immediately, retaining only the
already accepted exact `GraphicsMeshInstance` cold-root shadow deferral.

Run 65 makes the smallest falsifiable correction. The shared
`probe::currentGpuContext` accessor now returns null off the thread that opened
the frame, so no current or future diagnostic class can borrow the immediate
context from a loader. The exact `TerrainRT::LoadRenderData` hook also gates
its GPU scope on the verified game main thread. Its CPU interval is preserved
and partitioned exactly into
`engine_terrain_rt_load_render_main` / `_main_us` and
`engine_terrain_rt_load_render_other` / `_other_us`; the common
`engine_terrain_rt_load_render` / `_us` population remains their sum. Only the
main-thread class can contribute `gpu_terrain_rt_load_render_ms`. This adds no
behavior switch and changes no game loading, terrain, culling, or shadow code.
If Run 65 passes save loading, it validates removal of the unsafe operation;
if it freezes again, the whole new runtime-terrain trace group must be bisected
rather than treating the hypothesis as fact.

The added fields moved the 317-field CSV schema just beyond its former 8 KiB
line buffer. The first off-game test correctly rejected that build for a
truncated header. Header and data rows now share one audited 16 KiB bound.
`verify-sites.py` passes 422 checks, and all 53 one-at-a-time terrain-chain
mutations are rejected, including removal of either thread gate, either side
of the count/duration partition, each new CSV identity, and the line bound.
Doctor, release build, and the complete off-game self-test pass. The self-test
opens a real timestamp frame, proves the render thread receives its immediate
context, and proves a worker thread receives null; timestamp retirement also
passes.

Run 65 is installed from
`cache/runs/run65-terrain-runtime-chain-threadsafe.ini`. Source and installed
DLLs are byte-identical: 704,000 bytes, SHA-256
`d0cc1c760309372f5ef5b50b0aab2e9e384fe231120716c4e305ea18730c5f98`.
Source and installed INIs are byte-identical at SHA-256
`28bb8420cd82d25a8367fe915713a1cbfea1f533d92760aa21ea217c86725b1c`.
The stale live Run 64 CSV and debug log were rechecked byte-identical to their
archives before only those two live names were removed. The game was not
launched. First establish whether the save reaches the **loading screen**;
then complete the normal route and press F12 after the felt **play** loading
burst. A freeze before the loading screen rejects the thread-gate diagnosis
and requires trace-group bisection.


## 74. Run 63: `TerrainType::PreLoad` is absent; the runtime `TerrainRT` owner admits layer textures only in render-data construction

Run 63 is archived as `tqflicker-frames.run63.csv`, SHA-256
`884de918c81327c838b61c5288a4a231faa57ad2020ab9d260d893a0a734390c`;
its live-written debug log is `tqflicker-debug.run63.log`, SHA-256
`0abd6cfd64cdc7b44d84c306962160eaf9824b58f24ffe4f54c110ee61ac70d5`.
The CSV contains 7,162 contiguous presented frames, 0--7161, and 313 fields.
The five session parts are **menu** 0--1831 (17.560 s), **load-game frame**
1832 (1.492884 s), **loading screen** 1833--2970 (10.244389 s), **first
world frame** 2971 (845.355 ms), and **play** 2972--7161 (63.970227 s).

F12 at **play** frame 6565 is a reaction anchor. The full-scene,
collision-active **play** loading candidate is frame 6544 at 180.607 ms,
beginning 688.157 ms before the marker and followed by a 50.615 ms recovery at
frame 6545. Frame 6564 at 77.565 ms is a separate `Engine::Update`-class
candidate with no Resource load. It is closer to F12 but must not be merged
with the loading transition merely because the reaction key followed it.

**Play** frame 6544 spends 170.045 ms in the `Engine::Render` class, 5.754 ms
in the `Engine::Update` class, 1.323 ms in the message-pump class, and 3.170 ms
in the game's `present_call` class. It carries 35 main-thread
`ResourceLoader::LoadResource` calls / 101.715 ms. The
`GraphicsShadowMapDx11::RenderDirectional` class owns 22 / 22.238 ms; the
color-terrain path outside that class owns 13 / 79.477 ms, all state-0 texture
Resources. The game's D3D calls take 43.020 ms in `Draw`/`DrawIndexed` and
1.818 ms in `Map`. Its whole-GPU interval is 185.804 ms. Named, nested GPU
classes include 33.244 ms directional shadow, 0.923 ms point shadow, 26.341 ms
enhanced grass, 0.433 ms SMAA, 0.278 ms bloom, and exactly 54.213 ms in the
DX11 `TerrainRenderInterfaceRT::RenderGround` class. These CPU and GPU spans
overlap and are not additive. The ground class itself made only six calls and
used 556 CPU microseconds, so the 54.213 ms is device work, not a hidden
54 ms CPU function.

The exact 13 color-terrain Resources belong to six `TerrainType` identities.
`TerrainType::PreLoad` ran zero times in **menu**, **load-game frame**,
**loading screen**, **first world frame**, and **play**. This is not a missing
trace hook: in the same session `TerrainType::SetShaderParams` ran 235,861
times, `TerrainType::SetGrassShaderParams` ran 54,722 times, and
`TerrainRenderInterfaceRT::RenderGround` ran 9,159 times. All four Run 63
terrain hooks installed atomically. Every retained cold texture record
therefore has true=0 and false=0 for the exact owning `TerrainType*`; the
semantic layer preloader was not merely called with the wrong bool or a few
frames late.

### The static correction: the game uses runtime `TerrainRT`, not exported editor `Terrain`

Fresh static analysis corrected the class boundary. The shipped object writes
the unexported vtable at Engine RVA `0x2f8820`; constructors at `0x23dde0` and
`0x23ded0` write that identity. Its slots select *runtime* `Load` at
`0x23d8d0` (`+0x24`), `LoadRenderData` at `0x23d6d0` (`+0x28`), `PreLoad` at
`0x23d400` (`+0x34`), layer count at `0x23d060` (`+0x44`), and layer lookup at
`0x23d020` (`+0x48`). The similarly named exported `Terrain` methods are the
editor-capable implementation and must not be substituted for this class.

The runtime `TerrainRT::Load` class creates 12-byte layer records in the vector
at owner `+0x84..+0x88`; the first dword of each record is the exact
`TerrainType*`. It does not call `TerrainType::LoadTextures` or
`TerrainType::PreLoad`. Runtime `TerrainRT::LoadRenderData` later walks those
records and, at the exact `E8` at RVA `0x23d742`, directly calls exported
`TerrainType::LoadTextures` (`0x240160`) before it creates opacity textures,
terrain buffers, and geometry. Runtime `TerrainRT::PreLoad` walks nearby
`TerrainObject`s, but its complete body directly calls neither
`TerrainType::PreLoad` nor `TerrainType::LoadTextures`. This is the concrete
omission Run 63 exposed: layer texture admission is tied to render-data
construction, while the runtime owner's ordinary nearby-object preload omits
the layer `TerrainType`s.

Run 63's upstream addresses also identify both later color classes.
The unexported five-argument function at RVA `0x236240`, named *TerrainPlug
color render* by behavior rather than a missing symbol, calls
`TerrainType::SetShaderParams` at `0x2366cd`. The corresponding unexported
*TerrainBlock color render* at `0x23e1e0` calls the same setter at `0x23e73f`.
The 24-byte body windows around both calls make those identities independent
of their identical 19-byte entry shapes. All runtime terrain, color-terrain,
resource, root-mesh, and shadow material/caster targets are now indexed in
`disassembly-targets.md`; both reproducible Ghidra seed sets were extended and
regenerated. The streaming audit contains 1,483 Engine functions from 189
roots; the shadow audit contains 695 Engine functions from 70 roots.

### Withdrawn Run 64 setup: §75 found its GPU scope was not passive

This is the pre-run setup record, retained for history and corrected forward
by §75. Its claim that Run 64 was passive is withdrawn: the exact
`TerrainRT::LoadRenderData` GPU scope could touch the immediate D3D11 context
from a non-render thread. The intended trace-only terrain group is atomic across ten
sites: it retains the four Run 63 exports, detours runtime `TerrainRT::Load`,
`TerrainRT::LoadRenderData`, and `TerrainRT::PreLoad`, patches only the exact
existing `LoadRenderData -> TerrainType::LoadTextures` call, and brackets the
unexported TerrainPlug and TerrainBlock color-render classes. It changes no
texture admission, preload, rendering, culling, or shadow behavior.

For the runtime owner, the CSV adds `engine_terrain_rt_load` / `_us`,
`engine_terrain_rt_load_render` / `_us`,
`engine_terrain_rt_load_textures` / `_us`, and
`engine_terrain_rt_preload` / `_us`. Layer enumeration is bounded at 64, with
`engine_terrain_rt_preload_layers` and an explicit
`engine_terrain_rt_layer_overflow`. The color functions add
`engine_terrain_plug` / `_us` and `engine_terrain_block` / `_us`. Non-blocking
game-time timestamps add `gpu_terrain_rt_load_render_ms`,
`gpu_terrain_plug_ms`, and `gpu_terrain_block_ms`; like draw submission,
mapping, and the existing GPU columns, these are not charged to the mod.

The fixed 2,048-slot identity table now retains, per exact `TerrainType*`, the
count and first/last frame for three runtime-owner events: layer attachment by
successful runtime `Load`, Resource creation completed by the exact
`LoadTextures` call, and enumeration by runtime-owner `PreLoad`. F12 formats
this beside the existing exact cold texture record after the candidate. The
result can distinguish “never admitted until first render,” “admitted during
render-data construction but never queued,” and “owner preload ran but omitted
its layer types” without another trace build.

The four new shared `55 8b ec 83 e4 f8` entries verify 19--23 bytes and steal
six. Runtime `LoadRenderData` verifies 20 bytes and steals its first two
complete instructions, eight bytes. The direct texture-admission site uses
`detour::patchCall`; all ten sites roll back if any dependency fails.
`verify-sites.py` now passes 419 independent checks. Its extension caught and
corrected two decompiler transcription errors before installation: one opcode
in each layer accessor and an accidental byte from the following function.
All 46 one-at-a-time mutations of the new RVAs, export name, vtable slots,
relocations, signature bytes, call offset, layer bound, event endpoints,
counter/GPU identities, and `_us` unit are rejected. Doctor, release build,
and the complete off-game self-test pass, including GPU timestamp retirement.

Run 64 is installed from `cache/runs/run64-terrain-runtime-chain.ini`. Source
and installed DLLs are byte-identical: 703,488 bytes, SHA-256
`d1e9edd8a15bac0145cac6fd2e29d2ad92f66227aad8e1d7416fb2c5a7ac7a9f`.
Source and installed INIs are byte-identical at SHA-256
`3368b138e9d935716a236f330dde5fbeb0aab5912d707cb60ecfb0bf3c6d025b`.
The stale live Run 63 CSV and debug log were byte-checked against their
archives before removal. The game was not launched. Run the normal route and
press F12 after the felt **play** loading transition; multiple or late presses
are safe.


## 73. Run 62: the marked play burst is a resource/first-use submission followed by GPU queue backpressure; the colour-terrain owner is exact

Run 62 is archived as `tqflicker-frames.run62.csv`, SHA-256
`33a1bc8eda5e59984c1803c2e34d5dd3fa191c751071a4726c16edcd62379912`;
its live-written debug log is `tqflicker-debug.run62.log`, SHA-256
`97d64a35c58e5bfea9eda5f7a2fa113df35eefeed36ec9ff8602dd564653b1ea`.
The CSV contains 7,202 contiguous presented frames, 0--7201. The five session
parts are **menu** 0--1734 (16.507 s), **load-game frame** 1735 (1.540 s),
**loading screen** 1736--2900 (10.053 s), **first world frame** 2901
(758.028 ms), and **play** 2902--7201 (66.877 s).

F12 at **play** frame 6455 is a reaction anchor. The large full-scene,
collision-active **play** transition is the contiguous pair 6441/6442 at
219.289/125.131 ms. It begins 641 ms before the marker and ends 296 ms before
it. A separate frame 6454 is 56.144 ms and ends only 20 ms before F12, but its
37.696 ms is in the `Engine::Update` class, it has no Resource load, and its
17.140 ms render is ordinary. Preserve that as a separate small update-class
candidate; proximity to the reaction key does not license merging it with the
large loading pair.

Frame 6441 spends 212.181 ms in `Engine::Render`, 3.943 ms in
`Engine::Update`, 0.686 ms in the message-pump class, and 2.300 ms in
`present_call`. Its 33 main-thread Resource loads / 66.163 ms partition
exactly into 20 / 17.973 ms inside the
`GraphicsShadowMapDx11::RenderDirectional` class and 13 / 48.190 ms outside
it. All 13 outside calls are state-0 render-phase terrain texture Resources.
The frame creates 15 textures, 172 buffers, and 10 shaders, while submitting
302.910 ms of whole-frame GPU work. Named GPU spans are 90.649 ms in the
`GraphicsShadowMapDx11` directional class, 0.901 ms in the point-shadow class,
20.324 ms in the enhanced-grass class, 0.423 ms in SMAA, and 0.269 ms in
bloom. Those CPU calls and nested GPU intervals are not additive, and the
current named spans do not classify most of the whole-GPU interval.

Frame 6442 proves the coupled second half. It has nearly the same scene load
(121 non-indexed and 1,498 indexed draws, versus 121/1,485 on frame 6441), no
main-thread Resource load at all, only 3.034 ms in the directional-shadow CPU
class, and only 22.267 ms of its own GPU work. Nevertheless it spends 104.021
ms inside the game's D3D11 `Draw`/`DrawIndexed` calls and 117.343 ms in
`Engine::Render`. Across the pair, draw submission consumes 123.531 + 104.021
ms. The first CPU frame finishes after 219.289 ms despite having placed
302.910 ms on the GPU queue; the following frame then blocks in D3D draw calls
while that prior work retires. Frame 6443 returns to 18.824 ms with 8.488 ms
of draw submission and 19.966 ms of GPU work. This is the same mechanism
measured in §37, now tied to the user's marked **play** transition rather than
selected by `max()`.

This does not require a Wine-only explanation. D3D11 resource creation,
first-use work, command submission, and queue backpressure exist on native
Windows as well. CrossOver/DXMT may change their exact cost, but the measured
class is not “a host round trip,” and the game's native-Windows stutter is
compatible with it. Conversely, the frame does not prove that every one of
the 302.910 GPU milliseconds is texture upload; the exact ground renderer is
still unnamed.

Run 62's upstream capture resolves the 13 colour-terrain texture requests.
For the ordinary base/bump layers, the leading verified return is
`E+0x23fc4e` or `E+0x23fcca`: each is immediately after a call to
`Resource::EnsureAvailable` inside exported
`TerrainType::SetShaderParams` (RVA 0x23fb90). The grass mask begins at
`E+0x23faf7`, immediately after the same call inside exported
`TerrainType::SetGrassShaderParams` (RVA 0x23fa40); the verified caller
`E+0x23b1f7` is inside the DX11
`TerrainRenderInterfaceRT::RenderGrass` class. These are exact class names,
not an inference from filenames. The rest of each raw stack remains a
call-shaped superset and is not promoted to a call stack.

Skipping either `EnsureAvailable` is not yet a safe fix. Immediately after the
call, `SetShaderParams` reads the Resource's resident render-texture fields and
binds the selected layer. The empty-vector path can bind null, but that does
not prove that a cold non-empty entry has an acceptable fallback. More
importantly, the entire observed colour-terrain CPU part is 48.190 ms of the
344.420 ms pair; it cannot remove the larger first-use/GPU component by
itself.

The engine already exports the more promising semantic boundary:
`TerrainType::PreLoad(bool)` at RVA 0x23fe80. Re-reading its body shows that
`false` skips all work, while `true` walks that same object's base-texture
vector at +0x24/+0x28, bump vector at +0x30/+0x34, and optional grass texture
at +0x88. Bump and grass entries call the exported
`ResourceLoader::EnqueueResource`; the base path performs the engine's
equivalent queue/touch logic. This is not a return to §23's generic bounded
prefetch proposal: it is the engine's own type-specific preloader for the
exact Resources forced by run 62. Static code alone cannot say whether it was
called for the exact `TerrainType` objects, or whether it ran too late.

Run 63 therefore changes no behavior. One atomic trace group detours the exact
four exports `TerrainType::PreLoad`, `TerrainType::SetShaderParams`,
`TerrainType::SetGrassShaderParams`, and
`TerrainRenderInterfaceRT::RenderGround`. The two shared-prologue targets
verify 24 bytes and steal six; the two parameter methods verify 21 bytes and
steal their first two complete instructions, eight bytes. A fixed 2,048-slot
identity table records true/false preload history. When an existing retained
outside-directional load occurs inside either parameter method, its record
copies the exact `TerrainType*`, material/grass path, material index, counts,
and last preload frames before calling the original loader. F12 formats it
later. An exhausted identity table has an explicit CSV counter rather than
silently reading as “never preloaded.” No allocator or log write runs on the
candidate frame.

The fourth hook adds `engine_terrain_ground` / `_us` and a non-blocking
`gpu_terrain_ground_ms` timestamp around the DX11
`TerrainRenderInterfaceRT::RenderGround` class. It always forwards the same
arguments and neither calls `PreLoad` nor changes texture fallback. All new
engine duration columns end in `_us`; the GPU result remains game time and is
not charged to the mod. The group installs only with the performance trace.
The accepted `shadow_defer_cold_alpha=1` behavior remains unchanged and
`shadow_split` is untouched.

`verify-sites.py` passes 377 checks. Fourteen independent changes to the four
target RVAs, relocation operands, trace group, table size, and signature bytes,
plus eight changes to the stolen widths, group installation, main-thread
association gate, preload snapshot, and GPU phase, plus the explicit full-table
signal are all rejected (23/23).
`npm run doctor`, the release build, and the complete off-game self-test pass,
including GPU timestamp retirement. The 697,344-byte run-63 DLL has SHA-256
`8d44db8931aa8eee9b68bf2ca75496b26e0e55d3202d94ac10ed30b3df80860a`.

Run 63 is installed from `cache/runs/run63-terrain-preload-ground.ini`.
Installed and source DLLs are byte-identical at the hash above; installed and
source INIs are byte-identical at SHA-256
`8fdbe65f72d77da088150b43189462f65b617dae0bdcc2d0ab6b164bfb5fb303`.
The stale live run-62 CSV and debug log were byte-compared with their archives
before removal. The game was not launched. Run the normal route and press F12
after the felt **play** loading transition; multiple or late presses are safe.


## 72. Run 61: the marked burst is two frames, terrain loads are only one part,
and the recorded caller is a generic wrapper

Run 61 is archived as `tqflicker-frames.run61.csv`, SHA-256
`18465ad1ce602ee43195b45ef7247812f933d72cb09664990ea2b2cdc40efaff`;
its live-written debug log is `tqflicker-debug.run61.log`, SHA-256
`c92d7b205b5680dfd9d883074515034769556906ae854207f88fd83cbed43412`.
Both archives were byte-compared with their live names. The CSV contains 7,135
contiguous presented frames, 0--7134. The five session parts are **menu**
0--1768 (17.088 s), **load-game frame** 1769 (1.565 s), **loading screen**
1770--2909 (10.129 s), **first world frame** 2910 (686.861 ms), and **play**
2911--7134 (64.397 s).

F12 at **play** frame 6560 is a late reaction anchor. It follows a contiguous
two-frame, full-scene, collision-active **play** burst: frame 6522 is 175.182
ms and frame 6523 is 127.451 ms. The burst begins 1.034 s before the marker
and ends 731 ms before it. Treating only the largest of those rows as the felt
event would discard almost half of the pause; the marked event is the pair.

The first frame spends 166.108 ms in `Engine::Render`, 5.462 ms in
`Engine::Update`, and 1.157 ms in the message-pump class. Its 33 main-thread
`ResourceLoader::LoadResource` calls / 49.683 ms partition exactly into 20 /
22.772 ms inside `GraphicsShadowMapDx11::RenderDirectional` and 13 / 26.911 ms
outside it. Every outside-directional call is in `Engine::Render`, entered in
state 0, and is a texture Resource. Their engine filenames are terrain layers:
river bed, mud, grass, swamp, forest, rock, normal, and grass-mask textures.
The longest one is 6.814 ms and the other twelve are 1.003--2.052 ms.

That is a real synchronous colour-render dependency, but it is not the whole
burst and it does not reproduce run 60's 79.135 ms complement. On frame 6522,
the complete GPU interval is 254.833 ms, including 90.117 ms in the
`GraphicsShadowMapDx11` directional class and 23.490 ms in the enhanced-grass
class. The CPU resource calls, enclosing CPU render interval, and GPU intervals
overlap and are not added.

The second frame is the sharper exclusion. Frame 6523 spends 119.320 ms in
`Engine::Render`, but has no outside-directional main-thread Resource load and
only one 7.194 ms texture load inside the `GraphicsShadowMapDx11` directional
class. Its own GPU interval is 28.272 ms, including 15.434 ms directional and
0.684 ms enhanced grass. This has the same shape as the queue-drain row proved
in §37: a large GPU submission on frame N and CPU time surfacing inside
`DrawIndexed` on N+1. Run 61 did not enable `draw_timing`, so that is a
well-founded hypothesis for this particular pair, not yet a measurement.

The immediate-caller field also failed to answer one part of its question.
All 13 terrain loads report `E+0x213137`. Re-reading the pinned `Engine.dll`
(SHA-256 `0aedbb1805b4a5616f74e34d4f609f392e2c2dd4561c64c118f4772ab4f694f6`)
shows that RVA is the instruction after the `E8` at 0x213132 inside exported
`Resource::EnsureAvailable` at RVA 0x2130f0; that call resolves to exported
`ResourceLoader::LoadResource` at RVA 0x213ed0. So the label is verified but
generic. It proves the normal EnsureAvailable path, not which terrain-renderer
class requested it. Calling it a terrain-renderer call site would repeat the
same immediate-versus-upstream mistake that earlier sections made with nested
durations.

The diagnostic itself is internally consistent: on all 7,135 frames, main
load count/duration equals directional plus outside-directional, and both the
phase and filename-type partitions independently sum to the outside pair.
The F12 report retained all 13 records and says `truncated=0`. The accepted
`GraphicsMeshInstance` root behavior also remained active on frame 6522,
omitting five state-0 roots, while the alpha-tested-caster behavior omitted 19;
neither enqueue-failure counter fired. This run makes no new visual-safety
claim because the reporter did not report one either way.

The supported next move is not to defer terrain textures yet. Even a perfect
elimination of the measured synchronous part is bounded by 26.911 ms on a
302.633 ms marked burst, and colour terrain has a visible fallback/popping
question that directional-shadow textures did not. Run 62 keeps all behavior
fixed, retains up to 24 verified-module call-shaped upstream stack candidates
for each marked outside-directional load, and enables the already validated
`draw_timing=1` instrument. The raw stack is explicitly a superset because the
engine omits frame pointers; repetition plus static disassembly must identify
the real increasing-order caller subsequence. This will name the class that
forces the terrain Resources and directly test whether frame 6523 is
`DrawIndexed` queue backpressure.

The new capture adds no patch site. It extends the existing fixed 128-record
ring and performs one bounded committed-stack scan while retaining a matching
load; formatting and live logging still occur only at F12. `verify-sites.py`
passes 353 checks. All 16 one-at-a-time changes to the new depth, stack handoff,
committed-range gates, two bounds, module/call filters, ordered stores,
duplicate collapse, rendering, and log identity are rejected; together with
run 61 this is 150/150 relevant mutations rejected. Doctor, the release build,
and the full off-game self-test pass, including GPU timestamp retirement.

Run 62 is installed from
`cache/runs/run62-outside-directional-upstream-draw.ini`. The 692,736-byte DLL
and installed copy are byte-identical at SHA-256
`790fb0e57280a7486d8ee75123084603222fece16c407cfeacbd3994f95382b0`.
The source and installed INIs are byte-identical at SHA-256
`1838456d57220419d652bcb78201358e3223c0847963e92d418768825e048786`.
The old live CSV and debug log were rechecked byte-identical to their run-61
archives before removal. The game was not launched. Run the normal route and
press F12 after the felt **play** burst; a late press like run 61's is within
the retained window, and multiple presses remain safe.


## 71. Run 61: name the main-thread Resource loads outside directional shadow

Run 60's F12 anchor identifies full-scene, collision-active **play** frame
6461 at 260.847 ms. Its 30 main-thread `ResourceLoader::LoadResource` calls
carry 102.518 ms of summed complete-call duration, but only 17 / 23.383 ms are
inside `GraphicsShadowMapDx11::RenderDirectional`. The arithmetic complement
is 13 calls / 79.135 ms outside that directional bracket. Those calls could
still be in `Engine::Render`, `Engine::Update`, or neither, and the existing
CSV supplies neither their Resource class nor their immediate caller. Calling
all 79.135 ms "render loading" would therefore be another inference from a
frame total. Run 61 measures the missing dimensions before choosing a lever.

The instrument reuses existing verified hooks rather than adding a new patch
site. `ResourceLoader::LoadResource` already verifies 22 bytes and steals
seven; `Engine::Update` and `Engine::Render` each verify 24 bytes and steal
six; the directional call uses the existing 23-byte `patchCall` window. The
loaded-state and filename accessors each verify 16 bytes and are called only
after their export/RVA identities match. The outside-directional attribution
becomes active only when all of those load, phase, directional, and accessor
dependencies are live. A missing dependency produces zeroes, not a falsely
large outside class.

For every main-thread LoadResource call outside the live directional bracket,
the CSV records one common count / `_us` duration pair. Render, update, and
other phase pairs partition it once; mesh, shader, texture, and other filename
classes partition it independently. These are complete game-call durations,
which can nest, and they are not added to their enclosing Engine phase. All
duration columns end in `_us`, so `tools/frames.py` does not charge them to the
mod. On every frame with the full instrument active, the expected count and
duration invariants are:

`engine_res_load_main = engine_shadow_res_load + engine_res_outside_dir`

`outside_dir = render + update + other = mesh + shader + texture + type_other`

F12 supplies the identities without making the candidate frame write a log.
Each outside-directional call copies its frame index, pre-call loaded state,
phase, filename class, at most 128 filename bytes, and immediate return address
into a 128-record rolling ring. On the existing F12 key-down retrieved by the
game's own `PeekMessageA`, the preceding 120 frames are formatted and handed
to the already asynchronous debug logger. A caller is labeled `E`, `G`, or
`T` plus RVA only when it lies in the verified Engine.dll, Game.dll, or TQ.exe
text and immediately follows one of the accepted call encodings; otherwise it
is explicitly `unverified`. Already emitted records are not repeated on a
later press. If 128 records cannot cover the retained pre-reaction window, the
F12 row sets `engine_res_outside_dir_marker_truncated` and the live log says so.

This is passive relative to run 60. The exact original LoadResource trampoline
runs once with unchanged arguments. During the candidate frame the added work
is bounded metadata copying and counters; filename formatting and log locking
happen only after F12. The accepted cold-root and texture omissions remain
enabled through `shadow_defer_cold_alpha=1`; colour rendering, all shadow-map
settings, culling, resource behavior, and `shadow_split` are unchanged.

`verify-sites.py` now passes 351 checks. All 25 one-at-a-time changes to the
three ring/window/name constants, outside-directional and classification
gates, filename/caller/phase/frame capture, ring bound and publish order,
window capacity/membership, one-shot reporting, caller label, truncation
counter, update/render brackets, F12 emission, dependency activation, and
shutdown are rejected: 25/25 new and 134/134 cumulative relevant mutations.
The off-game self-test additionally proves both independent partitions sum to
the common population, includes the exact 120-frame boundary, excludes future
events, and reports a one-record overflow. Doctor, release build, and the full
self-test pass, including GPU timestamp retirement.

Run 61 is installed from
`cache/runs/run61-outside-directional-resources.ini`. The 692,224-byte DLL and
installed copy are byte-identical at SHA-256
`47fbd6acff07fd4b97598edb8e448f181fca9f10cbf2512def30e1a162a2f20b`.
The source and installed INIs are byte-identical at SHA-256
`ed7a07f068851749af8f0b558188502cf6a83583fe63f7cd87625365c17513f7`.
The old live CSV and debug log were rechecked byte-identical to their run-60
archives before removal. The game was not launched. Run the normal route and
press F12 after the felt **play** loading burst; multiple presses are safe and
emit only new retained events.


## 70. Run 60 result: the narrow root-mesh omission is visually safe, but the
marked burst is mostly outside it

Run 60 is archived as `tqflicker-frames.run60.csv`, SHA-256
`87bcb615b8d81fc037f407b89e41246144361369b76be458d735c0431e536a03`;
its live-written debug log has SHA-256
`ab56a4629a000ad382e9013ac77e1db4e0e265768d409b31c4271ba446636697`.
Both archives are byte-identical to the current live names. The CSV contains
7,131 contiguous presented frames, 0--7130. The five session parts are
**menu** 0--1782 (16.542 s), **load-game frame** 1783 (1.507 s), **loading
screen** 1784--2865 (9.501 s), **first world frame** 2866 (767.918 ms), and
**play** 2867--7130 (64.725 s).

F12 at **play** frame 6481 is a reaction anchor. The only full-scene,
collision-active candidate over 40 ms in the preceding two seconds is
**play** frame 6461 at 260.847 ms; it ends 417 ms before the marker and starts
678 ms before it. The frame changes directional-shadow region. It spends
217.391 ms in `Engine::Render`, 9.206 ms in `Engine::Update`, and 31.661 ms in
the message-pump class. The pump's 31.635 ms across three `PeekMessageA` calls
returned two messages and dispatched both; this is not run 39's empty-peek
shape. Render is independently dominant, so this observation does not reopen
the message pump as a lever.

The new exact base-class `GraphicsMeshInstance::GetNumShadowRenderPasses`
behavior is active on the marked frame. It omits six state-0 root meshes and
successfully gives all six to their own `ResourceLoader`; no enqueue fails.
The next two directional builds omit eight and six roots respectively; the
third following build, frame 6464, omits none. The count-only data cannot prove
caster identity, but it does show that the cold-root population is transient
and clears within three subsequent builds. The reporter specifically saw no
missing shadow, shadow pop, or other visual fault during the session. That is
the required visual acceptance of this narrow temporary omission.

It is not a controlled magnitude result. Run 59 has a different route and a
two-frame marked burst, so comparing its maximum with run 60's maximum would
repeat the project's original mistake. Run 60 proves that the mechanism fires,
queues cold roots, stops omitting them once resident, and is visually
acceptable. It does not isolate how many milliseconds it saved.

The remaining marked frame is much broader than directional shadows. Its 30
main-thread Resource loads cost 102.518 ms. Only 17 / 23.383 ms occur inside
`GraphicsShadowMapDx11::RenderDirectional`: 16 meshes / 21.923 ms and one
shader / 1.460 ms, all entered in state 0. Therefore 13 main-thread loads /
79.135 ms occur outside directional shadow. The enclosing directional CPU
call is 25.145 ms. Its GPU interval is 30.071 ms while the whole-frame GPU
interval is 237.965 ms. CPU loads, enclosing CPU calls, and GPU intervals are
nested or overlapping and must not be added; the comparison only establishes
that directional shadow is no longer the dominant class in this felt burst.

Frames 6462 and 6463 are 28.030 and 17.356 ms. Frame 6462 synchronously loads
one remaining directional mesh / 4.831 ms inside a 6.970 ms shadow call;
frame 6463 has no Resource load and a 1.834 ms shadow call. Unlike run 59,
there is no second large render/backpressure frame after this onset. That
difference is descriptive only because the submitted scenes differ.

Across **play**, the new root hook omits 27 instances: 26 state 0 and one state
1. Ten state-0 roots are newly enqueued and no enqueue fails. Directional
shadow still performs 75 synchronous loads / 75.728 ms: 70 meshes / 72.173 ms
and five shaders / 3.555 ms, all entered in state 0. Texture loads remain zero.
All 3,046 directional builds report the context patch active; every context,
table, and enqueue failure is zero. The existing material, bump, base-override,
and alpha gates remain active without a failed enqueue.

The supported conclusion is to keep the narrow root-mesh behavior, not widen
it blindly. The next passive trace should classify the 13 main-thread Resource
loads / 79.135 ms outside directional shadow on the marked **play** frame by
exact Resource class and verified immediate caller. The remaining whole-GPU
interval is real, and §37 already establishes where GPU queue pressure blocks
the CPU (`DrawIndexed`); another per-draw timing boot would describe the sink
again without identifying which resource or scene transition created the
work.


## 69. Run 60: defer an exact caster before its cold root mesh is forced

Run 59 removes every directional-shadow texture load in **play**, leaving two
coupled costs on its marked full-scene, collision-active **play** onset. Frame
6692 synchronously loads 29 meshes / 38.219 ms and one shader / 2.843 ms inside
a 41.139 ms directional CPU call, while its directional GPU interval is
137.047 ms and whole-GPU interval 329.861 ms. Five root-mesh ensures at the
exact `GraphicsMeshInstance::GetNumShadowRenderPasses` boundary cost 11.111
ms. Frame 6693 then drains the prior GPU queue. A further texture or draw-time
instrument does not choose a new lever; the already proved root-mesh boundary
does.

Run 60 extends the existing `shadow_defer_cold_alpha=1` behavior at that one
earliest boundary. The exact exported base-class method at RVA `0x173440`
loads the root mesh pointer from instance+`0x4`, null-checks it, calls
`Resource::EnsureAvailable`, then reads the pass count at mesh+`0x7c`. Its
complete 24 bytes were already independently matched to the pinned image. The
entry detour now steals six complete, non-relative bytes—`push esi; mov
esi,[ecx+4]; test esi,esi`—with no relative operand. Its trampoline replays
those bytes before returning to the original null check.

The behavior predicate is deliberately narrower than “any cold mesh.” It is
the exact base `GraphicsMeshInstance` export, on the main thread, while inside
`GraphicsShadowMapDx11::RenderDirectional`, with a non-null root mesh in raw
loaded state 0 or 1. State 0 is handed to its own verified
`ResourceLoader` using the stock `(priority=1, notify=true, immediate=false)`
tuple; state 1 is already loading. The method returns zero passes until state
2, so no caster/pass record, later dependency, or GPU draw is created for that
one caster. State 2 and every non-directional call forward through the exact
original method. Derived renderables that override the virtual method never
enter this base export.

This is a temporary local missing-shadow trade, not whole-map reuse. It can
test more than the direct 11.111 ms because a cold root caster can own later
dependent mesh loads and directional GPU draws. It cannot remove non-mesh
casters, already-resident newly visible geometry, or the non-shadow resource
loads on the onset. Colour rendering, point/spot shadows, resident casters,
culling, map size, and `shadow_split` are unchanged. A visible local shadow
pop rejects or bounds it even if the burst becomes shorter.

The behavior detour is an atomic dependency of the existing shadow-fix chain.
It is installed only after the 24-byte signature and export/RVA identity pass;
a failure restores the material, context, record, and bump call patches. With
the trace enabled, the old internal Ensure call instrument is not patched
inside the detoured entry; the behavior wrapper itself emits count-only
`engine_shadow_mesh_omitted`, state-0/state-1, enqueued, and enqueue-failed
columns. The fix stays default-off, reaches `install()` with the performance
probe off, and brings no trace group.

`verify-sites.py` passes 336 checks. Twenty independent mutations of the new
mesh-field constant, reused RVA and byte identity, exact state predicate,
main/directional gates, field use, enqueue tuple, zero-pass return, six-byte
steal, atomic dependency, rollback/activation, diagnostic coexistence,
pre-verification, and both rollback and shutdown pointer clearing are all
rejected: 20/20 for run 60 and 109/109 cumulative relevant mutations. Doctor,
the release build, and the full off-game self-test pass, including GPU
timestamp retirement. Run 60 is installed from
`cache/runs/run60-shadow-cold-root-mesh.ini`; installed and source DLLs match
at SHA-256
`3f8856f82878d7d143b388295f36f4b210ffcb7a4e2534b87f62378eaaa01694`,
and installed and source INIs match at SHA-256
`918a2ef1eec7dd71f46f79dd9caf2011416e8c44a3428bfdef7089d16809edcd`.
The run-59 live CSV and debug log were byte-identical to their archives before
the stale live names were removed. The game was not launched.


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
