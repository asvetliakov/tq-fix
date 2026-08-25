# Runbook

## Next session: paste this

> Read `CLAUDE.md`, then `RUNBOOK.md`, then `docs/rev/`. We are at **Stage 0**.
> Run the free experiments in `docs/plans/stage-0-free-experiments.md` — they may
> end the project without a line of code. Report each result, then update
> `docs/rev/observed.md` and this block before doing anything else.

---

## Where things stand

Titan Quest Anniversary Edition runs correctly under CrossOver's DXMT backend
**except that moving things flicker**. Investigation on 2026-08-25 established
that there are **two independent defects** (`docs/rev/observed.md`, O1):

- **Defect A — the shadow pass.** The majority of the artefact. Turning shadows
  off greatly reduces it. The prime suspect is DXMT's inability to honour the
  game's sampler border colour of `-FLT_MAX` (O2, H-A) — the only warning DXMT
  emits all session.
- **Defect B — the residual.** Survives shadows-off. Seen on the FX/light in
  front of the resurrection shrine and on character hair or clothes. Cause
  unknown; alpha-blended/alpha-tested dynamic geometry is the common thread.
  Two live hypotheses (H-B1 constant-buffer range, H-B2 something else in the
  transparency pass).

**No configuration fixes either one** — DXMT has seven tunables and none is
relevant (O7). Ambient occlusion is cleared (O1); the THQ Nordic overlay is
cleared (O5).

The intended endgame is `tqflicker.dll`, a 32-bit shim loaded via a `winmm.dll`
proxy, which corrects the sampler description on its way into DXMT and whatever
Defect B turns out to need. But Stage 0 comes first, because the project may not
need to exist.

---

## Stages

Each stage has a **gate**. Do not tick a box until its gate has actually been
met, in the game, with your own eyes on the screen.

### ☐ Stage 0 — Free experiments

**Plan:** `docs/plans/stage-0-free-experiments.md`
Four things that cost one launch each and could each end the project outright.
The big one is `/dx9`, which **has never been tried on any backend**.
**Gate:** every experiment run, every result written into `observed.md`, and an
explicit decision recorded on whether to continue.

### ☐ Stage 1 — Get inside the process

**Plan:** `docs/plans/stage-1-proxy.md`
A 32-bit `winmm.dll` proxy that forwards everything and writes one line to a log.
Adapted from `../grimdawn-trash/scripts/gen-winmm-proxy.sh`, with stdcall
decoration handled.
**Gate:** the game launches, plays normally, and our log file exists with our
line in it. Plus an off-game self-test that loads the DLL and checks forwarding.

### ☐ Stage 2 — Reach the device

**Plan:** `docs/plans/stage-2-device.md`
Wait for `Direct3D11.dll`, IAT-hook its imported `D3D11CreateDevice` /
`D3D11CreateDeviceAndSwapChain`, capture the `ID3D11Device*` and
`ID3D11DeviceContext*`.
**Gate:** our log prints the device pointer, the context pointer and the feature
level (expect `11_0`), and the game still reaches gameplay and plays normally.

### ☐ Stage 3 — Observe the samplers, and the buffers

**Plan:** `docs/plans/stage-3-observe.md`
Vtable data-patch on `ID3D11Device::CreateSamplerState`; log every
`D3D11_SAMPLER_DESC` in full. While we are in there and it is nearly free, log
every constant-buffer `CreateBuffer` width too, for H-B1.
**Gate:** the `-FLT_MAX` sampler appears in **our** log with its complete
description, we know how many samplers exist and how many use `ADDRESS_BORDER`,
and the game is unharmed.

### ☐ Stage 4 — Fix Defect A

**Plan:** stub — write it once Stage 3 has produced the descriptions.
Rewrite the offending desc on its way through. At least three variants worth
A/B-ing behind a config key: address mode → `CLAMP`, border → opaque white,
border → transparent black.
**Gate:** the same scene, shadows **on**, no shadow flicker, no new artefact, no
crash.

### ☐ Stage 5 — Attack Defect B

**Plan:** stub — write it once Stage 4 has cleared Defect A out of the picture.
Two lines of attack, in this order: a **Metal frame capture** of a frame
containing the shrine-FX flicker (`DXMT_CAPTURE_FRAME`, the only real graphics
debugger here and still entirely unspent), then the constant-buffer reflection
check if the capture does not settle it.
**Gate:** a named cause backed by an observation, not a guess.

### ☐ Stage 6 — Ship it

**Plan:** stub.
Config file, install/uninstall scripts, a ZIP a stranger can unpack, the THQ
overlay put back, and a CodeWeavers bug report carrying the evidence — DXMT's
own warning line, and the note that DXVK ships a per-app profile for this exact
executable.
**Gate:** a clean bottle, a fresh install, someone else's hands.

---

## Risk log

| # | Risk | State | Note |
|---|------|-------|------|
| 1 | The whole project is unnecessary because `/dx9` works | **OPEN** | Stage 0. Never tried on any backend. Cheapest possible outcome |
| 2 | 32-bit stdcall decoration breaks the winmm proxy | **OPEN** | The sibling repo is 64-bit; its `.def` and stub generation do not transfer unexamined |
| 3 | DXMT hands out different vtables per interface version | **OPEN** | Patch what `D3D11CreateDevice` returned; `QueryInterface` for `ID3D11Device1` may yield a different object. Verify before assuming one vtable |
| 4 | Patching a vtable a thread is already inside | **OPEN** | Hook at device creation, before the render thread is running. Never patch mid-frame |
| 5 | H-A is wrong and the border colour is a red herring | **OPEN** | Stage 3's log settles it. Shadows-off evidence is correlational, not causal |
| 6 | Defect B is unfixable from outside the game | **OPEN** | If the capture shows a shader-internal problem, the honest outcome is a bug report, not a hack |
| 7 | A fix that crashes is worse than the flicker | **OPEN** | Standing. Every patch guarded; anything uncertain is logged and skipped |
| 8 | CrossOver rewrites `cxbottle.conf` on exit | **CLOSED** | Known: quit CrossOver before editing it |
| 9 | Measurements polluted by the THQ overlay's Present hook | **CLOSED** | Renamed to `overlay-disabled`; flicker unchanged (O5) |
| 10 | Trusting DXVK's log when DXVK does not work here | **CLOSED** | Only the app-profile line and the capability report are used, and why is written down (O6) |
