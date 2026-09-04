# Stutter-path disassembly and decompilation targets

This is the durable index for the static code behind the measured **play**
loading bursts. It records what to reopen in Ghidra or a disassembler and why;
it is not a substitute for the bytes compiled into `src/engine_probe.cpp` or
for `tools/verify-sites.py` checking those bytes against the installed game.

The supported binary is the PE32/i386 `Engine.dll` identified in
`../shadows/supported-build.md`, preferred image base `0x10000000`. Every
address below is an Engine-relative RVA. Add `0x10000000` for the preferred
virtual address shown by the generated audit; rebase at runtime for ASLR.
Exported names are exact. Names in italics are working names for unexported
functions, assigned from their vtable identity and behavior rather than from
missing symbols.

## Generic resource chain

| RVA | Exact target or site | Why it is in the chain |
| --- | --- | --- |
| `0x213ed0` | `ResourceLoader::LoadResource` | The complete synchronous loader call timed in the CSV. |
| `0x2130f0` | `Resource::EnsureAvailable` | Generic wrapper that reaches `LoadResource`; run 61 showed its return at `0x213137` is not an owning renderer. |
| `0x213180` | `Resource::GetLoadedState` | Proves loaded state is `Resource+0x30`. |
| `0x212d20` | `Resource::GetInLoadingQueue` | Proves the queue marker is `Resource+0x60`. |
| `0x2130e0` | `Resource::GetFileName` | Supplies the engine-owned filename used for mesh/shader/texture classification. |
| `0x212dc0` | `Resource::GetResourceLoader` | Returns the loader used for an engine-native asynchronous enqueue. |
| `0x2145c0` | `ResourceLoader::EnqueueResource` | The stock queue operation. |
| `0x1200e0` | `BaseResourceManager::PreLoadResource` | Establishes the stock `(priority=1, notify=true, immediate=false)` enqueue tuple. |

Run 80 adds no Engine patch site to this chain. Its off-main texture record is
taken at the already installed `ID3D11Device::CreateTexture2D` vtable proxy,
after the original call succeeds. It records the exact request descriptor,
loader thread, elapsed time, and frame extent without assigning the loader
thread to whichever reflection/deferred owner happens to be active on the main
render thread. A future texture-realization fix must still be anchored in the
resource chain above; the D3D proxy is an observation boundary, not evidence
that `CreateTexture2D` owns archive data or engine ready-state transitions.

Run 81 likewise adds no new Engine byte table. Its behavior boundary reuses
the exact reflection `BuildScene` call at `0x186501`, the exact following
`RenderLightStyle` call at `0x18694d`, and the exported
`GraphicsMeshInstance::RenderPass` entry at `0x172dd0`. The first two remain
five-byte `patchCall` retargets inside their independently verified 22/23-byte
caller windows. The mesh target has the shared
`55 8b ec 83 e4 f8` prologue: all 24 bytes and the relocated security-cookie
operand are verified, while the detour steals only six complete bytes. The
existing `ID3D11Device::CreateBuffer` vtable proxy supplies the successful
buffer count during `BuildScene`; it is an observation input to the fix, not a
new Engine code patch. At the 32-buffer boundary only mesh-instance dispatches
inside the immediately following reflection child are omitted. Terrain and
the later normal-colour owner retain their stock calls.

Run 82 adds no disassembly target. Run 81 proved the same boundary fired but
left a 41.959-ms terrain-only reflection interval after all 87 mesh calls were
omitted. The stronger behavior therefore stops at the existing caller site:
when the exact `BuildScene` count reaches 32, the wrapper at the verified
`0x18694d` `E8` does not enter that one `RenderLightStyle` call. With tracing
off, only the two verified `patchCall` sites at `0x186501` and `0x18694d` are
written; the `GraphicsMeshInstance::RenderPass` entry detour is not needed.
This skips neither the later main colour owner nor directional shadows, and
the reflection call resumes on the next frame.

