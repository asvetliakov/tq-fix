# Where the stutter work stands

Companion to `game-stutter-mitigation.md`, which is the plan. That document's
"Status" section records where the plan was wrong; this one records what is
built, what is measured, and what to do next. Current as of **run 45**, with
the invalid thread-CPU instrument removed and the corrected full-trace writer
validated by its control run, on branch `stutter-mitigation`.

---

## READ THIS FIRST: the brief for the next session

**Run 45 result -- read findings §46 first.** With one retained CSV handle and
250 ms batches, the old slow-Peek population disappeared: zero frames reached
50 ms in `PeekMessageA` anywhere in the run, and collision-active full-scene
play made 4,994 Peek calls with a 3.671 ms maximum per frame. Before the CSV
was read, the reporter said the frequent micro-stutters felt gone and only the
loading burst-stutter remained. Both F12 presses are in play and follow the
outdoor-transition render/resource/GPU burst. The broken full-trace writer was
therefore the source of the measured CrossOver pump class. This validates the
observer fix, not a general explanation of Titan Quest's historical
native-Windows stutter.

**Run 44 correction -- read findings §45 before using the brief below.** The
external sample found that `performance_trace=full` was manufacturing or
amplifying the pump tail: its worker opened, appended, and closed the CSV after
almost every frame, while those file opens and the main thread's Peek calls
shared wineserver. The prior claim that runs 41-44 isolated the normal game's
frequent felt class is withdrawn. Their F12 presses are real reactions, but
they happened with the broken observer active. Titan Quest's native-Windows
stutter remains unisolated.

**Run 43 correction -- read findings §44 before using the older history.** The
F12 reaction marker found 15 felt events in play, but its first implementation
called `GetAsyncKeyState` once per frame and probably created most of a new
unbracketed stall class. Run 40 is withdrawn as a baseline. The marker now
passively recognizes F12 in the game's existing `PeekMessageA` result. Run 41
confirmed the observer effect and then tied 7 of 8 captured felt events to
166-209 ms pump stalls; the eighth was the outdoor-transition render burst.
Run 42 tied another 9 of 13 likely reactions to pump stalls, then rejected its
sent-message hypothesis: only 14.4 ms of 1,322.8 ms in those events ran inside
sent window procedures. Run 43 then showed that adding `GetThreadTimes` around
each peek moves the stall into that new CrossOver server call: eight of nine
markers followed its 111-219 ms query stalls and no peek reached 50 ms. The
CPU instrument is withdrawn. Do not interpret run 40's apparent 10 residual /
4 pump / 1 outdoor-transition split as the normal game.

**The reporter has asked for a fresh-eyes review of everything, and the request
is well founded.** Over runs 34-39 this project made a correct measurement and
then attributed it wrongly three times in a row, and the reporter corrected it
each time from their own experience of playing the game:

| I claimed | they said | who was right |
| --- | --- | --- |
| the five-second frame is the largest in-play event (§34) | "I don't remember a 5-second stutter in game" | **them** -- it is in the loading screen (§35) |
| `draw_submit` is a per-draw first-use cost in the driver (§36) | "this is not a host issue, TQ stutters on real Windows too" | **them** -- it is GPU backpressure (§37) |
| shadow settings are the lever | "you are chasing the wrong variable, and shadow_split is a necessary option" | **them** -- §40 |
| the remaining stalls are CrossOver's event path (§17, §40) | "I don't have Windows to test and I don't think CrossOver matters; the game stutters on Windows without the mod" | **unresolved, and probably them** |

**Three of the four framings in §34-§40 were wrong, and every one of them was
overturned by a sentence from the person playing the game rather than by the
next measurement.** That is the single most important fact for a fresh reader,
and it should shape how much weight the documents below get relative to the
CSVs. Sections are corrected in place with pointers forward; do not read any
section without checking whether a later one overturns it.

**The specific thing to re-derive from scratch:** nobody has ever isolated the
stutter the reporter actually feels. What has been isolated is:

- a **loading pause** of 11.5-19.2 s once a session (classes A, A2, A3) --
  the game's, not felt as a stutter;
- an **outdoor-transition frame** of 290-439 ms plus a 120-218 ms successor,
  once or twice a session -- about half the game's synchronous resource load
  and about half GPU backpressure from **the mod's own** enhanced shadows and
  grass;
- **`PeekMessageA` stalls** of 60-460 ms, 7-21 a minute, 1.3-3.2 seconds per
  minute of play, which vary 3x between identical runs, respond to nothing
  tried, and whose attribution the reporter's Windows testimony contradicts.

The third had the right *frequency* to be a stutter someone complains about
after every run, but §45 identifies that frequency as observer-contaminated
and §46's corrected-writer control removes it. **Run 45 leaves the
outdoor-transition loading/render burst as the only felt class on this route.**

### Three concrete things nobody has tried

