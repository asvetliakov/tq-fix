# CPU setup, fitting, and culling

## Frame orchestration

The DX11 deferred-light routine at `0x10164050` performs the following work:

1. Checks the light's casts-shadows flag, renderer/global shadow enable state,
   and the relevant device/options bit.
2. Allocates at most one directional shadow entry for the frame, guarded by
   renderer offset `+0x94c`.
3. Constructs `GraphicsShadowMapDx11` in the light basis.
4. Enumerates visible renderables and each object's shadow render passes.
5. Calls `GraphicsShadowMapDx11::AddSurface` for eligible passes.
6. Calls `RenderDirectional` with the viewer camera at renderer `+0x28`, the
   viewer frustum at `+0x78`, algorithm 1, the single directional target, and
   an output matrix.
7. Stores the target and output matrix in the light's `0x68`-byte record.

The corresponding DX9 orchestration at `0x1017c830` also requests algorithm
1. The difference lies inside the renderer implementation, not at the caller.

## DX11 directional fit (`0x1018db80`)

The implementation:

1. Inverts the view/projection transform and reconstructs the four corner
   rays of the camera volume.
2. Evaluates each ray at `t=0.0` and `t=0.325`, producing eight points.
3. Transforms those points into a basis aligned with the directional light.
4. Fits a single axis-aligned box in that light space.
5. Creates a type-1 orthographic camera whose width and height are the box
   extents, near depth is zero, and depth extent is based on a fitted extent
   multiplied by `1.5`.
6. Calls `Camera::GetSubFrustum` for that cropped camera and queries caster
   entities through `Region::GetEntitiesInFrustum` or
   `World::GetEntitiesInFrustum`.
7. Renders the resulting scene to the directional depth target.
8. Multiplies by the D3D clip-to-texture bias matrix: X `+0.5/+0.5`, Y
   `-0.5/+0.5`, Z unchanged.

The `Frustum` argument and `Algorithm` argument have no effect on this fit.
There is no shadow-texel snapping, so the fitted projection center follows the
camera continuously.

## Caster queries

Region queries use the region's spatial index and portal traversal. World
queries enumerate loaded regions intersecting the query frustum, translate
the frustum into each region's coordinate space, and invoke the region query.
Selected entities are added to the temporary shadow scene together with their
owning regions.

This means the disappearing screen edge is not caused by a stale global 4:3
entity-update rectangle. Casters are queried correctly for the frustum the
shadow renderer asks for; the renderer asks for the wrong, cropped frustum.

## `AddSurface` and render styles

`GraphicsShadowMapDx11::AddSurface` at `0x1018d6e0` rejects non-finite bounds,
accumulates world and light-space AABBs, and stores accepted renderable passes.
Directional DX11 fitting does not consume those AABBs. Point shadows and the
DX9 implementation must not be assumed to share that omission.

Objects reporting one shadow pass with opaque-static style 0 include
billboards, decals, emitters, line effects, terrain, trails, and water.
Render groups report one pass and delegate style selection to their first
renderable child. `GraphicsMeshInstance` reports the mesh resource's pass
count and selects among six styles:

| Value | Style |
| ---: | --- |
| 0 | opaque static |
| 1 | opaque skinned |
| 2 | opaque foliage |
| 3 | transparent static |
| 4 | transparent skinned |
| 5 | transparent foliage |

## DX9 full-frustum path (`0x10192d40`)

Unlike DX11, DX9 copies the supplied frustum, incorporates the six visible
bounds planes, clips/augments the convex plane set using the light direction
and caster bounds, queries casters, and validates all generated planes as
finite. It then dispatches algorithm 0, 1, or 2.

`0x1028a4e0` constructs convex vertices from triple plane intersections and
keeps points inside every plane. `0x1021ca60` selects facing planes and creates
silhouette boundary planes for a direction. These helpers are the geometric
foundation needed by the replacement; copying only the final DX9 matrix
formula would omit required clipping inputs.