Run 83 also adds no disassembly or patch target. It reuses the exact
`TerrainPlug` and `TerrainBlock` entry wrappers, the verified 24-byte
`GraphicsMeshInstance::RenderPass` entry, the existing reflection call
contexts, the two direct second-owner geometry call contexts at `0x1663a8`
and `0x166412`, and the exact `GraphicsShadowMapDx11::RenderDirectional`
context. A bounded pointer-identity table classifies first visits inside those
already-bracketed consumers only; it does not infer a class from adjacency.

Run 84 likewise adds no disassembly target and does not patch a shared scene
owner. Its progressive secondary-pass behavior reuses the verified reflection
`BuildScene` and `RenderLightStyle` calls at `0x186501` and `0x18694d`, the
23-byte directional caller window whose `E8` is at `0x1644bc`, the 19-byte
`TerrainPlug`/`TerrainBlock` entries at `0x236240`/`0x23e1e0`, and the 24-byte
`GraphicsMeshInstance::RenderPass` entry at `0x172dd0`. Each of those three
shared-prologue renderable entries still steals only the first six bytes;
their full recorded windows and relocations remain the write authority. The
actual pending draw omission occurs in the mod-owned D3D11 `Draw` and
`DrawIndexed` vtable wrappers, not in another Engine byte site. Resource and
material setup therefore still executes before an object's secondary-pass
draw is admitted. Normal-colour contexts never set the behavior scope.

Run 85 adds no disassembly target and changes no byte authority above. The
progressive behavior now self-arms when the number of previously unseen shared
secondary identities in one presented frame exceeds its configured budget.
The old >=32-buffer reflection and directional-region signals remain passive
trace evidence only; neither controls the behavior. Consequently a trace-off
admission-only boot no longer requests the `ID3D11Device::CreateBuffer` slot.
That slot remains required by the two default-off reflection-omission
experiments and by the existing full trace. The mod-owned `Draw` and
`DrawIndexed` slots are now explicitly requested by progressive admission,
and Engine behavior activates only after both were installed successfully.
Failure of either leaves every game draw stock.

## Runtime terrain and color rendering

Do not replace these with exported `Terrain::Load`, `Terrain::LoadRenderData`,
or `Terrain::PreLoad`. Those belong to the editor-capable `Terrain` class. The
shipping game object uses the unexported runtime vtable at RVA `0x2f8820`;
constructors at `0x23dde0` and `0x23ded0` write that exact vtable.

| RVA | Exact target or site | Identity and role |
| --- | --- | --- |
| `0x2f8820` | runtime terrain vtable | Slots `+0x24`, `+0x28`, `+0x34`, `+0x44`, and `+0x48` select the five runtime functions below. |
| `0x23d8d0` | *`TerrainRT::Load`* | Vtable `+0x24`. Reads terrain data and creates the 12-byte layer records at owner `+0x84..+0x88`; it does not load their textures. |
| `0x23d6d0` | *`TerrainRT::LoadRenderData`* | Vtable `+0x28`. Walks the layer records, creates opacity/render data, and admits each layer's texture Resources. |
| `0x23d742` | call in *`TerrainRT::LoadRenderData`* | Exact `E8` to `TerrainType::LoadTextures`; verified by the 23-byte window beginning at `0x23d730`. This is the earliest existing texture-admission boundary. |
| `0x23d400` | *`TerrainRT::PreLoad`* | Vtable `+0x34`. Preloads nearby `TerrainObject`s but directly calls neither `TerrainType::PreLoad` nor `TerrainType::LoadTextures`. |
| `0x23d060` | *`TerrainRT::GetNumTextureLayers`* | Vtable `+0x44`. Computes `(owner+0x88 - owner+0x84) / 12`. |
| `0x23d020` | *`TerrainRT::GetLayerTerrainType`* | Vtable `+0x48`. Returns the first dword of the indexed 12-byte layer record. |
| `0x240160` | `TerrainType::LoadTextures` | Creates the base, bump, and grass texture Resource objects named by a layer type. |
| `0x23fe80` | `TerrainType::PreLoad(bool)` | `false` skips work; `true` walks the exact base/bump/grass Resources consumed by rendering. Run 63 measured zero calls in all five session parts. |
| `0x23fb90` | `TerrainType::SetShaderParams` | Ordinary terrain-layer color binding. Its `EnsureAvailable` calls produced run 63's exact cold base/normal texture loads. |
| `0x23fa40` | `TerrainType::SetGrassShaderParams` | Grass-mask binding under the DX11 `TerrainRenderInterfaceRT::RenderGrass` class. |
| `0x236240` | *`TerrainPlug` color render* | Unexported render function with `this` plus four explicit arguments (`ret 0x10`); unique body call at `0x2366cd` reaches `TerrainType::SetShaderParams`. |
| `0x23e1e0` | *`TerrainBlock` color render* | Unexported render function with `this` plus four explicit arguments (`ret 0x10`); unique body call at `0x23e73f` reaches `TerrainType::SetShaderParams`. |
| `0x23a530` | `TerrainRenderInterfaceRT::RenderGround` | Exact DX11 ground class used for the broader CPU/GPU span. |

