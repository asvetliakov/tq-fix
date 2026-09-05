#pragma once

#include <d3d11.h>

namespace tq {
namespace contact {

// CPU half of the screen-space contact shadows.
//
// The deferred receiver reconstructs world position from the depth buffer with
// an inverse view-projection held in its own cb0, but a screen-space march
// needs the forward transform, which the shader does not have. Inverting a 4x4
// per pixel is out of the question, so the inverse is read back, inverted once
// per frame on the CPU, and supplied in b13:
//
//   b13[0..3]  view-projection rows, in the receiver's own DP4 convention:
//              row i dotted with (world, 1) yields clip component i
//   b13[4]     view-projection times the light direction, w = 0, so a march
//              step is one mad in clip space rather than a matrix multiply
//   b13[5]     (stepLength, depthBias, 1/maxThickness, strength). Lengths
//              are world units; strength is dimensionless.
//   b13[6]     (a, b, uprightGate, 1/steps). The first two come from the
//              inverse view-projection's fourth row, so
//              the shader can read a view depth out of the buffer as
//              1 / (a * ndcZ + b). Without it the comparison happens in NDC z,
//              where one bias is 0.046 world units ten units out and 0.80 at
//              forty -- wider than the march itself.
//
// Four staging slots are polled without waiting and never overwritten while
// pending. History older than eight frames disables the effect until a fresh
// copy lands. Translation cancels in VP*L; zoom/light changes need a refresh.
//
// The march does not take its origin from VP * P, and does not need to.
// Writing A for the inverse the receiver holds and
// B = A-inverse for what goes in b13, the shader's own reconstruction is
// h = A * (ndc, 1) and P = h.xyz / h.w, so
//
//     B * (P, 1) = B * h / h.w = (ndc, 1) / h.w
//
// The clip-space origin is the pixel's own NDC, which the shader already has in
// a register, over a scalar it already computes. Scaling a homogeneous vector
// changes no UV, so the march runs as
//
//     (ndc, 1) + (i * step * h.w) * (VP * L)
//
// exact at i = 0 by construction, cheaper than four DP4s, and insensitive to
// how far the world sits from its origin -- a round trip through both float
// matrices costs 9.9e-7 in NDC at the camera that was sampled in game but
// 3.7e-3 at a synthetic five-figure camera, and nothing has bounded which of
// those Titan Quest can reach. b13[0..3] therefore carries the matrix for
// validation and to derive VP * L from, not because the march multiplies by
// it.

// True when [graphics] shadow_contact is on. Safe before install().
bool enabled();

// True when the module has work to do at all: the effect, or the timing that
// measures it. Timing runs with the effect off too, which is the only way to
// get a baseline from the same measurement point.
bool instrumented();

// Use native dispatch for our Maps: the visual hook attributes its Maps to
// the game and feeds grass tracking, neither of which applies to this work.
using MapFn = HRESULT (WINAPI*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
    D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void (WINAPI*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
// Creates the readback and parameter buffers on the game's one device.
void install(ID3D11Device* device, ID3D11DeviceContext* context,
             MapFn map = nullptr, UnmapFn unmap = nullptr);
void shutdown();

// The deferred receiver, as reported by the shader-creation hook.
bool noteReceiver(ID3D11PixelShader* shader, bool marched);
unsigned receiverKind(ID3D11PixelShader* shader);
bool ready();
void invalidateHistory();

#ifdef TQ_SELFTEST
struct TestState { unsigned pending, updates; bool valid, nonblocking; };
TestState stateForTest();
void forceReadbackBusyForTest(bool busy);
#endif

// Resets the once-per-frame gate on the constant readback.
void onPresent();

// Called with the receiver bound, immediately before and after its draw. The
// first refreshes the transform at most once per frame and binds b13; the pair
// brackets the draw with timestamps when timing is on and restores prior b13.
void beforeReceiverDraw(ID3D11DeviceContext* context, bool marched);
void afterReceiverDraw(ID3D11DeviceContext* context);

// Inverse of a 4x4 whose sixteen floats are the four DP4 operand rows. False
// when the matrix is singular or produces a non-finite result. Exposed because
// the whole GPU-side march is only as trustworthy as this inversion, and the
// self-test can check it without a device.
bool invertRowMatrix(const float in[16], float out[16]);

// Largest absolute error of the matrix route: reconstruct a pixel's world
// position with `inverse`, project it back with `forward`, and compare to the
// NDC it came from. This is what proves `forward` is the inverse of what the
// receiver holds, in the receiver's own convention -- a transposed or
// mis-scaled matrix lands O(1) away. Its floor is the float world scale
// described above, so read it as a matrix check, not as the march's accuracy.
float ndcRoundTripError(const float inverse[16], const float forward[16]);

}  // namespace contact
}  // namespace tq
