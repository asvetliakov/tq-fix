# Mitigating Titan Quest's own stutters

## Context

`17adf8a` closed out the mod-caused stutters and added two instruments: the
frame overlay (*is* this frame slow) and the performance probe (*why* was that
frame slow). It also landed `research/streaming/` — a Ghidra audit of
`Engine.dll` naming four places where the game's own resource loading reaches
the render thread, and one place where the mod's streaming optimization rests
on a false assumption. The audit ends with three "cross-references worth acting
on" and no code change followed from it.

---

# Status, and where the plan was wrong

Current implementation ownership is recorded in findings §111. Performance
behavior has moved from `engine_probe.cpp` into the shadow, terrain, secondary
admission and archive modules, coordinated by `engine_hooks.cpp`. Trace-off
shared hooks bypass the observer; this is an architectural/runtime-overhead
correction, not a newly measured game result. Historical file paths below
describe the implementation at the time of each stage.

Stages 0 and 1 landed as written. Two stages that are **not** in this plan
landed after them, because measurement redirected the work; and two of the five
"established beyond doubt" findings below turned out to be false for the
machine this is being built for. Recorded here rather than edited away, because
the reasoning that produced them is still worth reading.

**The reporter's install has a 92.4 GiB loose texture pack** (12,519 `.tex`,
all `TEX\x01`) that drops into `Settings/`, and it was parked when the audit
below was written. That single fact invalidates findings (1) and (2):

- **(1) is false with the pack installed.** `run4` recorded
  `upload_jobs_started = 369` and `upload_jobs_done = 369`: the progressive
  uploader has been running all along, moving 3.6 GiB in a 102-second session.
  It is true only of the archive-only configuration.
- **(2) is false.** Those counters go through `probe::count`, which drops
  non-render-thread writes, and they recorded fine — so these
  `CreateTexture2D` calls arrive on the **render thread**. Stage 0's engine
  channel is still worth having, and it found 2.16 s of off-thread texture
  creation nothing had ever seen, but it was not the prerequisite claimed.
- (5) stands, and is the load-bearing one.

**What the measurements actually found, and what was built instead:**

| | |
| --- | --- |
| The uploader's worst frames were **92–98% `UnmapViewOfFile`** — 1,032 ms inside Present over one session, up to 37.8 ms in a frame, while those frames uploaded 256–1024 KiB. `upload_release_us` totalled 0.7 ms, killing the other candidate. | The deferred unmap moved to a mod-owned worker. `upload_unmap_inline_us` is now 0 across three runs and `stream_step_ms` max fell 39.0 → 13 ms. |
| 984 loose textures are over 4096 on a side, up to **16384×16384 and 341 MiB** — 7.9% of the pack's files but **46% of its bytes**, against 6.4% for the same assets in the archives, all 984 of which exist. | `[performance] loose_texture_max` refuses them so the archive copy is used. Verified: 120–123 redirects per run in Eternal Embers, `arc_open` up ~1,460, and visually clean. |

**Consequences for the stages below.**

- **Stage 2 was sized against the wrong distribution.** §2.2's copy and §2.4's
  32 MiB budget assume a 21 MiB maximum; against a 341 MiB texture every large
  job would fail admission and fall back to the synchronous path, i.e. do
  nothing for the textures that hurt most. The loose cap fixes this by
  construction — the largest loose texture is now 21.33 MiB — so §2.2/§2.4 are
  viable again *as long as the cap is on*. §2.3 is already done by other means.
- **The uploader is no longer the problem.** `stream_step_ms` p99 is 5.1 ms and
  its max 13 ms. The remaining Stage 2 items — the `PSSetShaderResources` fast
  path, multi-job advance, the chunk-rate model — are polish.
- **The big hitches are the game's, and this is now measured rather than
  inferred.** Run 9 repeated the route with the texture pack removed entirely,
  everything served from `.arc`, every other setting identical:

  | | pack on (run 8) | archives only (run 9) |
  | --- | ---: | ---: |
  | p50 / p99 frame | 9.02 / 45.9 ms | **9.02 / 45.6 ms** |
  | worst frame | 1,503 ms (`arc_open` 1299) | **1,358 ms (`arc_open` 1300)** |
  | frames > 200 ms | 8, 3.87 s | **12, 4.17 s** |
  | hitch time in no mod column | 6.84 s | **8.68 s** |
  | `stream_step_ms` | 6,252 ms | 7 ms |
  | `engine_tex_create_off_us` | 1,513 ms | 205 ms |

  Removing 92.4 GiB of loose textures did not improve the hitches at all — it
  is arguably slightly worse, though 8 frames against 12 is within route
  variance. The worst frame is plainly the same event in both runs: ~1,300
  archive opens in a single frame. Around 30% of every archive open in a
  session lands inside a frame over 200 ms, in both configurations.

  The run also confirms finding (1) precisely for the configuration it was
  written about: with no loose files, `upload_jobs_started` and
  `upload_src_loose` are **0**. The progressive uploader really never runs on
  a stock install.

  So the remaining stutter is the game's own level loading and archive reads,
  independent of the pack. **Stages 3, 4 and 5 are the whole remaining
  answer**, and Stage 3 is the right next step because 4 and 5 are gated on
  what it measures. Note also that a second class of hitch — run 9's frames
  1327 and 5726, 256 ms and 228 ms — carries *no* archive opens and nothing
  named at all, so the archive path is not the only mechanism in play.

**Stage 3 landed, and run 10 measured it.** `findings.md` §8 has the numbers;
this is what they change. The instrumentation itself is free — p50, p99 and
the mod's share are unchanged against run 8 — so everything below is a
statement about the game and not about the probe.

*Four things are now closed rather than deferred, and the plan is wrong to
keep proposing them:*

| | |
| --- | --- |
| **6.1, the seven sweeps** | 51,443 calls costing **11.2 ms a session**. Delete it. |
| **6.2, the loader fence** | 7,349 waits costing **1.6 ms**, 0.22 µs each. The event is already signalled, exactly as 6.2 argued. Only `FUN_1011f490`'s thread walk remains unmeasured. |
| **6.3, `WaitForLoadingToFinish`** | called **0** times. The hazard is closed, not merely unlikely. |
| **the region lock (§1b), which 3.2 measured and 5 assumed** | **0 contended acquisitions** in a whole session across all three sites. The render path never blocks on it. |

*Two things are confirmed, with a caveat that resizes them:*

- **The renderer does force synchronous level loads onto the main thread.**
  `Region::LoadLevel` ran 203,419 times, 100% on the engine's own thread, and
  the worst frame spent **511 ms** in five of those calls. Stage 5's premise
  is now a measurement. **But exactly one frame in 7,347 has a `LoadLevel`
  costing over a millisecond** — the rest take the resident fast path and are
  free. Stage 5 fixes the single worst frame of a session and essentially
  nothing else. Size the work accordingly.
- **The archive path is worth 4.38 s a session** across 7,527 block inflates
  at 582 µs each, and it inflates 1.88 GiB to serve 1.03 GiB — the 1.8×
  amplification §4.1's multi-block cache exists to remove. That one is well
  aimed.

*And 2.4 finally has its number:* `upload_leased_mib` peaked at **1,064 MiB**.
The 128-lease count is not a safe limit; the byte budget is overdue.

**What Stage 3 did not explain, which is most of it.** At the >50 ms threshold
that reproduces runs 8 and 9 (6.72 s and 8.60 s), run 10 has 7.93 s of hitch
time in no mod column and the engine columns name **1.6–2.8 s of it — 21% to
35%**. Two thirds is still dark, including ~950 ms of the 1,466 ms worst frame
and the entirety of the second class, whose frames show *every* engine column
at zero.

**Run 11 answered that, and the answer is "both".** `findings.md` §9 has it;
the frame's time is now fully accounted for, to within 0.2%.

