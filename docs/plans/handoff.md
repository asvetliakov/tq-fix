# Where the stutter work stands

Companion to `game-stutter-mitigation.md`, which is the plan. That document's
"Status" section records where the plan was wrong; this one records what is
built, what is measured, and what to do next. Current after the **performance
module / trace-off refactor**, with Run 87 prepared, on `stutter-mitigation`.

---

## READ THIS FIRST: the brief for the next session

**READ FINDINGS §111 BEFORE §110.** The post-checkpoint refactor moves
performance behavior and installation into `shadow_defer.cpp`,
`terrain_preload.cpp`, `secondary_admission.cpp`, and `archive_hooks.cpp`;
`engine_hooks.cpp` coordinates shared sites. `engine_probe.cpp` is the
optional Engine observer. Shared ABI/byte contracts are in
`engine_internal.h`, and the verifier reads every implementation.

§110 proved the fixes execute without tracing, but missed real disabled-path
overhead: `probe::now()` always called QPC, several shared wrappers entered
observer helpers, and admission still installed the obsolete reflection
`BuildScene` observer. §111 removes these paths, gates recorder helpers
inline, and adds a test that executes real preload, cold-root and admission
wrappers without entering either recorder. Accepted behavior/settings and
every pre-existing Engine constant initializer are unchanged. No new game
result is claimed; Run 85 remains the latest measured result.

Run 87 is installed as `cache/runs/run87-performance-modules-trace-off.ini`.
It differs from `live-config.ini` only in its explanatory header; both traces
are off, so no F12 or CSV record is expected. Doctor/build/selftest pass,
including GPU retirement and the real-wrapper trace-off exercise. Verification
passes **865/865** checks, rejects **362/362** scalar/window perturbations,
and rejects **22/22** disabled-gate regressions. The broader perturbation
audit exposed and corrected 28 previously unchecked scalar contracts plus an
out-of-bounds call-offset error path in the verifier.

Build and installed DLL SHA-256:
`1ec22a61d7687bd5c97c5f69c2adf2835371783525ab75c1db8a2b6ce109edf4`.
Run-87 source/installed INI SHA-256:
`384c95d3ae868cee052113315371cdd3c249cf3408c6b8273dad9850117bb569`.
No stale live CSV/debug log existed. The game was not launched.

The old Run-86 DLL hash and validation count below describe checkpoint
`9b1cf2a`, not the refactored build. See §111 for the current implementation
and validation. Historical briefs follow unchanged.

**READ FINDINGS §110 BEFORE §109.** This audit is not another game run; Run
85 remains the latest measured and subjective result. The proven Run-85 state
was committed first as `b059254` (`fix: progressively admit secondary-pass
objects`). Findings §69--§108, which had accidentally been stored in reverse
order between §68 and the original §69, are now ascending without rewriting
any section body. The verifier enforces that ordering.

The shipping defaults are now the user's measured normal configuration:
`loose_texture_max=4096`, `archive_cache_mb=8`,
`shadow_defer_cold_resources=1`, `shadow_defer_cold_actor_pose=1`,
`terrain_preload_layers=1`, and `secondary_pass_admission_budget=8`. These
defaults apply with an omitted key and with no INI; explicit `0` remains the
stock rollback for each behavior. The renamed cold-resource switch includes
opaque and alpha-tested root meshes as well as the texture paths, which is why
`shadow_defer_cold_alpha` and the still-incomplete proposed
`shadow_defer_cold_textures` were rejected as current names.

The rejected behavior keys no longer parse: `async_level_load`,
`timer_period_ms`, `pump_timer_min_ms`, `shadow_transition_reuse`,
`reflection_defer_admission_mesh`, and `reflection_defer_admission_all`.
Their historical findings, run INIs, CSV columns, byte evidence, and
compile-time-disabled paths remain auditable. `archive_cache_mb` remains.
`draw_timing` is no longer an input: `performance_trace=full` always enables
the game's Draw/DrawIndexed/Map clocks, while hitch-only
`performance_trace=1` stays lightweight.

The next game confirmation is the normal trace-off route with these defaults.
It tests the same `GraphicsMeshInstance`/`Actor` directional-shadow resource
mitigations, `TerrainRT` layer preload, and shared reflection/directional
secondary-pass admission without measurement overhead. Report the five parts
separately—menu, load-game frame, loading screen, first world frame, and
play—and report any local shadow or reflection pop even if the old marked-area
play hitch remains absent.

That confirmation is installed as
`cache/runs/run86-promoted-defaults-trace-off.ini`. It has no parsed-setting
difference from `live-config.ini`; only its explanatory header differs. The
installed DLL matches the build at SHA-256
`5bd20787b9689668627259ea33e72ab60f71f0375d2df962baf011abb700a432`,
and the installed/archived INI hash is
`bd6c8bee9b6de574cf72dbb63cc31e1fef7bbe368207f1b7507dac47d8bc79bc`.
There was no stale live CSV or debug log, and the game has not been launched.
Validation is green: doctor, release build, zero-failure self-test including
GPU timestamp retirement, and 804/804 verifier checks. In addition to the
eleven cleanup mutations, twelve one-at-a-time trace-off regressions—six
install gates and six installed-hook behavior gates—each fail the verifier.

**ARCHIVED RUN-85 RESULT BRIEF — corrected above by findings §109.**

**READ FINDINGS §108 BEFORE §107. Run 85 confirms the fix.** Its five parts
are **menu** 0--1902, **load-game frame** 1903, **loading screen** 1904--2995,
**first world frame** 2996, and **play** 2997--7304. F12 is **play** frame
6583, deliberately pressed at the old location without a felt hitch; it is
not a reaction marker. The exact old transition class is **play** frame 6490,
1.917 s earlier. The user again reports that the old stutter was not
noticeable.

Frame 6490 is 40.117 ms CPU / 40.780 ms GPU. Against 90 controlled same-run,
collision-active full-scene **play** frames under 60 ms in the 1,200--1,699
indexed-draw band, its surcharge is 19.633 ms CPU / 20.309 ms GPU. Exact
reflection is only 0.128 ms GPU. Eight new identities are admitted and ten
pending shadow draws suppressed; the next two frames are 32.247 and 20.943 ms
CPU / 20.330 and 20.889 ms GPU, with no large rebound.

Run 85 records exactly one self-arm, on **first world frame** 2996, when eight
identities are admitted and identity nine proves the 449-call pending
population. The >=32-buffer reflection counter is zero for the whole session.
Later region changes do not retrigger it. A separate **play** population at
frame 6616 has neither old proxy, admits eight, and spreads 102 pending shadow
calls through frame 6631 without exceeding 59.033 ms. No subjective event is
assigned to that unmarked class. The shared budget never exceeds eight and
identity overflow is zero.

Do not add the rejected reflection omissions or more trace. The next run
should keep budget eight but return `trace=0` / `performance_trace=0`, testing
the normal trace-off Draw-hook path and subjective result without measurement
load. If that remains clean, promote budget eight into the user's normal INI.

Run-85 archives match the live files at CSV SHA-256
`61a90305a13a833f1ae08aabf1dd1e27cb1cd787da58f16534493fc53e8d4a97`
and debug SHA-256
`1bddaaafd7f2c9c6ea7b81edba8cc4cf117dde1f3ec99bfef8341b6bc684ca67`.
The result-annotated Run-85 INI SHA-256 is
`e352417f76d4859b2ccb7469564ffc68da472a8bcf689e364c97dc0d421b91e3`;
its launch-time installed hash was
`b9aec8863f2ed6bf27fc6bd54030c8e909bd6c7aaeb052f2e5a043ff3fca2f41`.

**ARCHIVED RUN-84 / RUN-85-PREPARATION BRIEF — corrected above and by
findings §108.**

**READ FINDINGS §107 BEFORE §106. Run 84 completed and its subjective result
is positive; Run 85 is the trigger-generalization confirmation.** Run 84's
five parts are **menu** 0--2141, **load-game frame** 2142,
**loading screen** 2143--3205, **first world frame** 3206, and **play**
3207--7309. F12 is **play** frame 6827. It was pressed deliberately at the old
route location without a felt hitch, so it has no reaction-window candidate.
The exact old transition class is **play** frame 6764: it changes the shadow
region, creates 93 buffers, and runs second-plane reflection 1.320 s before
the press. The user says the old stutter now appears fixed or is no longer
visible.

Frame 6764 is 38.229 ms CPU / 31.758 ms GPU. Against 58 controlled, same-run,
collision-active full-scene **play** reference frames under 60 ms in the
1,300--1,699 indexed-draw band, its surcharge is 17.548 ms CPU / 11.112 ms
GPU. Exact reflection is only 0.762 ms GPU. The budget admits 39 new identities
over frames 6764--6768 as 8, 8, 8, 8, and 7; the four following frames are
28.645, 21.397, 24.939, and 24.502 ms, and pending suppression reaches zero.
There is no one-frame rebound, no budget violation, and no identity-table
overflow.

Run 85 implements the general trigger without changing budget eight. In either
exact secondary class, the first eight previously unseen shared identities in
a presented frame render normally; identity nine self-arms and remains pending
for the next frame. Reflection-buffer and shadow-region signals are telemetry
only. A trace-off admission boot no longer requests `CreateBuffer`, and it
explicitly requests both D3D draw hooks; Engine behavior activates only if
both draw slots and all existing Engine dependencies installed. No new Engine
target or byte table is added.

Do not combine the rejected mesh-only or whole-reflection admission omissions.
They moved all postponed work to another consumer/frame, felt unchanged in
Runs 81--82, and would now duplicate a reflection path already reduced to
0.762 ms GPU while delaying Resource/material preparation.

Run-84 archives match the completed live files at CSV SHA-256
`ef3a9372e7d03b8cedfc234424dcf183a8456621a0317b165aea54f7e201f8e9`
and debug SHA-256
`91814fc42cd9ef8118fa0ecac44166d6622cbbe81c91937d3f879a75497fb149`.

The verifier passes 784 checks. Doctor, the 766,464-byte release build, and
the full off-game self-test pass, including GPU timestamp retirement. Run 85
is installed. Source/installed DLL SHA-256 is
`d654f91f5f98e8c931501fb4cf27554a9ed0f251432e6727f34c6a84c6033569`;
launch-time source/installed INI SHA-256 was
`b9aec8863f2ed6bf27fc6bd54030c8e909bd6c7aaeb052f2e5a043ff3fca2f41`.
No stale live CSV/debug log exists and the game has not been launched. Run the
same route; press F12 only if a **play** hitch is felt, and separately report
any reflection or directional-shadow popping.

**ARCHIVED RUN-81 / RUN-82-PREPARATION BRIEF — corrected above and by
findings §104.**

**READ FINDINGS §103 BEFORE §102. Run 81 completed and rejects the mesh-only
reflection treatment; Run 82 is the whole-reflection behavior A/B.** Run 81's
five parts are **menu** 0--1933, **load-game frame** 1934, **loading screen**
1935--3024, **first world frame** 3025, and **play** 3026--6938. F12 is
**play** frame 6409 at 24.416 ms. The plausible felt pair is **play** frames
6388/6389 at 115.778/138.573 ms, ending 579/440 ms before the press. The user
reports **no perceptible change in stutter**; that subjective result is
authoritative.

The Run-81 behavior did fire on play frame 6388. Exact reflection `BuildScene`
created 91 of the frame's 105 buffers, crossed the 32-buffer boundary, and the
following exact `RenderLightStyle` omitted all 87 `GraphicsMeshInstance`
calls. Zero mesh calls remained. The surviving reflection child is 50
`TerrainPlug` plus 18 `TerrainBlock` calls / 106 draws and costs 41.959 ms GPU;
its first three chunks cost 26.862, 8.739, and 5.816 ms. One TerrainBlock also
loads two Resources for 9.699 ms CPU, including 6.711 ms texture creation.

This is not the only producer. The same play frame has 81.536 ms directional-
shadow GPU. Its later exact second-owner geometry-scene call spends 52.994 ms
CPU, 51.871 ms of that in game draw submission, while its own GPU interval is
only 13.910 ms. Frame 6388 is 115.778 ms CPU / 214.312 ms GPU and frame 6389
then blocks 113.445 ms in game draw submission. Forty-nine off-main textures
take 66.054 ms of overlapping loader-thread time only from frames 6389--6407,
so that tail still does not cause onset.

Run 82 enables default-off `reflection_defer_admission_all=1` instead of the
rejected mesh-only switch. At the same exact >=32-buffer admission boundary it
skips the one immediately following whole reflection `RenderLightStyle` call;
terrain and mesh reflection return next frame. This directly removes the
terrain-only 41.959-ms producer and its synchronous terrain child from the
admission frame. Directional shadows, the main colour pass, culling, resource
loading, and `shadow_split` remain unchanged. Watch specifically for one frame
of stale water/object reflection and report it even if the hitch does not
change.

The fix uses only the already verified `patchCall` sites at Engine RVAs
`0x186501` and `0x18694d` when tracing is off. It installs no mesh detour and
no trace group in that configuration. The successful-buffer count still comes
from the existing D3D device vtable proxy. Run-81 archives are SHA-256
`801c30ef033205f4be9de082b26aa08769f1b27b583f6dc123242c81f762ccce`
for CSV and
`5ce41f83503a603a1b90fb43161ab98382104bc64f8518271d00794208604692`
for debug and matched the completed live files byte-for-byte.

The extended verifier, doctor, 761,856-byte release build, and full off-game
self-test pass, including GPU timestamp retirement. Run 82 is installed.
Source/installed DLL SHA-256 is
`85e1b1e48c2f616446f9e51dd767c1bb441d59ce51ca2187549a87ecdb758a7d`;
source/installed INI SHA-256 is
`9d9b9f2e33d0fd44bc3f2a763797dd9600b5d69a36a3b73a17a5a398cba90ac2`.
Run-81 live outputs were removed only after matching their archives. The game
has not been launched. Run the normal route and press F12 after the felt
**play** loading burst; also report any one-frame reflection flicker/staleness.

**ARCHIVED RUN-80 / RUN-81-PREPARATION BRIEF — corrected above and by
findings §103.**

**READ FINDINGS §102 BEFORE §101. Run 80 completed; Run 81 is the first
reflection-admission behavior A/B.** Run 80's five parts are **menu** 0--2191,
**load-game frame** 2192, **loading screen** 2193--3337, **first world frame**
3338, and **play** 3339--7494. F12 is **play** frame 6919 at 22.061 ms. The
probable felt pair is **play** frames 6895/6896 at 80.635/41.034 ms, ending
534/493 ms before the press. The user supplied only completion, so retain the
reaction-window qualification.

The onset is a reflection producer followed by a submission drain. On **play**
frame 6895, exact second-manager/first-plane `BuildScene` creates 95 buffers
and takes 8.896 ms. The following exact `RenderLightStyle` costs 35.892 ms GPU;
the later second-owner deferred geometry-scene class blocks 43.473 ms in game
draw submission while producing only 2.175 ms GPU. Directional shadow is a
separate 24.561 ms GPU producer. Against nine matched full-scene **play**
frames 6908--6916 under 60 ms, frame 6895 adds 60.212 ms total, 36.213 ms game
draw submission, 62.917 ms whole GPU, and 35.662 ms exact reflection GPU.

