#include "contact_shadow.h"

#include <float.h>
#include <math.h>
#include <string.h>

#include "hdr.h"
#include "shadow_fix.h"
#include "probe.h"

namespace tq {
namespace contact {

namespace {

// The receiver reads twelve float4s. The allocation can be larger: the live
// renderer binds a 2048-byte dynamic buffer for this 192-byte shader layout.
const UINT kConstantBytes = 192;
const unsigned kInverseViewProjection = 8;  // cb0[8..11], rows
const unsigned kLightDirection = 1;         // cb0[1].xyz, unnormalised
const unsigned kCameraPosition = 3;         // cb0[3].xyz; .w is bluriness

// b13: four view-projection rows, VP times the light, the parameters, and
// the two coefficients that turn a sampled NDC depth into a view depth.
const UINT kParameterSlot = 13;
const unsigned kParameterVectors = 7;
const UINT kParameterBytes = kParameterVectors * 16;

ID3D11DeviceContext* g_context;
MapFn g_map;
UnmapFn g_unmap;
// Pending slots are never overwritten. Consume completed copies before issuing
// another one; a slow GPU must not turn this optional effect into a CPU wait.
const unsigned kReadbackSlots = 4;
const unsigned kMaxHistoryAge = 8;
ID3D11Buffer* g_readback[kReadbackSlots];
bool g_readbackFilled[kReadbackSlots];
unsigned g_readbackFrame[kReadbackSlots];
ID3D11Buffer* g_parameters;
ID3D11Buffer* g_savedParameters;
bool g_parametersBound;
const GUID kReceiverTag = {0x63db0fc2, 0xe743, 0x451c,
    {0xa1, 0x37, 0x20, 0x0f, 0xf7, 0x87, 0x14, 0x31}};
bool g_installed;
bool g_describedSource;
bool g_refreshedThisFrame;
unsigned g_frame;
unsigned g_probes;
unsigned g_failures;
// How the readback is actually behaving. A staging buffer that is rarely ready
// leaves the march pinned to a stale camera while looking perfectly healthy in
// every other respect, so the counters are reported with each probe.
unsigned g_attempts;
unsigned g_updates;
unsigned g_skips;
unsigned g_updatedFrame;
// Runtime comparison changes strength. The shader skips its march at zero;
// use shadow_contact=off and restart for a baseline without added shader code.
bool g_active = true;
bool g_toggleKeyDown;
// The last block uploaded, so a toggle takes effect on the next frame rather
// than waiting for a readback to land.
float g_values[kParameterVectors * 4];
bool g_valuesReady;
#ifdef TQ_SELFTEST
bool g_forceReadbackBusy;
bool g_onlyNonblockingMaps = true;
#endif
HRESULT mapReadback(ID3D11DeviceContext* context, ID3D11Buffer* buffer,
                    D3D11_MAPPED_SUBRESOURCE* mapped) {
    const UINT flags = D3D11_MAP_FLAG_DO_NOT_WAIT;
#ifdef TQ_SELFTEST
    g_onlyNonblockingMaps &= flags == D3D11_MAP_FLAG_DO_NOT_WAIT;
    if (g_forceReadbackBusy) return DXGI_ERROR_WAS_STILL_DRAWING;
#endif
    return g_map(context, buffer, 0, D3D11_MAP_READ, flags, mapped);
}
unsigned g_probedFrame;
float g_probedCamera[3];
float g_probedScale;
void release(IUnknown* object) { if (object) object->Release(); }

// ------------------------------------------------------------------- timing
//
// GPU timestamps bracketing the deferred receiver's own draw, which is the
// pass the march was added to. Four sets in a ring, read three frames later so
// collecting never waits on the GPU. Timing runs with the effect off as well:
// the same bracket around the unmarched pass is the only honest baseline.
//
// Through a Metal translation layer these are approximate -- work either side
// can overlap the bracket -- so the number is a guide to the order of
// magnitude, not a profiler reading.
const unsigned kTimingSlots = 4;

struct TimingSlot {
    ID3D11Query* disjoint;
    ID3D11Query* start;
    ID3D11Query* end;
    bool pending;
    unsigned frame;
};

TimingSlot g_timing[kTimingSlots];
bool g_timingReady;
unsigned g_timingSlot;
unsigned g_samples;
double g_totalMs;
double g_shortestMs;
double g_longestMs;
unsigned g_timingWindow;
unsigned g_disjointFrames;

void releaseTiming() {
    for (unsigned i = 0; i < kTimingSlots; ++i) {
        release(g_timing[i].disjoint);
        release(g_timing[i].start);
        release(g_timing[i].end);
    }
    memset(g_timing, 0, sizeof(g_timing));
    g_timingReady = false;
    g_timingSlot = g_samples = g_timingWindow = g_disjointFrames = 0;
    g_totalMs = g_longestMs = 0.0;
    g_shortestMs = 1.0e9;
}

bool createTiming(ID3D11Device* device) {
    D3D11_QUERY_DESC disjoint = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC stamp = {D3D11_QUERY_TIMESTAMP, 0};
    for (unsigned i = 0; i < kTimingSlots; ++i) {
        if (FAILED(device->CreateQuery(&disjoint, &g_timing[i].disjoint))
            || FAILED(device->CreateQuery(&stamp, &g_timing[i].start))
            || FAILED(device->CreateQuery(&stamp, &g_timing[i].end))) {
            releaseTiming();
            return false;
        }
    }
    g_shortestMs = 1.0e9;
    g_timingReady = true;
    return true;
}

// Collects whatever the GPU has finished with. Never waits: a slot that is not
// ready is simply left for the next frame.
void collectTiming(ID3D11DeviceContext* context) {
    if (!g_timingReady) return;
    for (unsigned i = 0; i < kTimingSlots; ++i) {
        TimingSlot& slot = g_timing[i];
        if (!slot.pending || g_frame - slot.frame < 3) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT period = {};
        UINT64 start = 0, end = 0;
        if (context->GetData(slot.disjoint, &period, sizeof(period),
                             D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
            || context->GetData(slot.start, &start, sizeof(start),
                                D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
            || context->GetData(slot.end, &end, sizeof(end),
                                D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        slot.pending = false;
        if (period.Disjoint || !period.Frequency || end <= start) {
            ++g_disjointFrames;
            continue;
        }
        const double ms = 1000.0 * (double)(end - start) / (double)period.Frequency;
        ++g_samples;
        g_totalMs += ms;
        if (ms < g_shortestMs) g_shortestMs = ms;
        if (ms > g_longestMs) g_longestMs = ms;
    }
    if (++g_timingWindow < 600 || !g_samples) return;
    const tq::shadow::ContactSettings& settings = tq::shadow::contactSettings();
    tq::hdr::log("Contact shadow timing: deferred receiver %.3f ms mean over %u"
                 " samples (min %.3f, max %.3f), march=%s, %u steps,"
                 " %u unusable\r\n",
                 g_totalMs / g_samples, g_samples, g_shortestMs, g_longestMs,
                 settings.enabled ? (g_active ? "on" : "on, strength zeroed")
                                  : "off",
                 settings.enabled ? settings.steps : 0u, g_disjointFrames);
    g_timingWindow = g_samples = g_disjointFrames = 0;
    g_totalMs = g_longestMs = 0.0;
    g_shortestMs = 1.0e9;
}




float dot4(const float row[4], const float x, const float y, const float z,
           const float w) {
    return row[0] * x + row[1] * y + row[2] * z + row[3] * w;
}

// Projects a world point with a DP4-convention matrix and divides through.
// Returns false when the point is on or behind the plane w = 0.
bool project(const float matrix[16], const float world[3], float ndc[3]) {
    float clip[4];
    for (unsigned i = 0; i < 4; ++i)
        clip[i] = dot4(matrix + i * 4, world[0], world[1], world[2], 1.0f);
    if (!(fabsf(clip[3]) > 1.0e-6f)) return false;
    for (unsigned i = 0; i < 3; ++i) ndc[i] = clip[i] / clip[3];
    return true;
}

// The receiver's own reconstruction: screen UV and sampled depth become NDC,
// the inverse view-projection takes that to homogeneous world space, and the
// divide finishes it.
bool unproject(const float inverse[16], const float ndc[3], float world[3]) {
    float homogeneous[4];
    for (unsigned i = 0; i < 4; ++i)
        homogeneous[i] = dot4(inverse + i * 4, ndc[0], ndc[1], ndc[2], 1.0f);
    if (!(fabsf(homogeneous[3]) > 1.0e-9f)) return false;
    for (unsigned i = 0; i < 3; ++i) world[i] = homogeneous[i] / homogeneous[3];
    return true;
}

bool finite16(const float m[16]) {
    for (unsigned i = 0; i < 16; ++i)
        if (!_finite(m[i])) return false;
    return true;
}

// Largest absolute deviation of inverse * forward from the identity. A healthy
// projection matrix lands around 1e-6; anything larger means the read back
// values were not a view-projection at all.
float identityResidual(const float a[16], const float b[16]) {
    float worst = 0.0f;
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 4; ++j) {
            double sum = 0.0;
            for (unsigned k = 0; k < 4; ++k)
                sum += (double)a[i * 4 + k] * (double)b[k * 4 + j];
            float error = (float)fabs(sum - (i == j ? 1.0 : 0.0));
            if (error > worst) worst = error;
        }
    return worst;
}

struct Frame {
    float inverseViewProjection[16];
    float viewProjection[16];
    float lightDirection[3];
    float cameraPosition[3];
    float bluriness;
    float width;
    float height;
};

bool readConstants(const float* cb, Frame* frame) {
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned c = 0; c < 4; ++c)
            frame->inverseViewProjection[row * 4 + c] =
                cb[(kInverseViewProjection + row) * 4 + c];
    if (!finite16(frame->inverseViewProjection)) return false;
    // Linear depth below assumes perspective with no X/Y term in inverse W.
    // Reject unsupported projections instead of uploading misleading depths.
    if (fabsf(frame->inverseViewProjection[12]) > 1.0e-6f
        || fabsf(frame->inverseViewProjection[13]) > 1.0e-6f
        || fabsf(frame->inverseViewProjection[14]) < 1.0e-6f) return false;

    // The shader normalises cb0[1] before using it, and uses the result as the
    // direction towards the light: N dot L is its diffuse term. The march
    // follows the same vector.
    float length = 0.0f;
    for (unsigned i = 0; i < 3; ++i) {
        frame->lightDirection[i] = cb[kLightDirection * 4 + i];
        length += frame->lightDirection[i] * frame->lightDirection[i];
    }
    length = sqrtf(length);
    if (!(length > 1.0e-6f) || !_finite(length)) return false;
    for (unsigned i = 0; i < 3; ++i) frame->lightDirection[i] /= length;

    for (unsigned i = 0; i < 3; ++i)
        frame->cameraPosition[i] = cb[kCameraPosition * 4 + i];
    frame->bluriness = cb[kCameraPosition * 4 + 3];
    return invertRowMatrix(frame->inverseViewProjection, frame->viewProjection);
}

// What proves b13 before any shader reads it. The CPU cannot see the march, so
// it checks the three things that would make the march meaningless: that the
// uploaded matrix really is the inverse of what the receiver holds, that it is
// the right way round rather than transposed, and that a pixel reconstructed
// through it lands somewhere physically sensible. Rate limited, and tracing
// must be on in tqflicker.ini for any of it to appear.
void logProbe(const Frame& frame, const tq::shadow::ContactSettings& settings,
              const float clipLight[4], float stepLength) {
    const char* names[4] = {"row0", "row1", "row2", "row3"};
    tq::hdr::log("Contact shadow probe %u (frame %u, last landed %u): %u of %u"
                 " refreshes landed, %u refused\r\n",
                 g_probes, g_frame, g_updatedFrame, g_updates, g_attempts,
                 g_skips);
    for (unsigned i = 0; i < 4; ++i)
        tq::hdr::log("  invVP %s = % .6f % .6f % .6f % .6f\r\n", names[i],
                     frame.inverseViewProjection[i * 4 + 0],
                     frame.inverseViewProjection[i * 4 + 1],
                     frame.inverseViewProjection[i * 4 + 2],
                     frame.inverseViewProjection[i * 4 + 3]);
    for (unsigned i = 0; i < 4; ++i)
        tq::hdr::log("  VP    %s = % .6f % .6f % .6f % .6f\r\n", names[i],
                     frame.viewProjection[i * 4 + 0],
                     frame.viewProjection[i * 4 + 1],
                     frame.viewProjection[i * 4 + 2],
                     frame.viewProjection[i * 4 + 3]);
    // Grows with the world coordinates, because the inverse's own entries carry
    // their magnitude: 5e-6 near the origin, 3e-3 two hundred units out. The
    // round trip below is the number that matters.
    tq::hdr::log("  inverse residual = %.3e (grows with world scale)\r\n",
                 identityResidual(frame.inverseViewProjection,
                                  frame.viewProjection));
    // Measured in game between 1e-6 and 6e-5 across the menu and open world.
    // Anything of order 1 means the matrix is wrong way round.
    tq::hdr::log("  NDC round trip, matrix route = %.3e"
                 " (expect < 1e-3, O(1) means a wrong matrix)\r\n",
                 ndcRoundTripError(frame.inverseViewProjection,
                                   frame.viewProjection));

    // The camera sits at the view-space origin, so its clip W -- the fourth
    // DP4 -- must be zero. This is what catches a transposed upload: the
    // round trip alone would still pass against a consistently transposed
    // pair, because a transpose is its own inverse's transpose.
    const float* c = frame.cameraPosition;
    tq::hdr::log("  camera = (% .3f, % .3f, % .3f) bluriness = %.6f\r\n",
                 c[0], c[1], c[2], frame.bluriness);
    tq::hdr::log("  clip W at the camera = % .6f (expect ~0)\r\n",
                 dot4(frame.viewProjection + 12, c[0], c[1], c[2], 1.0f));

    // A point reconstructed from the middle of the screen must lie in front of
    // the camera, at a positive distance that grows with the sampled depth.
    for (unsigned n = 0; n < 2; ++n) {
        const float depth = n ? 0.9f : 0.1f;
        const float ndc[3] = {0.0f, 0.0f, depth};
        float world[3] = {0.0f, 0.0f, 0.0f};
        if (!unproject(frame.inverseViewProjection, ndc, world)) {
            tq::hdr::log("  centre at depth %.2f: unprojection failed\r\n", depth);
            continue;
        }
        float dx = world[0] - c[0], dy = world[1] - c[1], dz = world[2] - c[2];
        tq::hdr::log("  centre at depth %.2f -> (% .3f, % .3f, % .3f),"
                     " %.3f units from the camera\r\n",
                     depth, world[0], world[1], world[2],
                     sqrtf(dx * dx + dy * dy + dz * dz));
    }

    const float* l = frame.lightDirection;
    tq::hdr::log("  light = (% .4f, % .4f, % .4f)"
                 "  VP*L = (% .6f, % .6f, % .6f, % .6f)\r\n",
                 l[0], l[1], l[2],
                 clipLight[0], clipLight[1], clipLight[2], clipLight[3]);
    tq::hdr::log("  b13[5] = step %.5f bias %.5f thickness %.5f strength %.3f"
                 " (%u steps over %.3f world units)\r\n",
                 stepLength, settings.bias, settings.thickness,
                 settings.strength, settings.steps, settings.length);
    // The linearisation assumes an axis-aligned projection. It has held at
    // every camera sampled; report the two terms that would break it rather
    // than trusting it silently.
    tq::hdr::log("  b13[6] = upright gate %.3f (normal Y >= %.3f),"
                 " 1/steps %.4f\r\n",
                 (settings.upright + 1.0f) * 0.5f, settings.upright,
                 settings.steps ? 1.0f / (float)settings.steps : 0.0f);
    tq::hdr::log("  b13[6] = linearise (% .6f, % .6f), off-axis terms"
                 " (% .2e, % .2e)\r\n",
                 frame.inverseViewProjection[14], frame.inverseViewProjection[15],
                 frame.inverseViewProjection[12], frame.inverseViewProjection[13]);

    // What the configured march actually costs on screen, which is the only
    // thing that decides whether a world-space length is a contact shadow or a
    // full ray march. A fixed world length has a screen footprint that varies
    // with 1/distance, so it is reported across the depth range rather than at
    // one point. Steps far apart in pixels step over thin occluders; a total
    // far larger than a contact region is not a contact shadow.
    if (!(frame.width > 0.0f) || !(frame.height > 0.0f)) {
        tq::hdr::log("  march footprint unavailable: no single viewport bound\r\n");
        return;
    }
    tq::hdr::log("  march footprint at screen centre, viewport %.0fx%.0f:\r\n",
                 frame.width, frame.height);
    tq::hdr::log("     depth   distance   total px   px/step\r\n");
    const float depths[6] = {0.1f, 0.3f, 0.5f, 0.7f, 0.9f, 0.98f};
    for (unsigned n = 0; n < 6; ++n) {
        const float depth = depths[n];
        const float hw = dot4(frame.inverseViewProjection + 12, 0.0f, 0.0f,
                              depth, 1.0f);
        if (!(fabsf(hw) > 1.0e-9f)) continue;
        float first[2] = {0.0f, 0.0f}, last[2] = {0.0f, 0.0f};
        for (unsigned i = 1; i <= settings.steps; ++i) {
            const float t = (float)i * stepLength * hw;
            const float clip[4] = {t * clipLight[0], t * clipLight[1],
                                   depth + t * clipLight[2],
                                   1.0f + t * clipLight[3]};
            if (!(fabsf(clip[3]) > 1.0e-9f)) break;
            const float x = (clip[0] / clip[3] * 0.5f + 0.5f) * frame.width;
            const float y = (clip[1] / clip[3] * -0.5f + 0.5f) * frame.height;
            if (i == 1) { first[0] = x - 0.5f * frame.width;
                          first[1] = y - 0.5f * frame.height; }
            last[0] = x - 0.5f * frame.width;
            last[1] = y - 0.5f * frame.height;
        }
        tq::hdr::log("    %6.2f %10.3f %10.1f %9.1f\r\n", depth, 1.0f / hw,
                     sqrtf(last[0] * last[0] + last[1] * last[1]),
                     sqrtf(first[0] * first[0] + first[1] * first[1]));
    }
}

// Fills b13 from a frame's constants. Kept separate from the upload so a
// refresh that produces nothing usable leaves the previous frame's values in
// place rather than a partly written buffer.
void buildParameters(const Frame& frame,
                     const tq::shadow::ContactSettings& settings,
                     float out[kParameterVectors * 4]) {
    memcpy(out, frame.viewProjection, sizeof(frame.viewProjection));
    const float stepLength =
        settings.steps ? settings.length / (float)settings.steps : 0.0f;
    float* clipLight = out + 16;
    for (unsigned i = 0; i < 4; ++i)
        clipLight[i] = dot4(frame.viewProjection + i * 4,
                            frame.lightDirection[0], frame.lightDirection[1],
                            frame.lightDirection[2], 0.0f);
    float* parameters = out + 20;
    parameters[0] = stepLength;
    parameters[1] = settings.bias;
    // The reciprocal: the shader multiplies by it to get a falloff, so the
    // division happens once here rather than per step per pixel.
    parameters[2] = settings.thickness > 0.0f ? 1.0f / settings.thickness : 0.0f;
    parameters[3] = g_active ? settings.strength : 0.0f;

    // The march compares depths in world units, so the shader needs to undo
    // the projection: view depth is 1 / (a * ndcZ + b), taken from the inverse
    // view-projection's fourth row. Its X and Y are zero for an axis-aligned
    // projection, which every camera sampled in game has been.
    float* linearize = out + 24;
    linearize[0] = frame.inverseViewProjection[14];
    linearize[1] = frame.inverseViewProjection[15];
    // The receiver gate, converted into the G-buffer normal's encoded range so
    // the shader compares without decoding: n.y = 2*texel - 1, so n.y >= t is
    // texel >= (t + 1) / 2. The 0 default becomes 0.5; -1 admits everything.
    linearize[2] = (settings.upright + 1.0f) * 0.5f;
    // Averaging the occluded steps is what keeps the term from latching to full
    // strength on one hit, which is what made thin geometry speckle.
    linearize[3] = settings.steps ? 1.0f / (float)settings.steps : 0.0f;

    const float scale = frame.viewProjection[5];
    const float moved =
        fabsf(frame.cameraPosition[0] - g_probedCamera[0])
        + fabsf(frame.cameraPosition[1] - g_probedCamera[1])
        + fabsf(frame.cameraPosition[2] - g_probedCamera[2]);
    const bool changed = moved > 0.05f || fabsf(scale - g_probedScale) > 0.01f;
    if (tq::hdr::runtime().settings.trace && g_probes < 24
        && (g_probes < 3 || (changed && g_frame - g_probedFrame >= 30))) {
        ++g_probes;
        g_probedFrame = g_frame;
        g_probedScale = scale;
        memcpy(g_probedCamera, frame.cameraPosition, sizeof(g_probedCamera));
        logProbe(frame, settings, clipLight, stepLength);
    }
}

// The neutral contents b13 holds until the first successful readback: an
// identity transform, a zero-length step and zero strength. The march reads
// these on the first frames and on any frame the readback is not ready, and
// must produce no darkening from them.
void neutralParameters(float out[kParameterVectors * 4]) {
    memset(out, 0, kParameterVectors * 4 * sizeof(float));
    for (unsigned i = 0; i < 4; ++i) out[i * 4 + i] = 1.0f;
    // A zero linearisation would divide by zero and hand the comparison a NaN.
    // With b = 1 both depths come out as one, their difference as zero, and no
    // step can be occluded whatever the other parameters say. The gate and the
    // step reciprocal stay zero, which cannot lighten anything either.
    out[25] = 1.0f;
}

bool upload(ID3D11DeviceContext* context, const float* values) {
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(g_map(context, g_parameters, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))
        || !mapped.pData)
        return false;
    memcpy(mapped.pData, values, kParameterBytes);
    g_unmap(context, g_parameters, 0);
    return true;
}

// Poll each outstanding copy once, keeping only the newest usable result.
// Even when the GPU is many frames behind, no Map is allowed to wait.
void refresh(ID3D11DeviceContext* context) {
    tq::probe::Scope timing(tq::probe::PhaseContactRefresh);
    Frame newest = {};
    bool found = false;
    unsigned newestAge = kMaxHistoryAge + 1;
    if (g_valuesReady && g_frame - g_updatedFrame < newestAge)
        newestAge = g_frame - g_updatedFrame;
    for (unsigned i = 0; i < kReadbackSlots; ++i) {
        if (!g_readbackFilled[i]) continue;
        ++g_attempts;
        tq::probe::count(tq::probe::CounterContactReadbackPoll);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT hr = mapReadback(context, g_readback[i], &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            ++g_skips;
            tq::probe::count(tq::probe::CounterContactReadbackBusy);
            continue;
        }
        g_readbackFilled[i] = false;
        Frame frame = {};
        const bool usable = SUCCEEDED(hr) && mapped.pData
            && readConstants((const float*)mapped.pData, &frame);
        if (SUCCEEDED(hr)) g_unmap(context, g_readback[i], 0);
        const unsigned age = g_frame - g_readbackFrame[i];
        if (!usable) {
            tq::probe::count(tq::probe::CounterContactInvalid);
            if (g_failures++ < 4)
                tq::hdr::log("Contact shadow: invalid readback/projection (0x%08lx)\r\n",
                             (unsigned long)hr);
        } else if (age < newestAge) {
            newest = frame;
            newestAge = age;
            found = true;
        }
    }
    if (found) {
        D3D11_VIEWPORT viewport = {};
        UINT count = 1;
        context->RSGetViewports(&count, &viewport);
        newest.width = viewport.Width;
        newest.height = viewport.Height;
        float values[kParameterVectors * 4];
        buildParameters(newest, tq::shadow::contactSettings(), values);
        if (upload(context, values)) {
            memcpy(g_values, values, sizeof(g_values));
            g_valuesReady = true;
            g_updatedFrame = g_frame - newestAge;
            ++g_updates;
            tq::probe::count(tq::probe::CounterContactReadbackReady);
        } else tq::probe::count(tq::probe::CounterContactInvalid);
    }
    if (g_valuesReady && g_frame - g_updatedFrame > kMaxHistoryAge) {
        float neutral[kParameterVectors * 4];
        neutralParameters(neutral);
        if (upload(context, neutral)) g_valuesReady = false;
    }
    tq::probe::count(tq::probe::CounterContactHistoryAge,
                    g_valuesReady ? g_frame - g_updatedFrame : 0);
    if (!g_valuesReady) tq::probe::count(tq::probe::CounterContactNeutral);

    unsigned slot = 0;
    while (slot < kReadbackSlots && g_readbackFilled[slot]) ++slot;
    if (slot == kReadbackSlots) {
        tq::probe::count(tq::probe::CounterContactRingFull);
        return;
    }
    ID3D11Buffer* bound = nullptr;
    context->PSGetConstantBuffers(0, 1, &bound);
    if (!bound) { tq::probe::count(tq::probe::CounterContactInvalid); return; }
    D3D11_BUFFER_DESC desc = {};
    bound->GetDesc(&desc);
    if (!g_describedSource) {
        g_describedSource = true;
        tq::hdr::log("Contact shadow: cb0 bytes=%u usage=%u bind=0x%x\r\n",
                     desc.ByteWidth, (unsigned)desc.Usage, desc.BindFlags);
    }
    if (desc.ByteWidth >= kConstantBytes) {
        // Buffer copy coordinates are bytes. Capture only the prefix consumed
        // by the shader; CopyResource requires equal allocation sizes.
        const D3D11_BOX prefix = {0, 0, 0, kConstantBytes, 1, 1};
        context->CopySubresourceRegion(g_readback[slot], 0, 0, 0, 0,
                                       bound, 0, &prefix);
        g_readbackFilled[slot] = true;
        g_readbackFrame[slot] = g_frame;
        tq::probe::count(tq::probe::CounterContactReadbackCopy);
    } else {
        tq::probe::count(tq::probe::CounterContactInvalid);
        if (g_failures++ < 4)
            tq::hdr::log("Contact shadow: cb0 too small (%u bytes, need at least %u)\r\n",
                         desc.ByteWidth, kConstantBytes);
    }
    release(bound);
}

// Ctrl+Shift+C, edge triggered, mirroring the bloom comparison key.
void pollToggle(ID3D11DeviceContext* context) {
    if (!tq::shadow::contactSettings().toggle) return;
    const bool chord = (GetAsyncKeyState(VK_CONTROL) & 0x8000)
                    && (GetAsyncKeyState(VK_SHIFT) & 0x8000)
                    && (GetAsyncKeyState('C') & 0x8000);
    if (chord && !g_toggleKeyDown) {
        g_active = !g_active;
        if (g_valuesReady) {
            g_values[23] = g_active ? tq::shadow::contactSettings().strength : 0.0f;
            upload(context, g_values);
        }
        tq::hdr::log("Contact shadow toggle: %s\r\n", g_active ? "on" : "off");
    }
    g_toggleKeyDown = chord;
}

}  // namespace

bool invertRowMatrix(const float in[16], float out[16]) {
    if (!in || !out) return false;
    double m[16];
    for (unsigned i = 0; i < 16; ++i) {
        if (!_finite(in[i])) return false;
        m[i] = in[i];
    }
    // Cofactor expansion in double precision. World coordinates in Titan Quest
    // run to five figures, and the projection's translation row carries their
    // magnitude, so single precision here loses more than the march can spare.
    double c[16];
    c[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15]
           + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    c[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15]
           - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    c[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15]
           + m[8]*m[7]*m[13]  + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    c[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14]
           - m[8]*m[6]*m[13]  - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    c[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15]
           - m[9]*m[3]*m[14]  - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    c[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15]
           + m[8]*m[3]*m[14]  + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    c[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15]
           - m[8]*m[3]*m[13]  - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    c[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14]
           + m[8]*m[2]*m[13]  + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    c[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15]
           + m[5]*m[3]*m[14]  + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    c[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15]
           - m[4]*m[3]*m[14]  - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    c[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15]
           + m[4]*m[3]*m[13]  + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    c[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14]
           - m[4]*m[2]*m[13]  - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    c[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11]
           - m[5]*m[3]*m[10]  - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    c[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11]
           + m[4]*m[3]*m[10]  + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    c[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11]
           - m[4]*m[3]*m[9]   - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    c[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10]
           + m[4]*m[2]*m[9]   + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

    double determinant = m[0]*c[0] + m[1]*c[4] + m[2]*c[8] + m[3]*c[12];
    if (!_finite(determinant) || fabs(determinant) < 1.0e-20) return false;
    for (unsigned i = 0; i < 16; ++i) {
        double value = c[i] / determinant;
        // The GPU consumes floats. A finite double can still overflow when
        // narrowed, even when all input elements and the determinant are finite.
        if (!_finite(value) || fabs(value) > FLT_MAX) return false;
        out[i] = (float)value;
    }
    return true;
}

float ndcRoundTripError(const float inverse[16], const float forward[16]) {
    if (!inverse || !forward) return FLT_MAX;
    // The corners and centre of the screen at a near, middle and far depth.
    // The shader turns v1.xy into NDC as (2u - 1, 1 - 2v) and reads NDC z
    // straight out of the depth buffer, so these are exactly the coordinates a
    // pixel arrives with.
    const float uv[6][2] = {{0.5f, 0.5f}, {0.0f, 0.0f}, {1.0f, 0.0f},
                            {0.0f, 1.0f}, {1.0f, 1.0f}, {0.25f, 0.75f}};
    const float depths[3] = {0.05f, 0.5f, 0.95f};
    float worst = 0.0f;
    for (unsigned i = 0; i < 6; ++i)
        for (unsigned d = 0; d < 3; ++d) {
            const float ndc[3] = {2.0f * uv[i][0] - 1.0f,
                                  1.0f - 2.0f * uv[i][1], depths[d]};
            float world[3], back[3];
            if (!unproject(inverse, ndc, world)) return FLT_MAX;
            if (!project(forward, world, back)) return FLT_MAX;
            for (unsigned c = 0; c < 3; ++c) {
                float error = fabsf(back[c] - ndc[c]);
                if (!_finite(error)) return FLT_MAX;
                if (error > worst) worst = error;
            }
        }
    return worst;
}

bool enabled() { return tq::shadow::contactSettings().enabled; }

bool instrumented() {
    const tq::shadow::ContactSettings& settings = tq::shadow::contactSettings();
    return settings.enabled || settings.timing || tq::probe::enabled();
}

void install(ID3D11Device* device, ID3D11DeviceContext* context, MapFn map, UnmapFn unmap) {
    if (g_installed || !instrumented() || !device || !context) return;
    void** slots = *(void***)context;
    g_map = map ? map : (MapFn)slots[14];
    g_unmap = unmap ? unmap : (UnmapFn)slots[15];
    const tq::shadow::ContactSettings& configured = tq::shadow::contactSettings();
    if (configured.timing && !tq::probe::enabled() && !createTiming(device))
        tq::hdr::log("Contact shadow: GPU timestamp queries unavailable\r\n");
    // With the effect off and only timing on there is nothing to upload, and
    // the readback would otherwise add work to the baseline being measured.
    if (!configured.enabled) {
        g_context = context;
        g_installed = true;
        tq::hdr::log("Contact shadow: timing only, effect off\r\n");
        return;
    }
    D3D11_BUFFER_DESC readback = {};
    readback.ByteWidth = kConstantBytes;
    readback.Usage = D3D11_USAGE_STAGING;
    readback.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    D3D11_BUFFER_DESC parameters = {};
    parameters.ByteWidth = kParameterBytes;
    parameters.Usage = D3D11_USAGE_DYNAMIC;
    parameters.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    parameters.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    bool ok = true;
    for (unsigned i = 0; i < kReadbackSlots; ++i)
        ok &= SUCCEEDED(device->CreateBuffer(&readback, nullptr, &g_readback[i]))
           && g_readback[i];
    float neutral[kParameterVectors * 4];
    neutralParameters(neutral);
    D3D11_SUBRESOURCE_DATA initial = {neutral, 0, 0};
    ok &= SUCCEEDED(device->CreateBuffer(&parameters, &initial, &g_parameters))
       && g_parameters;
    if (!ok) {
        shutdown();
        tq::hdr::log("Contact shadow: constant readback resources failed\r\n");
        return;
    }
    g_context = context;
    g_installed = true;
    const tq::shadow::ContactSettings& settings = tq::shadow::contactSettings();
    tq::hdr::log("Contact shadow: b%u ready, steps=%u length=%.3f bias=%.3f"
                 " thickness=%.3f strength=%.3f, toggle=%s\r\n",
                 kParameterSlot, settings.steps, settings.length, settings.bias,
                 settings.thickness, settings.strength,
                 settings.toggle ? "Ctrl+Shift+C" : "off");
}

void shutdown() {
    releaseTiming();
    for (unsigned i = 0; i < kReadbackSlots; ++i) {
        release(g_readback[i]);
        g_readback[i] = nullptr;
        g_readbackFilled[i] = false;
    }
    release(g_savedParameters);
    g_savedParameters = nullptr;
    g_parametersBound = false;
    release(g_parameters);
    g_parameters = nullptr;
    g_context = nullptr;
    g_map = nullptr;
    g_unmap = nullptr;
    g_installed = false;
    g_describedSource = false;
    g_refreshedThisFrame = false;
    g_frame = g_probes = g_failures = 0;
    g_attempts = g_updates = g_skips = 0;
    g_updatedFrame = g_probedFrame = 0;
    g_active = true;
    g_toggleKeyDown = false;
    g_valuesReady = false;
    memset(g_values, 0, sizeof(g_values));
    memset(g_probedCamera, 0, sizeof(g_probedCamera));
    g_probedScale = 0.0f;
}

bool ready() { return g_installed && g_parameters; }

void invalidateHistory() {
    g_refreshedThisFrame = false;
    g_updatedFrame = g_frame - kMaxHistoryAge - 1;
    for (unsigned i = 0; i < kReadbackSlots; ++i)
        g_readbackFrame[i] = g_updatedFrame;
}

#ifdef TQ_SELFTEST
void forceReadbackBusyForTest(bool busy) { g_forceReadbackBusy = busy; }
TestState stateForTest() {
    TestState state = {};
    for (unsigned i = 0; i < kReadbackSlots; ++i) state.pending += g_readbackFilled[i];
    state.updates = g_updates;
    state.valid = g_valuesReady;
    state.nonblocking = g_onlyNonblockingMaps;
    return state;
}
#endif

bool noteReceiver(ID3D11PixelShader* shader, bool marched) {
    if (!shader || !instrumented()) return false;
    const UINT tag = marched ? 2 : 1;
    return SUCCEEDED(shader->SetPrivateData(kReceiverTag, sizeof(tag), &tag));
}

unsigned receiverKind(ID3D11PixelShader* shader) {
    UINT tag = 0, size = sizeof(tag);
    return shader && SUCCEEDED(shader->GetPrivateData(kReceiverTag, &size, &tag))
        && size == sizeof(tag) ? tag : 0;
}

void onPresent() {
    if (!g_installed) return;
    ++g_frame;
    g_refreshedThisFrame = false;
    pollToggle(g_context);
    collectTiming(g_context);
}

void beforeReceiverDraw(ID3D11DeviceContext* context, bool marched) {
    if (!g_installed || context != g_context) return;
    tq::probe::count(tq::probe::CounterContactReceiverDraw);
    if (marched && g_parameters) {
        tq::probe::count(tq::probe::CounterContactMarchedDraw);
        if (!g_refreshedThisFrame) {
            g_refreshedThisFrame = true;
            refresh(context);
        }
        if (g_active && g_valuesReady && tq::shadow::contactSettings().strength > 0)
            tq::probe::count(tq::probe::CounterContactActiveDraw);
        // Bound for every receiver draw, not only the refreshed one: the
        // shader must never read an unbound b13, whose registers would read as
        // zero and divide the march by a zero W.
        context->PSGetConstantBuffers(kParameterSlot, 1, &g_savedParameters);
        context->PSSetConstantBuffers(kParameterSlot, 1, &g_parameters);
        g_parametersBound = true;
    }
    tq::probe::gpuBegin(context, tq::probe::GpuContactReceiver);
    // Opened last so the readback's copy stays outside the bracket.
    if (!g_timingReady) return;
    TimingSlot& slot = g_timing[g_timingSlot];
    if (slot.pending) return;
    context->Begin(slot.disjoint);
    context->End(slot.start);
}

void afterReceiverDraw(ID3D11DeviceContext* context) {
    if (!g_installed || context != g_context) return;
    tq::probe::gpuEnd(context, tq::probe::GpuContactReceiver);
    if (g_parametersBound) {
        context->PSSetConstantBuffers(kParameterSlot, 1, &g_savedParameters);
        release(g_savedParameters);
        g_savedParameters = nullptr;
        g_parametersBound = false;
    }
    if (!g_timingReady) return;
    TimingSlot& slot = g_timing[g_timingSlot];
    if (slot.pending) return;
    context->End(slot.end);
    context->End(slot.disjoint);
    slot.pending = true;
    slot.frame = g_frame;
    g_timingSlot = (g_timingSlot + 1) % kTimingSlots;
}

}  // namespace contact
}  // namespace tq
