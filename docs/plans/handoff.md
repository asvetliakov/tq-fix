# Where the stutter work stands

Companion to `game-stutter-mitigation.md`, which is the plan. That document's
"Status" section records where the plan was wrong; this one records what is
built, what is measured, and what to do next. Current as of **run 33** on
branch `stutter-mitigation`.

**Start at "The order of work" near the end, and read findings.md §34 before
anything else.** §34 is a fresh-eyes review of all nineteen recorded runs and
it invalidates the framing of a lot of what sits above it here: the frame this
project chased from run 21 to run 33 is the menu's load-game, not an in-game
stutter, and the real in-game stutter had been mis-anatomised since §25.
Everything above "The order of work" is history and should be read as such.

## Read these first

1. `research/streaming/findings.md` **§34** — the corrected freeze taxonomy
   (classes A–D) and what each is worth. Then **§31 and §33** for how Stage 5
   ended, and **§25** for the three-class split §34 corrects.
2. `docs/plans/game-stutter-mitigation.md` — the plan, and its Status section,
   which corrects two of its five headline findings and records what Stage 3
   measured away. Note it predates §34.
3. `research/streaming/findings.md` — §4–§7 for the probe's blindness, the
   archive `File` class and the verified patch sites; §8–§17 for what Stage 3
   measured; **§18 for the block routine**, read end to end, which is what
   Stage 4.1 is built on; §26–§30 for the instrument chain and how it was
   built.
3. `research/streaming/arc-format.md` — the container format and what was
   checked across all 135 archives. Read R1 for the container, not for the
   runtime structures: §18 records one field the engine rewrites at load.

## The install this is built for

A 32-bit GOG Titan Quest AE under CrossOver/DXMT, 5120×1440, plus a **92.4 GiB
loose texture pack** (12,519 `.tex`, all `TEX\x01`) living in `Settings/`. The
pack is the reporter's normal configuration and both `File` classes must keep
working. Park it with `mv Settings Settings-back` to get an archive-only
install; the game's three `.txt` config files live inside the pack folder, so
if it objects, recreate `Settings/` containing just those.

## What landed

| commit | what |
| --- | --- |
| `0e970bc` | Stage 0. `probe::engineCount`, a thread-safe counting channel that folds into the frame record at `endFrame`; off-thread `CreateTexture2D` timing; the rejection split; CSV buffers raised; `arc-format.md` + `arcinfo.py`; findings §4–§7. |
| `1409059` | Stage 1. The uploader extracted to `src/upload.{h,cpp}` with injected device calls, an injected clock, and a `retain`/`release` pair so the mapping lease stays in `visual.cpp`. |
| `f44b493` | Recorded the pack; withdrew an unsupported claim about the loose path being a live use-after-free. |
| `d08ace1` | `upload_unmap_us` / `upload_release_us`, to split the retire path. |
| `714b6ba` | **The unmap worker** and **the loose texture cap.** |
| `6d5b5e2`, `a6785fe` | Two bugs in the cap, both caught by its own counters. |
| `cd9536e` | `upload_leased_mib`, to size a byte budget rather than guess one. |
| `134a8ee` | Run 9: the pack measured out of the picture. |
| `eb550ca` | Plan item 2.5 — the full SRV is now referenced. |
| `b161e08` | **Stage 3.** `src/detour.{h,cpp}` and `src/engine_probe.{h,cpp}`; fourteen verified sites in `Engine.dll`. |
| `ff2b1bd` | `Engine::Update` and `Engine::Render` bracketed; `GameEngine::Update` in `Game.dll`. |
| `3d52496` | `detour::patchImport`; TQ.exe's main loop measured through its import table, patching no code. |
| `7af497d`, `2c10504`, `b7352d7` | The rest of the loop, and the message pump split. Eleven imports of TQ.exe and two of `Engine.dll`. |
| `53773b3`, `60950cf`, `e49d274` | Greece; the message histogram; the timer experiment and its `[performance] timer_period_ms` switch. |
| `96527e6` | The pump closed: the timer is not the game's, and the re-arm path is refused. |
| `6d760c2` | `verify-sites.py`, which reads every byte table out of the source and compares it to the installed binaries. |
| `d6a4e79` | **Stage 4.1** — `src/arc_cache.{h,cpp}`, `[performance] archive_cache_mb`, seven `arc_cache_*` columns, five more verified windows in the block routine. Then six measurement runs (21–26) and four more instrument groups: the array allocator (`2048`), the archive syscalls (`4096`), everything that blocks (`8192`), and the `_main` split on all three. Findings §18–§25. |

Verification is `npm run doctor && npm run build && npm run selftest`.

**And `research/streaming/tools/verify-sites.py`**, which reads every byte
table out of `src/engine_probe.cpp` and compares it to the installed binaries,
resolving relocated dwords the way `detour::matches` does at runtime. Run it
after touching a table and after any game update. It caught two real bugs the
first time it ran: a relocation offset one byte short, which would have
silently skipped all three region-lock hooks, and a `moduleText` check that
failed for every hook after the first.

## What the runs measured