The cold Gadir textures are not that GPU cost. Their `TerrainBlock` call costs
7.865 ms in Resource loading, but its GPU bin is only 0.495 ms. Four preceding
all-`GraphicsMeshInstance` bins cost 16.845 ms total, and the next mixed bin
with eighteen `TerrainPlug` plus two mesh calls costs 14.451 ms. The new
off-main records begin only on frame 6896: one loader realizes 45 initial-data,
fully-mipped BC textures, about 168.668 MiB, in 64.082 ms through frame 6901.
That can amplify the tail but cannot cause frame 6895. Do not make texture
throttling the first fix or claim lower mips solve the onset.

The exact reflection admission population is stable across Runs 73--80: the
primary transition creates 69, 64, 132, 63, 172, 80, 87, and 95 buffers in the
second-plane reflection class. Run 80's largest neighboring population is 30.
Seven of eight transition frames have 24.751--63.225 ms exact reflection GPU;
Run 79's 0.440 ms event is the measured exception.

Run 81 therefore enables default-off
`reflection_defer_admission_mesh=1`. At 32 exact `BuildScene` buffer creations,
only `GraphicsMeshInstance` calls in the immediately following reflection
`RenderLightStyle` are omitted. Terrain reflection stays present, the later
normal colour class stays stock, and mesh reflection returns next frame. This
tests the proved 16.845-ms mesh class without skipping the whole reflection,
changing shadows, touching `shadow_split`, or rewriting resource loading. It
does not claim to remove the independent TerrainPlug range or texture tail.

The fix uses the already verified `patchCall` sites at Engine RVAs `0x186501`
and `0x18694d`, plus the exact exported mesh `RenderPass` at `0x172dd0`; that
shared prologue is verified for 24 bytes and steals six. It reaches install
with the performance probe off and enables no trace group. The verifier passes
771 checks; changing the new threshold from 32 to 33 fails it. Doctor, the
760,320-byte release build, and the full self-test pass, including GPU timestamp
retirement. Run 80 archives match the completed live files at CSV SHA-256
`68ca3bd58f89dde72a64b31fbbe337acb070cc8d552ccf9c75d0e6f6c88c9b19` and
debug SHA-256
`6fd82332ad539ad94cdf7d437ea97b9cd311cc0527ec4aadb9eeabfe099bb365`.
Run 81 is installed. Source/installed DLL SHA-256 is
`d1856797e4e9d0870ccf955c930ead8823a11ce5b0babff997f1d79201eb9545`;
source/installed INI SHA-256 is
`2890b546c468625797ab01af0f906a8f6e1d39a22487d15ed660de435cd7dfa3`.
Run-80 live files were removed only after matching the archives. The game has
not been launched.
Run the normal route with Run 81 and press F12 after the felt **play** loading
burst. Also report any one-frame change in water reflections or object
reflection, even if the hitch improves.

**ARCHIVED PRE-RUN-79 BRIEF — corrected above and by findings §101. Read §100
before §99 only when reconstructing that earlier state.**
Run 77's five parts are **menu** 0--2046, **load-game frame** 2047,
**loading screen** 2048--3194, **first world frame** 3195, and **play**
3196--7442. F12 is **play** frame 6915 at 19.264 ms. **Play** frame 6914 is
62.928 ms but ended only 19 ms before the press and is not a human-reaction
candidate. The probable event is **play** frame 6892 at 60.329 ms, ending
539 ms before F12. No subjective classification accompanied completion.

On **play** frame 6892, exact second-manager/first-plane
`GraphicsForwardRenderer::RenderLightStyle` is 27.003 ms CPU / 24.708 ms GPU.
Its sixteen state-0 Resource loads take 25.715 ms, including 17.769 ms of
nested texture creation. The sixteen Run-77 query bins for draws 65--192 total
only 5.502 ms GPU, with no bin above 2.161 ms. The event reaches draw 193 and
sets overflow. The remaining 19.206 ms of the whole child lies either in
draws 1--64 or after draw 192; Run 76's different marked **play** event
measured the former at 0.091 ms, making the latter the next target without
proving it.

Run 77 retained 37 terrain calls through draw 193, none with nested work. This
is not an absence result: draw overflow disabled `active.recording`, and the
terrain-call scope incorrectly used that same flag, so it stopped before the
cold `TerrainBlock` call it was intended to identify. Run 78 moves the same
sixteen eight-draw query pairs to draws 193--320 and begins terrain-call
retention only at that boundary, preserving the 128 fixed slots for the late
tail. It adds no query, Engine patch, D3D getter, behavior change, or shadow
instrumentation. All accepted fixes and `shadow_split` remain unchanged.

Under the same collision-active, full-scene **play**-below-60-ms,
1,000--1,499-indexed-draw, no-main-Resource, no-region-change filter, Run 76 /
Run 77 mean total time is 22.293 / 21.811 ms and mean mod Present class is
0.079 / 0.052 ms over 46 / 37 frames. This excludes a gross logging
regression, not a fine effect.

The verifier passes 753 checks and rejects all 7/7 independently perturbed
trace bounds. Doctor, the 754,688-byte release build, and the full off-game
self-test pass, including GPU timestamp retirement. Run 78 is installed; the
game has not been launched. The source/installed DLL SHA-256 is
`86880e95234db37420087447192537316e52b9a62b7c8ff5ef8667385d2a6a28`;
the source/installed INI SHA-256 is
`be46abecf6a32764ac50f477a73ece4b7392d6fc2de984554d6129194753bff1`.

Run-77 archives are SHA-256
`8afdff308c0402e6ecc59d45bb5bdb59dd3cb687c01bba90c95fd800917fd491`
for the CSV and
`caedce37049ad80274176d1585dc55b124e4a623d44829805724e90f9fa5094a`
for the during-session log; both matched the completed live files.

**READ FINDINGS §98 BEFORE §97. Run 77 preparation record follows.** It is
passive. Exact second-manager/first-plane
`GraphicsForwardRenderer::BuildScene >= 2,000 us` remains only the sparse
selector for the following `GraphicsForwardRenderer::RenderLightStyle`.
Draws 1--64 are counted without timestamps; sixteen unique eight-draw GPU
intervals cover draws 65--192. Each next interval opens immediately after the
preceding boundary, so Resource/D3D work between renderables is included.

While that selected reflection child is active, the already verified exact
unexported `TerrainPlug` and `TerrainBlock` wrappers retain at most 128 calls
per event with object identity, start/end draw ordinal, CPU duration,
`TerrainType`/material, and nested Resource/texture/buffer creation totals.
F12 writes those bounded records during the session and reports overflow. No
new Engine patch or D3D state getter is added.

Run 77 removes all directional setup/chunk query IDs and CSV columns. Exact
`GraphicsShadowMapDx11::RenderDirectional` no longer arms a chunk event and
the `GraphicsShadowMapRenderer` executor `E8` is no longer patched. Its exact
23/24/21-byte evidence remains verified in the static audit. Rendering,
Resource behavior, all three accepted fixes, and `shadow_split` are unchanged.

The verifier passes 752 checks and rejects all 7/7 one-at-a-time new/changed
numeric-bound mutations. Doctor, the 754,688-byte release build, and the full
off-game self-test pass, including GPU timestamp retirement. Installed/source
DLL SHA-256 is
`ebbda98ba661a745d4206d39c1d9f9eeca4913490274e36be85457bfcb7aca91`;
installed/source Run-77 INI SHA-256 is
`2806e64c0de188d05ff38d628c03df67c0ecc2fa753320106f559313e5b75166`.
Run-76 live outputs matched their archives before removal, both stale live
names are absent, and the game has not been launched. Run the normal route and
press F12 after the felt **play** burst.

**READ FINDINGS §97 BEFORE §96. Run 76 completed.** Its five parts are
**menu** 0--1818, **load-game frame** 1819, **loading screen** 1820--2954,
**first world frame** 2955, and **play** 2956--7309. F12 is on play frame
6575 at 24.320 ms. The probable route event is play frame 6548 at 90.945 ms,
ending 601 ms before F12; a later 423.042 ms maximum occurs after the marker
and must not be substituted. The completion message supplied no further
subjective classification.

On play frame 6548 the exact second-manager/first-plane
`GraphicsForwardRenderer::RenderLightStyle` class is 8.888 ms CPU / 38.739 ms
GPU / 169 draws. Its corrected chunks are 0.091 ms for draws 1--64, 7.932 ms
for 65--128, and 30.715 ms for 129--169. Thus draws 65--169 own 99.8% of the
reflection child GPU interval. Two state-0 Gadir terrain textures load for
7.400 ms inside the exact unexported `TerrainBlock` colour-render class, with
6.041 ms in four nested texture creations. This exact `TerrainType` was
semantically preloaded once during the loading screen at frame 2858, but the
stock runtime `TerrainRT::PreLoad` owner that ran through play frame 6547 does
not refresh layer types. That establishes a stale near-use preload gap, but
does not yet distinguish texture upload from newly admitted terrain draws.

The ordering corrects the remaining diagnosis. The game's D3D submission
class is 49.302 ms on frame 6548; 48.151 ms blocks in the exact second
`GraphicsDeferredRendererX::Render` geometry-scene call at `0x166412`, whose
own GPU interval is only 4.732 ms. Reflection executes before the deferred
owner, while deferred geometry scene `0x166412` executes before shadow-map
construction `0x166454`. The wait is therefore a same-frame queue drain after
reflection, not a wait caused by the later directional shadow.

Exact `GraphicsShadowMapDx11::RenderDirectional` is only 7.090 ms CPU /
18.195 ms GPU on that play frame. Its newly separated setup is 9.017 ms; all
twelve exact DX11 `GraphicsShadowMapRenderer` executor chunks total 8.897 ms,
and none exceeds 1.866 ms. This marked event has no pathological shadow-record
range. That does not erase Run 75's separate 84.599 ms directional sample,
but it closes shadow as the cause of Run 76's submission drain.

The ordinary-draw gate shows no large broad logging regression. In matched
collision-active, full-scene play frames below 60 ms with 1,000--1,499
indexed draws, no main-thread Resource load, and no region change, Run 75 /
Run 76 means are 22.103 / 22.293 ms total and 0.047 / 0.079 ms in the mod
Present class (40 / 46 frames). Never quote a cross-run p50. Run 76 has zero
chunk overflow and zero collision; the F12 debug report is written after the
candidate.

The next passive trace should remove directional chunk queries and narrow
only the reflection tail. Preserve the sparse exact second-manager/first-plane
BuildScene selector; count draws 1--64 without timestamps, then cover draws
65--192 in sixteen eight-draw GPU bins. During that exact reflection event,
the existing unexported `TerrainPlug` and `TerrainBlock` wrappers should retain
bounded call identity, start/end draw ordinal, CPU duration, and nested
Resource/create totals for the F12 report. This adds no new Engine patch and
will distinguish the cold `TerrainBlock` call from a different resident
population. Do not yet call semantic preload on every layer during every
runtime-owner preload: this owner has 142 layers and ran almost every frame.

Run 76 archives are SHA-256
`dd4118f6896bf27c895e4e8c11adc97a394c096339912516bec7af2f93c623a8`
for the CSV and
`d797b52321f0623637b6b501b98e0ad009b69c15f48f01969c91144f9627a71d`
for the during-session log. Both completed live files are byte-identical to
their archives.

**READ FINDINGS §96 BEFORE §95. Run 76's preparation record follows.** It is
passive: rendering choices, resource behavior,
`shadow_split`, and the accepted fixes are unchanged. Its purpose is to
replace Run 75's two flawed measurement boundaries while reducing the trace
overhead the reporter may have felt.

Ordinary game `Draw`/`DrawIndexed` now reads one inline volatile flag. When no
selected exact class is active, it calls neither GPU-chunk helper. For exact
`GraphicsShadowMapDx11::RenderDirectional`, a region change opens a separate
setup GPU interval; the sixteen 64-draw chunks begin only at the verified
DX11 `GraphicsShadowMapRenderer` record-executor call
`0x18d05d -> 0x18c520` and close when that executor returns. The old/DX9
branch uses `0x187360` and is untouched.

For reflection, decompilation shows that exact
`GraphicsForwardRenderer::BuildScene` has no admitted-count return. A
second-manager/first-plane BuildScene duration at or above 2,000 us is
therefore used only as a sparse signal to open the immediately following
whole exact `GraphicsForwardRenderer::RenderLightStyle`; the exact trigger
duration is retained at F12. In Run 75's **play** part this criterion selects
only frames 6744 (12,766 us), 6747 (3,024 us), 6748 (2,590 us), and 6915
(3,274 us), including both implicated events. It does not attribute their GPU
cost to BuildScene.

The new 23-byte executor call window, 24-byte entry, and 21-byte `ret 0x0c`
tail are re-read at runtime before either shadow `E8` is written. The source
uses `patchCall`, restores the inner call before the outer call, and requires
the exact executor before GPU chunks activate. `verify-sites.py` passes 750
checks and rejects all 73/73 one-at-a-time new constant/table-byte mutations.
Doctor, the 755,200-byte release build, and the complete off-game self-test
pass, including GPU timestamp retirement. Run 76 is installed; DLL SHA-256 is
`96a7347716e33e07edbd97535739e6a77b9897cbe99cff88758563be5a0ddede` and INI
SHA-256 is
`f9fc60731be367987bfd1b584f5bb23a64b41ed77a112f80354a5a06cf692ade`, with
both source/installed pairs byte-identical. The stale live Run-75 outputs were
archive-verified and removed. The game has not been launched. The user should
run the normal route and press F12 after the felt **play** burst.

**Run 75 completed.** Its five parts are
**menu** 0--1943, **load-game frame** 1944, **loading screen** 1945--3146,
**first world frame** 3147, and **play** 3148--7342. F12 is play frame 6760.
Frame 6759 ends only 21 ms before F12 and is not a human reaction candidate.
The probable felt sequence is play frames 6744/6745 at 107.764/136.323 ms,
ending 522/385 ms before the marker, followed by a 35.960 ms frame and a
45.498 ms reflection/resource frame ending 304 ms before F12. The reporter
felt “a little more delay” and suspects logging.

Matched ordinary full-scene **play** frames below 60 ms show no broad
regression: under the same collision-active 1,000--1,499-draw/no-load/no-region
filter, Run 74 and Run 75 means are 22.137/22.103 ms total and 0.048/0.047 ms
in the mod Present class. The F12 debug report was written after the candidate
frames, and the CSV writer remains asynchronous. The trace is still not free:
it enters two helpers around every game draw, scans 32 extra GPU phase slots
per frame, and issues timestamps on seven **play** event frames. Optimize that
ordinary-draw path before another run, but the data supports real extra game
work in this session more strongly than logger delay.

Directional subdivision succeeded. On play frame 6744, exact
`GraphicsShadowMapDx11::RenderDirectional` is 5.299 ms CPU / 84.599 ms GPU /
743 draws. Twelve chunks total 84.278 ms; five ranges own 79.282 ms (94.1%).
Chunk 0 is 42.968 ms but still includes pre-executor setup plus draws 1--64;
the other costly ranges are draws 129--192 (6.251 ms), 193--256 (12.407 ms),
449--512 (14.699 ms), and 641--704 (2.957 ms). Instrument the already verified
`0x18d05d -> 0x18c520` executor call next so setup and exact records separate.

