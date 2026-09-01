# Proven shadow-related object layouts

These are partial layouts. An offset is listed only when the audited code
directly reads or writes it; names express observed use rather than complete
class semantics.

## `GraphicsSceneRenderer`

| Offset | Observed value |
| ---: | --- |
| `+0x28` | active `Camera` copy/reference used by deferred shadow setup |
| `+0x78` | active `Frustum` |
| `+0x88c` | active `Viewport` |

`GraphicsSceneRenderer::SetViewer` at `0x10187950` populates this state.

## `GraphicsDeferredRendererX`

| Offset | Observed value |
| ---: | --- |
| `+0x94c` | per-frame directional-shadow allocation/render guard |
| `+0x960` | shadow enable state |
| `+0x9dc` | world/renderer shadow-softness value |

The directional target is global `DAT_1037446c`. Eight point targets begin at
`DAT_1037444c`; the first four use full point-shadow size and the last four use
half size.

## Deferred light shadow record

The renderer uses records of size `0x68`.

| Offset | Observed value |
| ---: | --- |
| `+0x14` | shadow render-surface/SRV source |
| `+0x18` | 4x4 world-to-shadow matrix |

Only one target and one matrix are present for a directional light.

## `GraphicsLight`

| Offset | Observed value |
| ---: | --- |
| `+0x00` | light type |
| `+0x38..+0x44` | color/vector fields used by light binding |
| `+0x48` | distance/radius, default `10.0` |
| `+0x4c` | shadow intensity, default `1.0` |
| `+0x50` | casts-shadows flag, default true |
| `+0x51` | adjacent light-enable/status flag |

## `GraphicsMeshInstance`

| Offset | Observed value |
| ---: | --- |
| `+0x04` | `GraphicsMesh` resource |
| `+0x20c` | `shadowBias` float |

`SetShaderParameters` at `0x10173480` resolves a parameter named
`shadowBias` and uploads `+0x20c`. `SetShadowBias` at `0x10175a20` writes it.
The instance constructor initializes it to approximately `0.01`. The audited
directional DXBC vertex programs contain only the world-to-screen matrix (and
bones for skinned variants), so this parameter is not consumed by those
programs.

## `GraphicsShadowMapDx11`

| Offset | Observed value |
| ---: | --- |
| `+0x6c` | region/context used to resolve world-space entities |
| `+0x70..+0xcc` | light-basis transform and absolute-axis extents |
| `+0xd0..+0xe4` | accumulated visible light-space AABB |
| `+0xe8..+0xfc` | accumulated finite world-space AABB |

`AddSurface` updates both AABBs and a renderable collection. The directional
DX11 fitter does not read either AABB; the data is vestigial for that path.

## DX9/DX11 shared prefix and directional compatibility

The audited DX9 and DX11 shadow-map objects place the collection at
`+0x00..+0x08`, the copied light at `+0x0c`, the region at `+0x6c`, and the
light basis and accumulated bounds at `+0x70..+0xfc`. However, their
constructors do not initialize those fields identically for a directional
light: DX9 constructs a light-space basis, while DX11 leaves the vestigial
fields unused because its native fitter builds a separate projection.

`GraphicsShadowMapDx9::RenderDirectional` reads the vector only once at
`0x10192e43`, comparing `+0x00` and `+0x04` as an early-out gate. It does not
read, write, or destroy any of the `0x78`-byte records. Its actual caster list
is queried independently after constructing the completed convex volume.

The method then consumes the copied light at `+0x0c`, region at `+0x6c`,
directional basis at `+0xb8..+0xc0`, and finite world AABB at `+0xe8..+0xfc`.
It neither checks a concrete shadow-map vtable nor calls a D3D9 device method.

The replacement therefore constructs a temporary DX9 object from the live
DX11 light and region, shallowly aliases the live source vector, and copies
the live world AABB. If the live vector is empty but the AABB proves that
`AddSurface` ran, a local non-owning `0x78`-byte bounds-only sentinel satisfies
the early-out gate. The temporary is never destructed because it owns no
vector allocation; the three aliased vector pointers are cleared after the
call. The native method performs its normal independent caster query and
renders through the shared active backend.

## `Frustum`

The copied object is `0x404` bytes: a plane count plus capacity for 64
four-float planes. The DX9 path builds and validates this plane set. The DX11
directional path accepts the object as an argument but does not use it for
fitting.
