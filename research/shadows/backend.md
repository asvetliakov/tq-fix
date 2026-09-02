# D3D11 resources and render state

## Allocation

`GraphicsDeferredRendererX::CreateRenderTargets` at `0x10168190` creates:

- one global square `directionalShadowTarget`;
- eight square point-shadow targets, with indices 0-3 at full configured
  point size and 4-7 at half size;
- no color target for shadow rendering.

Native quality sizes are:

| Quality | Directional | Point |
| --- | ---: | ---: |
| Low | 512 | 256 |
| Medium | 1024 | 512 |
| High | 2048 | 1024 |

The current enhanced-shadows mod doubles supported map dimensions, so High
uses 4096 for the directional target. Resolution changes density, not the
projection footprint, and therefore cannot solve the cutoff.

## Direct3D11.dll surface

Backend target type 9 resolves to a typeless depth texture:

| Object | Format/state |
| --- | --- |
| texture | `DXGI_FORMAT_R32_TYPELESS` (`0x27`) |
| bind flags | depth-stencil + shader-resource (`0x48`) |
| DSV | `DXGI_FORMAT_D32_FLOAT` (`0x28`) |
| SRV | `DXGI_FORMAT_R32_FLOAT` (`0x29`) |
| mip levels / array size | 1 / 1 |
| multisampling | none |

This format is appropriate for manual depth comparison and preserves the full
float depth value. No shadow color buffer is involved.

## Render sequence

`GraphicsShadowMapRenderer::Render` at `0x1018ce70`:

1. begins a GPU marker named `GraphicsShadowMapRenderer`;
2. binds the depth target;
3. sets the target viewport and an inset scissor based on target dimensions;
4. clears depth to `1.0`;
5. loads `Shaders/Pieces/Shadow.ssh`;
6. renders the temporary caster scene in shadow-only mode;
7. restores the canvas default state and prior viewport;
8. ends the GPU marker.

Relevant abstract render-device vtable slots used by this sequence are
`+0x34` target binding, `+0x4c/+0x50` dimensions, `+0x60` viewport,
`+0x68` scissor, `+0x6c` clear state, `+0x74` depth clear, and
`+0x1cc/+0x1d0` GPU markers.

## Light binding

The deferred binding routine at `0x10164640` selects the shadowed directional
or point style only when the light, render target, global enable, and options
state all permit it. It binds:

- `shadowTexture` from light-record offset `+0x14`;
- `worldToShadowMatrix` from `+0x18`;
- `shadowBluriness = renderer_shadow_softness / shadow_map_width`;
- `LightShadowIntensity` from `GraphicsLight +0x4c`.

There is no second directional texture, second matrix, cascade split, or
receiver array binding hidden in this code.