Runs are in `cache/` (gitignored, still on disk); their inis are in
`cache/runs/`. `tools/frames.py <csv>` summarises one.

- **The deferred `UnmapViewOfFile` was the uploader's whole cost.** 1,032 ms
  inside Present in one session, up to 37.8 ms in a frame, 92–98% of every one
  of the six worst `stream_step` frames — while those frames uploaded only
  256–1024 KiB. `upload_release_us` totalled 0.7 ms, which killed the
  competing explanation outright. Moving it to a worker took
  `stream_step_ms` max from 39.0 to 13 ms, and `upload_unmap_inline_us` has
  read 0 across three runs since.
- **The loose cap works.** 120–123 redirects per run in Eternal Embers,
  `arc_open` up ~1,460, `upload_src_none` up as those textures start arriving
  from the archive, and visually clean per the reporter.
- **The texture pack is not the cause of the remaining stutter.** Run 9
  repeated the route with the pack removed and every other setting identical:
  p50 frame time 9.02 ms in both, worst frame 1,358 ms against 1,503 — the
  same event, ~1,300 archive opens in one frame — and frames over 200 ms went
  8 → 12. Removing 92.4 GiB changed nothing. About 30% of every archive open
  in a session lands inside a frame over 200 ms, in both configurations.
- Run 9 also confirms the plan's finding (1) for the stock configuration:
  with no loose files, `upload_jobs_started` and `upload_src_loose` are 0.

## Stage 2, item by item

Not skipped — reduced by measurement. Current state:

| item | status |
| --- | --- |
| 2.1 recognise the archive `File` class | **Deferred.** Archive-served textures bypass the uploader, and that costs 295 ms of render-thread time in a 96 s session (0.31%), against 97% of hitch time being the game's own loading. Revisit only if Stage 3 shows the loader thread's texture creation feeding the `Engine::Update` fence. |
| 2.2 copy the retained mips | **Not needed as designed.** It existed to sever the engine's buffer lifetime; the lease plus the worker unmap solved the measured problem without it. Would only return if the leases are dropped. |
| 2.3 delete `MappingLease` | **Superseded.** The worker unmap took the 1.03 s without the risk. The leases stay. |
| 2.4 bound the pool | **Half done.** `upload_leased_mib` added; the budget itself waits on the number. See below. |
| 2.5 `AddRef` the full SRV | **Done** (`eb550ca`). |
| 2.6 `PSSetShaderResources` fast path | **Open.** The lock is still held across the D3D call on a 2400–5000/frame path. `ps_set_srv` led 3 of 48 hitches in run 8. Modest. |
| 2.7 advance more than one job per Present | **Open, low value.** |
| 2.8 chunk-rate model | **Open, low value.** `upload_ms_per_mib_x100` was never added. |

**The one open question with a number attached.** The pool's only real limit is
a count — `kMaxMappingLeases = 128` in `visual.cpp`. Run 8 shows it binding
exactly: peak concurrent jobs 128, and all 42 `upload_reject_alloc` rejections
at exactly 128 in flight, none below. Do **not** simply raise it: each lease
holds a `MapViewOfFile` of a whole texture file, so 128 leases is ~340 MiB of
address space at the pack's median and would be 2.7 GiB at the 4K cap's
maximum, in a process already carrying ~336 MiB of the mod's shadow targets.
`upload_leased_mib` (added `cd9536e`, sampled once a frame, reads as a gauge)
answers it on the next pack-on run with any route. With a real peak, replace
the count with a byte budget.

## Stage 3 is finished, and the frame is fully accounted for

Eleven instrumented runs (10–20). None of them cost anything measurable: p50,
p99 and the mod's share never moved, across a detour on a function entered
12 million times a session. `findings.md` §8–§17 has the detail.

| | session | frames over 50 ms |
| --- | ---: | ---: |
| `Engine::Render` | 57.2% | 51.0% |
| `Engine::PresentSurface` | 21.6% | 5.2% |
| `Engine::Update` | 10.2% | 8.8% |
| **`PeekMessageA`** | **8.2%** | **20.3%** |
| unexplained | 2.3% | 12.2% |

**Both classes of hitch have names.**

*The big one* is the game's own loading, inside `Engine::Render`: the worst
frame is 1,453.8 ms of which 1,448.6 is render, with a forced main-thread
`Region::LoadLevel` of 505.7 ms and 264 ms of archive inflate under it.

*The second one* — unnamed until run 15 — is **`PeekMessageA`**. 7.8–8.5% of
wall clock, 12–20% of the hitch time, a single call of 126–212 ms on the worst
frames. Greece reproduces it at the same share on entirely different content.
The messages most likely to be slow are the ones that cannot be answered
without asking the host, and the `WM_TIMER` behind most of them has no window,
an id of `0x7fff`, an `lParam` that is not a callback, and no `SetTimer` call
anywhere in a session — so it is not the game's to change, and `SetTimer`
could not re-periodise a thread timer by id even if it were. That path is
refused in code rather than left loaded.

**The mod owns the import slot, can measure it exactly, and has nothing to put
in its place, because the work is the round trip.** What remains is a host
question — CrossOver's synchronisation settings — and a Windows comparison,
where the same build should report `pump_peek_us` near zero.
`[performance] timer_period_ms` stays in the tree at its default of 0, inert.

