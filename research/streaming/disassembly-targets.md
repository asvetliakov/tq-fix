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
| `0x18c8fe` | caster-record decision call | Exact `E8` in the window at `0x18c8f5`, reaching the helper at `0x18c650`. A false result omits that caster/pass record before dependent work and draw submission. |
| `0x18c650` | unexported build-record helper | Calls the renderable virtual at slot `+0x24` to test eligibility, then appends the accepted record. |

## `GraphicsMeshInstance` shadow-caster dependencies

| RVA | Exact target or site | Why it matters |
| --- | --- | --- |
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