- **Stages 4 and 5 keep their target.** `Engine::Render` is 57.9% of the
  session and **46.9% of the hitch time**, and the worst frame — 1,453.8 ms —
  is 1,448.6 ms of `Engine::Render`, with `Region::LoadLevel` inside it. §1a's
  forced synchronous load through `AddElementsInBox` is confirmed as a
  measurement. Even the ~950 ms §8 could not place is inside the render pass.
- **But 38.4% of the hitch time, and 18 of the 32 frames over 100 ms, is
  outside `Engine.dll` altogether** — outside `Engine::Update`, outside
  `Engine::Render`, outside the mod, outside `Present`. Those frames draw 400
  to 1,600 times with the mod completely idle, and spend 100–225 ms where a
  normal frame spends 0.21 ms.

That second finding is not in this plan anywhere, and no stage in it would
improve those frames. It is now instrumented: `GameEngine::Update`
(`Game+0x19a230`) is bracketed the same way, and it is the first hook in this
work outside `Engine.dll`. Run 12 decides whether the time is `Game.dll`'s
simulation or something below the process — and those point at completely
different work.

**Runs 12–16 finished the job.** The frame is now fully accounted for and the
dark time has a name. `findings.md` §10–§14:

| | session | frames over 50 ms |
| --- | ---: | ---: |
| `Engine::Render` | 57.2% | 51.0% |
| `Engine::PresentSurface` | 21.6% | 5.2% |
| `Engine::Update` | 10.2% | 8.8% |
| **`PeekMessageA`** | **8.2%** | **20.3%** |
| unexplained | 2.3% | 12.2% |

**The second class of hitch is `PeekMessageA` on an empty queue** — 730 µs on
average and up to 212 ms, against 0.67 dispatched messages a frame, with the
message pump owned by `Engine.dll` rather than by the executable. Under
CrossOver that is a wineserver round trip. It is not the game's, and it is
not reachable from this mod; the lever is CrossOver's own synchronisation
settings. Three checks rule out the alternatives: the instrument does not
create it, it does not correlate with archive I/O, and it is not a message
flood.

Also closed, so that nothing below proposes them again: `GameEngine::Update`
is 0.3% of the session, the online platform poll 44 ms, sound 2 ms, the
jukebox and quest triggers nothing, graphics options 28 ms, and free address
space never falls below 3,445 MiB.

**So the remaining work is one item.** `Engine::Render` is the half that is
ours, and §8 priced what is inside it. **Stage 4.1 is the last thing in this
plan with both a number and a fix**: 4.38 s a session across 7,527 block
inflates at 582 µs each, inflating 1.88 GiB to serve 1.03 GiB — the 1.8×
amplification a single-slot cache in front of a 2 GB entry produces by
construction.

Stage 4.2 and 4.3 stay gated on what 4.1 leaves. Stage 5 stays parked: its
premise is confirmed — `Region::LoadLevel` is 100% main-thread and cost 505.7
ms in the worst frame — but exactly **one frame in 7,347** has a load costing
over a millisecond, so it fixes the worst frame of a session and nothing
else. Stage 6 is deleted.

**Stage 4.1 is built, and the plan's design survived contact with the
binary.** `src/arc_cache.{h,cpp}` and one detour that was already there.
`findings.md` §18 is the record; three things are worth carrying forward here.

- *The plan's key was right, and its structure note was one word off.*
  §4.1 says `desc = *(void**)(entry+0x20) + blockIndex*12`, which is exactly
  what `1011d101`/`1011d108` do — but R1's on-disk layout has `+0x20` as
  `firstBlockIndex`, an integer. The engine rewrites that field to a pointer
  when it opens the archive. Reading the container format as the runtime
  structure would have built the key out of an integer treated as an address.
- *The uncompressed branch cannot reach the cache at all.*
  `Archive::ReadFromFile` tests the compressed bit at `1011d390` and branches
  away before the block routine, so `arc-format.md`'s 6,880 uncompressed
  dialog entries are not a hazard here. Worth knowing before 4.2, which
  over-reads and therefore does have to care.
- *`8verify` is cheaper than the plan imagined.* Rather than inflating into a
  shadow buffer and comparing, it never serves and compares **on insert**: a
  request whose key is already resident is compared against what the engine
  just produced for it. Same proof, no second buffer, no thread-local
  anything, and every request that would have been a hit becomes a test.

It defaults to `0`, which allocates nothing and leaves the block routine
byte-identical to today, and it is the one hook in `src/engine_probe.cpp` that
installs with the performance probe off — because it is a fix rather than an
instrument. It opens no other gate: the module, export and byte checks are
unchanged, and four windows past the prologue are additionally required before
anything is cached.

**Runs 21 and 22 answered Stage 4.1, and the answer is that §4.1's opening
premise was wrong.** `findings.md` §19 and §20.

*Correct beyond doubt:* 914 blocks compared byte for byte across the two
boots, 0 mismatches, 0 `arc_cache_skip` over 15,186 requests.

*But the premise is measured false.* §4.1 opens by calling the 1.8x
amplification what "a single-slot cache in front of one 2 GB entry" produces —
i.e. re-inflation. With a 256 MiB cache holding 1,024 blocks resident, **91.8%
of block requests are still for a block nothing has seen before.** The excess
is partial consumption: a 218 KiB read at an arbitrary offset straddles 1.52
blocks and discards the unused head and tail, and nothing comes back for them.
No block cache of any size recovers that. §8 of `findings.md` carries the
correction.

*What reuse exists is burst-local and small.* 3.8% of blocks at 32 slots and
8.2% at 1,024 — 32x the address space for 4.4 points — but concentrated where
it hurts: 19.4% on the archive-heaviest frame against 8.2% overall. Computed
value is ~136 ms off the worst frame of a session at 8 MiB, which stays over
1.3 s regardless, because `Region::LoadLevel` and the ~950 ms nothing names
are what dominate it.

At this stage `archive_cache_mb` shipped at `0`; findings §109 later promoted
the measured 8 MiB ceiling to the default. Whether 8 MiB was worth switching
on was a judgement call, not a measurement, and run 23
(`cache/runs/run23-archive-cache-serving.ini`, `archive_cache_mb=8`) is the
first boot that actually serves a block and so the first that measures rather
than computes the win.

**And run 23's frame anatomy reprices 4.2 and 4.3 downward.** The freeze frame
is 1,310 ms: the *entire* archive path is 260 ms of it (20%),
`Region::LoadLevel` is 509 ms (39%, Stage 5), and **795 ms (61%) is named by
nothing in this plan**. 4.2 and 4.3 are competing for slices of the 260, and
they overlap — both attack the same 580 µs a block, one by removing syscalls
and one by removing zlib time, so whichever half is small makes the
corresponding item worthless.

The 795 ms now has a verified candidate: `FUN_1014d020`, the archive `File`
constructor, allocates *two* buffers of up to 256 KiB through `operator new[]`
per compressed entry opened and frees them on close — and frame 4311 opened
1,299 files, up to 649 MiB of allocate/free in one frame in a 32-bit MSVC heap
under Wine. **This plan already names that and declines it for want of
evidence** (see the "not doing yet" note under Stage 4); frame 4311 is that
evidence's shape, and the instrument for it is four bytes of import table.

So the order is: run 24's two instruments, then whatever the heap says, then
Stage 5.1 plus a widened preload distance (509 ms), then 4.2 only if the split
says syscalls dominate, then 4.3 last or never. `findings.md` §21 and §22.

**4.2 and 4.3 are now gated on one number that does not exist yet.**
`engine_arc_inflate_us` brackets the whole block routine — the
`SetFilePointerEx`, the `ReadFile` and the `uncompress` — so its 4,310 ms is
read and inflate together. Mostly zlib points at 4.3; mostly syscall points at
4.2, which given §14–§17 (one host round trip costing 126–212 ms) would be in
character. **P8** — the `E8` at `0x1011d1d6`, already at offset 20 of
`kArchiveInflateWindowBytes` and already byte-checked by `verify-sites.py` — is
the instrument: one `detour::patchCall`, one column pair, no new reverse
engineering. Write that before either 4.2 or 4.3.