Reflection subdivision failed by starting too late. The exact second-manager/
first-plane `GraphicsForwardRenderer::RenderLightStyle` on play frame 6744 is
12.382 ms CPU / 63.180 ms GPU, but its first cold Resource occurs only at draw
200. The remaining 39-draw chunk is 0.382 ms. Frame 6747 independently has six
cold Resources / 15.547 ms and 17.503 ms `RenderLightStyle` GPU; its trigger at
draw 31 leaves four chunks totaling 9.700 ms. The next reflection trace must
cover the whole child, preferably armed by a verified sparse signal from the
preceding `BuildScene`; another post-cold trigger cannot answer it.

Run 75's completed CSV/debug SHA-256 identities are
`be2e4bb57de660b15c70e94371a84f1c33adaaa8571adf246fac84d37c0abf3c` and
`ab315f205f7f84d22c4ccfd458840c8468af507261dd35048961ab6d4be5b000`.
Both live files matched their archives byte-for-byte.

**Run 75 preparation record follows.** It
subdivides both exceptional Run-74 GPU producers without changing rendering.
For the exact reflection `GraphicsForwardRenderer::RenderLightStyle` child,
sixteen 64-draw GPU chunks arm only at the first verified state-0 Resource
load in that child, before the load executes. For the exact
`GraphicsShadowMapDx11::RenderDirectional` class, a separate sixteen chunks
arm only on a verified region change, before the class executes. F12 writes a
fixed 120-frame/32-event ring with draw ordinal ranges, draw/index/element
totals, null shader/SRV0 counts, binding transitions, and first/last tracked
VS/PS/SRV0/VB0/IB identities. Ordinary frames open no chunk query; collision
and 1,024-draw overflow are explicit.

The directional static chain is now byte-verified as DX11-specific:
`GraphicsShadowMapRenderer::Render` at `0x18ce70`, record build
`0x18d04f -> 0x18c870`, execution `0x18d05d -> 0x18c520`, then renderable
virtual `+0x28` at `0x18c613`. The old-renderer branch uses another executor.
The streaming audit was regenerated at 1,592 functions / 205 roots. The
verifier passes 743 checks; all four new numeric-bound perturbations fail it.
Doctor, the 754,688-byte release build, and the full off-game self-test pass,
including GPU timestamp retirement. The game has not been launched.

Run 75's source/installed DLL SHA-256 is
`45768956bd265ca16c49d8e8d83dcb4c57aa041d28cedacae72a9c5534b3fb76`;
the source/installed `cache/runs/run75-gpu-draw-chunks.ini` SHA-256 is
`630b798d1c66d968193d6583be3ff56eb2350222803c88507411d5755a3e600a`.
Both pairs are byte-identical. Run 74's live CSV/debug log matched their
archives before the two stale live names were removed.

Run 74's five parts are **menu** 0--2027, **load-game frame**
2028, **loading screen** 2029--3162, **first world frame** 3163, and **play**
3164--7303. F12 is on play frame 6724. The probable normal-route pair is play
frames 6705/6706 at 92.269/165.636 ms, ending 551/386 ms before F12. The
reporter supplied no subjective classification beyond “done.”

Run 74 rejects the proposed large cross-pass buffer population. Frame 6705
creates 82 buffers, 64 inside the exact second-manager/first-plane reflection
class, but no fresh buffer is used by a reflection draw on that frame. Thirty-
six fresh buffers first appear in deferred color and one in directional
shadow. Only that one 11,200-byte buffer joins shadow and deferred; its first
reflection use is on frame 6706, after the exceptional frame-6705 reflection
GPU interval. The F12 window has no omission, index overflow, or recent ring
eviction. Across all play, 2,597 buffers are created and 1,114 first appear in
deferred color, while only three ever join all three classes. Generic
color-first staging of a large shared buffer population is unsupported.

The reflection producer is now exact. On play frame 6705, the second
reflection manager's first plane takes 19.805 ms CPU / 47.164 ms GPU.
`GraphicsForwardRenderer::BuildScene` takes 7.287 ms CPU and no measurable GPU;
the following exact `GraphicsForwardRenderer::RenderLightStyle` takes
10.851 ms CPU / 47.117 ms GPU. Its chain synchronously loads the two known
Gadir terrain textures for 7.820 ms and creates four textures in 5.635 ms.
Those clocks nest. The next-largest play `RenderLightStyle` GPU interval is
only 0.325 ms.

The exact `GraphicsShadowMapDx11::RenderDirectional` class is an independent
producer on frame 6705: 5.920 ms CPU / 81.471 ms GPU, 748 draws, zero nested
Resource loads, and only the one fresh shadow buffer. Its draw count is lower
than the matched 799.783-draw mean; the next-largest play directional GPU
interval is 11.800 ms. Raw draw count, cold Resource loading, and a large fresh
buffer population are all excluded. Frame 6706 is the drain: whole-frame GPU
is back to 22.467 ms, but the exact second-owner deferred geometry-scene class
blocks 145.067 ms in game draws, including individual 63.183 and 55.072 ms
`DrawIndexed` waits.

The same-run reference is 46 collision-active full-scene play frames below
60 ms in the 1,000--1,499 indexed-draw band, with no Resource load or shadow
region change. Relative to those means, frame 6705 adds 47.043 ms
`RenderLightStyle` GPU, 74.290 ms directional-shadow GPU, and 35.147 ms
second-owner geometry-scene game-draw time. Frame 6706 adds 134.971 ms in that
same exact color game-draw class while its reflection and directional GPU
intervals are ordinary. These are overlapping producer/drain scopes, not
amounts to add.

Run 75 implements that next passive measurement for both producers. A single
exceptional directional chunk supports inspecting/admitting that bounded
record range at the recovered executor; cost spread across its roughly twelve
chunks instead supports incremental map construction. For reflection, chunk
zero includes the cold texture load/upload trigger; a later exceptional chunk
identifies the subsequent draw/binding population. Do not choose a behavior
until the marked **play** event says which shape occurred, and do not retry
whole-map reuse: §50 showed visible flicker and deferred work.

Run 74 is archived byte-for-byte as SHA-256
`7d6da968ffd189a09efe382be9ae72dcb4cd6d63028f5b1114b72e788735f0f3`
for the CSV and
`2f8860bab93dbe367486afc5554b5c7e606b20b4950cbbd745f57c94818b88cf`
for the during-session log. The installed Run-74 DLL and INI remain in place.

**READ FINDINGS §90 BEFORE §89. Run 73 is installed and ready; the game has
not been launched.** It restores the desired `Graphics` enhanced grass after
Run 72 rejected original grass as a solution, keeps the three accepted
shadow/terrain behaviors, and changes no render, culling, reflection, terrain,
shadow, or resource-loading choice. The new work is passive attribution only.

The unique recursive DX11 branch E8 into
`GraphicsReflectionManager::RenderReflections` and the manager's unique
per-water-plane E8 are patched with `patchCall`. Each call is guarded by its
independent 16--24-byte caller, entry, and callee-cleanup tail evidence before
either write; the two install atomically and restore in reverse. The first two
manager invocations and first two planes in each manager receive exact CPU,
game-draw, Resource, D3D-creation, and non-blocking GPU fields. Extra managers
or planes increment explicit overflow counters. Existing timing samples are
reused. Manager and plane intervals nest and must not be added. Off-main
texture creation is deliberately not assigned a stale
main-thread reflection identity.

The verifier passes 681 checks and rejects all 280/280 Run-73 one-at-a-time
mutations, including every byte in the six new tables. `npm run doctor`, the
738,816-byte release build, and the complete corrected off-game self-test pass;
GPU timestamp retirement passed on the first valid run. Installed and source
DLLs are byte-identical at SHA-256
`e4a0195a7c5bb04a5062e1b231fc3b2897ac2223176ea1e6faf9c5ab9d8cc4b7`.
Installed and source Run-73 INIs are byte-identical at SHA-256
`6c3b1e7a9eb7779914da43ec8daa52af3422b5e4e664c2011a89f583c059f08d`.
Run 72's live CSV/log matched their archives before removal; both Run-73 live
outputs are absent.

Run the same normal five-part route and press F12 after the felt **play**
transition. Separately report subjective size, flicker, shadow pop, missing
geometry, or unusual slowness. The analysis must state the new run's **menu**,
**load-game frame**, **loading screen**, **first world frame**, and **play**
boundaries before quoting any number. F12 is a reaction anchor, not authority
to select the nearest or largest frame. Compare only matched full-scene
**play** frames below 60 ms; never use a cross-run p50.

**READ FINDINGS §89 BEFORE §88. Run 72 completed; no new runtime build has
been installed.** Its five parts are **menu** 0--1979, **load-game frame**
1980, **loading screen** 1981--3098, **first world frame** 3099, and **play**
3100--8101. F12 is on play frame 7318. Frame 7317 ended only 14 ms before it
and cannot be a human reaction; it is a separate 62.312 ms frame with
48.462 ms in `Engine::Update`. The probable normal-route transition is play
frames 7295/7296/7297 at 46.065/117.857/64.646 ms, ending 575/457/392 ms
before F12. The reporter supplied no subjective size, so retain “probable.”

The grass A/B is informative but not a fix. With `Graphics` original grass,
the transition remains and its draw drain moves from Run 71's second-owner
geometry scene into Run 72's second-owner geometry-setup class. On play frame
7296 that exact `GraphicsDeferredRendererX::Render` child takes 26.799 ms,
including 25.748 ms in the game's draw calls, while its GPU interval is only
4.825 ms. One `DrawIndexed` waits 25.275 ms. Original grass does reduce
matched steady full-scene play means, so enhanced grass is an amplifier with
a real steady cost; it is not the necessary producer of the native event.

The producer starts on play frame 7295: 66 buffers / 0.648 ms, ten main-thread
textures / 4.980 ms, and nine `ResourceLoader::LoadResource` calls /
21.465 ms. Eight terrain textures / 20.226 ms occur in `Engine::Render`
outside both deferred owners. The exact
`GraphicsShadowMapDx11::RenderDirectional` class performs no Resource load,
but the whole-frame GPU interval reaches 196.066 ms. Play frame 7296 then
contains 33 off-main texture creations / 37.104 ms, 26.475 ms blocked in game
draws, 13.487 ms progressive upload, and 53.466 ms in the enhanced-bloom CPU
class while its whole-frame GPU interval is only 22.501 ms. Play frame 7297
finishes the triplet with 32.706 ms in the game's `Present` call. These are
overlapping producer/backpressure/drain classes, not additive work.

The full static flow was not already understood. The old generated audit was
broad, but nobody had interpreted the top-level chain. The regenerated audit
has 205 roots and a 1,592-function closure. The verifier-backed durable map is
in `research/streaming/disassembly-targets.md`. The important correction is:
`GraphicsPortalRenderer::Render` recursively renders portal/region branches;
each DX11 branch calls `GraphicsReflectionManager::RenderReflections` before
admitting its regions and before its `GraphicsDeferredRendererX::Render`.
Each water-reflection plane uses `GraphicsForwardRenderer::BuildScene` and
`RenderLightStyle`, then the shared sorted render-list executor.

Run 72's eight terrain-load stacks follow that exact reflection chain and
return from the per-plane reflection renderer before entering a deferred
owner. Therefore `outside owner` no longer means `unknown`: those play-frame
7295 loads are reflection forward-render work. Also, the historical
“geometry setup” callee at `0x1653a0` builds and executes its own sorted scene
list; it is not just state setup.

The next supported action is one passive attribution trace, not another A/B:
restore desired `grass=enhanced`, patch the unique reflection call at
`0x17f2d3` and per-plane call at `0x1872bb`, and tag existing Resource,
D3D-creation, and game-draw samples as reflection versus deferred branch.
Capture bounded per-branch/per-plane CPU, draw, and GPU intervals. That will
say whether the 196.066 ms play GPU producer is reflected-scene admission and
whether reflection update budgeting is the workable fix boundary. It does
not reopen the pump, generic prefetch, pooling, locks, sleep, Stage 5, or
libdeflate, and it does not justify rewriting shadows, culling, or resource
loading wholesale.

The expanded verifier checks 18 new 16--24-byte flow windows, eight exact
`E8` destinations, the renderable virtual dispatch, and every documented RVA;
it passes 647 checks and rejects all 72/72 targeted flow mutations. The audit
seed expansion and documentation are the only new changes after Run 72. Do
not claim that a new tracing DLL is installed.

**READ FINDINGS §88 BEFORE §87. Run 71 completed.** Its five parts are
**menu** 0--1975, **load-game frame** 1976, **loading screen** 1977--3121,
**first world frame** 3122, and **play** 3123--7268. F12 is on play frame
6703. Frame 6702 ended only 21 ms before it and cannot be a human reaction;
it is a separate 80.524 ms `Engine::Update` event. The reporter felt a little
stutter, and the normal-route region pair at play frames 6679/6680 is the
probable event. It is 82.592/51.919 ms and ended 600/548 ms before F12.

The wait point is exact. On frame 6679, the second
`GraphicsDeferredRendererX::Render` invocation's geometry-scene call at
`0x166412` takes 42.545 ms, including 42.073 ms blocked in the game's draw
calls; its GPU interval is only 6.554 ms. On frame 6680 it takes 24.116 ms,
including 23.687 ms draw blocking, while its GPU interval is only 1.411 ms.
The exact GPU interval equals the nested
`TerrainRenderInterfaceRT::RenderGrass` interval on both frames. This is a
queue drain/backpressure point, not 66 ms of GPU execution by those draws.

Frame 6679 admits two Gadir colour-terrain textures / 8.523 ms, four
main-thread textures / 6.496 ms creation, and 91 buffers / only 0.801 ms
creation before/outside the deferred owner. It also adopts and twins 35 new
enhanced-grass streams, then submits 38 cross draws beside the game's 39
original grass draws. The worst original game draw waits 26.765 ms, but the
same vertex-buffer identity waits only 0.944 ms on frame 6680. The draw is
where accumulated work drains; it is not intrinsically expensive geometry.

The Run-71 ring retained only owner-contained creations, so `new -1` does not
prove those buffers are old: all candidate-frame creation happened outside
the owner. The next cheapest discriminator is a trace-identical run changing
only optional `grass=enhanced` to `grass=original`. This tests amplification,
not whether the native game has the base transition. If it materially reduces
the probable play pair, keep enhanced grass and budget only newly adopted twin
activation across later frames. If it does not, restore it and capture all
same-frame main-thread buffer creations plus SRV-to-texture mappings outside
the owner.

Run 71 archives are SHA-256
`16a4c1c7c117aa8a24e47ba4a77f96703425b2665036bf700be5b93674bdceee`
for the CSV and
`8a26253fb3cc524066f201b76fb5920ac115f419ca6763a735f2ab93e7c4abcf`
for the during-session log; both matched their live files.

**Run 72 is installed.** It is byte-for-byte identical to the Run-71 settings
except `grass=enhanced -> grass=original`; all accepted shadow/terrain behavior
and the complete trace remain. This tests only whether the optional crossed
grass amplifies the native first-use transition. The installed/source DLLs
remain byte-identical at 732,160 bytes and SHA-256
`bf1e4691c0a897b40acf17aa259a3ad4daa24dec3c9a8051c01e1ab5c60ace40`.
The installed/source Run-72 INIs are byte-identical at SHA-256
`d655f091ca5d0be4ffd287598a3446f915843a3c21960c0e9c9b07b96c2f294e`.
Run 71 was archived and byte-compared before its live CSV was removed; the
live Run-72 CSV is absent and the game was not launched. Run the same route,
press F12 after the felt **play** transition, and report the subjective size.

