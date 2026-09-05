#pragma once

#include <windows.h>

namespace tq {
namespace shadow {

// Widens Titan Quest's directional shadow projection.
//
// The engine fits the shadow map to the camera frustum evaluated between t=0
// and a fixed ray parameter of 0.325. That constant predates widescreen: the
// corner rays of a wide frustum travel much further, so the same split covers
// proportionally less of the screen and shadows are cut off toward the left
// and right edges. Redirecting the eleven reads of that constant inside
// GraphicsShadowMapDx11::RenderDirectional to a configured value widens the
// fitted box without touching the shared engine constant.
//
// Coverage scales as split^1.90, measured from two fitted projections, so a
// wider split costs texel density. Pair it with a larger shadow map.
void install(HMODULE engineModule);
void shutdown();

// Reported by the texture-creation hook for every shadow map it creates, with
// the size actually created rather than the size the game asked for. The fit
// stabiliser snaps the projection centre onto the *directional* map's texel
// grid, because that is the projection being fitted; the smallest map seen is
// kept only as a fallback for the lowest shadow quality, where no request is
// large enough to be classified as directional.
void noteShadowMapSize(unsigned texels, bool directional);

// Forgets every reported size. Called when the renderer drops and rebuilds its
// shadow targets (a resolution or quality change), so a smaller regenerated
// map is not snapped onto the old, finer grid.
void resetShadowMapSizes();

// PCF tap offsets are UV distances, so the blur they produce measures
// 0.5 * bluriness * world coverage: widening the projection softens shadow
// edges in world space no matter how large the map is. Returns the factor that
// holds world-space softness at what the native split produces. 1.0 means no
// change. Reads configuration directly and is safe to call before install().
float blurCompensation();

// The receiver's depth bias is normalised to the fitted depth range, so a
// wider split scales it up in world units and detaches shadows from their
// casters. Returns the factor that holds the bias at its native world size.
float biasCompensation();

// Whether the receiver's four taps should sit on the corners of a 3x3
// footprint instead of the native axis cross, covering an area rather than a
// cross for the same four texture instructions.
bool cornerFilterEnabled();

// Screen-space contact shadows: an optional short-range occlusion march the
// deferred receiver runs after its PCF taps. The shadow map cannot resolve
// contact detail below one texel -- about 0.03 world units at the shipped
// defaults -- and raising density costs coverage, which is already the
// constraint on a wide display. The march only ever darkens: it is combined
// with the native shadow term by `min`.
struct ContactSettings {
    bool enabled;
    unsigned steps;    // march samples, 4..16
    float length;      // total march distance, world units
    float bias;        // depth comparison slack, world units
    float thickness;   // largest depth gap still treated as an occluder, world units
    float strength;    // 0..1, how dark a fully occluded pixel becomes
    // Least upward-facing a surface may be and still receive the march, as the
    // world Y of its G-buffer normal. Default 0 accepts upward and vertical
    // surfaces but excludes downward-facing ones; -1 accepts everything. This
    // does not identify materials or prevent grass casting onto other surfaces.
    float upright;
    bool toggle;       // [debug] shadow_contact_toggle: Ctrl+Shift+C compares
    bool timing;       // [debug] shadow_contact_timing: time the receiver pass
};

// Read once with the rest of the shadow dials. Enabled unless explicitly off.
const ContactSettings& contactSettings();

#ifdef TQ_SELFTEST
void resetShadowMapSizesForTest();
bool validateSupportedImageForTest(HMODULE engineModule);
bool redirectCropRoundTripForTest(HMODULE engineModule);
bool validateFitCameraCallForTest(HMODULE engineModule);
bool retargetFitCameraCallRoundTripForTest(HMODULE engineModule);
bool validateBasisCallForTest(HMODULE engineModule);
const float* chooseReferenceUpForTest(const float* direction,
                                      const float* fallback);
void stabilizeFitForTest(void* camera, unsigned texels, unsigned steps);
#endif

}  // namespace shadow
}  // namespace tq
