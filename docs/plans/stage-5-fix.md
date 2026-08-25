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