1. **Before run 40, nobody had asked the reporter *when* it stutters.** Every claim in
   this project about "the stutter" is inferred from `max()` over a CSV, and
   §34 and §35 are two separate occasions where that inference picked a frame
   the reporter was not even looking at the game for. The mod owns the message
   pump and already has a hotkey path (`bloom_toggle`). **A keypress that
   writes a marker column into the frame record ties a felt stutter to a frame
   index for the first time. **This is built, but the first polling version
   perturbed run 40; use only the passive event-based version. Run 45 validates
   that version with the corrected writer.**
2. **`PeekMessageA` runs inter-thread `SendMessage` handlers inline, on
   Windows as much as under Wine.** A blocking `PeekMessage` that returns
   *nothing* -- run 39 frames 3721 and 5289, 154 and 185 ms with
   `pump_peek_miss` of 1 -- is exactly the shape of the pump dispatching a
   message another thread sent to the main window and waiting for the window
   procedure. That mechanism is platform-independent, which fits the
   reporter's Windows testimony where "CrossOver's event path" does not. It
   has never been looked at; `hookDispatchMessage` only sees `DispatchMessageA`,
   which is not the path a sent message takes.
3. **The apparent 3x stall rate is now explained.** Full tracing generated one
   file-open request per emitted frame and one main-thread empty Peek per frame.
   Run 35's faster visuals-off path increased both exposures. Across eight
   runs the 7-22 slow-Peek counts fit one common per-peek probability, while
   arrivals cluster within the route. A one-minute threshold count was never a
   stable effect estimate; §45 has the full calculation.

---

**After that brief, start at "The order of work" near the end.** Everything
between here and it is history and should be read as such.

## Read these first

1. `research/streaming/findings.md` **§46, §45, §44, §43, §42, §41** (the
   corrected-writer control, the full-trace writer defect, the migrating
   CrossOver server-call stall, the rejected sent-message cause, the
   now-contaminated felt-stutter runs, and the withdrawn polling-marker run),
   then **§40** (the pump, closed as a
   lever twice and with its attribution withdrawn), **§39, §38, §37** (the shadow series
   and the correction to §36), **§36** (marked withdrawn in place), **§35**
   (the corrected world-entry marker and the four-part session). Then **§34**,
   which §35 corrects, **§31 and §33** for how Stage 5 ended, and **§25** for
   the three-class split all of them correct.
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

**This section is the most-rewritten thing in the file and has been wrong at
every version.** Read the brief at the top before it.

1. **The current felt classes are finally separated.** Run 45 removed the
   observer defect; the reporter felt no frequent micro-stutters, the old
   slow-Peek tail vanished, and both remaining F12 reactions followed the
   outdoor-transition render/resource/GPU burst. Never restore run 40's
   per-frame `GetAsyncKeyState`; it manufactured a second residual class.
2. **Do not run another pump A/B.** The retained-handle, 250 ms batched writer
   is now required measurement infrastructure. If frequent micro-stutters
   recur with that writer, capture them with passive F12 and treat them as a
   new observation; do not revive runs 41-44's contaminated attribution.
3. **The apparent 3x pump-event count is explained.** It combined per-frame
   writer and Peek exposure with only 7-22 clustered threshold crossings.
   Across eight runs the counts fit one common slow-event probability per
   Peek. Do not use the old per-minute count to resolve a modest route A/B.
4. **Inline inter-thread `SendMessage` is rejected for the contaminated pump
   class.** Run 42's likely felt pump
   events spend 1,322.8 ms inside `PeekMessageA`, of which only 14.4 ms is in
   sent window procedures. The hooks armed and observed 5,484 calls, so this
   is not a missing-instrument zero.
5. **Do not repeat run 44's coarse sampler as a route A/B.** It condensed away
   time order and independently suspended the game task and ARM64 wineserver.
   Its structural writer finding is valid without event alignment; any future
   external stack trace needs a common timeline, such as one
   `spindump -timeline -onlyTarget -proc` capture, and only for a newly
   reproducible class under the corrected writer.
6. **The remaining felt class is the outdoor-transition burst.** Run 45's two
   marked bursts contain 164.4 and 71.7 ms of main-thread resource loads,
   while their resolved GPU intervals contain 237.0 and 125.4 ms of
   directional shadows. These overlap; do not add them. This is the same
   two-axis class separated in §§37-39, not a pump event.
