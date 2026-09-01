# Directional shadow pipeline

Status: complete for the supported build. Findings below were checked against
Engine.dll and Direct3D11.dll disassembly, embedded DXBC, runtime captures, the
failed early cascade experiment, and the failed DX9 substitution.
`findings.md` supersedes any receiver claim here that predates the
runtime census.

## End-to-end flow

```text
GraphicsSceneRenderer::SetViewer
    camera + frustum + viewport
                |
                v
DX11 deferred-light orchestration (Engine.dll 0x10164050)
    enumerate visible renderables and shadow passes
    GraphicsShadowMapDx11::AddSurface
                |
                v
GraphicsShadowMapDx11::RenderDirectional (0x1018db80)
    reconstruct camera rays
    keep t = 0.000 .. 0.325 only
    fit one light-space orthographic box
    query entities in that cropped sub-frustum
                |
                v
GraphicsShadowMapRenderer::Render (0x1018ce70)
    clear one directional D32_FLOAT map
    render Shaders/Pieces/Shadow.ssh casters
                |
                v
deferred material receiver
    worldToShadowMatrix + shadowTexture
    four manual depth comparisons
    fade outside UV footprint to fully lit
```

## Root cause of the disappearing edge shadows

The DX11 implementation is a single-map near crop, not a cascaded shadow-map
implementation. It reconstructs the four view rays, creates eight points at
ray parameters 0 and `0.325`, and fits one orthographic light projection around
only those points. The supplied full `Frustum` and requested `Algorithm` are
not used for fitting.

Camera elevation, zoom, and perspective can expose receivers outside that
cropped footprint. The receiver shader deliberately fades samples outside the
map to fully lit over the outer 5% of UV space. That fade is the moving blurred
line observed in testing; after it passes an object, its shadow is absent.

This explains all observations:

- `shadows=original` still fails because the defect predates the mod.
- stairs expose more world without changing the fixed 32.5% ray crop.
- zooming inward can reduce the visible covered area because the crop is a
  fraction of reconstructed camera rays, not a screen-space distance budget.
- raising the fraction fixes reach but spreads the same texels over a larger
  world footprint, producing softer and more visibly moving shadows.
- changing PCF or rewriting only the receiver cannot create missing depth.

## The dormant DX9 design is not a compatible shortcut

`GraphicsShadowMapDx9::RenderDirectional` at `0x10192d40` uses the supplied
frustum, caster bounds accumulated by `AddSurface`, and the requested
algorithm:

| Algorithm | Function | Meaning |
| --- | --- | --- |
| 0 | `0x10191d90` | uniform orthographic fitting |
| 1 | `0x10191180` | light-space perspective shadow mapping (LiSPSM) |
| 2 | `0x10190170` | trapezoidal shadow mapping |

Both the DX9 and DX11 deferred orchestrators request algorithm 1. DX11 ignores
that argument and substitutes the hard-coded near orthographic crop. Runtime
testing showed that the DX9 method cannot safely be transplanted onto an
emulated DX11 shadow object: its private convex-volume, fitting, caster-query,
and receiver state do not match the DX11 call. The resulting stages diverged
and produced missing or oversized moving shadows.

## What the audit ruled out

- No dormant directional cascade array exists in target allocation or light
  binding. There is one directional target, one SRV, and one matrix per light.
- The failure is not directional-light origin or sign. Native near shadows
  remain geometrically correct when the experimental changes are removed.
- It is not caster animation staleness. Caster geometry refreshes; coverage is
  what moves and disappears.
- `GraphicsShadowMapDx11::AddSurface` does collect world and light-space
  bounds, but the DX11 directional fitter never consumes them.
- Per-mesh `shadowBias` is bound by `GraphicsMeshInstance`, but the decoded
  DX11 directional caster programs do not read it. It cannot fix coverage.
- The map format and clear path are valid depth resources; there is no hidden
  fixed-point or color-target truncation in this path.

## Replacement conclusion

The coherent replacement invokes the proven DX11 routine twice, with private
`1.05` far and native `0.325` near crops. Each pass therefore owns matching
projection fitting, caster enumeration, rasterization, and receiver matrix.
The mod retains one copied far depth map and extends exact receiver shader
pairs to blend it with the engine's near map. See `replacement-design.md` for
the validated binding, lifecycle, fallback, and shader-inventory boundary.