---

This plan acts on them. Five things are now established beyond doubt, all
verified this session against the pinned binaries and the installed archives:

1. **The progressive texture uploader has never run.** It requires the
   loose-file `FileDirectory` vtable (`Engine+0x2f71ec`). This install serves
   *everything* from `.arc`: 67,873 entries across every archive, and **not one
   is stored uncompressed**. Its `PSSetShaderResources` substitution scan —
   2400–5000 calls a frame, each an O(slots × 256) walk inside a critical
   section, with the lock held across the D3D call — is overhead paid for
   nothing.
2. **We cannot currently see that.** `probe::countInternal` and
   `addPhaseInternal` both drop every write that is not from the render thread
   (`src/probe.cpp:449`, `:458`). Texture loads run on the game's loader
   thread, so `CounterUploadRejected`, `CounterUploadJobsStarted` and
   `PhaseTextureCreate` are **already being discarded** for exactly the loads
   under investigation. Instrumentation is a prerequisite, not a nicety.
3. **The archive read path is the game's entire I/O path, and it is
   pathological.** Block size 256 KiB. `Archive::ReadFromFile` issues one
   `SetFilePointerEx` + `ReadFile` pair per block under a per-archive critical
   section, then one zlib `uncompress()` per block — a full
   `inflateInit_`/`inflate`/`inflateEnd` cycle, window allocation included, per
   256 KiB. 949 `.tex` entries are ≥ 2 MiB; the largest are 21.33 MiB, i.e.
   **86 syscall pairs and 86 inflate cycles for one texture**, synchronously
   inside `File::Lock`, on whichever thread touched it.
4. **The renderer already degrades gracefully when a region is loading.** Both
   `AddElementsInBox` overloads call `Region::LoadLevel(region, false)` and then
   immediately test `region[0x74]`, branching to the epilogue when it is set. So
   redirecting that call to the engine's own `Region::BackgroundLoadLevel` gives
   "region absent for a few frames", not a broken frame.
5. **The uploader must copy, not borrow.** `GraphicsTexture::Initialize`
   (`0x10194120`) has two branches. The `"DDS "` branch calls the renderer once.
   The `"TEX"` branch is a `while` loop that calls the renderer repeatedly,
   walking forward through the *same* buffer by a per-sub-blob length prefix.
   I extracted and inflated real entries: **every terrain texture is
   `TEX\x01…`, not `DDS `** — so the loop is the only branch that matters here.
   Any design that takes ownership of the source buffer at the first
   `CreateTexture2D` is a use-after-free while the loader thread iterates. (This
   is also a live latent bug in the shipped loose-file path, which defers
   `UnmapViewOfFile` to job completion on the render thread; it has never fired
   only because that path never engages on a stock install.)

Intended outcome: a zone transition stops being a visible freeze. The forced
main-thread level load becomes asynchronous, the archive read behind it gets an
order of magnitude cheaper, and the texture uploader finally does the job it was
written for. Every game-behaviour change is individually switchable from
`tqflicker.ini`, defaults to **off**, and is preceded by an instrument that
proves the hazard is actually being hit on this machine.

---

# Reference material established this session

Stage 0 commits the parts that belong in the repo; the audit records none of it.

## R1. The ARC container, fully decoded

```
header (0x800 bytes; file data begins at 0x800)
  +0x00  char magic[4]  "ARC\0"     +0x10  u32 partTableSize == partCount*12
  +0x04  u32  version   1           +0x14  u32 stringTableSize
  +0x08  u32  fileCount             +0x18  u32 tocOffset
  +0x0c  u32  partCount

toc, at tocOffset:
  [partCount × 12]  block records { u32 offset; u32 compressedSize; u32 uncompressedSize; }
  [stringTableSize] name blob, NUL-separated, lowercase, '/' separated
  [fileCount × 44]  file records
     +0x00 flags (bit 1 = compressed)   +0x1c blockCount
     +0x04 offset of first block        +0x20 firstBlockIndex
     +0x08 compressedSize               +0x24 nameLength
     +0x0c decompressedSize             +0x28 nameOffset
     +0x10..0x18 timestamps
```

Consistent with the runtime structures the audit recovered (entry stride
`0x44`, block descriptor stride `0xc`, `entry[0] & 2` = compressed,
`archive[0x40]` = block size = 256 KiB). Blocks are zlib-wrapped deflate
(`78 da`).

Three facts checked exhaustively over all 67,873 entries in every archive:

- **Every entry is compressed.** The uncompressed branch of
  `Archive::ReadFromFile` is dead code on this install.
- **The data region of every archive is globally contiguous** —
  `record[k].offset + record[k].compressedSize == record[k+1].offset` for all k,
  with zero exceptions — and an entry's blocks are consecutive record indices.
  So one `ReadFile` covers any contiguous *run* of blocks. Block size is
  uniformly 256 KiB, and that is set in code, not inferred:
  `1011ea94  c7 46 40 00 00 04 00  MOV dword [ESI+0x40], 0x40000`, the sole
  writer of `archive[0x40]`.
- **`Resources/Levels.arc` contains exactly one entry**: `world/world01.map`,
  2,004,303,764 bytes uncompressed, 656 MiB compressed, **7,646 blocks**. There
  is no loose `.map` anywhere in the install. Every level of every act comes out
  of that single entry.

That last fact is the one that reframes the archive work, because of what sits
opposite it: **the block cache is a single slot.** `FUN_1011d240`'s cache test is
`CMP dword ptr [EBX],EAX` against one `cachedBlockIndex`. So any read into
`world01.map` that crosses a 256 KiB boundary, or that revisits a block the
one-slot cache has since evicted, costs a complete 256 KiB inflate plus a
seek/read syscall pair under the per-archive lock. A `Level::Load` that
alternates between two areas of that file re-inflates the same block
repeatedly. This is a mechanism for "camera crosses into new territory →
hitch" that is independent of which thread pays for it.

## R2. The archive `File` class

vtable `Engine+0x2f719c`, object 0x28 bytes, constructed at `Engine+0x14d020`
by `FileSourceArchive::OpenFile` (`0x1014ed30`):

| slot | address | member |
| --- | --- | --- |
| 0 | `0x10028310` | scalar deleting destructor — frees `+0x18`, `+0x24`, `+0x20` |
| 2 | `0x1014cf30` | `Lock(offset, size)` |
| 3 | `0x1014cfc0` | `LockAll()` → `Lock(0, GetLength())` |
| 4 | `0x1014cf20` | `Unlock()` — `c6 41 10 00 c3`. **Frees nothing.** |
| 6 | `0x1014cf00` | `GetLength()` → `entry[0x0c]` |

```
+0x00 vtable             +0x10 locked flag (byte)
+0x04 FileSourceArchive* (→ +0xc = Archive*)
+0x08 entry record*      +0x14 scratch capacity   (uint)
+0x0c entry index        +0x18 scratch pointer    (operator new[])
+0x1c BlockBuffer.cachedBlockIndex (init 0xffffffff)
+0x20 / +0x24  two 256 KiB block scratch buffers
```

`Lock` grows `+0x18` to `size`, sets locked, calls `Archive::ReadFromFile`, and
returns the buffer. So `pSysMem` on the archive path is **a fully materialized
heap buffer** — no mapped view, and none of the page-fault-on-Present hazard the
audit flags for the loose path.

Ownership is *technically* transferable — the destructor's `operator delete[]`
is `MSVCR110.dll!??_V@YAXPAX@Z`, reached through Engine's IAT slot `0x102ac304`
(`objdump -p` confirms, and `Archive::FreeFileBuffer` at `0x1011dce0` is
literally `PUSH EAX; CALL [0x102ac304]`) — but finding (5) means we must not.
Recorded as the escape hatch only.