7. **The mod's own GPU cost — the one lever this project owns, and it works.**
   Enhanced shadows are 8.09 ms and grass 4.47 ms of a 25.4 ms steady GPU
   frame at 5120x1440, and the outdoor transition is 421 + 182 ms of which the
   directional shadow pass is 351.6 ms. `shadow_map_scale=2` takes the
   transition to 350 + 144 and the steady frame to 24.4 ms **without touching
   shadow distance** (§38). `shadow_split=0.325` is worth far more (263 + 86)
   and is **refused**: it exists to fix shadow distance, which is the feature
   (§39, the reporter's call). Grass has never been priced on its own.
8. **The game's synchronous resource load**, 147-336 ms on the transition
   frame, inside `Engine::Render` on the main thread. Real, the game's, and
   nothing short of the archive work already measured out touches it.
9. ~~4.3, libdeflate~~ — worth 35-50 ms on a 340 ms frame (§35). The riskiest
   item in the plan for the smallest remaining return. **Struck** unless the
   loading screen becomes the target.
10. ~~`timeBeginPeriod`~~ — **struck**. Main-thread `Sleep` is 0.0 ms on every
   in-play stutter frame in nineteen runs.
11. ~~Stage 5 / `async_level_load`~~ — finished and measured out, §33.
12. ~~4.2 bounded prefetch, buffer pooling, the block cache past 8 MiB~~ —
   struck, runs 22 and 24.

**Switches that are built, verified, inert and worth nothing on this route.**
All default `0`, all reach `install()` with the probe off, none bring a trace
group: `archive_cache_mb` (§20), `async_level_load` (§33),
`pump_timer_min_ms` (§40). They cost nothing and they are correct; none of them
is a fix for anything measured here.

**Instruments added this session.** `[debug] draw_timing` adds `draw_submit_ms`
and `map_resource_ms` around the game's own `Draw`/`DrawIndexed` and `Map` --
the driver call only. It is what made the residual visible and then what
disproved §36's reading of it. In-play p50 is unchanged with it on (13.7 ms
against a 13.5-14.3 ms band), so it is affordable. `tools/frames.py` now splits
the session into menu / loading screen / play on `draw_indexed >= 500` and
reports the two D3D columns as the game's time, not the mod's.
The passive `stutter_marker` recognizes F12 in the existing message stream;
the sent-window-procedure columns are a probe-only causal split. Run 43's
thread-CPU columns are withdrawn because their queries moved the stall. Run
44 then found that the CSV writer itself was an observer: it now retains one
session handle and flushes ordinary rows in 250 ms batches. Run 45 validates
that correction: no 50 ms Peek frames and no reported frequent micro-stutters.

**Two traps this project has fallen into twice each, both now documented.**
`game_collisions` is not world entry (§35). Whole-session p50 -- and then whole
*in-play* p50 -- are not comparable between runs, because the menu share and
then the indoor/outdoor share both vary with how the route is walked (§34,
§39). Compare full-scene in-play frames under 60 ms, or compare nothing.

## State a new session needs, beyond the repo

- **`cache/` is gitignored but present on the reporter's machine.**
  `cache/runs/` holds every run ini, each with its reasoning in the header;
  `run34`-`run39` are this session's and their headers carry the argument for
  each experiment and, where it failed, what it disproved.
  `cache/runs/live-config.ini` is the reporter's normal `tqflicker.ini`, which
  every run ini is built from and diffed against, and which restores the
  playable configuration:
  `cp cache/runs/live-config.ini "$GAME/tqflicker.ini"`.
- **The CSVs live in the game directory** as
  `tqflicker-frames.runN.csv`, runs 9-45, plus `tqflicker-debug.runN.log` for
  runs 9-33. **Runs 34-39 have no debug log** because they ran with `trace=0`;
  if the message histogram or the slow-caller tables are wanted, a run needs
  `[debug] trace=1`.
- **Runs 34-45 are the only ones with `draw_submit_ms` / `map_resource_ms`,**
  only run 39 has `pump_timer_full` / `pump_timer_split`, runs 40-45 have the
  F12 marker, run 42 has the sent-window-procedure split, and only run 43 has
  the withdrawn thread-CPU/query columns, run 44 has the external
  `sample` reports under `cache/samples/`, and run 45 is the corrected-writer
  control. Earlier CSVs are
  missing those columns off the end rather than shifted -- `tools/frames.py`
  and `csv.DictReader` handle it.
- **The route is scripted and identical in all twenty-five full runs**: menu,
  load-game, a 9-16 s loading screen, an outdoor stretch, an indoor stretch, an
  outdoor stretch, exit. World entry is the first frame with
  `draw_indexed >= 500`; the in-play transition stutter lands at
  play + 3,245-3,527 frames.
- **The reporter runs the game from the CrossOver UI. Never launch it.**
  Prepare a run ini in `cache/runs/`, diff it against `live-config.ini`,
  install it and the DLL, and clear any stale `tqflicker-frames.csv` so the run
  appends to nothing.
- **The pinned `Engine.dll` is SHA-256 `0aedbb18...f694f6`**;
  `research/streaming/tools/verify-sites.py` checks every byte table in
  `src/engine_probe.cpp` against it and the other two modules. **139 checks**,
  and every constant perturbation must fail it.
- **The installed `winmm.dll` is byte-identical to `build/winmm.dll`** and the
  live `tqflicker.ini` is the reporter's normal one, so the mod is inert as
  left.

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

- **Titan Quest stutters on Windows too, without this mod.** The reporter has
  said so directly, and it is the constraint that kills "it is CrossOver" as an
  explanation for anything felt. Any attribution that only works under Wine has
  to explain why the same complaint exists on native hardware before it is
  believed.
- **When the reporter says something about how the game behaves, weight it
  above the next measurement.** Four times in this session a sentence from them
  overturned a conclusion drawn from the CSVs, and the CSVs were not wrong --
  the readings of them were. See the brief at the top of this file.


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
