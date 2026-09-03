# Where the stutter work stands

Companion to `game-stutter-mitigation.md`, which is the plan. That document's
"Status" section records where the plan was wrong; this one records what is
built, what is measured, and what to do next. Both are current as of the Stage 3
work on branch `stutter-mitigation`.

## Read these first

1. `docs/plans/game-stutter-mitigation.md` — the plan, and its Status section,
   which corrects two of its five headline findings.
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

## Stage 3 landed; run 10 measured it

`src/detour.{h,cpp}` and `src/engine_probe.{h,cpp}`, plus 27 `engine_*`
columns. Ten hook groups, all installed on the pinned build; the byte tables
are re-verified against `Engine.dll` and the whole thing costs nothing
measurable — p50, p99 and the mod's share are unchanged against run 8 despite
a detour on a function called 12.0 million times in the session.

`research/streaming/findings.md` §8 has the numbers and the plan's Status
section has what they change. In short:

- **Four hypotheses closed by measurement, not deferred**: the region lock is
  never contended (0 hits), the seven sweeps cost 11.2 ms a session, the
  loader fence wait costs 1.6 ms, and `WaitForLoadingToFinish` is never
  called. Stage 6.1, 6.2 and 6.3 can be deleted.
- **Stage 5's premise is confirmed but small**: `Region::LoadLevel` is 100%
  main-thread and cost 511 ms in the worst frame — but exactly one frame in
  7,347 has a load costing over a millisecond.
- **Stage 4.1 is well aimed**: 4.38 s a session in block inflates, 1.88 GiB
  inflated to serve 1.03 GiB.
- **2.4 has its number**: `upload_leased_mib` peaked at 1,064 MiB.
- **Two thirds of the hitch time is still dark**, including ~950 ms of the
  1,466 ms worst frame and the whole of the second class, whose frames show
  every engine column at zero.

## Run 11 closed the frame's accounting

`Engine::Update` and `Engine::Render`, bracketed whole. The session now adds
up to 100.2%: render 57.9%, Present 12.5%, update 10.3%, **outside everything
10.0%**, the mod 9.5%.

- **The worst frame is Stage 5's.** 1,453.8 ms, of which 1,448.6 is
  `Engine::Render` with `Region::LoadLevel` inside it. §1a confirmed.
- **`Engine::Render` is 46.9% of the hitch time.** Stages 4 and 5 are aimed
  correctly.
- **38.4% of the hitch time, and 18 of the 32 frames over 100 ms, is outside
  `Engine.dll` entirely** — on frames that draw normally with the mod idle,
  spending 100–225 ms where a normal frame spends 0.21 ms. Nothing in this
  plan would improve them.
- The mapping-lease hypothesis for those frames is tested and dead: the worst
  stall cluster held 18 MiB, the 1,024 MiB cluster had the least stall time.

## Run 12 ruled out the last candidate inside the game

`GameEngine::Update` is **307 ms of a 102,662 ms session — 0.3%**, 1.3% of
hitch time and **0.0%** of the time in frames over 100 ms. The stalls are not
the game's simulation.

It also corrected run 11's accounting: the game's `Present` is called
**outside** `Engine::Render` (median frame 2.00 ms render, 3.57 ms Present),
so the brackets really are disjoint and the frame decomposes exactly —
render 55.2%, Present 14.2%, **unexplained 11.6%**, update 9.6%, the mod
9.2%, `GameEngine::Update` 0.3%. In hitching frames the residual is 40.9%
over 50 ms and 44.7% over 100 ms.

The residual is not a steady tax: median 0.12–0.61 ms on ordinary frames, and
twenty discrete events of 50–398 ms arriving in **bursts**.

## Next: run 13, and the instrument that patches nothing

TQ.exe imports `Sleep`, `WaitForSingleObject`, `GetMessageA` and **no
`PeekMessage`** — so its pump is the blocking one — and it imports
`?NeedsSleep@GameEngine@GAME@@QBE_NXZ` beside `Sleep`, so it is a frame
limiter. All three are now timed through **TQ.exe's import table**, which
patches no code and is scoped to that one module.

`loop_sleep_req_us` against `loop_sleep_us` is the pair that decides it: a
loop that asks for a millisecond and is handed two hundred is an environment
problem, not a game one, and the fix would be mod-side or a CrossOver setting
rather than anything in Stages 4–6. `proc_avail_va_mib` rides along free and
closes the address-space question for good.

Ini: `cache/runs/run13-main-loop.ini`; settings identical to runs 10–12.

## Superseded: run 12

Answered above. Stage 4.1 remains safe to write in parallel on its own
evidence; Stage 5's premise is confirmed but it fixes one frame a session, so
it waits on where the residual lands.

## The old Stage 3 notes

Instrument Engine.dll itself. Everything so far has been vtable slots and
mod-side code; Stage 3 writes into `.text` on paths that run thousands of times
a second, so it is a real step up in exposure. The plan's §3.1/§3.2 have the
design and the mitigations. Points worth carrying in:

- The eight patch sites in `findings.md` §7 were re-verified byte-for-byte
  against the pinned `Engine.dll` at the start of this work. Re-verify anyway.
- `55 8b ec 83 e4 f8` is shared by `Region::LoadLevel`,
  `Archive::ReadFromFile`, `FUN_1011d0e0` and
  `Region::GetEntitiesInFrustum`. **A six-byte prologue match proves nothing.**
  Verify 16–24 bytes, steal 6–7.
- Engine duration columns are `_us`, never `_ms`, or `tools/frames.py` charges
  them to the mod.
- What Stage 3 has to explain: ~7–8.7 s per session of hitch time in no mod
  column, a reproducible ~1,400–1,500 ms frame carrying ~1,300 archive opens,
  and a *second* class the archive story does not cover — run 9's frames 1327
  and 5726, 256 ms and 228 ms, with no archive opens and nothing named at all.

Stages 4 (archive read path) and 5 (asynchronous level loads) remain gated on
what Stage 3 measures.

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