## R3. Verified patch sites

All 5-byte `E8 rel32` unless noted; all read byte-for-byte out of
`research/streaming/generated/disassembly.asm`.

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

Both `AddElementsInBox` sites share the same shape, and the two instructions
after the call are what makes Stage 3 safe:
```
  6a 00 / 8b cf              PUSH 0 ; MOV ECX, Region*
  e8 <rel32>                 CALL Region::LoadLevel
  80 7f 74 00                CMP byte [EDI+0x74], 0
  c7 47 6c 00 00 00 00       MOV dword [EDI+0x6c], 0
  0f 85 <rel32>              JNZ epilogue     ; region skipped while loading
```

`FUN_10065760` **is zlib `uncompress` built `__fastcall`**: `ECX` = dest,
`EDX` = `uLongf* destLen`, `source` and `sourceLen` on the stack, caller cleans.
Body is `inflateInit_`(`0x10067680`) → `inflate`(`0x10067810`) →
`inflateEnd`(`0x10068eb0`) with `uncompress`'s exact `Z_BUF_ERROR`/
`Z_NEED_DICT` mapping. Its prologue `55 8b ec 83 e4 f8` is **byte-identical to
`src/grass.cpp`'s `kRenderPrologue`**, so the existing `attach()` machinery
applies — but that also means a 6-byte prologue match proves nothing about
identity, so hooks must verify 16–24 bytes and steal 6–7.

Almost every hook target is **exported by decorated name**
(`?LoadLevel@Region@GAME@@QAE_N_N@Z`, `?ReadFromFile@Archive@GAME@@QBE…`,
`?LoadResource@ResourceLoader@GAME@@QAEXPAVResource@2@@Z`, …), so resolve by
name and use the RVA as an identity assertion rather than as the lookup. The
recorded main-thread id is at `Engine+0x41a5dc` (`CMP EAX,[0x1041a5dc]` at
`0x1014476b`), free to read for main-thread attribution.

---

# Stage 0 — Make the instrument able to see the game

Nothing here changes rendering. It lands first because finding (2) means every
later measurement is blind without it.

**0.1 Fix the CSV writer's silent truncation.** `src/probe.cpp:210`
`char line[2048]` (header) and `:305` `char line[3072]` (row). The header
already uses ~1400 bytes; adding columns overruns it, and the overrun is
silent — `n += snprintf(...)` returns the would-be length and the header is
written truncated, without its terminator. Raise to 4096 / 8192 **before** any
column is added.

**0.2 A thread-safe engine channel.** Append to `enum Counter` (never
reordering — the header forbids it) a block after `CounterPsSetSrv`, guarded by
`CounterEngineFirst`, written only through a new API:

```cpp
// Written from the game's loader thread and its main thread, which the frame
// record cannot accept: countInternal drops every write that is not the render
// thread's, and would tear the record if it did not. These accumulate in an
// interlocked side array and fold into the frame at endFrame, so the column,
// its median and the `unusual` attribution all work unchanged.
void engineCountInternal(Counter, uint32_t);
inline void engineCount(Counter c, uint32_t n = 1) {
    if (detail::active) engineCountInternal(c, n);
}
uint32_t microsecondsSince(int64_t startTicks);   // saturating; 0 while disabled
```

`InterlockedExchangeAdd` into `g_engineCounters[]`, drained with
`InterlockedExchange(...,0)` at the top of `endFrame` before the exclusive-phase
arithmetic, so the values belong to the frame that just closed.

**Durations are named `_us`, not `_ms`, and this is load-bearing.**
`tools/frames.py` builds its "the mod's share" total from every column ending
in `_ms`; an engine `_ms` column would be silently misattributed to the mod.
`_us` columns fall into its `counters` bucket, parse as integers, and are not
printed by its `interesting` filter — so **`frames.py` needs no change to keep
working**, and a follow-up adds an `engine` section that reconciles
`Σ engine_*_us / 1000` against the "the game's frame" residual, which is the
line that exists to be explained.

**0.3 Stop discarding the loads we care about.** In `hookCreateTexture2D`
(`visual.cpp:1050`), when `GetCurrentThreadId() != g_renderThread`, route the
timing and count through `engineCount` as `engine_tex_create_off` /
`engine_tex_create_off_us` instead of through `probe::Scope`/`count`, which
throw them away today. This needs **no Engine.dll hook at all** and is probably
the single most useful number in the whole investigation: how much time the
loader thread spends inside D3D texture creation.

**0.4 Split the rejection counters.** `CounterUploadRejected` currently
conflates three unrelated outcomes (`visual.cpp:696`, `:701`, `:736`). Add
`upload_reject_pool`, `upload_reject_budget`, `upload_reject_alloc`,
`upload_reject_scan`, and source-class counters `upload_src_arc` /
`upload_src_loose` / `upload_src_none`. Keep `upload_rejected` as their total so
runs before and after still compare on that column. All of them go through
`engineCount`, per 0.3.

**0.5 Commit the reference material.** `research/streaming/arc-format.md` (R1,
with the exhaustive-check results) and `research/streaming/tools/arcinfo.py`
so the claims are reproducible; R2/R3 and findings (2) and (5) appended to
`findings.md` as a new section. Fix its three stale references
(`g_diagUploadsCreated` is gone, `visual.cpp:3144` is out of range, the
`GetTickCount` claim is now QPC) and correct its closing cross-reference, which
recommends binding to the archive class — finding (5) shows why that shape is
wrong.

**Config.** No new key yet; everything is behind `performance_trace`.

**Self-test.** Header integrity — after a `full`-mode frame, read the CSV and
assert the header's comma count equals a row's and that it ends with `unusual`;
that is the permanent regression test for 0.1 and for every future column.
Assert `engineCount` from a spawned thread lands in the next frame's record,
that plain `count()` from that thread still records nothing (the existing
invariant must not regress), and that both are inert while disabled.

---

# Stage 1 — Make the uploader testable

`scripts/selftest-offgame.sh` links every source **except `src/visual.cpp`**, so
none of the streaming logic is testable off-game today. That is the main
obstacle to landing Stage 2 safely, and it is worth one behaviour-preserving
move first.

Extract into `src/upload.{h,cpp}`, `namespace tq::upload`, unchanged in
behaviour: the budget constants and `chunkBytesForTargetMs` (`visual.cpp:144-165`),
`UploadJob` / `g_uploadJobs` / `g_uploadLock`, `reserveUploadJob` (`:644`),
`lowMipFor` (`:651`), the SRV bookkeeping from `hookCreateShaderResourceView`
(`:778-805`), the substitution lookup from `hookPSSetShaderResources`
(`:818-830`), and `advanceTextureUploadsInternal` (`:833-947`).

The module takes injected dependencies so it knows nothing of Engine.dll or of
hooks — `createTexture2D`, `createShaderResourceView`, `updateSubresource`, and
**an injectable clock**, which is what makes the chunk controller testable at
all. `visual.cpp` keeps `progressiveTextureCandidate` (`:661`, the renderer
identity guard), `findTextureOwner`, and three now-thin hook bodies. Add
`src/upload.cpp` to both `scripts/build.sh` and `scripts/selftest-offgame.sh`.

**Risk.** Medium — it moves live code. Mitigate by keeping it behaviour-
preserving to the byte, so the diff reviews as a move.
**Verification.** A run before and after must produce statistically identical
`tools/frames.py` output, and nothing else.
**Self-test.** Lock in today's behaviour before Stage 2 changes it: with a fake
`Calls` and injected clock, drive a synthetic 2048² BC1 through `create()`,
assert one job starts, that `advance()` issues one `UpdateSubresource` with a
block-aligned `D3D11_BOX`, and that a 40 ms fake chunk halves the next while a
0.1 ms chunk grows it to the ceiling.

---