## Directional-shadow construction and map ownership

| RVA | Exact target or site | Why it matters |
| --- | --- | --- |
| `0x1644bc` | deferred renderer call to `RenderDirectional` | The `E8` inside the verified window at `0x1644ac`; its arguments identify camera, frustum, canvas, surface, and output matrix. |
| `0x18db80` | `GraphicsShadowMapDx11::RenderDirectional` | The single directional-map build class. It must not be described as the whole frame or as color rendering. |
| `0x18d427` | shadow-map constructor field write | Proves the region identity is stored at `GraphicsShadowMapDx11+0x6c`. |
| `0x18dbd5` | output matrix argument capture | Proves the seventh argument is saved at stack `+0x7c`. |
| `0x18f0c0` | output matrix copy | Copies 16 dwords (64 bytes) to that output. These three sites support exact one-frame transition-map reuse. |
| `0x18ce70` | `GraphicsShadowMapRenderer::Render` | Shared map-render owner reached after DX11 directional caster admission. Its branch at `0x18d043` is the DX11 record path; the older branch calls a different executor at `0x187360`. |
| `0x18d04f` | DX11 shadow-record builder call | Exact `E8` in the verified window at `0x18d043`, reaching the 0x88-byte record builder at `0x18c870`. |
| `0x18c870` | *DX11 shadow-record builder loop* | Walks the admitted renderable list, queries each shadow-pass count, invokes the per-pass helper at `0x18c8fe`, and appends accepted 0x88-byte records. |
| `0x18d05d` | DX11 shadow-record executor call | Exact `E8` in the verified 23-byte window at `0x18d054`, reaching the record executor at `0x18c520`. Run 76 patches only this call so the outer setup GPU interval ends and the first 64-record chunk begins at the executor boundary. |
| `0x18c520` | *DX11 shadow-record executor* | Its independently verified 24-byte entry and 21-byte tail at `0x18c631` prove three explicit arguments (`ret 0x0c`). It sorts and walks the 0x88-byte records, switches the record's shader Resource/style, and dispatches each accepted record. |
| `0x18c613` | shadow renderable virtual dispatch | Verified `FF 56 28` inside the record loop: renderable virtual slot `+0x28`, passed the record and its parameter block at record+`0x78`. This is the exact per-record draw boundary, not a DX9-only path. |
| `0x18c8fe` | caster-record decision call | Exact `E8` in the window at `0x18c8f5`, reaching the helper at `0x18c650`. A false result omits that caster/pass record before dependent work and draw submission. |
| `0x18c650` | unexported build-record helper | Calls the renderable virtual at slot `+0x24` to test eligibility, then appends the accepted record. |

## Complete DX11 play-render flow

The earlier audit contained most of these functions in a 1,363-function
closure, but it did not interpret this end-to-end control flow. Regenerating
the audit with the explicit roots in `research/streaming/seeds.txt` produces a
1,592-function closure and recovers the previously depth-boundary
`GraphicsForwardRenderer::RenderLightStyle`. The useful result is a top-level
map, not a claim that every polymorphic renderable leaf has a trustworthy C++
decompilation.