**Closed by measurement, not deferred.** The region lock is never contended (0
hits in a session across three sites), the seven `UnloadUnreferencedResources`
sweeps cost 11.2 ms, the loader-fence wait 1.6 ms over 7,349 waits,
`WaitForLoadingToFinish` is never called, `GameEngine::Update` is 0.3% of the
session, the online platform poll 44 ms, `SoundManager::Update` 2 ms, and free
address space never falls below 3,445 MiB. **Stage 6 is deleted, not
postponed.**

## The runs on disk

**`cache/` is gitignored, so none of this is in the repository — it exists
only on the reporter's machine.** Run CSVs and logs live in the game directory
as `tqflicker-frames.runN.csv` / `tqflicker-debug.runN.log`, and the ini each
was booted with is in `cache/runs/`. The ini headers carry the reasoning for
each run and are worth reading before re-running anything; runs 21–26 are the
current work. `tools/frames.py <csv>` summarises one.

| run | what it settled |
| --- | --- |
| 8, 9 | pack on / pack off. The texture pack is not the cause. |
| 10 | Stage 3's first boot. Region lock 0, sweeps 11 ms, fence 1.6 ms, `WaitForLoadingToFinish` 0. |
| 11 | `Engine::Update` / `Engine::Render` bracketed. 38% of hitch time outside `Engine.dll`. |
| 12 | `GameEngine::Update` is 0.3%. Present is called outside `Engine::Render`. |
| 13 | TQ.exe's `Sleep` (1 call), `GetMessageA` (0), `WaitForSingleObject` (0), address space fine. |
| 14 | `Engine::PresentSurface` is 24% of the session but 4.6% of hitch time. |
| 15 | **`EWindow::ProcessMessages` is 20.3% of hitch time.** Platform poll 44 ms. |
| 16 | `PeekMessageA` is 98.8% of the pump. |
| 17 | **Greece.** Reproduces at the same share; archive amplification 2.3x there. |
| 18 | The message histogram: `WM_TIMER` is 76% of slow retrievals. |
| 19 | `SetTimer` is never called while installed. |
| 20 | The timer has no window — the re-arm experiment cannot work. |
| 21 | **The block cache is correct** — 286 blocks compared byte for byte, 0 mismatches, 0 skips. And 99.6% of stores evicted a live block, so its 3.8% hit rate measures 32 slots, not reuse. |
| 22 | **The ceiling.** 256 MiB / 1,024 slots: 8.2% session, 19.4% on the heaviest frame, against 3.8% / 17.3% at 8 MiB. 32x the memory buys 2 points where it matters. The 1.8x is partial consumption, not re-inflation. |
| 23 | **Serving.** `engine_arc_inflate_us` 4,310 → 4,180 ms; a hit costs **1.4 µs**, not the 25 assumed. And the freeze frame broken down: 61% of it is named by nothing, with a verified candidate. |
| 24 | **The heap is innocent** — 173 ms a session, 3.6 ms on the freeze frame. And the block routine splits **77% zlib / 23% ReadFile / 0.1% seek**, the opposite of the prediction: 4.2 is dead, 4.3 is the only archive item left with a number. 996 ms of the frame still unnamed. |
| 25 | **The archive lock is innocent too** — 0 contended acquisitions on the freeze frame, 222 ms a session across every critical section in Engine.dll. The waits/sleeps were built without a `_main` split and read larger than wall clock. But **`Sleep` runs 250×/frame in frames over 200 ms against 5.8 normally** — a poll loop. |
| 26 | **The main thread polls with `Sleep(1)`** — 350 calls / 435 ms inside a 513 ms forced level load. Granularity is only 1.24×, so `timeBeginPeriod` is worth ~85 ms, not the lever. **And the freeze frames are three different mechanisms, not one.** |

## Stage 4.1: built, measured, and marginal

The archive block cache. What it was sized against, measured in two acts:

| | blocks | inflated | requested | amplification | inflate |
| --- | ---: | ---: | ---: | ---: | ---: |
| Eternal Embers | 7,491 | 1,917,696 KiB | 1,070,055 KiB | 1.8x | 4,418 ms |
| Greece | 4,560 | 1,167,360 KiB | 505,729 KiB | **2.3x** | 2,131 ms |

It attacks `Engine::Render`, which is the half that is ours, and the base game
wastes proportionally more than the expansion — which is what a one-entry
cache in front of a 2 GB file does when the reads are smaller.

`src/arc_cache.{h,cpp}` is it: a fixed slab of 256 KiB slots with a clock
victim, keyed on `{archive, file handle, block offset, compressed size,
uncompressed size}`, behind the detour on `FUN_1011d0e0` that was already
there for `engine_arc_blocks`. The lock is held across the lookup and the copy
out and across the insert, and never across the `ReadFile` or the inflate.
`archive_cache_mb` defaults to `0`, which allocates nothing and leaves the
block routine byte-identical to today.

**Read `findings.md` §18 before touching any of it.** The block routine was
re-read end to end against the pinned binary rather than taken from §6/§7, and
three things came out of that which the documents did not say:

- `entry+0x20` is a **pointer** at runtime where the on-disk file record has
  `firstBlockIndex`, an integer. The engine rewrites it when it opens the
  archive. Building the key from `arc-format.md`'s layout would have
  dereferenced an index.
- The uncompressed branch never reaches the block routine at all —
  `Archive::ReadFromFile` branches on the compressed bit at `1011d390` — so
  the 6,880 uncompressed dialog entries are not a hazard for 4.1. They will be
  for 4.2.
- A hit's whole observable contract is `mov [edi],ebx` and `mov al,1`, from
  the epilogue at `1011d230`. It also takes the archive's critical section
  zero times, which is a contention win nobody costed.

`8verify` came out differently from the plan and better: it never serves, and
compares **on insert** against what the engine just produced for a key that is
already resident. Every request that would have been a hit becomes a proof, at
the cost of an uncached run, with no shadow buffer and no thread-local state.

Five new windows in that routine are byte-verified — the forty-seven byte
address derivation, the seek, the read, the inflate and the epilogue, plus the
block-size writer at `1011ea94` — and `verify-sites.py` additionally checks
every offset `arc_cache.cpp` dereferences with against the *operand* that uses
it. Perturbing one constant fails the tool; that was tested.

**Runs 21 and 22 have been run, and Stage 4.1 is answered.** `findings.md`
§19 and §20 are the record. Both were `verify` boots, so **no block has ever
actually been served**.

*It is correct, and that is not in doubt.* Across the two runs: **914 blocks
compared byte for byte, 0 mismatches, 0 `arc_cache_skip` over 15,186
requests.** `describeBlock` never once refused — so §18's offsets are right at
runtime on every archive the route touched, not merely right in the byte
tables. Run 22 exercised 32x as many distinct keys as run 21.

*Its value is small, and the plan's premise was wrong.*

| | 8 MiB / 32 slots | 256 MiB / 1,024 slots |
| --- | ---: | ---: |
| would-be hits, session | 3.8% | **8.2%** |
| frames > 200 ms | 13.3% | 14.4% |
| heaviest frame | 17.3% | **19.4%** |
| evictions | 99.6% of stores | 85.4% |

**32x the address space buys 4.4 points of session hit rate and 2.1 points
where it matters.** With 1,024 blocks resident, 91.8% of requests are still
for a block nothing has seen before — so §8's claim that the 1.8x is "the
single-slot cache re-inflating what it has already inflated" is **measured
false**, and §8 now says so. The excess is partial consumption: a 218 KiB read
at an arbitrary offset straddles 1.52 blocks and throws away the unused head
and tail, and nothing comes back for them.

What reuse exists is **burst-local** and lands where it hurts — 19.4% on the
archive-heaviest frame against 8.2% overall — but nearly all of it is inside a
window of a few dozen blocks, which is why 32 slots gets 17.3% of that frame
and 1,024 gets 19.4%.

Computed value, at 562 µs a block: **~136 ms off the worst frame of a session
for 8 MiB**, ~152 ms for 256 MiB. About a ninth of a 1,488 ms frame, which
stays over 1.3 s either way. Session-wide it is 0.15%.
`proc_avail_va_mib` never fell below 3,458 MiB with 256 MiB committed, so
address space was never the limit — the locality was.

**The decision is yours and it is genuinely close.** 8 MiB is free, proven,
and takes a ninth off the one frame a session that actually reads as a freeze;
it also does not change the experience. It ships at `0` either way.

**Run 23 served blocks for the first time.** `engine_arc_inflate_us` 4,310 →
4,180 ms, 291 hits, and **a hit costs 1.4 µs rather than the 25 assumed** (420
µs over 291 hits) — so §20's estimate was pessimistic by fifteen times on the
per-hit cost, though the verdict stands because the limit is the hit *rate*.
Three runs: **1,205 blocks compared byte for byte, 0 mismatches, 0 skips over
22,684 requests.** `archive_cache_mb=8` is correct, nearly free, and worth
~130 ms a session with ~140 of it on the worst frame. It ships at `0`; turning
it on is a judgement call, not a measurement.

## Run 23's frame anatomy, and a candidate that did not survive

Run 23's frame 4311, **1,310.2 ms** — the zone transition — is the first
complete anatomy of the hitch this project exists to fix:

```
engine_render               1,303.9 ms    99.5% of the frame
  |- Region::LoadLevel        508.8 ms    38.8%   (100% main thread)
  |    |- LoadResource        366.1 ms
  |         |- read+inflate   259.8 ms            (1,468 blocks, 254 hits)
  |- texture_create            25.2 ms            (715 textures)
  +- UNNAMED                  795.1 ms    60.7% of the frame
```

**The whole archive path is 260 ms of 1,310.** 4.1 is done, and 4.2 and 4.3
are arguing over a fifth of this frame. **The level load is 509 ms and wholly
main-thread**, which is Stage 5 and is worth more than all of Stage 4.
**And 795 ms — 61% — is named by nothing**, and has been the largest
unexplained cost in this project since run 8.