# Stage 2 — Make the uploader work, for every install

**2.1 Recognise both `File` classes; use the result as a diagnostic, not a
gate.** `findTextureOwner` (`visual.cpp:549`) keeps its stack scan for
`Engine+0x213cad` / `slot[7]` — that mechanism is frame-derived and
class-agnostic. Replace the loose-only checks at `:569-570` with two accepted
shapes:

- **loose**: `vtable == Engine+0x2f71ec`, `[2] == +0x14e560`, `[4] == +0x14e540`;
- **archive**: `vtable == Engine+0x2f719c`, `[2] == +0x14cf30`,
  `[4] == +0x14cf20`, `[6] == +0x14cf00`, and `dds` inside
  `[*(BYTE**)(source+0x18), + *(uint*)(source+0x14))`.

Note the archive `File` is **0x28 bytes**, so the existing
`readable(source + 0x34)` guard at `:567` would read past its end — it must
become class-specific. Record the outcome into `upload_src_arc` /
`upload_src_loose` / `upload_src_none` and skip the call entirely unless
`probe::enabled()`.

**2.2 Copy the retained mips; never take ownership.** Because of finding (5),
this is not a preference. In `create()`, before calling through to
`g_createTexture2D`:

1. `lowMip = lowMipFor(desc)`; `retain = Σ rows(mip) × SysMemPitch(mip)` for
   `mip ∈ [0, lowMip)`. Reject on a zero pitch or on overflow.
2. Admission under `g_uploadLock`: `upload_reject_pool` if the pool is full,
   `upload_reject_budget` if `g_retainedBytes + retain > kMaxRetainedBytes`.
3. `VirtualAlloc` `retain` bytes — page-granular and outside the CRT heap,
   which matters in a 32-bit address space already fragmented by 384 MB of
   shadow maps. On failure, `upload_reject_alloc`.
4. `memcpy` each retained mip in and repoint `job.source[mip].pSysMem` into the
   copy. Mips `≥ lowMip` still go straight to the driver from the engine's
   buffer, synchronously, as today.

The copy is paid on the *loading* thread and replaces a strictly larger
synchronous driver upload measured at 3.2 ms/MiB, so it should cost a small
fraction of what it buys — and `engine_tex_create_off_us` from Stage 0 prices
it directly. The mod never writes to an engine object and never frees engine
memory, so every failure path is a plain `E_FAIL` with `*handled = false` and
nothing to unwind. That is the strongest fail-open property this path has ever
had.

**2.3 Delete the `MappingLease` machinery.** The copy subsumes the loose-file
path and is *better* on it: no `UnmapViewOfFile` deferred across frames, no
address-space retention in a 32-bit process, no page faults taken inside
`PhaseStreamStep` on the render thread, no Engine.dll vtable patch, and no
use-after-unmap for `TEX` containers. Remove `MappingLease`, `findLease`,
`createLease`, `hookArchiveUnmap`, `ensureArchiveUnmapHook`, `g_archiveUnmap`,
`g_archiveVtablePatched`, `kMaxMappingLeases` (`visual.cpp:171-183`, `:583-642`)
and the deferred-unmap tails at `:836`, `:933-939`, `:945`. Behaviour becomes
identical for every install, loose or archived — which is what "ships to
others" actually wants, far more than a saved memcpy on one configuration. The
README paragraph that promises the opposite must be rewritten in the same
change. Delete the now-false comment at `:884-886` about faulting pages off
disk; its disappearance is the point of the change.

**2.4 Bound the pool.** Today `kMaxUploadJobs = 256` with no byte budget; at up
to ~10.5 MiB retained per job that is 2.6 GB in a 32-bit process. Introduce
`kMaxUploadJobs = 16` and `kMaxRetainedBytes = 32 MiB`, admission-checked, with
over-budget failing open to the engine's synchronous path — which is exactly
the pre-mod behaviour and the right thing to do under pressure. `shutdown()`
must free every retained block or a mid-session `streaming=original` toggle
leaks.

**2.5 `AddRef` the full SRV.** `job.fullView` is stored without a reference
(`visual.cpp:797`) and compared as a raw pointer. If the engine releases it and
D3D reuses the allocation, the mod substitutes a low-mip view for an unrelated
texture. Harmless while there are zero jobs; a real visual-corruption path once
jobs run during level loads, when SRV churn is highest.

**2.6 The `PSSetShaderResources` fast path.** Add
`volatile LONG g_substitutionCount`; when it is zero — the overwhelming
majority of frames — the hook forwards immediately: one volatile load and a
branch, no lock, no array copy, no scan. With jobs in flight, replace the
O(slots × 256) walk with a compact table of at most `kMaxUploadJobs` entries and
copy into the output array lazily on the first hit. **The lock is no longer
held across the D3D call**, which today nests D3D's internal locks under ours on
a 5000-call-per-frame path.

The lock-free read is safe by thread ownership, which is worth stating because
it is what makes it safe: inserts happen in `hookCreateShaderResourceView` on
the loading thread; removals and the `Release` happen in `advance()` on the
render thread; reads happen in `hookPSSetShaderResources` on the render thread.
Removal can therefore never race a read. An insert publishes `low` before
`full`, so a half-formed entry is unobservable, and releasing our reference on a
still-bound view is safe because `PSSetShaderResources` takes its own.

**2.7 Advance more than one job per Present.** With jobs finally real, a zone
transition can queue dozens and each would take N× longer to resolve, leaving
terrain visibly soft. Loop `advance()` under a per-frame budget of
`kUploadTargetMs` with at most `kMaxChunksPerFrame = 8` iterations, selecting
FIFO by a monotonic `job.sequence` to bound worst-case per-texture latency. The
3 ms/frame ceiling — the property that made this safe — is unchanged.

**2.8 Fix the chunk-rate model and make it visible.** `g_uploadMsPerKib` is a
one-term fit to a path with a large per-call fixed cost: a small chunk measures
a high ms/KiB, which shrinks the next chunk, which measures higher, and the
asymmetric weights (0.5 up, 0.1 down) bias that ratchet upward. Update the EWMA
only from chunks ≥ 128 KiB, clamp it to `[0.0002, 0.05]` ms/KiB, reset it to the
seed in the existing resize/device-recreate callback (where the driver's
resource path genuinely changes), and record it every frame as
`upload_ms_per_mib_x100`. It is currently entirely invisible, which is why the
34 ms outlier took a whole run to find. Keeping it process-global is right —
the rate is a property of DXMT and the format, not of a texture.

**Self-test.** All off-game against the fake `Calls`, and this is where Stage 1
pays for itself:
- **Copy independence**: create a job, then overwrite the source buffer with a
  poison pattern, advance to completion, and assert every byte handed to the
  fake `updateSubresource` matches the original. That is the direct test that
  the copy severed the engine's lifetime.
- Admission: 17 jobs on a 16-slot pool → 16 started and one
  `upload_reject_pool`; an oversized job → `upload_reject_budget`; both still
  increment `upload_rejected`.
- `g_retainedBytes` returns to zero after all jobs complete, and after
  `shutdown()` with jobs still in flight.
- Substitution: inactive with no jobs; with a job, the matching slot is
  replaced and every other slot is byte-identical.
- Advance loop: four queued jobs and a clock reporting 0.5 ms per chunk issues
  exactly six chunks in one `advance()`, lowest-sequence first.

**Run 1 — the first run that can see anything.** Reporter's own `tqflicker.ini`
(5120×1440, `hdr=auto`, `tonemap=agx`, `bloom=enhanced`, `grass=enhanced`) plus
`performance_trace=full`. Load a save and walk a route crossing at least two
zone boundaries into territory not yet visited this session, then quit normally.
Read out: `upload_src_arc > 0` and `upload_src_loose == 0` (the confirmation
that finding (1) is true on this machine); `upload_jobs_started > 0` and
`upload_jobs_done ≈ upload_jobs_started` (the feature engaging for the first
time); the rejection breakdown; `stream_step_ms` p99 ≤ ~3.5 ms;
`engine_tex_create_off_us`, which prices the copy; and whether frame time on
load-free frames moved at all, which is the test of 2.6. Subjectively: does
terrain visibly pop from soft to sharp on entering a region, and is that better
than the hitch it replaces?