For the **play** part of a session, `Engine::Render` calls
`GraphicsEngine::Update`, which begins the canvas frame and calls
`Display::Render`. `Display::Render` invokes slot `+0x4` on every display
object; the `GraphicsPortalRenderer` instance reaches the named render entry
below. That indirect edge is established by the display-object vtable at
runtime, not by a direct static call in `Display::Render`.

| RVA | Exact target or site | Identity and role in the flow |
| --- | --- | --- |
| `0x1584c0` | `GraphicsEngine::Update` | Called by `Engine::Render`; begins the graphics frame and directly calls `Display::Render` at `0x15872d`. |
| `0x133240` | `Display::Render` | Iterates display objects and dispatches each object's render virtual at slot `+0x4` (`0x133269`). |
| `0x182230` | `GraphicsPortalRenderer::Render` | Runtime portal display-object renderer; calls the recursive traversal at `0x1822bd`. |
| `0x17e2c0` | *portal recursion* | Calls `World::GetRegionsInFrustum`, tests front/back portal visibility, and recursively visits connected regions through helper `0x180ee0`. It is why two deferred owners are not two fixed flat passes. |
| `0x17e579` | DX11 branch call | Verified 23-byte window containing the `E8` at `0x17e585` to the DX11 region branch. |
| `0x17ead0` | *DX11 region branch* | Constructs a local `GraphicsDeferredRendererX`, sets its viewer, renders reflections, admits regions/portal geometry, and only then calls the deferred owner. One invocation belongs to one recursively selected portal/region branch. |
| `0x17f2c6` | reflection-manager call | Verified 24-byte argument/call window; exact `E8` at `0x17f2d3` reaches `GraphicsReflectionManager::RenderReflections` before region admission and deferred rendering. |
| `0x187270` | `GraphicsReflectionManager::RenderReflections` | Collects visible water-reflection records through helper `0x186a00`, then renders each 72-byte record. |
| `0x1872b0` | per-reflection-plane call | Verified 20-byte loop window; exact `E8` at `0x1872bb` reaches the forward-render helper for one reflection record. |
| `0x1861d0` | *reflection forward-render helper* | Constructs `GraphicsForwardRenderer`, sets the reflected viewer/region transform, builds its scene, allocates the temporary 1024x1024 surface, and renders it. |
| `0x1864f0` | reflection BuildScene call | Verified 22-byte window containing the exact `E8` at `0x186501` to `GraphicsForwardRenderer::BuildScene`. |
| `0x17d9d0` | `GraphicsForwardRenderer::BuildScene` | Exported scene-admission child of one reflection plane. It gathers regions through `World::GetRegionsInFrustum` (or the current region when no world exists), then calls `GraphicsSceneRenderer::AddRegionToScene` for each eligible region. It returns no admitted-count signal. Its independently verified 24-byte entry and epilogue at `0x17dac3` prove one explicit `bool` argument (`ret 0x04`). |
| `0x18693b` | reflection RenderLightStyle call | Verified 23-byte window containing the exact `E8` at `0x18694d` to `GraphicsForwardRenderer::RenderLightStyle`. |
| `0x179a40` | `GraphicsForwardRenderer::RenderLightStyle` | Forward color-render owner used by one reflected scene; its independently verified 19-byte entry and epilogue at `0x179bc4` prove four explicit arguments (`ret 0x10`), and its call at `0x179ba1` reaches the scene-list wrapper. |
| `0x179b8f` | forward scene-list wrapper call | Verified 23-byte window containing the exact `E8` at `0x179ba1` to helper `0x17aaa0`. |
| `0x17aaa0` | *forward scene-list wrapper* | Builds render records through helper `0x188600`, then passes them to the shared sorted executor. |
| `0x17ab04` | sorted scene-list executor call | Verified 23-byte window containing the exact `E8` at `0x17ab16` to helper `0x1883f0`. |
| `0x1883f0` | *sorted scene-list executor* | Sorts the pointer list, switches shader/resource state, ensures the selected shader Resource, and dispatches each renderable. Shared by forward reflection and deferred paths. |
| `0x1885a2` | renderable virtual dispatch | Verified 20-byte window ending around the call at `0x1885b1`; dispatches virtual slot `+0x28`, whose terrain leaves include `TerrainPlug` and `TerrainBlock` color render. |