**READ FINDINGS §87 BEFORE §86. Run 71 is installed.** It is a passive
trace for the reporter-selected **play** transition, not another behavior A/B.
The sole direct owner call at Engine RVA `0x17fc9b` numbers the two
`GraphicsDeferredRendererX::Render` invocations. Geometry setup `0x1663a8`
and geometry scene `0x166412` now each receive exact CPU, draw, and GPU fields
for invocation one and two. Resource loads and D3D creation are tagged as
setup, scene, or other owner work in the exact invocation.

F12 retains at most eight geometry-draw-heavy frames from the prior 120. For
each it writes only the twelve slowest draws, with draw arguments, exact owner
and site, bound vertex/index buffers, shaders, and eight pixel resources; it
also reports matching buffer creation descriptors and at most 32 same-frame
texture creations. Binding identity is updated by setter hooks. There are no
per-draw GPU queries, state getters, extra clock reads, or candidate-frame log
writes. No game behavior changes.

The owner uses `patchCall`, not the shared six-byte prologue. Its 24-byte
caller window and independent 20-byte `ret 0x1c` tail prove the seven-argument
wrapper. All owner/child/tail bytes verify before any write and installation
rolls back atomically. The verifier passes 602 checks; 190/190 new mutations
and the prior 716/716 Run-70 mutations are rejected. Doctor, the 732,160-byte
release build, and two complete off-game self-tests pass, including both GPU
timestamp-retirement checks. The installed/source DLLs are byte-identical at
732,160 bytes and SHA-256
`bf1e4691c0a897b40acf17aa259a3ad4daa24dec3c9a8051c01e1ab5c60ace40`.
The installed/source INIs are byte-identical at SHA-256
`dd063f9164acbdfee266356e20c25187e353517cb8d469d9e78029c22a5c4868`.
Run 70's stale live CSV matched its archive before removal and the live CSV is
absent. The game was not launched. Run the normal five-part route and press
F12 after the felt **play** transition.

**READ FINDINGS §86 BEFORE §85. Run 70 completed.** Its five parts are
**menu** 0--2572, **load-game frame** 2573, **loading screen** 2574--3677,
**first world frame** 3678, and **play** 3679--7882. F12 is at play frame
7283. The automatic nearest candidate, frame 7282, ended only 17 ms before
the keypress; the reporter correctly rejects that as an impossible human
reaction and identifies the earlier region-transition frame 7264 as
**probably** the felt stutter. It began 583 ms and ended 461 ms before F12.
Keep the qualification, but do not let max/nearest-frame inference override
the reporter again.

Frame 7264 is 122.155 ms, with 114.700 ms in `Engine::Render`. The exact
`GraphicsDeferredRendererX` geometry-child class takes 52.406 ms, of which
50.893 ms is blocked in the game's `Draw` / `DrawIndexed` calls. Shadow-map
construction is only 7.656 ms. The frame admits 221 buffers, but their
`CreateBuffer` calls cost only 1.765 ms. Sixteen main-thread colour-render
Resource calls cost 30.218 ms: four meshes / 2.913 ms and twelve textures /
27.305 ms; fourteen main-thread texture creations cost 22.610 ms. These
clocks may nest and must not be added. The exact
`GraphicsShadowMapDx11::RenderDirectional` class performs no synchronous
Resource load and successfully defers the cold Actor-root
`GraphicsMeshInstance` poses, so the accepted shadow fix is still working.

The Run 70 GPU partition has one corrected limitation: there are two
`GraphicsDeferredRendererX::Render` invocations per frame. One query pair per
group consequently spans both invocations, making the six GPU fields
overlapping intervals rather than separable pass costs. Never sum or attribute
them. The CPU and draw partitions are exact. Frame 7282 is a separate,
apparently unperceived 73.550 ms **play** event with 54.816 ms in
`Engine::Update`; it is not the corrected felt candidate.

The next passive trace should split geometry sites `0x1663a8` and `0x166412`
by first/second owner invocation, tag Resource/D3D creation with the active
site or owner gap, and retain only slow `Draw`/`DrawIndexed` records with bound
resource identity. Reuse the draw hook's existing QPC sample and use narrow
per-invocation GPU pairs; do not add per-draw queries or state getters. That
is the evidence needed to choose among warming exact first-use resources,
budgeting colour-scene admission over frames, or briefly deferring exact cold
colour renderables. A bounded lower-mip-first archive path remains plausible
for the texture subset, but cannot alone explain 221 buffer admissions plus
50.893 ms of draw blocking. This does not reopen buffer pooling and does not
justify a shadow/frustum/deferred-renderer rewrite.

Run 70 archives are SHA-256
`4f8dac182590173fd9de4dd5c017dbe2d00c7a8619f23e85716174b178c18509`
for the CSV and
`55bf83319b31272decec52255b0249539f5263a495927f5244313ba37b329c52`
for the during-session log; both matched their live files. The verifier at
the installed checkpoint passes 583 checks and rejects all 716/716 targeted
mutations. Doctor, release build, and the complete off-game self-test passed
before installation; the documented GPU-retirement assertion required its
second rerun. No new binary has been built or installed after Run 70.

**READ FINDINGS §85 BEFORE §84.** Run 70's setup and exact verified call sites
are recorded there; §86 corrects its assumption that one GPU pair per group
would stay inside one owner invocation.

**READ FINDINGS §84 BEFORE §83.** The post-run checkpoint now forwards stock
`Actor::UpdateMeshInstance` if a state-0 queue request cannot be confirmed, or
if it makes the root resident immediately. It skips pose work only for state 1
or state 0 with an observable queue link. This prevents Run 69's 165 repeated
failure observations from becoming a multi-second missing caster. No binary
site or INI setting changed. The verifier passes 456 checks and all 91/91
relevant mutations are rejected; doctor, release build, and the complete
off-game self-test pass. The built but not installed
710,656-byte DLL is SHA-256
`8cd457072f927455b1f8a4bc4a2af3f399f6501235d268a2cd6c21935ad8ffba`.

**READ FINDINGS §83 BEFORE §82. Run 69 completed.** Its five parts are
**menu** 0--2013, **load-game frame** 2014, **loading screen** 2015--3168,
**first world frame** 3169, and **play** 3170--7405. F12 at play frame 6826
has a nearer 65.694 ms `Engine::Update`-class candidate at 6825, but the
normal-route transition pair is 6797/6798 at 95.290/120.984 ms, beginning
850 ms and ending 633 ms before the press. Do not merge them or claim the
marker proves which was felt.

On transition frame 6797, the exact `GraphicsShadowMapDx11::RenderDirectional`
class sees 30 state-0 `GraphicsMeshInstance` Actor roots before pose, confirms
23 new queue insertions, skips all 30 pose updates, and omits all 30 roots at
the later pass-count gate. There are zero directional Resource loads, versus
Run 68's 20 meshes / 16.419 ms plus one shader / 0.963 ms. Directional CPU
falls from 19.318 to 5.146 ms. The targeted synchronous class is gone; this is
stronger evidence than the total pair changing from 242.708 to 216.274 ms.

The remaining pair spends 159.309 ms in the game's `Draw` / `DrawIndexed`
calls and 2.885 ms in `Map`. Frame 6797's whole GPU interval is 168.095 ms:
54.441 ms directional, 10.236 ms terrain ground, 4.798 ms enhanced grass, and
97.007 ms not yet assigned to a named GPU class. Frame 6798 spends 97.651 ms
in draw submission while its own GPU interval is only 28.044 ms, alongside
23 off-main texture creations / 55.814 ms. This is the established
first-use/submission/backpressure class, not remaining synchronous shadow-mesh
loading and not a message-pump or Wine-only explanation.

The checkpoint has corrected one unsafe edge in the tested code. Away from the marked burst, frames
6920--7084 record one unconfirmed state-0 Actor-root enqueue on every frame.
It now forwards stock `Actor::UpdateMeshInstance` whenever
the queue postcondition cannot be confirmed; continue skipping only state 1,
already-queued roots, and confirmed new enqueues. Frame 6797 had no such
failure, so the correction preserves the measured win. The completion message did not
say whether any missing actor or shadow pop was visible; obtain that answer
separately. Then classify the 97.007 ms unnamed GPU interval before proposing
another behavior change.

Run 69 archives are SHA-256
`38f95a3cd21c499e9057432f9447da1f15124e148210889382b4ed48c05a79ae`
for the CSV and
`2b7d1ff73c04fc87926218f0b23a1681d89e7a115fc222086a64bd0ef23fa3ae`
for the during-session log; both matched their live files.

**READ FINDINGS §82 BEFORE §81. Run 69 was the earlier-deferral setup.** Run 68's five parts are
**menu** 0--1897, **load-game frame** 1898, **loading screen** 1899--2967,
**first world frame** 2968, and **play** 2969--7292. F12 at play frame 6654
follows the normal-route pair 6631/6632 at 108.875/133.833 ms, beginning
740 ms and ending 498 ms before the press. A nearer separate candidate is
frame 6644 at 51.206 ms, dominated by 31.117 ms in the `Engine::Update` class;
the marker cannot by itself prove which the reporter felt. Do not collapse
them into one max-selected claim.

Frame 6631 has 20 cold `.msh` Resource calls / 16.419 ms plus one `.ssh` /
0.963 ms inside the exact `GraphicsShadowMapDx11::RenderDirectional` class.
The 20 mesh calls do **not** include their textures. Two Gadir terrain `.tex`
loads / 9.464 ms occur separately in the color-render class, and directional
texture loads are zero. Frame 6631 submits a 200.097 ms whole-GPU interval,
including 101.390 ms in the directional-shadow class and 18.741 ms enhanced
grass. Frame 6632 has no Resource load but blocks 84.977 ms in the game's
`DrawIndexed` calls and 9.459 ms in `Map`. These nested/queued intervals are
not additive.

All 20 cold meshes, plus one at play frame 6641, share the verified deepest
chain `Resource::EnsureAvailable <- GraphicsMeshInstance::UpdatePose <-
Actor::UpdateMeshInstance <- Actor::AddToScene`, under the live DX11
directional bracket. The exact returns are Engine RVAs `0x213137`, `0x1765a2`,
`0x112133`, `0x111fda`, and the enclosing scene-add virtual returns at
`RenderDirectional+0x129b` (`0x18ee1b`). The other logged stack candidates are
the documented raw superset, not callers. This proves the accepted
`GetNumShadowRenderPasses` root gate is too late for this Actor class: pose
work has made the root resident before eligibility is asked. It is not a
stale DX9-only route.

Run 69 changes only the behavior variable
`shadow_defer_cold_actor_pose: 0 -> 1` relative to Run 68. It retargets the
exact `Actor::AddToScene -> Actor::UpdateMeshInstance` call. For a state-0/1
root on the main thread inside the directional class, it queues state 0 and
skips this one pose update; the already accepted exact-class root gate then
omits the still-cold caster until state 2. Color, point shadows, workers,
resident actors, every other Actor update caller, and `shadow_split` remain
unchanged. This may remove both the measured 16.419 ms CPU load and the shadow
draws those newly admitted meshes added; it is not claimed to remove all
242.708 ms of the pair. Reject it for visible missing actors or objectionable
shadow pop.

Both sides are independently verified: a 23-byte caller window and 24-byte
callee window cover the direct call, shared six-byte prologue, exported target,
and `Actor+0x184` field. The verifier passes 454 checks and all 87/87 relevant
one-at-a-time mutations are rejected. Doctor, release build, and the full
off-game self-test pass, including GPU timestamp retirement. The installed
710,656-byte DLL is SHA-256
`29f0f725734c3412e609e1b3b4afb69919c66a0e64d876fedfbd616cda6a6dd5`;
the installed Run 69 INI is SHA-256
`2f75bdc85228220473aa389d7cb9b2dd7769397110a91657676dce9e1776d6a7`.
Both installed/source pairs match. Run 68 live files matched their archives
before removal, both stale live names are absent, and the game was not
launched. Run the same normal route, press F12 after the **play** transition,
and separately report any missing actor or shadow pop.

Run 68 archives are SHA-256
`f68a640d69949ffd4adc9e1ec346ca14939d6eb342754a5bd3a30df33542fec9`
for the CSV and
`0af78f91699c3da0791f3294a321d114e262f27e25cf338967fb3f3e81ceaba9`
for the during-session log. Its **first world frame** is 828.877 ms, so Run
67's 1,352.9 ms spike did not repeat. In matched collision-active full-scene
**play** rows under 60 ms, Run 68 versus Run 67 stays within 0.27 ms in all
reported draw bands; never use an across-run p50.

**READ FINDINGS §80 BEFORE §79. Run 68 was the passive caller trace.** Run 67's
reduced marked **play** transition still loaded 26 state-0 meshes / 23.583 ms
inside the exact directional-shadow class despite twelve base
`GraphicsMeshInstance` root casters already being omitted. Run 68 retained
each mesh's engine filename, queue state, frame/duration, verified immediate
caller, and up to 24 verified call-shaped upstream candidates. Its F12 report
was complete and the result is corrected forward by §81.

**READ FINDINGS §79 BEFORE §78. Run 67 completed and the user says the old
stutter feels much smaller.** Its five parts are **menu** 0--2074,
**load-game frame** 2075, **loading screen** 2076--3441, **first world frame**
3442, and **play** 3443--7604. During the loading screen, 144 exact
`LoadTextures` calls pair one-for-one with the new 144
`TerrainType::PreLoad(true)` calls; there are no false calls.

F12 at **play** frame 7013 anchors frames 6974/6975, 73.472/83.720 ms. The
first frame now has only two outside-directional colour-terrain textures /
5.094 ms, versus 13 / 47.359 ms on Run 66's marked onset, and there is no
second colour-load frame like Run 66 frame 6603. Both survivors are the Gadir
rocky-pebbles base/normal pair. Their exact type was preloaded once during the
**loading screen** but both are state 0 at first colour use; the trace cannot
distinguish never serviced from later eviction. Do not turn the remaining
5.094 ms into periodic whole-layer preload without evidence.

The dominant remaining **play** class is 27 state-0 directional-shadow loads /
24.190 ms (26 meshes / 23.583 ms and one shader / 0.607 ms), a 59.807 ms
directional GPU interval, then frame 6975's 50.607 ms `DrawIndexed` submission
with no Resource load. The existing root behavior omitted 12 exact base-class
casters, so the 26 loads follow another shadow dependency/class. The next
instrument should retain those exact mesh Resources and verified caller chains
at F12 before changing behavior.

The Run 66 observer slowdown is gone. In matched collision-active, full-scene
**play** frames under 60 ms with no Resource load or region change, Run 67
minus Run 63 mean frame time is +0.078 ms at 1,500--1,999 draws and -0.002 ms
at 2,000-plus. Never use an across-run p50.

Do not hide the **first world frame**: Run 67 is 1,352.9 ms versus Run 66's
581.6 ms, with similar main-thread load counts but much larger Resource,
texture-creation, and GPU durations. One sample cannot attribute it, but an
acceptance run must reject a repeatable loading regression even though the
**play** transition improved.

