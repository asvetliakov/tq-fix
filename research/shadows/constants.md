# Shadow constants

Preferred Engine.dll virtual addresses are shown for the supported build.

| Address | Value | Confirmed use |
| --- | ---: | --- |
| `0x102f93c4` | `0.0` | near end of the DX11 camera-ray crop |
| `0x102f9420` | `0.0001` | numerical tolerance in geometry/math paths |
| `0x102f9550` | `0.325` | far end of the DX11 camera-ray crop |
| `0x102f9578` | `0.4` | shared renderer constant; not the shadow split |
| `0x102f95ac` | `0.5` | clip-to-texture bias and midpoint calculations |
| `0x102f95f4` | `0.75` | shared renderer constant |
| `0x102f9648` | `1.0` | identity, depth clear, and homogeneous math |
| `0x102f96a8` | `1.5` | DX11 light-camera depth extent multiplier |
| `0x102f96d0` | `1.6` | shared renderer constant |
| `0x102f9770` | `2.0` | normalized-screen conversions |
| `0x102f9964` | `50.0` | shared renderer constant |
| `0x102f9aa0` | `-0.5` | clip-to-texture Y scale |
| `0x102f9aa4` | `-1.0` | camera-corner reconstruction |
| `0x102f9ac8` | `-2.0` | shared renderer constant |
| `0x102f9afc` | `-10.0` | shared renderer constant |

The important value is `0.325`: it is a ray interpolation parameter, not a
world-space distance, percentage of screen width, cascade split, or receiver
fade distance. Replacing it with `0.70` expands the single fitted map; it does
not create a second level of detail.

The shipped fix does not overwrite this shared constant. All eleven audited
operands inside DX11 `RenderDirectional` are redirected to one mod-owned float
holding the configured split, so point shadows and unrelated engine code keep
reading the original `0.325`. Coverage scales as `split^1.90`; see
`findings.md`.