**It now has a verified candidate.** `FUN_1014d020`, the archive `File`
constructor, allocates *two* buffers of up to 256 KiB via `operator new[]`
(`0x102ac318`) for every compressed entry opened, and frees them on close.
Frame 4311 opened **1,299** of them — up to **649 MiB of `new[]`/`delete[]` in
one frame**, ~2,600 alloc/free pairs, in a 32-bit MSVC heap under Wine. The
plan already names this and declines it for want of evidence; frame 4311 is
that evidence's shape.

**Run 24 answered both instruments, and the answers were a deletion and a
reversal.** `findings.md` §23.

*The heap is innocent.* `operator new[]` and `delete[]` across all of
Engine.dll cost **173 ms a session and 3.6 ms on the 1,534.8 ms freeze frame**
— 7,324 allocations and 6,471 frees, 150 MiB of churn, for three and a half
milliseconds. Wine's heap is not slow. §21's candidate is gone and **pooling
the archive `File`'s scratch buffers is struck from the plan.**

*And I had the read/inflate split backwards.* I predicted syscalls on the
strength of §14–§17's 126–212 ms host round trips. Wrong:

| | session | per block | share |
| --- | ---: | ---: | ---: |
| whole block routine | 4,543 ms | 594 µs | 100% |
| **zlib `uncompress`** | **3,494 ms** | **457 µs** | **76.9%** |
| `ReadFile` | 1,043 ms | 136 µs | 23.0% |
| `SetFilePointerEx` | 5 ms | 1 µs | 0.1% |

`engine_io_read` counted 7,761 against `engine_arc_blocks`' 7,646, which is the
attribution check. File I/O under CrossOver is **not** the pathology the
message pump is: the seek is free, and `ReadFile` moved 626 MiB in 1,043 ms
(~600 MB/s), so it pays for bytes rather than round trips.

- **4.2, the bounded prefetch, is dead and struck.** Its pitch was 86 syscall
  pairs per texture; the syscall half is worth 5 ms a session, batching moves
  the same bytes, and 4.2 deliberately over-reads so it would move more.
- **4.3, libdeflate, is the only archive item left with a number**: 3,494 ms a
  session, 189 ms on the freeze frame, and libdeflate is typically 2–3× faster.
  Still the riskiest item in the plan, but no longer a coin flip.

## Run 24 left the frame more unexplained, not less

```
frame 1906, 1,534.8 ms
  engine_render                1,529.0 ms   99.6%
    |- Region::LoadLevel         505.7 ms   (100% main thread)
    |    |- LoadResource         404.7 ms
    |         |- read + inflate  311.2 ms   (122 read, 189 zlib)
    |- texture_create             23.4 ms
    |- heap alloc + free           3.6 ms
    +- STILL UNNAMED             996.4 ms   64.9%
```

A 908 ms frame from the same run has `Region::LoadLevel` at **0.01 ms** and
still carries 758 ms unnamed — so the missing time is not a side effect of
level loading.

**The obvious unmeasured thing: every instrument here times the main thread
*doing* something. Nothing has ever timed it waiting.** The region lock read
zero contention at three call sites; the fence 1.6 ms at one. **Engine.dll's
archive lock `archive+0x60` is held across every one of 7,646 block reads a
session — 136 µs of `ReadFile` each — and has never been instrumented.** If
the render thread force-loads a level inside that window it blocks, invisibly.
Right shape, roughly right size.

**Runs 25 and 26 closed the waiting question.** `findings.md` §24 and §25.

*Locks are innocent at whole-module scope.* `engine_cs_wait` on run 25's
freeze frame: 0 contended acquisitions, 0.00 ms; 222 ms a session across every
critical section in Engine.dll, the archive's own `archive+0x60` included.
§8's per-site region-lock zero now generalises: **lock contention is not a
mechanism in this game on this machine.**

*Run 25's waits/sleeps were built without a `_main` split* and read larger
than wall clock (183 s of object wait in a 96 s session) because they summed
across every thread. My error; §24 records it. Run 26 added the split.

*The main thread does poll, and it is the game's own loop.*

| | all threads | **main thread** |
| --- | ---: | ---: |
| `engine_obj_wait` | 183,036 ms | **158 ms** |
| `engine_sleep` | 164,060 ms | **518 ms** |

```
frame 1911, 1,376.9 ms
  engine_render                1,371.1 ms
    |- Region::LoadLevel         512.6 ms   (100% main thread)
    |- main-thread Sleep         434.9 ms   350 calls, 350 ms requested
    |- texture_create             23.5 ms
    |- heap / locks / waits        3.6 ms / 0.0 / 0.0
```

**350 `Sleep(1)` calls on the main thread in one frame** — 84% of the whole
session's main-thread sleeping. The forced level load is 85% *waiting*.

*The granularity lever is small.* 418 calls asked 418 ms and got 518 ms —
**1.24×**, not the 2–15× §24 hoped for. `timeBeginPeriod` (a `winmm` export,
in the library this mod *is*) would recover ~100 ms a session and ~85 ms of
the freeze frame. Nearly free, worth a switch eventually, not the fix.

## The finding that reorganises the plan: the freeze is three mechanisms

