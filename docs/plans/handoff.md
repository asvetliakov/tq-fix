# Where the stutter work stands

Companion to `game-stutter-mitigation.md`, which is the plan. That document's
"Status" section records where the plan was wrong; this one records what is
built, what is measured, and what to do next. Both are current as of the Stage 3
work on branch `stutter-mitigation`.

## Read these first

1. `docs/plans/game-stutter-mitigation.md` — the plan, and its Status section,
   which corrects two of its five headline findings and records what Stage 3
   measured away.
2. `research/streaming/findings.md` §4–§7 — the probe's blindness, why the
   uploader must not take ownership, the archive `File` class, and the eight
   verified patch sites.
3. `research/streaming/arc-format.md` — the container format and what was
   checked across all 135 archives.

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

Verification is `npm run doctor && npm run build && npm run selftest`.

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

Seven instrumented runs (10-16), each one costing nothing measurable: p50, p99
and the mod's share never moved. `research/streaming/findings.md` §8-§14 has
the detail. Where the frame goes:

| | session | frames over 50 ms |
| --- | ---: | ---: |
| `Engine::Render` | 57.2% | 51.0% |
| `Engine::PresentSurface` | 21.6% | 5.2% |
| `Engine::Update` | 10.2% | 8.8% |
| **`PeekMessageA`** | **8.2%** | **20.3%** |
| unexplained | 2.3% | 12.2% |

**The two classes of hitch both have names now.**

*The big one* is the game's own loading, inside `Engine::Render`: the worst
frame is 1,453.8 ms of which 1,448.6 is render, with a forced main-thread
`Region::LoadLevel` of 505.7 ms and 264 ms of archive inflate under it.

*The second one* — unnamed since run 8, on frames that draw normally with the
mod idle — is **`PeekMessageA` on an empty queue**, 730 µs on average and up
to 212 ms, against 0.67 dispatched messages a frame. Under CrossOver that is a
wineserver round trip. It is not the game's and **not reachable from this
mod**; the lever is CrossOver's own synchronisation settings.

**Closed by measurement, not deferred**: the region lock is never contended
(0 hits), the seven sweeps cost 11.2 ms a session, the loader-fence wait 1.6
ms, `WaitForLoadingToFinish` is never called, `GameEngine::Update` is 0.3%,
the online platform poll 44 ms, sound 2 ms, address space never below 3,445
MiB. Stage 6.1, 6.2 and 6.3 can be deleted.

## Next: Stage 4.1, the last item with both a number and a fix

The archive block cache. 4.38 s a session across 7,527 inflates at 582 µs
each, inflating 1.88 GiB to serve 1.03 GiB — a 1.8x amplification that is the
single-slot block cache of R1 re-inflating what it already has. It attacks
`Engine::Render`, which is the half that is ours.

Stage 5 stays parked: its premise is confirmed — `Region::LoadLevel` is 100%
main-thread — but exactly **one frame in 7,347** has a load costing over a
millisecond, so it fixes the worst frame of a session and nothing else.

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