---

# Stage 3 — Instrument the game itself

**3.1 Shared hooking helpers.** Lift `moduleText`, `absoluteBranch`,
`writeBytes`, `Detour`, `attach`, `detach` out of `src/grass.cpp`'s anonymous
namespace (`:200-285`) into `src/detour.{h,cpp}`; `grass.cpp` then uses them
unchanged. Add two things it lacks:

- a separate `verify` length from `stolen` — verify 16–24 bytes, steal 6–7 —
  because `Region::LoadLevel`, `Archive::ReadFromFile`, `FUN_1011d0e0` and
  `Region::GetEntitiesInFrustum` all open with the identical
  `55 8b ec 83 e4 f8`, so a 6-byte match proves nothing about build identity;
- `patchCall(site, expectedBytes, replacement)` — the verified call-site
  retargeter for `E8 rel32` and `FF 15 imm32` sites, in `src/shadow_fix.cpp`'s
  style: verify, resolve, rewrite the displacement into a thunk page, remember
  for exact restoration.

**3.2 `src/engine_probe.{h,cpp}`.** Installed only when `performance_trace` is
on and `[debug] engine_trace` is not `0` — so the shipping default is
byte-identical to today. Each target is resolved by decorated export name and
then *asserted* against its expected RVA; a mismatch skips that one hook and
leaves the rest.

| event | target | mechanism |
| --- | --- | --- |
| `engine_level_load`, `_us`, `_main` | `Region::LoadLevel` `0x1020bec0` | detour; returns immediately when resident, so the µs column *is* the forced-load cost |
| `engine_res_load`, `_us`, `_main` | `ResourceLoader::LoadResource` `0x10213ed0` | detour; includes the `EnterCriticalSection(resource+0x4c)` wait, which is exactly the stall worth naming |
| `engine_region_unload`, `_us` | `Region::UnloadLevel` `0x1020e040` | detour |
| `engine_arc_kib` | `Archive::ReadFromFile` `0x1011d320` | detour; one add |
| `engine_arc_blocks`, `engine_arc_inflate_us` | `FUN_1011d0e0` | detour — **not exported**, so verify 24 bytes and cross-check its format-string reference |
| `engine_res_enqueued` | `ResourceLoader::EnqueueResource` `0x102145c0` | detour; tells you the backlog is growing before it becomes a hitch |
| `engine_fence_wait_us` | **P6** | `patchCall` to a `__stdcall(HANDLE, DWORD)` thunk |
| `engine_region_lock_hits`, `_us` | the `EnterCriticalSection` call sites inside `Region::GetEntitiesInFrustum` `0x10209840` and `AddElementsInBox` `0x101677e0` | `patchCall` to a thunk that does `TryEnterCriticalSection` first and takes timestamps **only on failure** — two atomics and zero timestamps in the uncontended case, which is what makes measuring the render-path block affordable |
| `WaitForLoadingToFinish` entries | `0x1020bde0`, 7 bytes | replace wholesale with a semantically identical counting version — proves whether it is ever hit before anything is changed |

**Risk.** The highest in the plan: several writes into Engine.dll on paths that
run thousands of times a second. Mitigations: each hook is independent and fails
to install alone; nothing installs unless the trace is on; export-name
resolution with the RVA as cross-check; long signature verification; and the two
hot ones are designed to cost ~nothing uncontended. `shutdown()` restores every
site in reverse, verifying the bytes are still ours first.

**Self-test.** `patchCall` and `attach` are fully exercisable off-game against
synthetic targets — the pattern `test/selftest.cpp:173` already uses for the
grass probe: redirect a synthetic `FF 15` site and assert exact restoration;
assert `attach` refuses a target whose verify bytes differ while its stolen
bytes match; assert the `TryEnterCriticalSection`-first thunk records nothing
uncontended and a plausible duration when a second thread holds the section.

**Run 2** — `performance_trace=1` (hitch rows only; this mode is the point of
Stage 3), same save and route. Hitch rows whose `unusual` column now leads with
`engine_level_load_main_us` or `engine_res_load_main_us` are the game's own
synchronous loads — the residual `frames.py` has always reported as "the game's
frame". `engine_region_lock_us > 0` on a hitch row is direct proof of the
audit's §1b. `engine_fence_wait_us` spiking one frame *after* a heavy
`engine_arc_inflate_us` frame is the audit's §3 prediction, and confirming it
would be the first direct evidence for it. That run decides the order of
everything below.

---

# Stage 4 — Fix the archive read path

Ordered by Run 2's read-time-vs-inflate-time split; both halves are worth doing.

**4.1 A multi-block decompressed cache — do this one first.** *(Built. See
the Status note above and `findings.md` §18 for where this text and the
binary disagreed.)* Inline detour on
`FUN_1011d0e0` (`0x1011d0e0`). Key a cache entry on
`{archive, archive[0xc] file HANDLE, desc[0] offset, desc[1] csize,
desc[2] usize}` where `desc = *(void**)(entry+0x20) + blockIndex*12`. On a hit,
`memcpy` into `blockBuffer[2]`, set `blockBuffer[0] = blockIndex`, and return
without reading or inflating anything. On a miss, call the trampoline and then
copy the freshly inflated block in. A fixed slab of `archive_cache_mb` MiB in
256 KiB slots with a clock victim, under its own critical section held only
across lookup and insert — never across the `ReadFile` or the inflate.

A 256 KiB `memcpy` is on the order of 25 µs; a 256 KiB inflate under Rosetta is
on the order of a millisecond. Against a single-slot cache in front of one
2 GB entry, this is the highest-value, lowest-risk item in the archive work, and
it should land before either of the two below. Including the handle and both
sizes in the key makes a wrong hit implausible; `archive_cache_mb=8verify`
inflates anyway and `memcmp`s for one measurement boot, which is the honest way
to buy confidence in that claim rather than asserting it.

**4.2 Bounded compressed prefetch.** *(**STRUCK by run 24.** The syscall half of this item is worth 5 ms a session: `SetFilePointerEx` costs 1 µs a block
and `ReadFile` is throughput-bound, 626 MiB in 1,043 ms. Batching moves the
same bytes, and this design deliberately over-reads, so it would move more.
`findings.md` §23.)* On a cache miss, read
`min(prefetch_budget, fileSize - desc[0])` bytes from `desc[0]` into a
per-thread staging buffer under one lock/seek/read, and inflate the requested
block out of it; later misses inside that window skip the syscall entirely. For
a 21 MiB texture this collapses 86 seek+read pairs and 86 acquisitions of
`archive+0x60` toward a handful.

**The budget is the whole point, and my earlier framing was wrong.** "One
`ReadFile` for the entry's whole compressed extent" is correct for a texture and
catastrophic for `Levels.arc`, whose single entry is 656 MiB compressed. Default
the window to 1 MiB. Global contiguity (R1) is what makes an over-read past the
requested block safe — it never leaves valid data, and a block is only ever
inflated through its own descriptor — but it must still be clamped against the
file size at the last block.

**4.3 libdeflate for block decompression.** *(**Confirmed as the target by
run 24**, which split the block routine 77% zlib / 23% read / 0.1% seek:
3,494 ms of `uncompress` a session at 457 µs a block. It is the only archive
item left with a number. Do it after Stage 5.1 — it is still the riskiest
item in this plan.)* Reach for this only if 4.1 and 4.2
leave inflate dominant — a cache hit costs 25 µs and the best possible inflate
still costs hundreds. Vendor `libdeflate` under
`third_party/libdeflate/` at a pinned revision with its MIT license, in the
style of `third_party/smaa/REVISION`, and add its `.c` files to the existing
single link line in `scripts/build.sh`. Blocks are zlib-wrapped, so
`libdeflate_zlib_decompress` is the entry point, with a per-thread cached
decompressor.

