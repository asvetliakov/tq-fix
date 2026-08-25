# Titan Quest Flicker

Titan Quest Anniversary Edition runs well under CrossOver on Apple Silicon — at
full speed, on the DXMT backend — **except that things which move flicker**:
shadows, light and FX effects, and some character geometry. Static scenery is
fine.

This repo is the investigation into why, and the fix if one is reachable: a
32-bit shim loaded into `TQ.exe` that corrects what the D3D11→Metal translation
cannot express.

**It is not a mod.** It changes nothing about the game, its saves or its balance.
It sits between the game and the graphics translation layer and adjusts one
sampler description on the way past.

## Status

**Stage 0.** Two defects identified and separated; no code written yet. The
free experiments in `docs/plans/stage-0-free-experiments.md` come first, because
one of them — running the game's own DirectX 9 renderer instead — could make the
whole thing unnecessary.

## Where to start

`CLAUDE.md`, then `RUNBOOK.md`, then `docs/rev/`. The notebook in `docs/rev/` is
the point of the project: most of what is known here cost a game launch to learn,
and none of it is visible in a diff.

## Requires

macOS on Apple Silicon, CrossOver (Preview 27.0.0 is what this was developed
against), Titan Quest Anniversary Edition, and `i686-w64-mingw32-g++` for the
DLL. `npm run doctor` will tell you what is missing.