The complete static order relevant to the measured **play** transition is:

```text
Engine::Render
  -> GraphicsEngine::Update
    -> Display::Render
      -> [virtual +0x4] GraphicsPortalRenderer::Render
        -> portal recursion
          -> for each selected DX11 portal/region branch:
             -> GraphicsReflectionManager::RenderReflections
                -> for each visible water-reflection plane:
                   -> reflection forward-render helper
                      -> GraphicsForwardRenderer::BuildScene
                      -> GraphicsForwardRenderer::RenderLightStyle
                         -> build + sorted scene-list executor
                            -> [virtual +0x28] renderable color draw
             -> AddRegionToScene / portal geometry admission
             -> GraphicsDeferredRendererX::Render
                -> geometry setup's internal sorted scene list
                -> geometry scene sorted list
                -> directional/point shadows
                   -> GraphicsShadowMapDx11::RenderDirectional
                      -> GraphicsShadowMapRenderer::Render
                         -> build 0x88-byte DX11 shadow records
                         -> sort + execute accepted records
                            -> [virtual +0x28] renderable shadow draw
                -> lighting, resolve/AO, later lists, fog/composite
```

The directional chain above is independently byte-anchored at its owner,
both direct `E8` edges, both unexported helpers, and the final virtual
dispatch. It also answers the DX9 question explicitly: the verified
`0x18d04f -> 0x18c870 -> 0x18d05d -> 0x18c520` chain is selected by the DX11
branch in `GraphicsShadowMapRenderer::Render`; the older renderer uses the
other branch and does not pass through this executor.

Run 76 uses the exact executor edge rather than the outer directional entry.
The new source tables independently verify the 23-byte call window, the
24-byte executor entry, and the 21-byte `ret 0x0c` tail before either the
outer or inner `E8` is written. For reflection, decompilation shows no
admitted-count return from `GraphicsForwardRenderer::BuildScene`; its exact
elapsed `_us` value is therefore only a sparse selection signal for opening
the following whole `GraphicsForwardRenderer::RenderLightStyle` interval, not
an attribution of cause.

Run 77 leaves those executor bytes in the reproducible audit but writes
neither shadow call. Run 76's marked **play** event put its submission drain
after reflection and before shadow-map construction; no exact shadow executor
chunk exceeded 1.866 ms. The only sparse queries now cover reflection draws
65--192 in eight-draw intervals. The existing exact unexported `TerrainPlug`
and `TerrainBlock` entry wrappers retain their start/end draw ordinals and
nested Resource/D3D-creation totals only while that selected reflection child
is active. This introduces no new Engine patch: the terrain entries and
reflection child calls remain the already verified targets above.

Run 77's marked **play** event reaches at least draw 193, and its draws
65--192 total only 5.502 ms against 24.708 ms for the whole exact
`GraphicsForwardRenderer::RenderLightStyle`. Run 78 therefore changes no
disassembly target: it moves the same sixteen query pairs to draws 193--320
and begins retention in the same already-patched exact `TerrainPlug` and
`TerrainBlock` entry wrappers at draw 193. The exact reflection child call,
the two terrain entries, and the dormant shadow-executor evidence above remain
the complete target set.

Run 78's probable marked **play** event instead ends its exact reflection
`RenderLightStyle` at draw 192, so neither fixed ordinal window covered that
producer. Run 79 keeps the same sixteen query pairs and covers draws 1--320
continuously. It also adds one exact-class identity target:

| RVA | Exact target or site | Why it matters |
| --- | --- | --- |
| `0x172dd0` | `GraphicsMeshInstance::RenderPass` | The sorted scene-list executor dispatches virtual slot `+0x28`, so there is no direct `E8` to patch. The exported override verifies a 24-byte entry with the relocated security cookie at `+13`, steals only the shared six-byte prologue, and has an independently verified `ret 0x10` tail at `0x173127`. Together with the existing exact `TerrainPlug` and `TerrainBlock` wrappers, it classifies the major reflection renderables across the complete query window. |

