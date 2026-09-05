# FP16 flip presentation with VSync off

The renderer's complete Present wrapper at RVA `0x61190` loads the swap chain
at object offset `0x34`, normalizes the byte at `0x5db` to interval 0 or 1,
and calls its current vtable slot 8 with flags zero. The existing signature
gate validates all 28 bytes before installing the renderer hook. No renderer
offset is newly accepted without that check.

The FP16 candidate now queries `IDXGIFactory5::CheckFeatureSupport` for
`DXGI_FEATURE_PRESENT_ALLOW_TEARING`. Missing interfaces, false capability
reports and query failures all leave tearing disabled. The capability query
runs during candidate creation, alongside output detection, not per frame.
Requested refresh rate, window mode and unrelated creation flags are retained.
If the tearing candidate fails creation or color-space activation, creation
retries FP16 without tearing before the existing original-description fallback.

After successful creation, the Present installer records eligibility from the
actual FP16 flip-discard description and its `ALLOW_TEARING` flag. This happens
even when Steam prevents installation of our shared ResizeBuffers hook. The
verified game renderer recreates its primary chain for mode changes; where
our ResizeBuffers hook is installed, it preserves the chain's immutable
tearing flag while retaining the caller's unrelated flags.

Eligible VSync-off frames query current exclusive-fullscreen state to handle
mode transitions without stale cached state. Only confirmed windowed frames
call `Present(0, DXGI_PRESENT_ALLOW_TEARING)`, via the live DXGI vtable. The
pre/post callbacks, HRESULT handling and timing remain around the operation.
VSync-on frames and ineligible chains take the original renderer wrapper;
they do not perform the additional fullscreen-state query. No DXGI Present
slot is patched or native Present pointer cached. Query failure and exclusive
fullscreen use the original wrapper. Invalid-argument/invalid-call rejection
of the optional flag retries original Present once and disables tearing for
the tracked chain; device errors propagate without another Present.

Self-tests cover capability success/absence/failure, descriptor preservation,
actual execution through the synthetic renderer wrapper, normalized VSync,
windowed/exclusive transitions, fullscreen query failure, current overlay
dispatch, rejected flag fallback, device-error propagation, ResizeBuffers
flags, and non-FP16/ineligible chains.

This is a compatibility improvement, not a demonstrated fix for the user's
reported Frostbite slowdown. The user subsequently reported that OBS was
running and that the frame rate was better without it. Their VSync-on setup
does not use the new tearing Present flag.

DXGI requirements:
- https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays
- https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present
