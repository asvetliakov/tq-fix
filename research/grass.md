# Grass: what the vertex data actually contains

Written after seven in-game runs, all of which produced no visible change. The
conclusion is negative and worth keeping, because every plausible-looking route
to "3D grass" from inside a shader runs into the same wall.

## How grass is drawn

`base/grass.ssh` is its own material: two vertex permutations (differing only
by a clip-plane output) and one pixel program. It is ordinary CPU-placed
geometry -- no instancing, no geometry or compute shaders anywhere in the
453-program archive, and no `SV_InstanceID` or `SV_VertexID`.

Vertex inputs: `POSITION`, `TEXCOORD0`, `TEXCOORD1`, `NORMAL`. Uniforms include
`bladeHeight` (`cb0[7].w`), `time` and `clippingPlane`. The vertex shader is

    r0.xy   = sin(v0.xz + time)                     wind, phased by position
    weight  = 1 - v1.y                              0 at the base, 1 at the tip
    xz      = v0.xz + wind + v2.xy * weight
    y       = v0.y - (bladeHeight - sqrt(bladeHeight^2 - |v2*weight|^2))

The pixel shader samples one texture, alpha-tests with `discard` at 0.35, and
writes four G-buffer targets: `o0` albedo (texture x tint), `o1` the normal as
`n * 0.5 + 0.5`, `o2` a raw texture copy, `o3` zero -- grass has no specular.
`o3` is the specular target: 30 other materials write `specularPower / 64`
there and the deferred receiver multiplies by 64 to recover it.

## The three things that are not what they look like

**`TEXCOORD1` is not the blade's facing. It is the bend**, added on top of the
mesh position and weighted to zero at the base, with the vertical term dropping
the tip to preserve length. **And it is zero in the data.** Rotating it by 90
and by 180 degrees, and scaling it from 1.0 down to 0.1, all produced no
visible change whatsoever.

**`NORMAL` points straight up.** Measured: a probe displacing every vertex
along its own normal made the whole grass field float vertically. So a card's
width axis, which would be `(n.z, -n.x)`, is the zero vector. Every blade in
the scene therefore also receives identical lighting, which is the flat, waxy
look, and it is in the mesh data rather than the shader.

**Nothing carries the card's orientation.** A blade's facing exists only in the
spatial arrangement of its four vertices, which a vertex shader cannot see.
There is no per-blade pivot to rotate about and no way to derive one.

**The object origin is not the clump either.** Turning `POSITION.xz` about it
sent the grass off screen, so a grass draw's geometry sits far from the origin
it is expressed in -- chunk- or world-relative coordinates, not a clump centred
on its own pivot.

## What that rules out

Every pivot a vertex shader could turn a blade about, and how each was ruled
out by measurement rather than by argument:

| pivot | result |
| --- | --- |
| per-blade, from the normal | normals point up, so the width axis `(n.z, -n.x)` is the zero vector |
| per-blade, from `TEXCOORD1` | it is a bend, and it is zero in the data |
| the object origin | geometry sits far from it -- grass left the screen |
| a snapped grid | would tear any clump whose vertices straddle a cell boundary |

Also ruled out: **excluding grass from a screen-space effect by its G-buffer.**
Its zero specular is shared with `terrainstandard`, so gating on it removes the
ground too, which is where the effect is wanted.

## What is left

Two G-buffer channels grass writes itself: albedo and normal.

**Per-vertex normals were built, measured, and dropped.** Every vertex of a
card shares one normal, so a card shades as a flat sheet; splaying the four
across the card's width -- left edge toward -w, right toward +w -- makes it
shade like a rounded blade. The channel is live: at an absurd bend the field
visibly changes, which proves the vertex normal reaches the deferred lighting
through `dp3 o2.x, v3, cb0[12]` in the vertex shader and
`mad o1.xyz, v2, 0.5, 0.5` in the pixel shader.

It was still not worth keeping. **The camera is the reason.** An isometric ARPG
looks down at grass, so a gradient across a blade's width is the detail that
never faces the player: barely visible at tasteful settings, ugly at strong
ones. Reported in play as a darkening toward the base -- which a horizontal
splay does not directly produce, and which nobody chased, so treat that
observation as unexplained rather than as a description of the effect.

Earlier attempts at the same channel from inside the shader were also judged
worse; bending there equalises the lighting further, which is the opposite of
what it promises. See the git history for both transforms.

The real fix is data, not code. `ArtManager.exe` and `ModelCompiler.exe` ship
in the game directory; crossed grass meshes with proper normals would work
where every shader-side route fails, because the crossing would live in the
geometry the shader already expects the blade shape to come from.

