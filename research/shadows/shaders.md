# Caster and receiver shaders

## Caster package

The base archive's `pieces/shadow.ssh` contains seven valid DXBC containers:

| Ordinal | Style | Size | SHA-256 prefix |
| ---: | --- | ---: | --- |
| 1 | directional static vertex | 1520 | `ff5fab...` |
| 2 | directional skinned vertex | 2296 | `e9362d...` |
| 3 | directional foliage vertex | 1520 | `ff5fab...` |
| 4 | point static vertex | 1696 | `663231...` |
| 5 | point skinned vertex | 2472 | `6670a6...` |
| 6 | point foliage vertex | 1696 | `663231...` |
| 7 | alpha-test pixel | 764 | `c5d8cf...` |

Directional foliage and static are byte-identical, as are their point-shadow
counterparts. Directional static transforms position by one four-row matrix.
Directional skinned first blends indexed bone matrices and then applies the
same projection. The mod's bone-index clamp is therefore relevant to shadow
caster stability.

Transparent styles use the pixel shader to sample base alpha and discard when
alpha is below `0.5`. Its color output is irrelevant because the target is
depth-only.

The `shadowBias` name is resolved and uploaded by mesh instances, but the
decoded directional vertex programs do not declare or read a separate bias
constant. Bias is not the source of missing far coverage.

## Directional receiver vertex stage

Runtime capture `active-vs-03.asm` computes world position against four stored columns
of `worldToShadowMatrix`, divides XYZ by W, and emits the projected coordinate.
It also emits validity derived from projected Z and a constant. The `dp4`
instruction order agrees with the CPU matrix upload; there is no unresolved
row/column transposition in the native one-map path.

DXBC reflection classifies `worldToShadowMatrix` as a 4x4
`matrix-columns` value at byte offset 16. The four DP4 operands are its stored
columns, so the receiver evaluates a row-vector times matrix.

Cascade matrix composition happens on the raw uploaded arrays, not on that
logical form. Since each `dp4` produces result component *i* from array row
*i*, the array acts as `A * column-vector`, and the near-to-far array must be
`far * inverse(near)`. The logical column-packed equivalent,
`inverse(M_near) * M_far`, is its transpose and is not what gets uploaded.

`active-vs-04.asm` is the point-shadow radial/paraboloid variant.
`active-vs-01.asm` is a fullscreen/menu route and does not receive shadows.

## Directional receiver pixel stage

Runtime capture `receiver-ps.asm` binds `shadowTexture` at `t2` and a regular
sampler at `s2`. It performs four manual depth samples in a cross pattern,
controlled by `shadowBluriness`, compares each sampled depth against receiver
depth, and accumulates four `0.25` contributions.

It then computes:

```text
edge = saturate(min(u, v, 1-u, 1-v) * 20)
```

The result blends to fully lit outside the texture and across the outer 5% of
its UV footprint. This is the blurred moving boundary seen in the stair test.
Removing the fade would replace it with a hard boundary, not restore missing
shadows.

## Receiver-family inventory

The three installed archives contain 148 `.ssh` files. Seventy containers
mention `worldToShadowMatrix`; 68 have recoverable shadow-bound DXBC. Across
those resources, `inventory.csv` records 613 shadow-related programs: 336
vertex and 277 pixel permutations, representing 367 unique DXBC hashes.

The receiver set includes standard static/skinned/foliage families, blended
and glow variants, scrolling and wiggling variants, dissolve, environment-map,
terrain, lightmap, tile-blending, iridescent/icy expansion materials, and
XPack2 standard families. A replacement that preserves the existing one-map
binding automatically covers them all.

These families are legacy. At runtime the directional shadow map is bound only
for the deferred screen-space receiver described in `findings.md`; none of the
per-material permutations below ever carried it. Counts from this archive
therefore describe what exists, not what runs.

Every one of the 367 unique programs disassembles offline into a
content-addressed file under `shaders/generated/`, regenerated on demand rather
than committed. The CSV is the index from resource/style ordinal to hash, so
identical programs shared across resources remain deduplicated without losing
provenance.

## The receiver that actually runs

`deferred-receiver-ps.asm` is the deferred screen-space pass described in
`findings.md`. It reconstructs world position from the depth buffer, projects
it with a matrix in its own constant buffer, and runs the four PCF taps against
the shadow map at `t3`. Everything above this section describes shaders that
exist in the archive; this is the one the renderer uses for directional light.

Two containers, `base/grass.ssh` and `xpack2/vertexcolorlayers.ssh`, mention
the matrix in container metadata but expose no embedded DXBC carrying a
shadow binding. The generated CSV records only validated DXBC boundaries and
does not infer proprietary `.ssh` style-table semantics.