Run 26's three worst frames have almost nothing in common:

| | frame 1911 | frame 3168 | frame 6914 |
| --- | ---: | ---: | ---: |
| frame | 1,376.9 ms | 1,113.3 ms | 437.2 ms |
| `engine_render` | 1,371.1 ms | 1,040.3 ms | **52.5 ms** |
| `Region::LoadLevel` main | 512.6 ms | **0.02 ms** | 0.00 ms |
| main-thread `Sleep` | **434.9 ms** | 0.00 ms | 0.00 ms |
| main-thread `EnterCriticalSection` | 0.00 ms | **50.2 ms** | 0.00 ms |
| main-thread object wait | 0.00 ms | 0.05 ms | **56.2 ms** |
| `texture_create` | 23.5 ms | **193.3 ms** | 1.9 ms |
| `arc_open` | 1,299 | 199 | — |

- **1911 — the zone transition.** Forced synchronous load, 85% a `Sleep(1)`
  poll. **Stage 5.1 addresses this one and only this one.**
- **3168 — a render hitch with no level load at all** (0.02 ms) and still
  1,040 ms in `Engine::Render`: 193 ms texture creation, 276 ms inflate,
  50 ms main-thread lock contention, ~790 ms unaccounted. Its own mechanism,
  its own instrument needed.
- **6914 — not in `Engine::Render` at all** (52.5 of 437 ms). The §13–§17
  message-pump class. Closed as a host question.

**"The worst frame" has been three different frames wearing the same number**,
and every attribution in §21–§24 was computed against whichever happened to be
slowest that run. Any future claim about it must say which class it means.

## A pattern in my own errors, worth naming

§21 predicted heap churn. §23 predicted syscalls over zlib. §23 predicted lock
contention. §24 predicted sleep granularity. **All four were mechanisms
verified in the disassembly, plausible in shape, and wrong about magnitude.**
Each cost one boot to kill because the instrument went in before the fix. That
discipline is the reason to keep resisting the urge to build first.

## The order of work, as it now stands

**Read findings.md §34 first.** A fresh-eyes review of all nineteen runs, once
`game_collisions` gave a reliable marker for "the player is in the world",
found that the frame this project has been chasing since run 21 is the menu's
load-game, that the real in-game stutter was mis-anatomised, and that there is
a five-second frame nobody has ever looked at. §31 and §33 are the background.

1. **Class B — the in-game stutter — and it already has an answer waiting.**
   578–1,938 ms, ~1,000–1,600 frames after the player enters the world,
   reproduced in 16 of 19 runs, `Region::LoadLevel` 0.0 ms on every one.
   **`engine_res_load_main_us` names 33–68% of `Engine::Render` on it (median
   44%)** — the main thread synchronously running
   `ResourceLoader::LoadResource`, with the archive inflate and texture
   creation nested inside. The column has existed since run 10; §25 left it
   out of the frame-3168 table and called the remainder unaccounted. **Start
   by re-reading these frames, not by building an instrument.**
2. **Class C — the five-second frame.** Runs 16 and 33 each hold one in-game
   frame of 5,016.9 / 5,024.9 ms, `engine_render` 5,007.5 / 5,007.9 ms, with
   nothing else named at all. Two runs within 0.4 ms of 5.007 s is a timeout,
   not a coincidence. Largest event in the dataset and never investigated.
3. **`tools/frames.py`: split the session at world entry.** First frame with
   `game_collisions` non-zero. Menu is 25–47% of every session and has been in
   every p50, p99 and "mod's share" ever quoted; in-game p50 is 12.1–13.4 ms
   against the 8.3–10.0 ms reported. Menu length also varied 1,719–5,454
   frames between runs while in-game frames stayed near-constant, so
   run-to-run comparisons were differencing a varying amount of menu.
4. **4.3 (libdeflate)** — now re-founded on class B rather than on a session
   total: 106–366 ms of inflate sits *inside* the main-thread resource load on
   the frame the player actually feels. Still the riskiest item in the plan.
5. **`timeBeginPeriod` behind a switch** — ~100 ms a session, and 85 ms of it
   is on class A, which is a loading screen. Nearly free, nearly pointless.
6. ~~**Stage 5 / `async_level_load`**~~ — **finished and measured out (§33).**
   All three synchronous `Region::LoadLevel` sites found, attributed, priced;
   4,191 calls through them in run 33 and zero deferrals. Traversal was
   already asynchronous all along. The switch stays: verified, inert, default
   `0`.
7. **The instruments stay and are the reason any of this was settled.**
   `hookLoadLevel` / `hookGuaranteedGetLevel` record slow callers; the stack
   scan names the whole path across `Engine.dll`, `Game.dll` and `TQ.exe`.
   Decode the call in front of each return address rather than trusting
   nearest-export names; treat a repeated group as stale, with the per-frame
   CSV columns as the arbiter.
8. ~~4.2, bounded prefetch~~ — **struck**, run 24.
9. ~~Pooling the archive scratch buffers~~ — **struck**, run 24.
10. ~~The archive block cache beyond 8 MiB~~ — **struck**, run 22.