Run 67 archives are SHA-256
`b146a9a0238514d15fddb2bbec61c3d843f79aac103d8c9d4e93a21ee4604f47`
for the CSV and
`1f5cc7b4b6c8ffc1e1bb13f5a54289909125b8f7dcd0598e91341d915043be5a`
for the during-session log.

**READ FINDINGS §78 BEFORE §77. Run 67 is installed.** It implements the
specific missing operation proved by Run 66: after the exact runtime
`TerrainRT::LoadRenderData -> TerrainType::LoadTextures` call returns, invoke
the stock `TerrainType::PreLoad(true)` on that same layer type. The stock body
queues base, bump, and grass Resources through the game's existing loaders and
does not wait. There is no custom loader, draw omission, culling change,
shadow change, or `shadow_split` change.

`terrain_preload_layers` defaults to `0`, reaches `install()` with the
performance probe off, and brings no trace group. Run 67 sets it to `1` beside
the previously accepted `shadow_defer_cold_alpha=1` and the four measurement
settings. The exact call uses `patchCall`; its caller window, original
LoadTextures target, exported PreLoad RVA, 24-byte signature, and relocation
all verify before behavior activates.

The Run 66 observer regression is removed: TerrainPlug/TerrainBlock retain CPU
counts and engine `_us` spans but no longer issue per-call GPU timestamp ends
or expose their two GPU CSV fields. `verify-sites.py` passes 429 checks and all
64 terrain mutations are rejected. Doctor, release build, and the full
off-game self-test pass, including GPU timestamp retirement.

The installed Run 67 DLL is 705,024 bytes, SHA-256
`3171d5301076962d41c67659444d50e1e873eb2bae5d730f6c45510d51c5dd07`.
The installed `cache/runs/run67-terrain-layer-preload.ini` is SHA-256
`e57b0537009088cf6b061322da2b118d4f94a75871e45a619672d194be99fd52`.
Installed/source pairs are byte-identical. The stale Run 66 live CSV/log were
byte-identical to their archives before removal. The game was not launched.

Run the same normal route and press F12 after the old **play** loading
transition. First require successful save loading and correct terrain. Then
use the marker to ask whether Run 66's 13 outside-directional colour-terrain
texture loads / 47.359 ms disappeared; do not compare maxima or Run 66's
observer-inflated steady-state frame time.

**READ FINDINGS §77 BEFORE §76.** Run 66 has all five session parts: **menu**
0--1936, **load-game frame** 1937, **loading screen** 1938--3208,
**first world frame** 3209, and **play** 3210--7046. Successful loading proves
the four-explicit-argument TerrainPlug/TerrainBlock ABI correction fixed the
Run 64/65 observer freeze.

F12 at **play** frame 6618 anchors the old loading-transition class at frame
6601, 109.223 ms, beginning 642 ms before the press. Frame 6617 is a separate
61.014 ms update class with no Resource load. Frame 6601 has 36 main-thread
Resource loads / 62.355 ms: 23 / 14.996 ms inside directional shadow and 13 /
47.359 ms outside it, all render-phase textures.

The affected runtime layer records and their texture Resources already existed
during the **loading screen**. Runtime-owner `TerrainRT::PreLoad` repeatedly
enumerated those layers through **play** frame 6600, but exact
`TerrainType::PreLoad(true/false)` counts remain zero. Ordinary colour render
then synchronously first-used the base/bump/grass Resources on frame 6601.
The defect is the missing runtime-layer semantic preload call. The next narrow
behavior is to call the stock `TerrainType::PreLoad(true)` on the exact layer
immediately after the verified `LoadRenderData -> LoadTextures` call returns.

The user's report of general slowness is also real observer cost. In matched
full-scene **play** frames under 60 ms, Run 66 minus Run 63 mean frame/GPU time
is +8.266/+8.279 ms at 1,500--1,999 draws and +13.184/+13.141 ms at 2,000-plus;
`present_call` is unchanged. The new per-call TerrainPlug/TerrainBlock GPU
scopes issued roughly 221 and 377 immediate-context timestamp ends per frame
in those bands. Their one useful result is captured in §77; both scopes and
their CSV fields are being removed before another run. CPU counts and `_us`
spans remain. Never treat Run 66 steady-state timing as game or fix cost.

Run 66's archived CSV is SHA-256
`515cdb29ffd35d1a7fa35f014da6a4b3c2586ad6c6f93386712515e11eec8951`;
its during-session log is
`91a46af9bd67d423a46f10ca126d3605db41d0de5c9bf1c3b137f9eacd585686`.
The installed DLL/INI still describe the completed Run 66 setup until the next
build is verified and installed.

**READ FINDINGS §76 BEFORE §75. Run 65 disproves §75's proposed freeze
cause.** Run 65 wrote 3,200 completed **menu** frames, 0--3199 (30.260581 s),
then froze in the unfinished **load-game frame**. There is no Run 65 **loading
screen**, **first world frame**, or **play** data. All 17 completed exact
`TerrainRT::LoadRenderData` calls were in the non-main class and the fixed
accessor suppressed every GPU scope, yet loading still froze. The unsafe Run
64 GPU operation was real, but it was not this freeze's cause.

The second audit found the stronger defect. Ghidra's five parameters for each
unexported colour-render function include `this`; both the *TerrainPlug colour
render* class at `0x236240` and *TerrainBlock colour render* class at
`0x23e1e0` end in `c2 10 00` (`ret 0x10`), proving four explicit stack
arguments. Runs 64/65 declared five explicit arguments in addition to `this`,
so either wrapper would pop 20 bytes from a caller that supplied 16 and corrupt
its stack. Both counters are zero in completed **menu** rows; the first call
can occur in the unwritten **load-game frame**. Run 66 corrects only those two
ABIs. Treat causality as a prediction until the save reaches the **loading
screen**.

The verifier passes 424 checks and reads both `ret 0x10` sites directly from
the pinned Engine image. All 56 terrain-chain mutations are rejected,
including the wrapper typedef and each forward independently. Doctor, release
build, and the full off-game self-test pass, including timestamp retirement.

The 704,000-byte Run 66 DLL is installed and matches the build at SHA-256
`97149c6b2d1caeb1f131511c9d1279e63fee2364dc15b8500858bc6982f9ce28`.
The INI was intentionally not recopied; the installed Run 65 INI remains at
SHA-256
`28bb8420cd82d25a8367fe915713a1cbfea1f533d92760aa21ea217c86725b1c`
and has settings identical to the Run 66 record. Run 65's live CSV/log matched
their archives before the stale live names were removed. The game was not
launched.

**READ FINDINGS §75 BEFORE §74. Run 64 aborted; none of its data is play
data.** It wrote 2,162 completed **menu** frames, 0--2161 (21.607161 s), then
froze after save loading began. The in-progress **load-game frame** never
closed, so there is no Run 64 **loading screen**, **first world frame**, or
**play** interval and no CSV row that measures the freeze. The partial CSV and
live debug log are archived under their Run 64 names.

The leading fault is in our new instrument, not the game: the
`TerrainRT::LoadRenderData` class borrowed the active frame's D3D11 immediate
context unconditionally. If save loading reaches that class on a loader
thread, its `End(timestamp-query)` can run concurrently with the render thread
and deadlock the device. The partial Run 64 file cannot prove that thread
identity, so keep this explicitly as a hypothesis until the corrected run
loads successfully. It is not a Wine-only explanation of the native Windows
stutter and says nothing adverse about the game's terrain path.

The installed INI was immediately replaced by the trace-off
`run64-abort-safe.ini`, retaining only accepted
`shadow_defer_cold_alpha=1`. Run 65 makes the narrow correction: both the exact
hook and the shared GPU-context accessor reject non-render threads, while CPU
timing is split into exact `main` and `other` count/duration classes. If Run 65
still freezes, bisect the new runtime-terrain trace group; do not assume this
diagnosis was proved.

The first corrected self-test caught the 317-field header exceeding its old
8 KiB line buffer. Header and rows now use one verified 16 KiB bound. The
verifier passes 422 checks and all 53 terrain-chain mutations are rejected.
Doctor, release build, and the complete off-game self-test pass, including a
real-frame proof that the render thread receives its GPU context while a
worker receives null, plus timestamp retirement.

Run 65 is installed. Source and installed DLLs are byte-identical: 704,000
bytes, SHA-256
`d0cc1c760309372f5ef5b50b0aab2e9e384fe231120716c4e305ea18730c5f98`.
Source and installed INIs are byte-identical at SHA-256
`28bb8420cd82d25a8367fe915713a1cbfea1f533d92760aa21ea217c86725b1c`.
The stale live Run 64 CSV/debug log matched their archives before removal; the
game was not launched. First establish that the save reaches the **loading
screen**, then run the normal route and press F12 after the felt **play** burst.

**Withdrawn Run 64 setup record -- §75 above corrects it.** Run 63 has 7,162
contiguous frames. Its five session parts are **menu** 0--1831 (17.560 s),
**load-game frame** 1832 (1.492884 s), **loading screen** 1833--2970
(10.244389 s), **first world frame** 2971 (845.355 ms), and **play**
2972--7161 (63.970227 s).

F12 at **play** frame 6565 anchors the full-scene, collision-active **play**
loading candidate at frame 6544, 180.607 ms, beginning 688.157 ms before the
press. Frame 6545 is its 50.615 ms recovery. Frame 6564 at 77.565 ms is a
separate `Engine::Update`-class candidate with no Resource load; never merge it
with the loading transition merely because it is closer to F12.

**Play** frame 6544 spends 170.045 ms in the `Engine::Render` class and
101.715 ms in 35 main-thread Resource loads. The
`GraphicsShadowMapDx11::RenderDirectional` class owns 22 / 22.238 ms; the
colour-terrain path outside it owns 13 / 79.477 ms, all state-0 textures. The
frame submits 185.804 ms of GPU work, including overlapping named classes of
33.244 ms directional shadow, 26.341 ms enhanced grass, and exactly 54.213 ms
in the DX11 `TerrainRenderInterfaceRT::RenderGround` class. Do not add those
nested CPU and GPU intervals.

The decisive Run 63 result is that `TerrainType::PreLoad` ran zero times in
**menu**, **load-game frame**, **loading screen**, **first world frame**, and
**play**. The group was active: `TerrainType::SetShaderParams` ran 235,861
times, `TerrainType::SetGrassShaderParams` 54,722 times, and the exact DX11
ground class 9,159 times. All six exact `TerrainType` identities behind the 13
cold **play** textures therefore had no true or false semantic-preload history.

Static analysis then corrected the class boundary. The game uses the
unexported runtime `TerrainRT` vtable at Engine RVA `0x2f8820`, not the
exported editor `Terrain` implementation. Runtime `TerrainRT::Load`
(`0x23d8d0`) creates 12-byte layer records but no texture Resources.
`TerrainRT::LoadRenderData` (`0x23d6d0`) directly calls
`TerrainType::LoadTextures` at `0x23d742` before creating opacity/render data.
`TerrainRT::PreLoad` (`0x23d400`) walks nearby TerrainObjects but directly
calls neither `TerrainType::PreLoad` nor `LoadTextures`. Later ordinary colour
binding is owned by unexported TerrainPlug (`0x236240`, setter call
`0x2366cd`) and TerrainBlock (`0x23e1e0`, setter call `0x23e73f`). This is a
concrete missing layer-preload path, not evidence that the terrain, culling,
or shadow system needs a rewrite.

Run 64 is one passive trace of that whole chain. The trace-only terrain group
is atomic across nine detours and one exact `patchCall`: runtime `Load`,
`LoadRenderData`, `PreLoad`, its exact `LoadTextures` call, TerrainPlug,
TerrainBlock, and the four prior TerrainType/DX11 ground hooks. It changes no
loading, rendering, culling, or shadow behavior. Per exact `TerrainType*`, F12
retains first/last/count for successful layer attachment, completed
`LoadTextures`, and runtime-owner PreLoad. CSV `_us` columns time every CPU
boundary; `gpu_terrain_rt_load_render_ms`, `gpu_terrain_plug_ms`, and
`gpu_terrain_block_ms` are non-blocking game-time spans and are not charged to
the mod.

The durable static map is `research/streaming/disassembly-targets.md`. It also
records the generic Resource chain, directional-shadow construction,
`GraphicsMeshInstance` root/alpha paths, and material texture use. Both Ghidra
seed sets were extended and regenerated: the streaming export now has 1,483
Engine functions / 189 roots, and the shadow export has 695 / 70.

All shared `55 8b ec 83 e4 f8` entries verify 19--23 bytes and steal six;
runtime `LoadRenderData` verifies 20 and steals eight. The verifier passes 419
checks and caught two accessor transcription errors before installation. All
46 new one-at-a-time RVA, name, vtable-slot, relocation, byte, offset, bound,
event, unit, counter, and GPU mutations are rejected. Doctor, release build,
and the full self-test pass, including timestamp retirement.

Run 64 is built from `cache/runs/run64-terrain-runtime-chain.ini`. Source and
installed DLLs are byte-identical: 703,488 bytes, SHA-256
`d1e9edd8a15bac0145cac6fd2e29d2ad92f66227aad8e1d7416fb2c5a7ac7a9f`.
Source and installed INIs are byte-identical at SHA-256
`3368b138e9d935716a236f330dde5fbeb0aab5912d707cb60ecfb0bf3c6d025b`.
The stale live Run 63 CSV/log matched their archives before removal. The game
was not launched. Run the normal route and press F12 after the felt **play**
loading burst; a late press is safe.

**Run 63 completed; its setup record follows -- read findings §74, then §73,
before §72.** Run 62 has 7,202 contiguous frames. Its five session parts are
**menu** 0--1734 (16.507 s), **load-game frame** 1735 (1.540 s), **loading
screen** 1736--2900 (10.053 s), **first world frame** 2901 (758.028 ms), and
**play** 2902--7201 (66.877 s).

F12 at **play** frame 6455 anchors the large full-scene, collision-active
**play** transition at frames 6441/6442, 219.289/125.131 ms. The pair begins
641 ms before the press and ends 296 ms before it. Frame 6454 is a separate
56.144 ms update-class candidate ending 20 ms before F12; do not merge it with
the loading pair merely because it is closer to the reaction key.

Frame 6441 has 33 main-thread Resource loads / 66.163 ms, partitioned exactly
as 20 / 17.973 ms in the `GraphicsShadowMapDx11::RenderDirectional` class and
13 / 48.190 ms outside it. All 13 outside loads are state-0 render-phase
terrain textures. It creates 15 textures, 172 buffers, and 10 shaders, spends
123.531 ms inside D3D draw submission, and places 302.910 ms of whole-frame
work on the GPU. Named GPU classes include 90.649 ms directional shadow and
20.324 ms enhanced grass, but do not classify most of that interval.

Frame 6442 has no main-thread Resource load and only 22.267 ms of its own GPU
work, but blocks 104.021 ms inside D3D `Draw`/`DrawIndexed` while rendering a
nearly identical scene. Frame 6443 is normal again. This is the exact
resource/first-use submission plus next-frame GPU queue-backpressure shape
from §37, now tied to the felt **play** transition. It does not require a
Wine-only cause: D3D resource first use and queue backpressure exist on native
Windows too, although the translation layer can change their cost.