## The engine side: what is reachable

Everything above is about the shader, which is the wrong end of the pipe. The
right end is Engine.dll, and it is open.

**Engine.dll exports 5599 named symbols with full C++ mangling**, sixteen of
them grass. So the subsystem is reachable by `GetProcAddress` -- no pattern
scanning, no offsets to break on a patch:

    Terrain::CreateGrassGeometry() -> TerrainBase::GrassGeometry   rva 0x22f9c0
    Terrain::DisplaceGrass(float, float, float, float)
    TerrainRenderInterface::RenderGrass(...)                       rva 0x2390b0
    TerrainRenderInterfaceRT::RenderGrass(...)                     rva 0x23afc0
    TerrainType::SetGrassShaderParams(GraphicsShader2*)
    GraphicsEngine::EnableGrass(bool) / IsGrassEnabled()
    Terrain::maxNumGrassPlanes = 350
    Terrain::useGrassBufferCache = true, maxGrassBufferCacheSize = 15

**Grass is terrain, not entities.** Every entry point hangs off `Terrain`, and
the blade parameters are database fields on terrain-type records --
`BladeHeight`, `BladeSpacing`, `BladeWidth`, `BladeVariations`,
`BladeTextureFileName`, each grass type having a `_noBlade` twin. The level
stores a painted terrain type; the planes are generated from it at run time.
There is nothing in level data to duplicate.

**Engine.dll relocates.** It was observed at 0x79730000 against a preferred
base of 0x10000000, so every absolute operand in a validated prologue has to be
rebuilt from the loaded base. A hardcoded one does not match and silently
refuses to patch, which looks exactly like a failed hook.

### The live path

Measured, not read: with both `RenderGrass` implementations detoured,
`TerrainRenderInterfaceRT::RenderGrass` ran 6600 times in one session and
`TerrainRenderInterface::RenderGrass` never ran at all. Suppressing the RT one
removes grass from the scene and nothing else.

**`Terrain::CreateGrassGeometry` is not on that path.** It was detoured for the
same session and called zero times against those 6600 renders, so it belongs to
the non-RT implementation. Its two allocations still describe the format,
because they are baked immediates: 44800 and 11200 bytes, which at 350 planes
of four vertices is 32 and 8 bytes per vertex.

The RT path instead calls an unexported function at rva 0x2331b0 once per
terrain block, which returns a small descriptor. `RenderGrass` then reads it
directly:

| word | use |
| ---: | --- |
| `+0x00` | plane count |
| `+0x08` | stream 0 buffer, bound with stride 0x20 |
| `+0x0c` | stream 1 buffer |

and draws `count * 4` vertices as `count * 2` triangles. So a grass plane is
**four vertices and two triangles, 32 bytes of stream 0 and 8 bytes of stream 1
each**, which agrees exactly with the sizes the other implementation allocates.

The descriptor is not built there. The function at rva 0x2331b0 is a pure
lookup: a terrain block holds a `vector<int>` of layer ids at `+0x4c/+0x50` and
an array of 16-byte descriptors at `+0x58`, and the lookup returns the
descriptor whose layer matches. **Nothing on the render path allocates** -- the
buffers are built once when a block loads and only bound thereafter.

### The vertex format

`RenderGrass` passes a static four-element array at rva 0x3709d4 to
`RenderDevice::SetVertexBufferLayout`. `VertexElement` is 12 bytes, three
dwords, which decodes the array as

| element | stream | usage | type | bytes |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 0 | 0 | 2 | 12 |
| 1 | 0 | 1 | 2 | 12 |
| 2 | 0 | 4 | 1 | 8 |
| 3 | 1 | 1 | 7 | 8 |

Three elements on stream 0 summing to exactly the 32-byte stride the draw
binds, and one on stream 1 of 8 -- both buffer sizes accounted for, and four
elements for the shader's four inputs.

Reading type 2 as a float3 and type 1 as a float2 puts `POSITION` at offset 0
and `NORMAL` at offset 12 of stream 0, with `TEXCOORD0` at 24, and leaves
`TEXCOORD1` alone on stream 1. **A captured buffer confirms all four**, and it
explains an old result: the bend this file records as zero in the data is a
separate 11200-byte buffer, every byte of it zero, which is why rotating and
scaling it changed nothing.

### What a captured buffer contains

Read back from the bound stream during a live grass draw, first two planes:

| | position | normal | uv |
| --- | --- | --- | --- |
| 0 | 147.581 24.103 31.273 | -0.183 0.982 0.055 | 0.0 0.0 |
| 1 | 148.538 24.103 32.006 | -0.183 0.982 0.055 | 0.5 0.0 |
| 2 | 148.538 22.899 32.006 | -0.183 0.982 0.055 | 0.5 1.0 |
| 3 | 147.581 22.899 31.273 | -0.183 0.982 0.055 | 0.0 1.0 |
| 4 | 147.880 24.674 31.876 | -0.532 0.779 0.330 | 0.5 0.0 |
| 5 | 149.399 24.674 31.403 | -0.532 0.779 0.330 | 1.0 0.0 |
| 6 | 149.399 23.083 31.403 | -0.532 0.779 0.330 | 1.0 1.0 |
| 7 | 147.880 23.083 31.876 | -0.532 0.779 0.330 | 0.5 1.0 |

Four vertices per plane wound top-left, top-right, bottom-right, bottom-left,
y being up. **Every plane has its own centre, its own size, and its own facing
about y**: plane 0 measures 1.205 wide by 1.204 tall at 37.4 degrees, plane 1
1.591 square at -17.3 degrees. So the field is not a set of aligned billboards;
the blades are already turned randomly, and a pivot is simply the average of
four positions.

`TEXCOORD0` spans u 0.0..0.5 on the first plane and 0.5..1.0 on the second:
the blade texture is an atlas and `BladeVariations` selects a column of it.

**Correction to the claim above that the normal points straight up.** It does
not. It is a per-plane, mostly upward unit vector -- the two here are 11 and 39
degrees off vertical -- constant across a card's four vertices. The old probe
that displaced along it saw the field rise because the vectors are y-dominated,
which is consistent with both readings; "straight up" was more than that
measurement supported. What survives is the part that matters: **all four
vertices of a card share one normal**, so every card is lit as a flat sheet,
and that is the waxy look.

The buffers are `D3D11_USAGE_DYNAMIC` with `CPU_ACCESS_WRITE`, 44800 and 11200
bytes, drawn 50 planes at a time in the block sampled. Dynamic plus the
15-entry buffer cache means **a buffer is rewritten and reused across terrain
blocks**, so nothing derived from one may be cached against its pointer.

### Why this matters

A vertex shader sees one vertex at a time, which is why none of the pivots in
the table above exist for it. In a 32-byte-stride buffer the four corners of a
plane are contiguous. **A per-plane pivot is the average of four positions**,
and crossing a quad is a rotation about it -- the operation the whole
shader-side effort was trying and failing to express. Only the twelve position
bytes of each vertex need to change; the rest of the format can stay unread.

The 44800-byte allocation is fixed, so adding planes in place is not possible.
Crossing the existing ones is: 350 singles become 175 crossed pairs, at the
same vertex count, the same buffer, and the same draw cost.

## Crossed blades: what shipped

`[graphics] grass = enhanced`. Every blade keeps its position and gains a
second card through it, turned a quarter turn about its own centre. Density is
unchanged; the cost is a second draw per block and twice the grass overdraw.

The turn is position only -- twelve bytes per vertex -- and keeps the card's
centre, width, height, uv column and normal. A card that has been turned is
still a well-formed card, and two turns return the original corners swapped.

### How it is built

1. `CreateBuffer` notes buffers whose descriptor matches a grass stream (44800
   bytes, dynamic, vertex, CPU-writable). That is a **candidate**, not a
   conclusion: the game creates 56 of them and most are not grass.
2. `Unmap` inspects a candidate's first plane against the card fingerprint --
   128 bytes, not the whole buffer. A match promotes it to a grass stream, and
   the turned copy is uploaded to a twin buffer of ours.
3. The draw hook, while `RenderGrass` is on the stack, repeats the draw with
   the twin bound in place of stream 0.

A stream is also adopted at the draw itself, which is authoritative, with the
twin then seeded from a staging copy. That path is the fallback for streams
filled before the hooks existed.

Enhanced grass requests the Draw hooks itself, including when SMAA, tone
mapping, native bloom control, secondary admission and tracing are all off.
Buffer tracking alone cannot render the extra cards.

### Buffer lifetime and pending copies

Tracking tables own their source buffers. A raw COM address alone is not a
resource identity: after the game releases a buffer, the driver can reuse its
address for a smaller buffer or a texture. Previously a stale grass record
could accept that resource's mapped pointer and read 44,800 bytes from it.
Keeping a reference prevents address reuse while the record is indexed and
makes the descriptor checked at admission valid for that record's lifetime.