The mesh detour is needed by either the renderable trace or Run 81's mesh-only
behavior and is installed atomically after the reflection call patches;
failure detaches it and restores those calls in reverse. Run 82's whole-child
behavior does not need it when tracing is off. Its wrapper reads only inactive
flags outside a selected event and takes a CPU clock only while retaining a
selected renderable call.
Any GPU bin not overlapped by one of the three named classes remains an
explicit unclassified gap rather than being attributed by proximity.

Run 74 brackets the two reflection children at their unique call sites, not at
their shared exported entries. Before either displacement is written, runtime
verification covers both 16--24-byte caller windows, both independently
relocation-aware entries, and both callee-cleanup tails. The cross-pass part
does not add another Engine patch: it retains successful main-thread D3D11
buffer identities from the existing device hook and reads the existing four
vertex-buffer plus index-buffer setter snapshot after each already-timed game
draw. Active scopes classify that use as one exact reflection manager/plane,
the exact `GraphicsShadowMapDx11::RenderDirectional` call, or one exact
`GraphicsDeferredRendererX::Render` owner/pass/site. Reflection takes
precedence over directional, which takes precedence over the enclosing
deferred owner, so a directional draw is never mislabeled as generic deferred
work.

This corrects two earlier working assumptions. First, the two observed
`GraphicsDeferredRendererX::Render` owners are recursive region/portal branch
renders, not fixed “pass one/pass two” stages. Second, the function called
“geometry setup” at `0x1663a8` is not mere state setup: its callee `0x1653a0`
builds and executes another sorted scene list through `0x188600` and
`0x1883f0`. Keep the column name for CSV continuity, but interpret it as a
rendering owner.

Run 72's `TerrainType::SetShaderParams -> Resource::EnsureAvailable` stacks
end at `0x1885b4`, then unwind through `0x17ab1b`, `0x179ba6`, `0x186952`,
`0x1872c0`, and `0x17f2d8`. That exact return chain places those **play**
terrain loads in the reflection forward-render class, before the associated
`GraphicsDeferredRendererX::Render` owner. It is the immediate static target
for the next passive attribution trace.

The Run 73 trace boundary follows those exact calls without detouring either
shared entry. The branch call at `0x17f2d3` is patched after its 24-byte caller
window, the 21-byte exported
`GraphicsReflectionManager::RenderReflections` entry at `0x187270`, and its
independent `ret 0x08` tail at `0x1872c3` all verify. This proves a `thiscall`
receiver plus two explicit arguments. The manager's plane call at `0x1872bb`
is patched only after its 20-byte caller window, the relocation-aware 20-byte
entry at `0x1861d0`, and the independent `ret 0x0c` tail at `0x1869e4`
verify. This proves three stack arguments for the per-record helper. Both E8
patches install atomically and restore in reverse order.

Runtime attribution is deliberately bounded: manager invocations one and two
and planes one and two within each manager have separate CPU, game-draw,
Resource-load, D3D-creation, and GPU fields. A third manager or plane increments
an explicit overflow counter. Existing clocks are reused, so no per-draw or
per-resource timer was added. Reflection context is main-thread context:
off-main `CreateTexture2D` calls can be correlated to adjacent frames but are
not falsely assigned to a main-thread reflection scope. Manager and plane
intervals nest; they describe containment and must not be added.

The mod's enhanced bloom is different: `hookDraw`/`hookDrawIndexed` invokes it
after a game gamma-bound draw, inside `Engine::Render`, with `g_inside=true`.
Its D3D calls are therefore excluded from `draw_submit_ms` but included in
`bloom_ms`. A long bloom CPU interval can be a driver queue-drain point and is
not proof of equally long bloom GPU execution. The real `Present` happens in
the next main-loop iteration; `visual::onPresent` records the preceding frame
and advances progressive uploads before forwarding it.

## DX11 deferred-frame pass partition

`GraphicsDeferredRendererX::Render` is the top-level DX11 deferred-render
class, not the whole `Engine::Render` class and not the directional-shadow
class. Run 70 partitions its ordered direct children so the remaining
first-use/submission class can be assigned without per-draw GPU queries. The
working group names below describe their observed role and ordering; only the
exported owner has a recovered C++ name.

