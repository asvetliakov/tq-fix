# Where the stutter work stands

Companion to `game-stutter-mitigation.md`, which is the plan. That document's
"Status" section records where the plan was wrong; this one records what is
built, what is measured, and what to do next. Both are current as of the Stage 3
work on branch `stutter-mitigation`.

## Read these first

1. `docs/plans/game-stutter-mitigation.md` — the plan, and its Status section,
   which corrects two of its five headline findings and records what Stage 3
   measured away.
2. `research/streaming/findings.md` — §4–§7 for the probe's blindness, the
   archive `File` class and the verified patch sites; **§8–§17 for everything
   Stage 3 measured**, which is where the current picture lives.
3. `research/streaming/arc-format.md` — the container format and what was
   checked across all 135 archives. R1 there is what Stage 4.1 is built on.

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

`cache/` is gitignored but present. Each run has `cache/runN-*.csv`, its log,
and the ini it was booted with in `cache/runs/`. The ini headers carry the
reasoning; they are worth reading before re-running anything.

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

## Next: Stage 4.1 — the only thing left with both a number and a fix

The archive block cache, and it is now the only thing left worth building.
Measured in two acts:

| | blocks | inflated | requested | amplification | inflate |
| --- | ---: | ---: | ---: | ---: | ---: |
| Eternal Embers | 7,491 | 1,917,696 KiB | 1,070,055 KiB | 1.8x | 4,418 ms |
| Greece | 4,560 | 1,167,360 KiB | 505,729 KiB | **2.3x** | 2,131 ms |

It attacks `Engine::Render`, which is the half that is ours, and the base game
wastes proportionally more than the expansion — which is what a one-entry
cache in front of a 2 GB file does when the reads are smaller.

The design is §4.1 of the plan and R1 of `arc-format.md`: an inline detour on
the block routine `FUN_1011d0e0` — already hooked and verified for
`engine_arc_blocks` / `engine_arc_inflate_us`, so the site needs no new
reverse engineering — keyed on `{archive, file handle, block offset,
compressed size, uncompressed size}`, a fixed slab in 256 KiB slots with a
clock victim, and a lock held only across lookup and insert, never across the
`ReadFile` or the inflate.

Two things the plan asks for that should not be dropped: `archive_cache_mb`
defaults to `0`, which allocates nothing and is byte-identical to today; and
`8verify` inflates anyway and `memcmp`s for one measurement boot, so the first
run *proves* the cache never returns a wrong block rather than asserting it.

Stage 5 stays parked: its premise is confirmed — `Region::LoadLevel` is 100%
main-thread and cost 505.7 ms in the worst frame — but exactly **one frame in
7,347** has a load costing over a millisecond, so it fixes the worst frame of
a session and nothing else. Stage 4.2 and 4.3 stay gated on what 4.1 leaves.

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
  bytes, steal 6–7.
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