Candidate and stream caches each retain at most 256 sources; promotion removes
the candidate record. The maximum retained source payload is 21.875 MiB, plus
driver allocation overhead. The existing twin cache is separate. Eviction,
failed index insertion, and shutdown release references. There are no new
descriptor queries or reference operations in the ordinary Map/Unmap path;
known streams also skip the candidate lookup.

Evicting a stream cancels its pending upload and staging seed before the slot
can be reassigned. Refilling a stream cancels a seed of its previous contents.
Promotion removes the candidate's mapped pointer, so it cannot survive an
Unmap through the stream record. Grass Map/Unmap processing and crossing draws
are restricted to the game's immediate context, which owns the shared scratch
buffer. Staging readback still waits for a Present and uses `DO_NOT_WAIT`.

The off-game self-test covers reference ownership and bounded eviction,
protected memory after Unmap, reuse of an evicted address by a smaller buffer,
seed cancellation on eviction/refill, and successful nonblocking seeding.
These checks address concrete lifetime defects; the reported fountain crash
still needs confirmation with the updated DLL on the affected machine.

### Four failures worth not repeating

Each of these cost a run, and all four were plumbing rather than geometry.

**Identifying a buffer by its size is a guess.** Matching 44800-byte dynamic
vertex buffers filled a 32-entry table with things that were not grass, so the
streams that were actually drawn had nowhere to go, and the feature did nothing
while every log line looked healthy.

**A twin is indistinguishable from a grass stream.** It has the same descriptor
and, by construction, contents that pass the fingerprint. Once creation-time
matching existed, creating a twin registered it, unmapping it promoted it, and
promoting it created another twin -- an unbounded recursion that froze the game
on the loading screen. Our own device and context calls now run behind a
re-entrancy fence, because they re-enter our own hooks: the context whose
vtable is patched is the one we call.

**Adopting at the draw is correct and too late.** A block is filled before it
is ever drawn, so a twin that waits for the next fill waits for the buffer to
be recycled, which for most blocks never happens. The first crossing draw
arrived at draw 19579; with fill-time promotion it arrives at draw 1. This was
also the whole of the reported flicker.

**A log window can outrun its own event.** The line that would have reported
the first successful crossing was gated on a draw counter that passes 400
inside the first second, so the one event worth knowing about could not report
itself. One-shots for the events that matter; windows only for noise.

### Placement is not why a field looks woven

A large lawn reads as a repeating weave. Two cards captured from the game
share a z exactly and step 0.58 in x, which looked like a lattice and like the
cause. It is not.

Every blade was moved off that lattice by a hash of its own centre, with the
height corrected through the card's normal so the base stayed on the ground,
and the offset scaled up until it was absurd. **At a 0.73-unit displacement --
more than a full lattice step, logged from the live fill to prove it reached
the game's own buffer -- the field looked identical.**

In hindsight the reason is in the numbers already recorded here: a card is 1.2
to 1.6 wide on a 0.58 spacing, so every patch of ground is covered by several
overlapping blades and moving individual ones changes nothing in aggregate.
Varying the crossing angle and the blade height alongside it changed nothing
either.

What is left for anyone who wants to chase it: the wind, which displaces by
`sin(x + t)` and `sin(z + t + 0.5)` and so bands blades on a period of about
6.3 world units -- test by standing still and seeing whether the pattern
crawls; and the texture, which is an atlas of **two** blade images
(`BladeVariations`, visible as uv columns 0..0.5 and 0.5..1.0), so the same two
pictures tile the whole field. The second is a database change, not a code one.

### Cost of watching

The map hooks see every mapped resource in the frame -- **2400 per frame**
measured in a grass area, almost none of them grass. Walking the tracked
streams cost 386 comparisons per lookup and grew with how far the session had
explored (43 streams to 163 while walking). It is a pointer index now, so the
lookup is a hash and a short probe whatever the session has seen.

## Method note

Six of the seven runs were spent refining maths that could not work, because
the diagnostics only ever confirmed that a *transform applied*, never that its
inputs were non-degenerate. Disassembly tells you what a shader computes; only
the running game tells you what the mesh data contains. Probe the data first:
displace along an attribute by an absurd constant, baked as an immediate, and
see whether anything moves.

Two traps found on the way, both of which cost a run each:

- **A vertex-stage constant buffer at b13 never arrived.** A program patched to
  read it saw zeros however early the bind was issued -- at `VSSetShader`, and
  again immediately before the draw. Baked immediates went straight through.
  The pixel stage's `b13` works, which is what the contact shadows use.
- **"Patched" is not "used".** The log said both blade programs were patched
  long before anything confirmed they were ever bound, let alone drawn.