Hook at **P8** — the `E8` to `FUN_10065760` — rather than detouring
`FUN_10065760` itself, so only archive block decompression is redirected and the
engine's three other `uncompress` callers keep zlib.

**The ABI needs a hand-emitted thunk, not a C declaration.** `FUN_10065760`
takes `ECX` = dest, `EDX` = `uLongf* destLen`, `[esp+4]` = source,
`[esp+8]` = sourceLen, and ends in a plain `RET` — the **caller** pops the 8
bytes (`1011d1df ADD ESP,8`). GCC's `__fastcall` is callee-pop, so the target
must be an assembled stub on a `VirtualAlloc`'d page in `shadow_fix::buildThunk`
style that re-pushes all four arguments to a `__cdecl` helper. Getting this
wrong desynchronises the stack on a path that runs thousands of times a second.

The helper writes `*destLen` and returns zlib's `Z_OK`/`Z_DATA_ERROR`/
`Z_BUF_ERROR` so the engine's own size check and its two error strings still
fire on a genuinely corrupt archive. On **any** libdeflate failure, fall through
to the original and count it: a decompressor that silently disagreed with zlib
on one block would corrupt a texture, and the fallback makes that impossible to
miss. Add `archive_decompress=libdeflate_verify`, which runs both and
`memcmp`s, for one measurement boot.

This is a latency win on the loader thread, not a contention win — the engine
already decompresses outside `archive+0x60` — but it shortens every load,
including the ones forced onto the main thread.

**Not doing yet, with reasons.** Parallel block decompression across a mod-owned
worker pool: real, but it changes the engine's threading model and the win is
bounded by 4.1 + 4.2. Pooling the two 256 KiB `operator new[]` scratch buffers
the archive `File` constructor allocates per compressed entry — 512 KiB of heap
churn per opened file in a fragmenting 32-bit MSVC heap — is cheap and tempting
but needs its own evidence that fragmentation is biting.

*(**The pooling idea is struck by run 24.** It got its evidence and the
evidence acquitted the heap: `operator new[]` and `delete[]` across all of
Engine.dll cost 173 ms a session, and 3.6 ms on a 1,534.8 ms freeze frame that
opened 1,299 archive files and churned 150 MiB. Wine's heap is not slow. This
is the right outcome for a "needs its own evidence" item, and it is recorded
rather than deleted so the reasoning stays legible.)*

**Config at this stage.** `[performance] archive_cache_mb = 0` (0 disables and allocates
nothing; `8verify` for a verification boot), `archive_prefetch_kb = 0`, and
`archive_decompress = original | libdeflate | libdeflate_verify`.

`archive_cache_mb` shipped as written, clamped to 256 MiB; the other two are
still unwritten.

**Run 3** — boots in this order, same save and route each time: cache alone at
8 MiB, then 32 MiB to find the knee; then cache + prefetch; then + libdeflate
only if inflate still dominates. The `engine_arc_*` columns price each
independently, and Stage 3's repeat-inflate ring is what says whether 4.1 is
working.

---

# Stage 5 — Stop the renderer forcing synchronous level loads

Depends on Run 2 showing forced loads actually occurring on the render thread.
The pop-in trade is accepted.

**5.1 The change.** Retarget P1 and P2 to one thunk: if `region[0x50] != 0` call
the original `Region::LoadLevel` unchanged; if a load is already in flight
(`region[0x74] | [0x75] | [0x78]`) return `true` immediately; otherwise call
`Region::BackgroundLoadLevel` (`0x1020be60`) and return `true`. That sets
`region[0x74] = 1`, and the caller's very next instructions —
`CMP byte [EDI+0x74],0` / `JNZ epilogue` — skip the region for this frame. The
engine already implements exactly the behaviour we want; the patch only chooses
it. Both sites pass `false`, so the only case they ever force is "level entirely
absent" — precisely the case `BackgroundLoadLevel` handles. Its second `bool` is
dead (it reads only `[ESP+4]`); pass 0.

Two details that make this safe rather than merely plausible. `MOV` does not
touch flags, so the `MOV dword [EDI+0x6c],0` between the `CMP` and the `JNZ`
runs **on the skip path too** — a deferred region still has its unload countdown
reset and cannot be evicted while we wait for it. And the load self-heals:
`FUN_1020a6b0` clears `[0x74]`/`[0x75]` on both success and failure, so the very
next frame's `AddElementsInBox` draws the region normally.

**5.2 A watchdog, so a region can never stay invisible.** Track deferrals in a
`grass::PointerIndex` (already implemented and unit-tested for
insert/lookup/remove/tombstone/overflow) with the tick at which each was first
deferred. If a region is still absent after `level_load_watchdog_ms` (default
2000), fall back to the synchronous load for that region and count it — trading
the hitch back for correctness rather than leaving a hole on screen.

**5.3 Left alone — and now proven, not assumed.** `Region::AddToScene`
(`0x1020e6f0`) is **already async-aware**: it checks `[0x74]`/`[0x75]`/`[0x78]`
and returns early, so there is nothing to gain. `Region::GuaranteedGetLevel`
(`0x1020e7b0`) **returns NULL to its callers** when `[0x74]` is set
(`1020e7ce xor edi,edi`) — its name is a promise, and making that promise fail
more often is a crash surface, not a stutter fix. Instrument both; patch
neither. **If Run 2 shows the forced loads arrive through those rather than
through `AddElementsInBox`, this stage needs redesigning before it is written.**

**Config.** `[performance] level_loads = original | async` (default `original`)
and `level_load_watchdog_ms = 2000`.

**Run 4** — A/B, `original` then `async`, same save, same route into unvisited
territory. Read the hitch count and p99, the `engine_level_load_*` columns, the
deferral and watchdog-fallback counts, and — by eye — whether the pop-in is
acceptable at 5120×1440. Then a third boot at `shadow_split=0.325`: the narrower
caster box should cut the deferral count sharply, and if the pop is objectionable
at 0.45 but invisible at 0.325 that is a real product answer rather than a
compromise. If it is unacceptable at both, the fallback is "defer only regions
far from the camera", which costs more to write and buys less.

---

# Stage 6 — The per-frame fence and sweeps

Lowest confidence, so last; Run 2 decides whether any of it is worth writing.

**6.1 Amortize the seven unload sweeps** (P3): retarget all seven to one thunk
that calls the original for a different manager each frame, round-robin.
Behaviourally this only delays eviction, but this is a 32-bit process already
carrying ~336 MiB of the mod's shadow targets, so it is gated on the sweeps
measurably costing something.

**6.2 The fence's cost is the thread walk, not the wait.** `0x10370258` is a
manual-reset event that `SetThreadFencesPaused` resets on the first pause and
re-signals at zero, so in the ordinary case it is *already signalled* and
`WaitForSingleObject(…, INFINITE)` returns immediately. Do not touch it — it is
a correctness rendezvous and it is not the cost. The cost is `FUN_1011f490`,
which per call does **`OpenThread` + `WaitForSingleObject(h,0)` + `CloseHandle`
per registered thread** (`1011f53e`, `1011f551`, `1011f632`) — under Wine, three
wineserver round trips per thread per `Engine::Update`, on the main thread.

Throttle it by retargeting **P5 only** (`0x10144789`), so the Level Loading
Thread's own two calls to the same function, and the four others, are untouched.
The thunk calls through only if `thread_fence_ms` (default 16) has elapsed.
This is safe by construction: `FUN_1011f490` writes the fence globals from the
current counter and then *lowers* them to the minimum over live threads, so
skipping a call leaves the previous, **smaller** value — strictly conservative.
Eviction frees slightly less, never more, and dead-thread pruning is delayed by
at most `thread_fence_ms`, not skipped.

