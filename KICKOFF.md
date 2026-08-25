# Kickoff prompt

Paste this into a fresh Claude Code session opened in `/Users/asvetl/tq-flicker`.

---

Read `CLAUDE.md`, then `RUNBOOK.md`, then everything in `docs/rev/`. They were
written at the end of an investigation session and contain the facts this project
runs on — including several negative results that exist specifically so you do
not spend a game launch rediscovering them.

Context in one paragraph: Titan Quest Anniversary Edition runs at full speed under
CrossOver Preview 27 on an Apple M5 Pro, on the DXMT graphics backend, but
everything that moves flickers — shadows, FX and light effects, and some
character geometry. We have separated it into two independent defects. Defect A
is the shadow pass and is the majority of it; the prime suspect is that DXMT
cannot honour the game's sampler border colour of `-FLT_MAX`, which is the only
warning it emits all session. Defect B is a residual that survives turning
shadows off, seen on the FX in front of the resurrection shrine and on character
hair or clothes; its cause is unknown. No DXMT configuration setting addresses
either — there are only seven tunables and none is relevant. The intended fix is
`tqflicker.dll`, a 32-bit shim loaded through a `winmm.dll` proxy, that corrects
the sampler description on its way into DXMT.

**We are at Stage 0, and Stage 0 is not code.** It is four experiments that cost
one game launch each and no source changes, and one of them — running the game's
own DirectX 9 renderer with `/dx9`, which has never been tried on any backend —
could end the project outright. Work through
`docs/plans/stage-0-free-experiments.md` in order.

You cannot launch the game yourself. For each experiment, tell me exactly what to
change and what to look for, wait for me to run it and report back, then write
the result into `docs/rev/observed.md` before moving to the next one — including
the experiments that change nothing, which are the ones most worth recording.
You *can* read the bottle, the game directory and the DXMT logs directly; do
that rather than asking me to paste things.

Two standing rules from `CLAUDE.md` that matter most here: never guess a call's
semantics when you could observe it, and free experiments come before expensive
ones. If you find yourself about to write a DLL during Stage 0, stop.

When Stage 0 is finished, update the "Next session: paste this" block at the top
of `RUNBOOK.md`, close or update the risk-log rows, and commit.

---

## If you are resuming later

Do not paste the above again. `RUNBOOK.md`'s "Next session: paste this" block is
kept current at the end of every stage — use that instead. It is one line and it
is always right.