| Group | Direct call site(s) | Callee(s) and independently verified ABI tail |
| --- | --- | --- |
| owner | `0x166130` | Exported `GraphicsDeferredRendererX::Render`; independently verified 24-byte entry. Its tail at `0x1665cf` is independently verified across 20 bytes and ends in `ret 0x1c`. |
| sole direct owner caller | `0x17fc9b` | `E8` in unexported `FUN_1017ead0`, inside the verified 24-byte window at `0x17fc8b`. Run 71 patches this call to number the two owner invocations without touching the shared renderer prologue. |
| geometry | `0x1663a8`, `0x166412` | `0x1653a0` (`ret 8` at `0x16557f`) and the shared five-argument scene-list helper `0x1883f0` (`ret 0x14` at `0x1885ec`). |
| shadow-map construction | `0x166454` | `0x164050` (`ret 8` at `0x16458b`), which constructs and renders the DX11 directional and point maps; the narrower `GraphicsShadowMapDx11::RenderDirectional` interval remains separately available. |
| light accumulation | `0x166461` | `0x164640` (`ret 8` at `0x16532a`), the deferred light/shadow receiver binding routine. |
| resolve / AO | `0x16647d`, `0x16648f` | Deferred resolve `0x166800` (`ret 0x0c` at `0x166bd2`) and canvas AO `0x15c8e0` (`ret 4` at `0x15c9c6`). |
| later scene lists | `0x1664a6`, `0x1664ae`, `0x166502` | Helpers `0x161c80` (`ret 8` at `0x16212d`), `0x161a00` (`ret 4` at `0x161a5d`), and a second call to `0x1883f0`. |
| post / fog / composite | `0x16650a`, `0x166515`, `0x166525`, `0x166588`, `0x1665a4` | Helpers `0x161a70` (`ret 4` at `0x161c6b`), fog `0x165aa0` (`ret 8` at `0x166120`), `0x162200` (`ret 4` at `0x1625b0`), composite `0x1657b0` (`ret 0x14` at `0x165a89`), and debug/optional `0x161720` (`ret 4` at `0x16193f`). |

All fourteen sites are direct `E8` calls inside the owner. Their overlapping
16-byte windows are verified together before the first write, then each exact
call and destination is changed with `patchCall`; a partial installation is
rolled back. The callee tails are a separate authority for the x86
callee-cleaned argument count, specifically so a decompiler's guessed
signature cannot reproduce the earlier terrain stack imbalance. Multiple
calls in one group deliberately use one GPU query pair: the GPU column spans
from that group's first child entry to its last child exit and therefore
includes any small owner-code gap between them. CPU whole-call and
`Draw`/`DrawIndexed` duration columns sum only time inside those direct
children.

Run 70 corrects the dynamic assumption behind that GPU design: the game calls
`GraphicsDeferredRendererX::Render` twice per measured frame. A single pair
per group therefore spans from the first occurrence in owner invocation one
through the last occurrence in owner invocation two; the six GPU columns
overlap and cannot be read or summed as pass costs. The CPU and draw sums are
still exact. The next target is the invocation identity plus the two geometry
sites at `0x1663a8` and `0x166412`, with independent GPU pairs no broader than
one owner invocation. Run 71 implements that correction: four independent
query pairs cover setup/scene in invocation one/two. It also tags synchronous
Resource and D3D creation by those four cells or the `other` portion of each
owner. Setter hooks for `PSSetShaderResources` (device-context vtable slot 8),
`PSSetShader` (9), `VSSetShader` (11), `IASetVertexBuffers` (18), and
`IASetIndexBuffer` (19) maintain bounded resource identities for slow draws;
the draw hooks issue no state getter.

## `GraphicsMeshInstance` shadow-caster dependencies