**6.3 `Region::WaitForLoadingToFinish` is dead code — reject as a fix, keep as a
proof.** A full `.text` scan for `E8`/`E9 rel32` targeting `0x1020bde0` returns
**zero** hits, and none of `Game.dll`, `TQ.exe`, `Direct3D11.dll` or
`Direct3D.dll` imports `?WaitForLoadingToFinish@Region@GAME@@QAEXXZ`. Nothing
calls it. The only residue is a hypothetical `GetProcAddress`, and ten lines
disposes of even that: the function is exactly
`80 79 78 01 74 fa c3` and takes no arguments, so **replace it outright** rather
than detouring it (a trampoline is impossible anyway — the stolen bytes contain
the relative `74 fa`). Write `68 <addr32> c3 90` over all seven and point it at a
counting loop with `YieldProcessor()` then `SwitchToThread()`. If the counter is
ever non-zero that is genuine news and the fix is already in; if it stays zero
for a session, this hazard is closed for good and `findings.md` should say so.

**6.4 Thread priority — both halves of my earlier claim were wrong.** I had
written that `ResourceLoader::StartThread` is only a naming call and that no
loader runs below normal. Corrected against the binary:

- `FUN_10284c80` is `Thread::Start(const char* name)` and it **does** create the
  thread (`CreateThread(0,0, 0x10284ba0, this, 0, &tid)`, handle stored at
  `this+0x1c`). `ResourceLoader::StartThread` (`0x102142c0`) therefore creates
  the resource loader.
- That loader **already raises itself to ABOVE_NORMAL**: its body at
  `0x102137a0` runs `102137b6  6a 01 / ff 76 1c / ff 15 90 c0 2a 10`, i.e.
  `SetThreadPriority(handle, THREAD_PRIORITY_ABOVE_NORMAL)`. There is nothing to
  raise. (The two `SetThreadPriority(h,-1)` sites are the **Recast** navmesh
  threads and `Region::UnloadFOW` — neither is a loader.)
- The thread that *is* at NORMAL is the **"Level Loading Thread"**
  (`FUN_10051800` never sets a priority), and Stage 5 makes it the critical path
  for terrain appearing. So the knob worth having is
  `level_thread_priority = original | above`, and it needs **no code patching at
  all**: read the singleton at `*(void**)(engine + 0x41a634)` (written at
  `0x10052080`), take the `HANDLE` at `+0x1c`, `SetThreadPriority`, restore in
  `shutdown()`. Guard the read with `readable()`. Not a default — an A/B knob,
  and only worth reaching for if Stage 5's pop lasts longer than tolerable.

**6.5 `EnableSingleProcessorMode` is inert — closed.** `0x102142b0` is
`this[4] = arg`, and `loader[4]` has exactly two readers, both of which lead to
a dead load whose result is discarded (`102142d6` and `1021382b`, each
`MOV EAX,[…+0x70]` with no use). Whatever it once selected was optimised away.
Patching it changes nothing. Removed from the plan.

---

# Files

| file | change |
| --- | --- |
| `src/probe.h`, `src/probe.cpp` | S0 — buffer sizes, `engineCount` channel, `_us` columns, split rejection counters |
| `src/upload.h`, `src/upload.cpp` | **new** — S1 extraction, S2 copy path, budgets, substitution table, advance loop |
| `src/visual.cpp` | S0 — off-thread texture-create routing (`:1050`); S1 — extraction; S2 — `findTextureOwner` (`:549`), lease deletion (`:171-183`, `:583-642`), SRV `AddRef` (`:797`), hook bodies, install (`:2753`), teardown (`:2970`) |
| `src/detour.h`, `src/detour.cpp` | **new** — S3, lifted from `src/grass.cpp:200-285`, plus `verify`≠`stolen` and `patchCall` |
| `src/engine_probe.h`, `.cpp` | **new** — S3 Engine.dll instrumentation |
| `src/archive_fix.h`, `.cpp` | **new** — S4 block cache, bounded prefetch, libdeflate thunk |
| `src/region_fix.h`, `.cpp` | **new** — S5 `LoadLevel` retarget, S6 sweep/fence/spin patches |
| `src/grass.cpp` | S3 — use the shared detour helpers instead of its private copy |
| `scripts/build.sh`, `scripts/selftest-offgame.sh` | new sources; S4 adds libdeflate |
| `third_party/libdeflate/` | **new** — S4, pinned revision + MIT license |
| `test/selftest.cpp` | probe channel and CSV header, upload module, detour and `patchCall` |
| `tools/frames.py` | S3 follow-up — an `engine` section reconciled against "the game's frame" |
| `research/streaming/arc-format.md`, `tools/arcinfo.py` | **new** — R1 |
| `research/streaming/findings.md` | R2/R3 and findings (2) and (5); fix three stale references and the closing cross-reference |
| `README.md` | new INI keys; rewrite the streaming paragraph, whose "retained mapped archive data, avoiding an extra texture-sized copy" is both wrong about `.arc` and no longer the design |

# Verification

- `npm run doctor && npm run build && npm run selftest` at every stage. The
  self-test runs against the real 32-bit DXMT device; Stage 1 exists to bring
  the streaming path inside it.
- Every Engine.dll patch verifies its bytes and its resolved target before
  writing, installs nothing on mismatch, and is restored byte-for-byte in
  `shutdown()` — the invariant `src/shadow_fix.cpp` and `src/grass.cpp` already
  hold. A build running on a different `Engine.dll` must simply not install
  them.
- The four in-game runs above. Each is one boot with an ini I prepare under
  `cache/runs/`, built from the reporter's live `tqflicker.ini` with only the
  variable under test changed; they launch from the CrossOver UI, play the
  agreed route, quit normally, and `tools/frames.py cache/<run>.csv` summarizes
  it.
- Ground truth for Stages 2 and 5 is partly visual — soft-to-sharp texture
  pop-in, and region pop-in. Neither ships on by default until they have looked
  at it.

# Open questions, flagged rather than assumed

- The `EnterCriticalSection` call-site offsets inside `0x10209840` and
  `0x101677e0` are not yet extracted (the functions are confirmed). A grep of
  the disassembly at implementation time, not a design risk.
- The stack position of `size` in `Archive::ReadFromFile` is inferred from its
  decorated name, not read off the disassembly.
- Which thread `AddElementsInBox` runs on during the *shadow* passes. It is
  `GraphicsSceneRenderer` work inside `Engine::Render`, so almost certainly the
  main thread, but the path from `GraphicsEngine::Update` to the caster gather
  was not traced. Stage 3's thread-classified counter settles it on the first
  boot, and Stage 5 is gated on it.
- Whether the "Level Loading Thread" singleton already exists in a normal
  session — it is constructed lazily. `Game.dll` imports `BackgroundLoadLevel`,
  so probably yes. Stage 3 reads `engine+0x41a634` once a second and reports.
- `Resources/Levels.arc` has an mtime of Aug 31, later than its siblings. Its
  header parses cleanly and all 7,646 records are contiguous, so nothing here
  depends on it being pristine — but if a mod has been installed into it, say
  so, because it changes what "the supported build" means for the archive
  stages.
- Real 32-bit memcpy throughput under CrossOver on Apple Silicon is unmeasured;
  it drives the claim that Stage 2's copy costs a fraction of the upload it
  replaces. `engine_tex_create_off_us` answers it on the first run, and the
  escape hatch is to steal the buffer for the `"DDS "` subset only, using R2's
  resolved `operator delete[]` — but every terrain texture measured is `TEX`,
  so that subset may be empty.
- Whether DXMT's `UpdateSubresource` cost is genuinely linear in bytes.
  `upload_ms_per_mib_x100` plotted against chunk size in a `full` run is the
  experiment; §2.8's two-term concern rests on it.
