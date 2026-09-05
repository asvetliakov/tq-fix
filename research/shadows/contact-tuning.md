# Approved contact-shadow defaults — 2026-09-05

Approved profile: [presets/contact-balanced.ini](presets/contact-balanced.ini).
After comparing the effect in gameplay and testing the normal threshold, the
user selected `shadow_contact_upright=0.0` and requested that it and the current
profile become defaults. Contact shadows now default to on with the settings
below. Debug tracing and the comparison hotkey retain their separate opt-ins.
The initial stronger candidate used upright 0.25; the approval changes it to
0.0 so vertical faces can also receive the effect.

## Evidence

The screenshot is a bright, elevated, zoomed-out village view. Broad building
and tree shadows already exist. Figures and cart wheels occupy little screen
area, while the fields, roofs and cobbles have dense texture detail. Additional
shadowing was initially intended to clarify feet, wheel contacts and the bases
of stonework. In the user's subsequent before/after comparison, the clearest
benefit was richer vegetation patches; improvement on buildings and small
props was limited. The approved profile retains that modest vegetation benefit
without increasing strength or reach to force a more obvious change elsewhere.

The earlier, weaker-profile trace snapshot contained 6,144 rows. Filtering for
more than 500 indexed draws and a marched receiver gives 3,874 gameplay rows. Every one had a
successful refresh, with zero invalid captures, neutral frames or full-ring
frames. Median and p95 history age were both one frame. This is now active
shading; the previous padded-buffer failure is resolved.

Of these rows, 3,024 had active contact strength and 850 had strength toggled
off. The full receiver's median GPU time was 0.705 ms active and 0.391 ms off;
the difference is about 0.31 ms. These are descriptive session statistics,
not a controlled same-scene performance delta. CPU refresh median was about
0.008–0.009 ms. Lowering the normal gate and adding samples increases the
eligible work; these figures do not measure the final approved profile's cost.

## Approved settings

| Key | Previous | Approved | Why |
| --- | ---: | ---: | --- |
| `shadow_contact` | on | on | Ship enabled as requested after gameplay comparison; users can still disable it. |
| `shadow_contact_steps` | 8 | 12 | Preserve sampling density as reach grows; more taps improve coverage, not darkness by themselves. |
| `shadow_contact_length` | 0.20 | 0.35 | Reach farther around wheels, feet and stones so the result can survive the small screen footprint. Avoid starting with a long 0.7–1.0 ray in dense vegetation. |
| `shadow_contact_bias` | 0.020 | 0.012 | Admit smaller near-contact depth separations; retain nonzero slack against false self-occlusion. |
| `shadow_contact_thickness` | 0.15 | 0.30 | The old narrow falloff discarded or heavily weakened nearby occluders. Widen it moderately without accepting a large foreground/background gap. |
| `shadow_contact_strength` | 0.50 | 0.70 | Increase the contrast of accepted contact shading while retaining headroom below maximum strength. This is not 70% scene darkening. |
| `shadow_contact_upright` | 0.50 | 0.0 | User-preferred threshold: upward and vertical faces qualify, downward-facing surfaces do not. This remains a geometric gate, not a material mask. |

Step spacing changes from 0.025 to approximately 0.0292 world units, only
17% farther apart despite 75% more reach. Twelve steps permit 50% more depth
fetches per eligible pixel than eight; sixteen would double them and is not
the first choice for a 5120×1440 frame.

Using the captured gameplay projection from probe 18 (frame 3007), at the
logged centre view depth of 40.161 the ray's search footprint grows from
11.74 to 20.58 native pixels. First-step displacement grows from 1.46 to 1.71
pixels. At hypothetical depths 50 and 60 the new reach is approximately 16.5
and 13.8 pixels. These are projections of the search ray, not predictions of
the visible shadow width. The screenshot alone does not supply its per-pixel
depth or an off/on difference image.

## Why strength alone was insufficient

For each accepted sample, the implementation uses a weight
`max(0, 1 - depthGap / thickness)`, provided `depthGap > bias`. Weights are
summed, divided by the total number of steps and multiplied by strength.
Normals below the threshold and invalid ray points contribute nothing.

As an illustrative example, two hits with a 0.10-unit depth gap yield:

- Previous: `0.50 × (2/8) × (1 - 0.10/0.15) = 4.17%` attenuation.
- Candidate: `0.70 × (2/12) × (1 - 0.10/0.30) = 7.78%` attenuation.

This is an example of the direct-light contact term, not a measured pixel
change. Hit count can change with reach and sampling. Ambient lighting remains,
and combining with the native term using `min` means an already darker native
shadow may hide the contact change entirely. The screenshot's large existing
building shadows are therefore poor indicators of this effect's visibility.

## Comparison guidance

Restart to load the new INI, return to the same zoom and location, and toggle
Ctrl+Shift+C while stationary. Judge wheel-to-ground joins on the cart left of
the fountain, feet on the pale paths, and exposed stones at the road edges.
Then walk past them to assess shimmer. Fields and the sunlit road should keep
their overall brightness.

If contact edges look too dark, reduce strength to 0.60 first. If dark fringes
extend too far around vegetation, reduce length to 0.30 or thickness to 0.25.
If there is fine speckling on otherwise exposed surfaces, restore bias to
0.02; raising the upward-normal threshold can restrict receiving surfaces but
cannot exclude grass as a caster. Change one setting at a time after this
coordinated profile comparison.

The local backup location is recorded in `build/contact-tuning-backup-path.txt`.