The upstream trace names the colour-terrain class exactly. Ordinary layers
come from exported `TerrainType::SetShaderParams`; the grass mask comes from
`TerrainType::SetGrassShaderParams`, under the DX11
`TerrainRenderInterfaceRT::RenderGrass` class. Both force
`Resource::EnsureAvailable` before reading and binding resident texture
fields, so simply skipping that call has unproven visible fallback semantics
and is bounded by 48.190 ms of the 344.420 ms pair.

The engine already exports `TerrainType::PreLoad(bool)`. Its verified body
shows `false` skips all work and `true` walks exactly that `TerrainType`'s base,
bump, and grass texture Resources, queueing/touching them through engine-native
paths. Static code does not show whether it ran for the exact types forced on
frame 6441. Run 63 is passive: it records true/false preload history per exact
`TerrainType`, snapshots it when either shader-parameter method forces a load,
and adds CPU/GPU spans for the DX11
`TerrainRenderInterfaceRT::RenderGround` class. This answers whether the
semantic preloader was unused/late and whether the currently unnamed GPU
first-use interval is ground work before any fix is proposed.

The four trace detours are atomic. `TerrainType::PreLoad` and
`TerrainRenderInterfaceRT::RenderGround` share the common six-byte prologue,
so each verifies 24 bytes and steals six. The two parameter methods verify 21
bytes and steal their first two complete instructions, eight bytes. Retention
uses a fixed 2,048-slot identity table; F12 performs the log formatting after
the candidate. All engine durations end in `_us`; `gpu_terrain_ground_ms` is
game time. The group installs only with the performance trace and changes no
rendering or resource behavior. The accepted `shadow_defer_cold_alpha=1`
behavior remains on for the run and `shadow_split` is untouched.

The run-62 CSV/debug archives have SHA-256
`33a1bc8eda5e59984c1803c2e34d5dd3fa191c751071a4726c16edcd62379912`
and `97d64a35c58e5bfea9eda5f7a2fa113df35eefeed36ec9ff8602dd564653b1ea`.
The run-63 verifier passes 377 checks and rejects all 23 new one-at-a-time
target, relocation, signature, table, install, association, and GPU-phase
mutations. Doctor, the release build, and the complete off-game self-test pass,
including GPU timestamp retirement. The 697,344-byte DLL SHA-256 is
`8d44db8931aa8eee9b68bf2ca75496b26e0e55d3202d94ac10ed30b3df80860a`.
Run 63 is built from `cache/runs/run63-terrain-preload-ground.ini`. Installed
and source DLLs match that hash; installed and source INIs match at SHA-256
`8fdbe65f72d77da088150b43189462f65b617dae0bdcc2d0ab6b164bfb5fb303`.
The stale live run-62 CSV/log were byte-checked against their archives before
removal. The game was not launched. Run the normal route and press F12 after
the felt **play** loading burst; a late press remains safe.

**Run 61 completed; run 62's setup record follows -- read findings §72 before
§71.** Run 61 has 7,135
contiguous frames. Its five session parts are **menu** 0--1768 (17.088 s),
**load-game frame** 1769 (1.565 s), **loading screen** 1770--2909 (10.129 s),
**first world frame** 2910 (686.861 ms), and **play** 2911--7134 (64.397 s).

F12 at **play** frame 6560 is a late reaction anchor for a contiguous
two-frame, full-scene, collision-active **play** burst: frame 6522 is 175.182
ms and frame 6523 is 127.451 ms. Together they start 1.034 s before F12 and
end 731 ms before it. The event is the pair, not whichever row has the larger
`max()`.

Frame 6522 spends 166.108 ms in `Engine::Render`. Its 33 main-thread Resource
loads / 49.683 ms partition exactly into 20 / 22.772 ms inside
`GraphicsShadowMapDx11::RenderDirectional` and 13 / 26.911 ms outside it. All
13 outside calls are render-phase state-0 texture Resources whose filenames
are terrain layers. The same frame carries a 254.833 ms whole-GPU interval,
including 90.117 ms in the `GraphicsShadowMapDx11` directional class and
23.490 ms in the enhanced-grass class. These nested/overlapping intervals are
not additive.

Frame 6523 is 127.451 ms and spends 119.320 ms in `Engine::Render`, despite no
outside-directional load and only one 7.194 ms texture load inside the
directional-shadow class. Its own GPU interval is 28.272 ms. That resembles
§37's next-frame `DrawIndexed` drain of the prior GPU backlog, but run 61 did
not arm draw timing; run 62 measures the same pair before adopting that label.

Run 61's immediate caller `E+0x213137` is real but unhelpful. In the pinned
Engine.dll it is the instruction after the call at RVA 0x213132 inside
exported `Resource::EnsureAvailable` (0x2130f0), and that call targets exported
`ResourceLoader::LoadResource` (0x213ed0). It is the generic wrapper, not the
upstream terrain-renderer owner. Do not propose colour-terrain deferral from
the filenames alone: the measured synchronous portion is only 26.911 ms of a
302.633 ms event and its visible fallback semantics are still unknown.

Run 62 changes no behavior from run 61. The existing 128-record reaction ring
now also retains up to 24 call-shaped return candidates from verified
Engine.dll, Game.dll, and TQ.exe text for each outside-directional load. Since
the engine has no frame pointers this is a bounded raw-stack superset, not a
claimed call stack; use repeated sequences plus static disassembly to select
the real increasing-order path. F12 still does the formatting/logging after
the candidate. `draw_timing=1` re-enables the already validated game-call
timers so frame 6523 can be tested directly. The accepted
`shadow_defer_cold_alpha=1` behavior stays on and `shadow_split` is untouched.

The verifier passes 353 checks. All 16 new one-at-a-time depth, stack,
committed-range, bound, filter, store, duplicate, format, and log mutations are
rejected, 150/150 cumulative relevant mutations with run 61. Doctor, release
build, and the full off-game self-test pass, including GPU timestamp
retirement. Run 62 is built from
`cache/runs/run62-outside-directional-upstream-draw.ini`; its only additional
switch relative to run 61 is diagnostic `draw_timing=1`. The run-61 CSV and
debug log are archived byte-identically at SHA-256
`18465ad1ce602ee43195b45ef7247812f933d72cb09664990ea2b2cdc40efaff` and
`c92d7b205b5680dfd9d883074515034769556906ae854207f88fd83cbed43412`.
The installed/source run-62 hashes are
`790fb0e57280a7486d8ee75123084603222fece16c407cfeacbd3994f95382b0`
for the 692,736-byte DLL and
`1838456d57220419d652bcb78201358e3223c0847963e92d418768825e048786`
for the INI. The stale live run-61 names were removed after the byte checks;
the game was not launched.

**Run 61 is installed -- read findings §71 before §70.** Run 60 has 7,131
contiguous frames. Its five session parts are **menu** 0--1782 (16.542 s),
**load-game frame** 1783 (1.507 s), **loading screen** 1784--2865 (9.501 s),
**first world frame** 2866 (767.918 ms), and **play** 2867--7130 (64.725 s).

F12 at **play** frame 6481 follows full-scene, collision-active **play** frame
6461 at 260.847 ms, ending 417 ms before the press. The accepted exact
`GraphicsMeshInstance::GetNumShadowRenderPasses` behavior fired on six cold
state-0 roots, queued all six, and its omitted population cleared within three
subsequent directional builds. The reporter saw no missing or popping shadow.
Keep this narrow root-mesh fix, but do not infer a cross-run millisecond win.

The remaining burst is not primarily inside directional shadow. Its 30
main-thread Resource loads carry 102.518 ms of summed complete-call duration;
only 17 / 23.383 ms are inside
`GraphicsShadowMapDx11::RenderDirectional`. The other 13 / 79.135 ms have no
class or caller in run 60. Directional CPU/GPU are 25.145/30.071 ms versus
217.391 ms in `Engine::Render` and a 237.965 ms whole-GPU interval. These
nested and overlapping durations are not additive. The 31.635 ms pump call
returned messages and render is independently dominant, so the twice-closed
pump route stays closed.

Run 61 is passive and reuses the existing verified LoadResource, Update,
Render, and RenderDirectional hooks plus the verified Resource state/name
accessors. New CSV `_us` pairs partition every main-thread load outside the
live directional bracket by render/update/other and, independently, by
mesh/shader/texture/other. The two sums must each equal the common outside-dir
pair; together with the directional pair it must equal the existing
main-thread pair. Complete LoadResource calls can nest and are not added to
their enclosing phase.

Each matching load is retained in a fixed ring during the frame, not logged.
F12 then writes the preceding 120 frames with exact frame, pre-call state,
phase, type, copied engine filename, and immediate caller. Module+RVA is shown
only when the return address lies in verified Engine.dll/Game.dll/TQ.exe text
and follows a valid call instruction; otherwise it says `unverified`. The F12
row and live log explicitly report if the 128-record ring truncated that
window. Later F12 presses emit only new events. No new patch site or behavior
change was added; `shadow_defer_cold_alpha=1` remains enabled and
`shadow_split` is untouched.

The verifier passes 351 checks. All 25 new one-at-a-time constant, gate,
capture, ring, window, caller, bracket, activation, F12, and shutdown mutations
are rejected (134/134 cumulative relevant mutations). Doctor, release build,
and full off-game self-test pass, including GPU timestamp retirement. Run 61
is installed from `cache/runs/run61-outside-directional-resources.ini`.
Installed/source hashes are
`47fbd6acff07fd4b97598edb8e448f181fca9f10cbf2512def30e1a162a2f20b`
for the 692,224-byte DLL and
`ed7a07f068851749af8f0b558188502cf6a83583fe63f7cd87625365c17513f7`
for the INI. The run-60 live CSV/log were byte-checked against their archives
before removal. The game was not launched. Run the normal route and press F12
after the felt **play** loading burst; pressing more than once is safe.

**Run 60 completed -- read findings §70 before §69.** It has 7,131 contiguous
frames. Its five session parts are **menu** 0--1782 (16.542 s), **load-game
frame** 1783 (1.507 s), **loading screen** 1784--2865 (9.501 s), **first
world frame** 2866 (767.918 ms), and **play** 2867--7130 (64.725 s).

F12 at **play** frame 6481 follows the one full-scene, collision-active
**play** candidate within two seconds: frame 6461 at 260.847 ms, ending 417 ms
before the press (onset 678 ms before it). The new exact
`GraphicsMeshInstance::GetNumShadowRenderPasses` deferral fired on six state-0
root meshes and enqueued all six. Eight roots were still omitted on frame
6462, six on 6463, and none on 6464. Across **play**, 27 root-mesh omissions
are 26 state 0 and one state 1; ten state-0 roots were newly enqueued and no
enqueue failed. The reporter saw no missing or popping local shadow. This
validates the narrow visual trade and its lifecycle, though route variation
means it does not by itself measure how much shorter the burst became.

The marked frame also shows where not to keep widening the shadow omission.
It spends 102.518 ms in 30 main-thread Resource loads, but only 23.383 ms / 17
loads are inside `GraphicsShadowMapDx11::RenderDirectional`: 16 meshes /
21.923 ms and one shader / 1.460 ms. The other 13 main-thread loads consume
79.135 ms outside directional shadow. Directional shadow CPU is 25.145 ms and
its GPU interval is 30.071 ms, versus 217.391 ms in `Engine::Render` and a
237.965 ms whole-GPU interval. Those nested and overlapping intervals must not
be added. The 31.635 ms `PeekMessageA` time returned two messages across three
peeks; it is not run 39's empty-peek shape, and the independently dominant
render work leaves the twice-closed pump route closed.

Keep the root-mesh fix. The next useful step is passive attribution of the 13
main-thread Resource loads outside directional shadow by exact Resource class
and verified immediate caller. Do not broaden caster omission until that
larger class is named. A new draw-timing boot would only repeat §37: the
remaining whole-GPU/render interval is known to surface as `DrawIndexed`
backpressure but not yet tied to the resources or scene work that caused it.

Across **play**, directional shadow performs 75 synchronous loads / 75.728 ms:
70 meshes / 72.173 ms and five shaders / 3.555 ms, all entered in state 0;
textures remain zero. All 3,046 directional builds report the context patch
active and every context, table, and enqueue failure is zero. The texture
omissions from runs 56--59 remain complete.

The run-60 CSV and live-written debug log are archived and byte-identical to
their live names. Their SHA-256 values are
`87bcb615b8d81fc037f407b89e41246144361369b76be458d735c0431e536a03`
and `ab56a4629a000ad382e9013ac77e1db4e0e265768d409b31c4271ba446636697`.
The installed build is still the verified run-60 build described in §69. The
game was not launched by the mod workflow.

**Run 59 completed -- read findings §68 before §67.** It has 7,547 contiguous
frames. Its five session parts are **menu** 0--1816 (16.947 s), **load-game
frame** 1817 (1.485 s), **loading screen** 1818--3077 (10.781 s), **first
world frame** 3078 (934.609 ms), and **play** 3079--7546 (69.075 s).

F12 at **play** frame 6710 follows full-scene, collision-active **play** frames
6692/6693 at 218.733/149.357 ms, ending 488/339 ms before the reaction. The
new base-override omission worked exactly: directional shadows loaded no
textures anywhere in **play**. It skipped 737,873 overwritten generic
`baseTexture` bindings, 3,535 of them cold. The earlier bump and unused
material filters also stayed complete. No context failure, enqueue failure,
or table overflow fired.

The remaining onset is no longer texture-shaped. Frame 6692 loads 29 shadow
meshes / 38.219 ms and one shader / 2.843 ms inside a 41.139 ms directional
CPU call. Five cold root meshes at
`GraphicsMeshInstance::GetNumShadowRenderPasses` account for 11.111 ms. The
same frame has a 137.047 ms directional GPU interval and 329.861 ms whole-GPU
interval. Frame 6693 has no resource load and only 1.928 ms directional CPU,
but spends 133.489 ms in `Engine::Render` while its own GPU interval is only
19.067 ms. Frame 6692 completed on the CPU in 218.733 ms, leaving about 111 ms
of its GPU work queued; that backlog plus a normal following render accounts
for frame 6693 and reproduces §37's already measured `DrawIndexed`
backpressure mechanism. Another draw-timing-only boot is not required.

Across **play**, the only remaining directional resource class is 100 meshes /
100.932 ms and five shaders / 4.990 ms. The next defensible behavior test is
at the already verified root-mesh pass-count boundary: for an exact
`GraphicsMeshInstance` root mesh in state 0/1, enqueue state 0 and return zero
passes until state 2. That omits one cold caster rather than a whole map and
can reduce both its synchronous CPU dependencies and its GPU draws. It must
remain directional-only; the colour pass, resident casters, `shadow_split`,
map size, and culling stay unchanged. No visual observation accompanied the
run-59 completion, so do not infer artifact safety from silence.

The run-59 CSV and live-written debug log are archived and byte-identical to
their live names. Their SHA-256 values are
`e74d2a84ba1c27688b0c0f5e6e3f7d71ae1b99eb532238de530f0385d211807c`
and `1f4b413788a4a1de0399bd9bcb60f2d3dc999a7c138d3f965bbacfbde8bbbe01`.

**Run 59 is installed -- read findings §67 before §66.** Run 58 has 7,286
contiguous frames. Its five session parts are **menu** 0--2011 (18.673 s),
**load-game frame** 2012 (1.566 s), **loading screen** 2013--3111 (9.405 s),
**first world frame** 3112 (864.484 ms), and **play** 3113--7285 (62.946 s).