| RVA | Exact target or site | Why it matters |
| --- | --- | --- |
| `0x111ea0` | `Actor::AddToScene` | The DX11 directional scene-gather virtual reached at `RenderDirectional+0x1298`; Run 68's repeated verified stack subsequence identifies this exact class before the cold mesh load. |
| `0x111fd5` | `Actor::AddToScene -> Actor::UpdateMeshInstance` | Exact `E8` inside the 23-byte window at `0x111fca`. This is the safe earlier behavior boundary: the wrapper can inspect the Actor's root before pose work, while every other caller stays stock. |
| `0x112060` | `Actor::UpdateMeshInstance` | The independently verified 24-byte entry reads the mesh instance at `Actor+0x184`, updates its transform, then calls `GraphicsMeshInstance::UpdatePose`. |
| `0x11212e` | `Actor::UpdateMeshInstance -> GraphicsMeshInstance::UpdatePose` | Run 68 records its return at `0x112133` between the Actor scene-add and mesh-load frames. |
| `0x176570` | `GraphicsMeshInstance::UpdatePose` | Its call at `0x17659d` synchronously ensures `GraphicsMeshInstance+0x4`; all 20 marked cold meshes share return `0x1765a2`. The Run 69 boundary acts before this method rather than letting it touch unloaded mesh fields. |
| `0x173440` | `GraphicsMeshInstance::GetNumShadowRenderPasses` | Reads the root mesh at instance `+0x4`, immediately ensures it, then returns mesh `+0x7c`. The accepted fix defers only state-0/1 root meshes at this exact class boundary. |
| `0x1733b0` | `GraphicsMeshInstance::GetShadowRenderStyle` | Obtains the base texture and maps opaque styles `0..2` to alpha-tested styles `3..5`. |
| `0x1731a0` | `GraphicsMeshInstance::GetTexture` | Ensures the owning mesh, then returns a material texture Resource at entry `+0x14`. |
| `0x173480` | `GraphicsMeshInstance::SetShaderParameters` frame | Saves the instance/pass context used to associate a texture load with an accepted shadow record. |
| `0x17385e` | call to `GraphicsMesh::SetShaderParameters` | Exact call at offset `+14` of the verified window; the callee is `0x169c40`. |
| `0x173acd` | base-override `EnsureAvailable` call window | Reads the instance override at `+0x14`; the later verified setter binds the exact `baseTexture` name. |
| `0x173b3f` | bump-override `EnsureAvailable` call window | Reads the optional instance texture at `+0x18`; the later verified setter binds the exact `bumpTexture` name. |

## Shadow material texture use

| RVA | Exact target or site | Why it matters |
| --- | --- | --- |
| `0x169c40` | `GraphicsMesh::SetShaderParameters` | Applies generic mesh material parameters for the shadow pass. |
| `0x169cab` | call to `GraphicsTexture::GetTexture` | Type-7 material entry's residency boundary, inside the verified window at `0x169ca8`. |
| `0x169cc1` | call to the texture-parameter setter | Inside the adjacent window at `0x169cb8`; distinguishes a texture present in the material from one used by the active shader. |
| `0x1948b0` | `GraphicsTexture::GetTexture` | Ensures its Resource before returning the resident render texture. |
| `0x18ba70` | `GraphicsShader2::HasParameter` | Ensures the shader and looks up the supplied `Name`; supports the used/unused texture distinction. |
| `0x035eb8` / `0x035f3a` | missing-parameter path and success return | Proves an absent shader parameter returns before reading the texture value, making omission of an unused binding narrow. |

## Reproduction and byte authority

`research/streaming/seeds.txt` now names the exported terrain targets and roots
all unexported runtime/color functions by preferred virtual address.
`research/shadows/seeds.txt` roots the exact mesh/material/resource functions
used by the shadow fixes. Regenerate the static artifacts with the respective
`tools/run-audit.sh`; neither audit launches the game.

For runtime changes, the authority order is:

1. pinned module identity in `research/shadows/supported-build.md`;
2. exact 16--24-byte tables, relocation descriptors, vtable identities, and
   call destinations in `src/engine_probe.cpp`;
3. independent checks in `research/streaming/tools/verify-sites.py`;
4. generated Ghidra disassembly/decompilation as navigational evidence.

The decompiler can assign a wrong signature or class name, and a stale prose
finding can be overturned. A site is writable only when the installed bytes
and, where applicable, its export, vtable slot, relocation, and call target all
agree.