**Deferred at the reporter's request:** `cache/runs/play-with-cache-verify.ini`
and `play-with-cache.ini` — the long-play validation and then serving. To be
done once the project is otherwise finished, since hour-long sessions are a
poor fit for the measure-fix loop. `archive_cache_mb` stays at `0` until then.

**Stage 5 is no longer parked, and the reason it was parked has been
answered.** The old objection was that exactly one frame in 7,347 has a
`Region::LoadLevel` costing over a millisecond, so it fixes the worst frame of
a session and nothing else. That is still true — and after six more runs it is
now the *best* remaining trade in the plan, because everything else has been
measured smaller: the cache is worth 140 ms, `timeBeginPeriod` 85 ms, and
libdeflate ~110 ms on the same frame, against 5.1's ~513 ms. "The worst frame
of a session" is also the only frame the reporter actually notices.

Run 26 added what was not known when it was parked: **the forced load is 85%
a `Sleep(1)` poll loop**, so it is mostly waiting, not working — which both
explains why it is so expensive and predicts that the pop-in will be brief.

## Stage 5.1, built and run. The design record, and where it was wrong

**Built behind `[performance] async_level_load`, default `0`, installing
nothing at `0`.** Every byte below was re-read against the pinned `Engine.dll`
and held, with the corrections marked; findings.md §26 records what
re-verification added.

**Then run 28 measured it and the target was wrong.** The two
`AddElementsInBox` sites never force a load — 2,849 calls, 2,849 already
resident — and on the zone-transition frame they are not called at all. Read
**§27 before this section**: everything below is sound about the mechanism and
wrong about where the cost is.

### The two call sites

Both are byte-identical apart from the call displacement, read out of the
pinned image (not the audit export):

```
0x10167847  GraphicsDeferredRendererX::AddElementsInBox
0x1017d8b7  GraphicsForwardRenderer::AddElementsInBox

  85 ff                  test edi,edi
  0f 84 dd 00 00 00      jz epilogue
  6a 00                  push 0            the `false` flag both sites pass
  8b cf                  mov ecx,edi       Region*
  e8 <rel32>             call Region::LoadLevel        <- offset 12
  80 7f 74 00            cmp byte [edi+0x74],0
  c7 47 6c 00 00 00 00   mov dword [edi+0x6c],0        unload countdown
  0f 85 <rel32>          jnz epilogue      region skipped while loading
```

**34 bytes** — the count above stops before the trailing `JNZ`'s displacement
— call at **offset 12**. Displacements: `e8 68 46 0a 00` (deferred) and
`e8 f8 e5 08 00` (forward); both resolve to `0x1020bec0`,
`?LoadLevel@Region@GAME@@QAE_N_N@Z`. Each window ends exactly where that
renderer's `kLockSites` window begins, which checks both tables at once. `detour::patchCall` handles `E8` sites by
rewriting the displacement, and `expectedTarget` is the safety check.

`MOV` does not touch flags, so `mov dword [edi+0x6c],0` runs on the skip path
too: a region deferred rather than loaded still has its unload countdown reset
and cannot be evicted while the load is in flight.

### The asynchronous entry point, and the one trap in it

`?BackgroundLoadLevel@Region@GAME@@QAEX_N0@Z`, RVA `0x20be60`, 85 bytes,
`__thiscall`, returns void, two bools, `RET 0x8`:

```
1020be60  8b 41 50     mov eax,[ecx+0x50]
1020be63  8a 54 24 04  mov dl,[esp+4]        only the FIRST bool is read
1020be6a  85 c0        test eax,eax
1020be6c  74 0d        jz  proceed
1020be6e  84 d2        test dl,dl
1020be70  74 3d        jz  EPILOGUE          <-- does nothing and returns
1020be72  80 b8 3d 6a 00 00 00  cmp byte [eax+0x6a3d],0
1020be79  75 34        jnz EPILOGUE
1020be7b  80 79 74 00  cmp byte [ecx+0x74],0   already loading? bail
1020be81  80 79 75 00  cmp byte [ecx+0x75],0
1020be87  83 79 50 00  cmp dword [ecx+0x50],0
1020be8d  c6 41 75 01  mov byte [ecx+0x75],1   ... or
1020be93  c6 41 74 01  mov byte [ecx+0x74],1
             then queues the work (call 0x10051fc0 / 0x10051660)
1020beb2  c2 08 00     ret 0x8
```

Two things follow, and **the first is the trap**:

1. **With `region[0x50]` non-null and a `false` flag — exactly what both call
   sites pass — this function returns having done nothing.** It does not set
   `[0x74]`, so the caller's `JNZ epilogue` would not fire and the renderer
   would draw an unloaded region. **Those must go to the original
   `Region::LoadLevel`.** This is why the plan's §5.1 says "if `region[0x50]
   != 0` call the original unchanged"; the disassembly is the reason.
2. **It guards its own re-entry** via `[0x74]` and `[0x75]`, so the thunk
   needs no in-flight check of its own — the plan's suggested
   `region[0x74] | [0x75] | [0x78]` test is redundant.

### The thunk

Same ABI as `Region::LoadLevel`: `__thiscall(bool)` → GCC `__fastcall` with a
dead `edx` argument, one stack argument, callee-pop. That is the file's
existing `LoadLevelFn` typedef.

```
int __fastcall hookAddElementsLoadLevel(void* region, void* edx, int flag) {
    if (!g_asyncLevelLoad || !g_backgroundLoadLevel
        || *(void* const*)((BYTE*)region + 0x50) != nullptr) {
        count(CounterEngineAsyncSync);
        return g_regionLoadLevel(region, edx, flag);   // the export
    }
    g_backgroundLoadLevel(region, edx, 0, 0);
    count(CounterEngineAsyncLoad);
    return 1;
}
```

Built as written, with one change: the flag is **forwarded** rather than
passed as a literal `0`. Both sites push `0` — it is in the byte tables — so
the two are the same call today; forwarding keeps that a fact about the sites
rather than an assumption baked into the thunk.

Call the resolved **export address** rather than the trace's trampoline, so
the thunk works with the probe off; if the trace *is* installed the call still
lands in `hookLoadLevel` and is counted, which is what we want. `patchCall`
retargets the call site, not the function, so there is no recursion. Reading
`region+0x50` needs no guard — the engine reads it unconditionally at the top
of both functions.

### The switch, and the fourth way into `install()`

`[performance] async_level_load = 0` (off, the default). Like
`archive_cache_mb` it is a fix rather than an instrument, so it must reach
`install()` with the performance probe off:

```
const bool cache = tq::arccache::configured();
const bool async = g_asyncLevelLoad;
decideTracing();
if (!g_tracing && !cache && !async) return false;
...
if (async) installAsyncLoad(engine);
```

`wants()` already refuses every trace group when `g_tracing` is false — that
gate was added this session and the self-test covers it, so a cache-only or
async-only boot installs no instrumentation.

Two counters, so a run can tell the switch engaged: `engine_async_load`
(regions deferred) and `engine_async_sync` (fell through because
`region[0x50]` was non-null).

### What it is worth, and what it is not

**~513 ms of a ~1,380 ms frame, on the zone-transition class only** (§25's
frame 1911). Nothing for the no-load render hitch or the pump class.

**The pop-in should be brief**, and that is a measurement rather than a hope:
run 26 found the forced load is 85% a `Sleep(1)` poll — 435 ms of waiting in a
513 ms call — so the loader thread is nearly finished by the time the renderer
is told to skip the region.

### Also to do — all done

- `verify-sites.py`: done, and further than asked. Both call-site windows,
  both `BackgroundLoadLevel` behaviour windows plus its epilogue, `E8` at
  offset 12 with its displacement re-derived onto `Region::LoadLevel`, the
  export RVA, the owner exports, the window adjacency, and the offset the
  renderer skips on asserted equal to the offset the async path raises. 120
  checks; every constant perturbation fails it.
- `README.md`: done, including a correction — the "gated twice" paragraph
  claimed nothing installs without the probe, which stopped being true when
  `archive_cache_mb` landed.
- `test/selftest.cpp`: done, eight assertions.
- Two run inis: `cache/runs/run27-async-baseline.ini` and
  `run28-async-level-load.ini`, both diffed against the live file. Three
  differences from live each (`trace`, `performance_trace`,
  `async_level_load`); the two differ from each other only in
  `async_level_load`. Run 28's header names the pop-in as the thing to
  watch.

## Conventions

- Never launch the game. The reporter runs it from the CrossOver UI.
- A run ini goes in `cache/runs/`, built from the live `tqflicker.ini` beside
  `TQ.exe` with only the variable under test changed, and is diffed against it
  before the run.
- `npm run install-dll` installs the built `winmm.dll`; verify with `cmp`.
- Every Engine.dll patch verifies its bytes and its resolved target, installs
  nothing on mismatch, and is restored in `shutdown()`.
- Every game-behaviour change gets its own `tqflicker.ini` switch, defaulting
  off. `loose_texture_max` is the one exception the reporter runs on
  deliberately, at `4096`.
- Re-verify bytes against the pinned binaries before writing them, even when
  a document already records them. `55 8b ec 83 e4 f8` is shared by four of
  the targets, so a six-byte prologue match proves nothing: verify 16–24
  bytes, steal 6–7. And re-verify *structure offsets* the same way: read the
  operand that uses the field, not a layout table. `verify-sites.py` now does
  both, and it is the thing to extend when a table is added.
- Engine duration columns end in `_us`, never `_ms`, or `tools/frames.py`
  charges the game's time to the mod.
- Prefer `detour::patchImport` to patching code. Twelve of the fourteen
  instruments added after `b161e08` are import-table entries: scoped to one
  module, four bytes, exactly restorable, and they cannot be wrong about an
  instruction boundary.
- **The game exits without unloading.** `fix.cpp` takes the `reserved` branch
  at `DLL_PROCESS_DETACH` and calls only `probe::flushOnExit()`, so
  `visual::shutdown()` and `engineprobe::shutdown()` never run in practice.
  Anything that must be reported has to be written during the session — the
  message histogram writes from the `Engine::Render` bracket every 1,800
  frames for exactly this reason.