F12 at **play** frame 6705 is a reaction anchor. The full-scene,
collision-active **play** loading pair is frames 6687/6688 at 269.817/57.089
ms, ending 401/344 ms before the press. Frame 6687 loads 23 directional meshes
/ 30.392 ms and one shader / 0.882 ms inside a 33.569 ms directional CPU call,
beside an 87.737 ms directional GPU interval and 275.310 ms whole-frame GPU
interval. Frame 6688 loads seven directional textures / 10.868 ms inside a
13.128 ms directional CPU call; all seven are direct mesh-material
`baseTexture` entries. CPU and GPU intervals overlap and are not additive.

Run 58 proves the bump omission worked. In **play** it skipped 561,588 unused
bump bindings, including 2,958 state-0 Resources. There is no unresolved
directional-shadow texture load anywhere in **play**. The formerly unresolved
class was removed, not displaced. The marked burst still starts with the much
larger mesh/GPU frame; do not claim the texture omission fixes that half.

The only remaining directional texture population in **play** is fourteen
cold direct material textures / 26.215 ms. Every one is an exact accepted
`GraphicsMeshInstance` join, exact `Name("baseTexture")`, and `base_other`.
The marked second frame contains seven / 10.899 ms: three style 3, four style
4; six pass 0, one other.

Run 59 removes only that redundant generic binding. The verified stock method
first invokes the generic `GraphicsMesh` material setter, then reads the same
live instance+`0x14`, ensures that override Resource, and binds it to the exact
static `Name("baseTexture")` before the draw. The getter is omitted only on the
main thread inside `RenderDirectional`, only for the exact 16-byte Name, only
with a non-null live override, and only when its Resource differs from the
generic Resource. The temporary null is therefore replaced by the verified
stock override. Every other getter forwards unchanged; worker and colour calls
do not expose or overwrite the context.

Colour rendering, meshes without an override, identical pointers, required
bump inputs, alpha base deferral, opaque geometry, culling, map size, and
`shadow_split` are unchanged. The change remains part of the default-off
`shadow_defer_cold_alpha=1` fix, reaches `install()` with the performance probe
off, and brings no trace group. The two new CSV fields are counts, not duration
columns.

The verifier passes 328 checks. Twenty-five one-at-a-time constant, byte,
relocation, pointer-predicate, context, forwarding, and atomic-install
mutations are rejected: 25/25 new and 89/89 cumulative relevant mutations.
Doctor, release build, and the full off-game self-test pass, including GPU
timestamp retirement. Run 59 is installed from
`cache/runs/run59-shadow-base-override.ini`; its DLL is byte-identical to the
release build at SHA-256
`2d1af22215fd48c5781603a77637cc59b645d9975385630456fb77add3823c20`.
The installed INI is byte-identical to the run file at SHA-256
`1511b3c95d46fdfdd3d1016b55f5d6bde64148e566e2ebbfef1ce21d511a619c`.
The archived run-58 live CSV and debug log were byte-checked before their stale
live names were removed. The game was not launched. Run the normal route,
press F12 after a felt loading burst, and report any shadow or scene pop even
if it occurs seconds later.

**Run 57 was installed -- read findings §65 before §64.** Run 56 has 7,436
contiguous frames. Its five session parts are menu 0--2007 (18.606 s),
load-game frame 2008 (1.543 s), loading screen 2009--3128 (9.760 s), first
world frame 3129 (1.442 s), and play 3130--7435 (65.053 s).

F12 at **play** frame 6860 is deliberately unresolved because the reporter
may have pressed late. Full-scene collision-active **play** frames 6831/6832
last 222.567/61.223 ms (283.790 ms total), ending 652/591 ms before the
marker. A separate full-scene collision-active **play** frame 6857 lasts
60.787 ms, spends 42.866 ms in `Engine::Update`, and ends 56 ms before the
marker. Preserve both; do not choose by proximity alone.

The repaired passive context is now proved. `context_patch_active` equals all
4,245 directional builds in the session and all 3,039 in **play**, every
failure status is zero, all per-row partitions balance, and context-table
overflow is zero. All eight cold shader-used material textures / 15.176 ms in
**play** join exact pass-0, style-3 alpha-tested `GraphicsMeshInstance`
records. All eight are `base_other`: the current omission waits for the base
texture, then returns the caster while another required material resource is
still cold.

That result explains only the smaller direct material class. Frame 6831 loads
15 directional-shadow meshes / 23.934 ms, with two / 10.162 ms at
`GraphicsMeshInstance::GetNumShadowRenderPasses`. Frame 6832 loads five
textures / 14.458 ms: two direct material / 3.604 ms and three unresolved /
10.854 ms. Across **play**, eleven unresolved textures / 45.254 ms exceed the
eight direct material textures / 15.157 ms. Do not widen the behavior around
the 15 ms class while the 45 ms class remains unnamed.

Run 56's `outer_other=8` is a probe-label artifact, not evidence of another
game caller. When the context `patchCall` is active, its C wrapper necessarily
replaces the original `GraphicsMeshInstance` return address visible inside
`GraphicsMesh::SetShaderParameters`. Run 57 treats an active wrapper context
as that same verified site.

Run 57 is passive relative to run 56. With `trace=1`, it writes at most eight
cold used material identities (verified `Name::Hash`, resource filename,
style/pass/base/join) and at most eight unresolved texture filenames with
bounded call-shaped stacks across the audited three modules. Each line is
written during the session, not at unload. No rendering or resource behavior
changes; `shadow_split` remains untouched. Press F12 after a felt loading
burst.

The verifier passes 293 checks. All seven new one-at-a-time RVA, byte, export,
gate, bound, and wrapper perturbations are rejected (43/43 cumulative relevant
mutations). Doctor, release build, and full off-game self-test pass, including
GPU timestamp retirement. Run 57 is installed: DLL and INI are byte-identical
to their sources, DLL SHA-256 is
`d84aea5579e093fe4daad2b610af702870b7bd1ec1c98bcc0dc90ecd6bf27427`,
and the archived run-56 live CSV plus stale debug log were cleared. The game
was not launched.

**Run 56 is installed -- read findings §64 before §63.** Run 55 has 7,257
contiguous presented frames. Its five session parts are menu 0--1932
(17.968 s), load-game frame 1933 (1.429 s), loading screen 1934--3053
(9.892 s), first world frame 3054 (672.733 ms), and play 3055--7256
(63.210 s). F12 at **play** frame 6698 follows full-scene, collision-active
**play** frames 6679/6680, 225.310/195.514 ms (420.824 ms total), ending
553/357 ms before the press.

The pair is again mesh/shader then texture work. Frame 6679 loads 27
directional-shadow meshes / 31.177 ms and two shaders / 1.796 ms, inside a
33.337 ms `GraphicsShadowMapDx11::RenderDirectional` call and beside a
128.058 ms directional GPU interval. Frame 6680 loads 19 textures / 49.604 ms:
eight direct mesh-material / 22.157 ms and eleven unresolved / 27.447 ms,
inside a 51.954 ms directional call and beside a 59.598 ms directional GPU
interval. These intervals overlap.

Run 55's caller diagnostic is decisive: all 14 cold shader-used material calls
in **play** return through the one verified direct base
`GraphicsMeshInstance::SetShaderParameters` site; `outer_other_site` is zero.
But all 2,958 actual directional builds report
`context_patch_frame_mismatch`, so no instance/pass context was ever captured.
That mismatch is now byte-explained. The 19-byte signature has `A1` at byte 6;
its dword at bytes 7--10 is preferred-base VA `0x1041b044`. At runtime
Engine.dll is rebased, but the signature omitted the relocation descriptor.
The on-disk verifier used the preferred base and therefore passed, while
runtime `detour::matches` correctly rejected the literal. This is an
instrumentation defect, not evidence of an alternate caster class or caller.

Run 56 adds exactly the missing relocation `{7, 0x41b044}` to that passive
signature. It changes no rendering or resource policy and makes no added
engine call; the color pass, opaque casters, alpha-tested
`GraphicsMeshInstance` behavior from run 51, `shadow_split`, map size, and
culling are unchanged. The required outcome is
`context_patch_active == engine_shadow_render`, with every other patch-status
bucket zero, followed by honest style/pass/base classification of cold used
materials. Press F12 after a felt loading burst.

The verifier passes 286 checks and rejects the relocation offset, target, and
encoded-operand perturbations individually (36/36 cumulative relevant
mutations). Doctor, release build, and the full off-game self-test pass,
including GPU timestamp retirement. Run 56 is installed: DLL and INI are
byte-identical to their build/cache sources, the DLL SHA-256 is
`650e8dd5b69a9e397d7d21aa14de44a78e86db6f7e325e98fc30618ec21f8d9c`,
and the archived run-55 live CSV was removed. The game was not launched.

**Run 55 is prepared -- read findings §63 before §62.** Run 54 completed
normally. The reporter confirms F12 at **play** frame 6913 followed the large
loading transition at frames 6897/6898, 251.589/147.926 ms (399.515 ms total),
ending 508/361 ms before the press. This also resolves run 53: its marker was
roughly 0.7 seconds after its large loading pair, not a report of the later
59.411 ms update frame.

The run-54 transition again splits into mesh/shader then texture work. Frame
6897 loads 27 directional-shadow meshes / 32.099 ms and one shader / 2.584 ms,
beside a 140.789 ms directional GPU interval. Frame 6898 loads 13 textures /
31.988 ms: six direct mesh-material / 11.517 ms and seven unresolved / 20.471
ms. Across **play**, all 14 cold material textures / 31.824 ms are shader-used.

All 14 also have `pass_unknown`, proving the verified base
`GraphicsMeshInstance::SetShaderParameters` adapter context was absent. The
simultaneous `lookup_exact=14` is **withdrawn**: a zero-initialized enum named
missing instance context `Exact`. Run 55 maps it to `instance_missing`, emits
one exact context-patch status for every actual directional build, and records
whether each cold used material call returns to the verified direct base-class
site or another caller. It adds no engine call and changes no rendering or
resource behavior. The run file is
`cache/runs/run55-shadow-context-install-caller.ini`; settings remain identical
to runs 51, 53, and 54.

Two new 23/24-byte windows verify the enclosing-caller stack offset; the
verifier passes 285 checks and all eight new mutations are rejected (33/33
cumulative relevant mutations). Doctor, release build, and full off-game
self-test pass, including GPU timestamp retirement. Run 55 is installed: the
DLL matches the release build at SHA-256
`502ff9d2044196d94d104c995e8cca613dddea79134c2e43c2840dc805074a48`,
the installed INI matches the run cache copy, and the archived run-54 live CSV
was cleared. Do not launch the game from a shell.

**Run 54 was prepared -- read findings §62 and §61 before §60.** Run 53 safely
completed through the loading transition, confirming the corrected adapter.
Its F12 marker at **play** frame 7075 is ambiguous. The nearest full-scene
candidate is frame 7074 / 59.411 ms, beginning 75 ms before the marker; it
spends 41.787 ms in `Engine::Update` and has no resource load. The earlier
full-scene loading transition, frames 7041/7042 at 254.638/87.588 ms, ended
772/685 ms before the marker. Preserve both until the reporter says which
timing matches the press.

The loading pair still has two classes. Frame 7041 loads 23 directional-shadow
meshes / 40.094 ms plus one shader / 0.734 ms, beside a 103.536 ms directional
GPU interval. Frame 7042 loads 12 shadow textures / 27.086 ms: five direct
mesh-material loads / 11.291 ms and seven unresolved / 15.795 ms. Across
**play**, all 12 cold material textures / 32.791 ms are shader-used, but all
are `context_unknown`; do not call them alpha or opaque.

Run 54 changes no rendering behavior and calls no additional engine method.
It stores every accepted renderable identity for only the current
`GraphicsShadowMapDx11::RenderDirectional` call. A rare cold used material
event is classified as exact base-class `GraphicsMeshInstance`,
derived/overriding class, same instance with a different pass, or no accepted
record. Only a failed cold lookup scans the fixed table; table overflow is
explicit. Style, base, pass, and lookup-reason partitions each have their own
unknown bucket and must balance independently. The run file is
`cache/runs/run54-shadow-context-miss.ini`; its settings remain identical to
runs 51 and 53. The verified adapter still supplies the actual material-call
pass when the record join misses; `pass_unknown` is reserved for an absent
adapter context. The verifier now passes 279 checks. The prior 24 perturbations
remain covered, and independently changing the new table-size constant is
rejected, for 25/25 relevant mutations. Doctor, release build, and the full
off-game self-test pass. The installed DLL is byte-identical to the release
build (SHA-256 `0c7da90d9aa7e35090c8832b102093e957461281b8e1907e9517bdf072116d8f`),
the installed INI is byte-identical to the run-54 cache copy, and the stale
live CSV was removed. Do not launch the game from a shell; it is ready for the
reporter to launch from the CrossOver UI.

**Run 53 is measured -- read findings §61 before §60. Run 52 is withdrawn.**
Both run-52 attempts froze at the prior transition/burst. The captured second
attempt ends at ordinary **play** frame 6598 / 8.248 ms; the freezing frame
never completed Present, and every new attribution counter was still zero.
This is a regression in the instrument, not evidence that the original game
burst became a permanent freeze.

The exact error is byte-visible: EBP began as arg3/pass, but immediately before
the patched call the original code multiplied it by the 0x34 render-info
stride and added the render-info base. The adapter nevertheless passed that
`MeshRenderInfo*` as the pass, and the first cold lookup used it as an index.
Run 53 removes both nested engine calls from the material path. It now
captures `GraphicsMeshInstance` style/pass identity at the earlier run-51
shadow-record gate, where those methods were already exercised safely, and
relates it to the later material call through a fixed generation-keyed table
valid only within one `GraphicsShadowMapDx11::RenderDirectional` call. Alpha
styles 3--5 retain the base identity already fetched by the run-51 gate;
opaque styles 0--2 report `base_unknown` rather than causing another lookup.
No stored pointer is dereferenced in the material path.

Run 53 remains behavior-identical to run 51 and keeps the independent
read-only direct-caller partition. Press F12 after a felt burst. The exact
invariants are `used = styles + context_unknown`, `used = base_match +
base_other + base_unknown + context_unknown`, `used = passes +
context_unknown`, and all caller buckets equal the directional-shadow texture
count/time. The run file is
`cache/runs/run53-shadow-texture-dependencies-safe.ini`.
The verifier now has 275 checks and rejected all 24 one-at-a-time
constant/table/adapter perturbations. The release DLL contains exactly one
matching adapter byte sequence with `push [esp+0xbc]` and `ret 8`.

**Run 52 was prepared -- read findings §60 before §59.** It made no behavior
change relative to run 51: `shadow_defer_cold_alpha=1` remains enabled, while
the color pass, opaque caster behavior, `shadow_split`, map size, culling, and
resource policy remain unchanged. The new trace answers the identity question
that run 51 could not. Every remaining cold shader-used material texture is
partitioned by its originating `GraphicsMeshInstance` shadow style, pass, and
whether it is the same base Resource checked by the omission gate. Every
directional-shadow texture load is also partitioned independently by the
direct `GraphicsTexture::GetTexture` caller, with an explicit unresolved
bucket for indirect paths.

