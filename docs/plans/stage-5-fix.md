# stage-5-fix

**Deliberately a stub.**

This plan cannot honestly be written yet — see `CLAUDE.md`. It replaces the
original `stage-4-fix-defect-a.md` and `stage-5-defect-b.md`, both of which were
deleted at the end of Stage 0:

- `stage-4-fix-defect-a.md` targeted **H-A**, the unrepresentable sampler border
  colour. **O10a refuted H-A** — the flicker does not move when shadow-map
  resolution moves the shadow frustum boundary, which that hypothesis required.
  The plan had no target left.
- `stage-5-defect-b.md` existed because O1 read the artefact as two defects.
  **O10b and O14 showed it is one** — a single per-object, single-frame draw
  failure with many victims. There is no separate Defect B to attack.

Write this plan once Stage 1's capture and Stage 4's draw counts have said
whether the game issues the missing draw. The fix for "the game never submitted
it" and the fix for "DXMT dropped it" have nothing in common, so committing to
either now would be guessing.

The gate is in `RUNBOOK.md`.

## Candidate, 2026-08-29 — reroute dynamic constant buffers (tests H-F)

From O37/H-F. All data-side; the device and context vtables are already held.

1. `hookCreateBuffer`: when `Usage == DYNAMIC && BindFlags & CONSTANT_BUFFER`,
   create it `DEFAULT` with `CPUAccessFlags = 0`, and register the returned
   `ID3D11Buffer*` → a shadow allocation of `ByteWidth` bytes in our heap.
2. `hookMap`: if the resource is registered, return the shadow pointer
   (`RowPitch = DepthPitch = ByteWidth`), `S_OK`, without calling DXMT.
   `WRITE_NO_OVERWRITE` returns the same pointer — the shadow keeps its
   contents, which is that flag's contract.
3. Hook `Unmap` (new slot, `gen-slots.sh`): if registered, call the real
   `UpdateSubresource(buffer, 0, nullptr, shadow, 0, 0)` instead of `Unmap`.
4. Log the count of rerouted buffers and maps per frame; `TQFLICKER_REROUTE=0`
   as the one-launch control.
5. Measure the O12/O14/O30 way, in the menu scene first (O36), then the shrine.

Extend to `DYNAMIC` vertex/index buffers only if constants alone do not move
the number. **Gate is unchanged:** a number, not an impression.


## Outcome — 2026-08-29

**Seventeen modes tried in one session** (`TQFLICKER_REROUTE`, O38–O46). The
candidate above (reroute to DEFAULT + UpdateSubresource) evolved into
**mode 4** (`DEFAULT|STREAM_OUTPUT`, no rename, GPU-blit updates), which
removes the flicker entirely and, at full speed, keeps every FX (O45). Cost:
3.6× on busy frames (O46) — DXMT splits the render encoder per update.
Shipped as `tqflicker.ini` (`reroute=`) beside TQ.exe, precedence
env > ini > compiled default. **The stage's formal gate (a 10fps-capped
60-second recording measured the O12 way, no dropouts) was NOT run** — the
reporter's eyes at full speed say no flicker; the measured proof is one
capped recording away if ever needed.

Rings, padding, barriers, latency, flush and alignment modes are all kept in
`frames.cpp` as documented dead ends — they are the bug report's evidence.