Do not infer a fix from a maximum. Press F12 after each felt loading burst; the
marker is a reaction-time anchor. First identify the candidate collision-active
full-scene **play** frames in its preceding two seconds, then check the exact
partitions. For every frame, used material count/time must equal each of its
style, base-identity, and pass partitions (including `context_unknown`), and
all texture-caller buckets must equal `engine_shadow_res_texture` and `_us`.
The useful decision is whether the remaining work is another dependency of a
returning alpha caster, another mesh style/pass, or a non-material caller.

The new sites were re-derived from the pinned Engine.dll. The verifier has 268
checks, including three new 17--22-byte windows and exhaustive coverage of all
ten direct calls to the exported texture getter. All eighteen one-constant or
one-table-entry perturbations were rejected. The run file is
`cache/runs/run52-shadow-texture-dependencies.ini`; its settings differ from
`live-config.ini` only by the retained behavior switch, full trace, and F12
marker.

**Run 51 is measured -- read findings §58 before §57.** The reporter noticed
no flicker and no shadow popping, so temporary omission of this alpha-tested
`GraphicsMeshInstance` class was visually safe in the session. Across
**play**, 71 caster/pass attempts were omitted (68 state 0, 3 state 1), 55
made a new queue/state transition, and enqueue failure was zero. The opaque
shader-unused filter also worked: it made 980,873 repeated skip decisions,
7,121 against a state-0 texture, and the actual cold material-load partition
became 13 shader-used / 30.155 ms and zero shader-unused or unknown. Skip
counters are repeated per-frame attempts, not unique resources.

The F12 marker at frame 6742 follows the only candidates in its reaction
window: collision-active full-scene **play** frames 6722 and 6723, 193.597 and
152.630 ms. The first omits 24 state-0 alpha records and newly queues 16, but
the second has no omission and still loads 12 cold shadow textures / 34.388 ms,
including 5 shader-used material textures / 14.510 ms. The two frames total
346.227 ms, so the felt loading burst remains; the run-50 pair totaled 315.029
ms, although differing scene work prevents calling the difference a
regression. Base-texture-only deferral is therefore not a completed fix.

Keep the behavior-preserving unused-texture filter as supported work, and keep
the alpha omission as a visually safe mechanism, but do not widen it blindly.
The next run should passively classify every remaining cold shader-used
material texture by `GraphicsMeshInstance` shadow style/pass and base-resource
identity, and partition the non-material shadow texture call sites. That will
show whether returned alpha casters have unenumerated dependencies or whether
the remaining texture work belongs to opaque/non-mesh casters. The run-51
sites still have 243 verifier checks with all eighteen new constants mutated
and rejected; `shadow_split` remains untouched.

**Run 50 is measured -- read findings §56 before §55.** Across **play**, its
material-texture partition is exact and has no unknowns: 77 cold material
textures / 225.958 ms split into 40 parameters used by the active shadow shader
/ 113.009 ms and 37 absent parameters / 112.949 ms. In the F12 reaction window,
collision-active full-scene **play** frame 6809 has 17 material textures /
47.705 ms: 9 used / 25.328 ms and 8 unused / 22.377 ms. Thus checking the Name
before `GetTexture` safely removes about half this interval, not all of it.
Temporarily omitting the used/alpha-tested caster is a plausible local visual
trade, but only if state-0 textures are explicitly enqueued and the caster stays
omitted through state 1 until state 2. Without the enqueue, “a few frames” is
not guaranteed and the synchronous load may simply move to the color pass.
That audit and implementation are now findings §57.

**Run 50 design -- read findings §55 before §54.** Static re-audit found
that `GraphicsMesh::SetShaderParameters` calls
`GraphicsTexture::GetTexture` for every type-7 material entry before its
texture setter checks whether the active shadow shader even has that parameter
Name. Run 50 passively times only cold material textures inside the DX11
directional build and partitions them as shader-parameter `used`, `unused`, or
`unknown`; it changes no rendering behaviour. A dominant unused bucket would
support checking the parameter before loading the texture. A used bucket
cannot simply be removed because opacity/base maps can determine cutout shadow
coverage. Run 49 also shows that small-mips-first is already partly active on
the marked full-scene **play** burst: 26 loose textures entered the progressive
uploader, yet 37 shadow texture loads still cost 104.701 ms. The run therefore
targets earlier synchronous file/parse/create work, not merely high-mip upload.
`shadow_split` remains untouched.

**Run 49 result -- read findings §54 before §53.** The pass-count boundary is
not stale DX9 code: `activeAPI==0` selects `GraphicsForwardRenderer` and
bypasses the list builder, while `activeAPI==1` selects
`GraphicsDeferredRendererX` and enters the list builder that makes the virtual
shadow-pass-count call. Run 49 also observed ten cold calls at the patched
`GraphicsMeshInstance` call site during **play**, so the path is live in the
reporter's DX11 session. It is not, however, the whole dependency gate. Across
**play**, 218 shadow-nested loads cost 447.434 ms: textures account for 126 and
359.551 ms, meshes 87 and 83.858 ms, and shaders 5 and 4.025 ms. Only ten cold
meshes and 15.315 ms occur at the pass-count boundary.

The F12 reaction follows collision-active full-scene **play** frame 6520. It
lasts 331.841 ms, with 142.782 ms in the directional-shadow call, 67 cold
loads taking 142.026 ms, and a 262.601 ms directional GPU interval. Those
intervals overlap. Textures consume 104.701 ms of the load interval, meshes
34.762 ms, and shaders 2.563 ms; the four cold meshes at the pass-count
boundary consume only 9.093 ms. The original option-3 predicate, "mesh state 0
at pass count," is therefore too narrow and must not be A/B tested as if it
covered the burst. Option 2 likewise needs the later texture/shader dependency
boundaries before an earlier-residency design can be scoped. The next useful
instrument is caller attribution for cold `EnsureAvailable` entries inside the
DX11 directional build. `shadow_split` remains untouched.

**Run 48 result -- read findings §52 and §51 first.** Across **play**, all 214
directional-shadow resource loads and all 451.588 ms enter in raw loaded state
0; state 1, state 2, other, and the queue-link count are all exactly zero. On
the marked collision-active full-scene **play** transition, all 65 nested loads
and all 147.135 ms are state 0 with no queue link, beside a 230.289 ms
directional GPU interval. The renderer discovers cold resources inside the
shadow build; it is not joining loader work already in flight. Pure loader
priority is therefore out. Earlier residency would need explicit earlier
shadow-caster discovery. The next boundary to identify is the caller above
`Resource::EnsureAvailable` and its per-caster selection/draw site, so an exact
preload or state-0-caster omission can be designed. `shadow_split` remains
untouched.

**Run 47 result -- read findings §50 and §49 first.** The one-frame whole-map
reuse is rejected. Before the CSV was read, the reporter observed three
whole-scene flickers. The switch fired seven times; the last active-play reuse
is 4.822 s after the F12 marker and fits the reported last flicker. At the
marked collision-active **play** transition, frame 6442 skipped the shadow CPU
and GPU work exactly as intended but still lasted 148.733 ms. The forced normal
call on frame 6443 then paid 117.641 ms of synchronous shadow resource loads
and a 217.272 ms directional GPU interval, and frame 6444 lasted another
122.691 ms. The felt burst remained. A later non-region shadow-load burst also
remained. Do not extend the reuse interval: that would prolong the observed
visual discontinuity and still defer the same build.

**Run 46 result -- read findings §48 and §47 first.** The one F12 reaction is
in **play** and follows the collision-active full-scene outdoor-transition
burst. On frame 6939, 167.799 of the 170.532 CPU milliseconds inside
`GraphicsShadowMapDx11::RenderDirectional` are 57 synchronous main-thread
resource loads; the same call has a 273.815 ms directional GPU interval. Its
region pointer changes before the call. The remaining felt class is therefore
localized: the game loads resources inside the directional-shadow build while
that build submits a large caster pass. No play pump call reaches 50 ms.

Run 47 tested the supported one-frame whole-pair reuse and rejected it. A
further shadow fix has to make caster resources ready before the visible
transition or genuinely distribute the build without displaying a stale map.
Postponing the complete call by a fixed number of frames is closed.

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

1. `research/streaming/findings.md` **§49, §48, §47, §46, §45, §44, §43,
   §42, §41** (the one-frame reuse A/B, transition-shadow result and design,
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
| `a5fd576` | Passive F12 marker corrections, removal of the invalid per-Peek CPU query, sent-message attribution, and the retained-handle/250 ms batched CSV writer that run 45 validated. Findings §41–§46. |

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
7. **Run 46 measures the exact overlap before changing it.** On the marked
   play transition, 167.799 of 170.532 shadow CPU milliseconds are synchronous
   resource loads, beside a 273.815 ms directional GPU interval. The region
   changes before the call. This supports one-frame reuse of the previous
   whole map/matrix pair as the next A/B, with no change to `shadow_split`.
8. ~~**Run 47 one-frame whole map/matrix reuse**~~ -- **rejected**, §50. It
   visibly flickers, defers the full shadow build to the next frame, leaves the
   felt three-frame **play** burst, and misses a later non-region shadow-load
   burst. Do not lengthen the reuse window.
9. **The mod's own GPU cost — the one lever this project owns, and it works.**
   Enhanced shadows are 8.09 ms and grass 4.47 ms of a 25.4 ms steady GPU
   frame at 5120x1440, and the outdoor transition is 421 + 182 ms of which the
   directional shadow pass is 351.6 ms. `shadow_map_scale=2` takes the
   transition to 350 + 144 and the steady frame to 24.4 ms **without touching
   shadow distance** (§38). `shadow_split=0.325` is worth far more (263 + 86)
   and is **refused**: it exists to fix shadow distance, which is the feature
   (§39, the reporter's call). Grass has never been priced on its own.
10. **The game's synchronous resource load**, 147-336 ms on the transition
   frame, inside `Engine::Render` on the main thread. Real, the game's, and
   nothing short of the archive work already measured out touches it.
11. ~~4.3, libdeflate~~ — worth 35-50 ms on a 340 ms frame (§35). The riskiest
   item in the plan for the smallest remaining return. **Struck** unless the
   loading screen becomes the target.
12. ~~`timeBeginPeriod`~~ — **struck**. Main-thread `Sleep` is 0.0 ms on every
   in-play stutter frame in nineteen runs.
13. ~~Stage 5 / `async_level_load`~~ — finished and measured out, §33.
14. ~~4.2 bounded prefetch, buffer pooling, the block cache past 8 MiB~~ —
   struck, runs 22 and 24.
15. **Run 50 found a nearly even split.** On the marked-window
   collision-active full-scene **play** transition, shader-unused textures cost
   22.377 ms and shader-used/alpha textures cost 25.328 ms. The former can be
   removed without changing output; the latter can be deferred only as an
   explicit local shadow-quality trade. Next audit the resource-loader enqueue
   contract and a per-caster suppression boundary, then A/B enqueue plus
   omission through state 0/1 with measured enqueue-to-state-2 latency.

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
  `tqflicker-frames.runN.csv`, runs 9-48, plus `tqflicker-debug.runN.log` for
  runs 9-33. **Runs 34-39 have no debug log** because they ran with `trace=0`;
  if the message histogram or the slow-caller tables are wanted, a run needs
  `[debug] trace=1`.
- **Runs 34-48 are the only ones with `draw_submit_ms` / `map_resource_ms`,**
  only run 39 has `pump_timer_full` / `pump_timer_split`, runs 40-48 have the
  F12 marker, run 42 has the sent-window-procedure split, and only run 43 has
  the withdrawn thread-CPU/query columns, run 44 has the external
  `sample` reports under `cache/samples/`, and run 45 is the corrected-writer
  control. Runs 46-47 add the directional-shadow attribution, and run 47 adds
  `engine_shadow_reuse`. Earlier CSVs are
  missing those columns off the end rather than shifted -- `tools/frames.py`
  and `csv.DictReader` handle it.
- **Run 46 is archived.** `tqflicker-frames.run46.csv` has SHA-256
  `bf1c5aff030bc11e2ac0a7f3055c138bccef378f021bdcb7b3ae71faa272641b`.
  Its INI differed from the normal config only by `performance_trace=full` and
  the passive F12 marker. See findings §48.
- **Run 47 is archived.** `tqflicker-frames.run47.csv` has SHA-256
  `f48562aaedc130722bc98648e3adc0a2e575a0b5f099cc7b7d969db51bdf0a12`.
  `cache/runs/run47-shadow-transition-reuse.ini` differs from the normal live
  config by one performance variable (`shadow_transition_reuse=1`) and the
  same two instruments as run 46. See findings §50; the switch is rejected.
- **Run 48 is archived.** `tqflicker-frames.run48.csv` has SHA-256
  `1c3a51470c485f21eb44fd18844d8cdf598ddf0435929051285eff9f3279609b`.
  `cache/runs/run48-shadow-resource-lifecycle.ini` differs from normal only by
  full performance tracing and the passive F12 marker. It changes no game
  behaviour and leaves `shadow_transition_reuse` absent/default-off. See
  findings §52.
- **Run 49 is measured.** `cache/runs/run49-shadow-caster-boundary.ini` differs
  from normal only by full performance tracing and the passive F12 marker. It
  adds resource filename classes and the cold-mesh pass-count interval; both
  are instruments and neither changes shadow/resource behaviour. The archived
  CSV SHA-256 is `69e07412d10c77ad123b236763f1c8c34c38f93bf112c8179ab1c39e7628d76c`.
- **Run 50 is measured.** `cache/runs/run50-shadow-material-textures.ini`
  differs from normal only by full performance tracing and the passive F12
  marker. It measures whether cold material textures loaded inside the DX11
  directional build are parameters of the active shadow shader; it changes no
  mesh, texture, shader, or shadow behaviour. Its archived CSV SHA-256 is
  `86c1e6d6b26a42b9a69bf2bedf3096cf3dfec3011525c44c94b4baa7ddf37bd1`.
  Findings §56 has the result and §55 the design.
- **Run 51 is installed.** `cache/runs/run51-shadow-defer-cold-alpha.ini`
  differs from normal by one behavior setting
  (`shadow_defer_cold_alpha=1`) and two instruments (full performance tracing
  and the passive F12 marker). Opaque casters retain their geometry while
  absent-parameter material textures skip loading; cold alpha-tested casters
  are enqueued and omitted until resident. The colour pass and `shadow_split`
  are unchanged. Findings §57 records the exact design and verification.
- **The route is scripted and identical in all twenty-seven full runs**: menu,
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
  `src/engine_probe.cpp` against it and the other two modules. **243 checks**,
  and every constant perturbation must fail it. All eighteen constants added
  for run 51 were independently perturbed and rejected.
- **Run 51 is the installed build.** Installed `winmm.dll` is byte-identical
  to `build/winmm.dll`, SHA-256
  `e3f21518f9229a4cf7b6fcad1ed9b14577968604adcd00357178b148c0951e12`.
  Installed `tqflicker.ini` is byte-identical to the run-51 cache copy,
  SHA-256
  `869e8e6b174a2cc5d3b6094eab19b263ebd21c5c94c6271db27584499232bc70`.
  `shadow_transition_reuse` remains absent/default-off; `shadow_split` is
  untouched. Run 50's completed CSV remains archived as
  `tqflicker-frames.run50.csv`, and the stale live CSV was removed. The game
  was not launched.

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
