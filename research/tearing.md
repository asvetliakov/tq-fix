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

## Device recreation

The reported failure happened on a second device/swap-chain creation: the
initial FP16 chain worked, but the later request changed the native buffer
count from two to one. Both FP16 retries returned `0x80070005` (`E_ACCESSDENIED`)
and the original 8-bit description succeeded. This is consistent with an
in-game VSync change, though the log does not record the actual option toggle.

The visual module retained the old swap chain and back-buffer views, then
skipped installation on the replacement device because its one-time guard was
still set. Windows allows only one flip chain per HWND. Retaining the old
chain therefore prevents successful same-window replacement on native DXGI.

The renderer's hooked creation import now retires the previous graphics state
before attempting a replacement. It joins the shader worker before touching
its resources or call-through pointers, restores device hooks, clears context
bindings, releases grass/AA/bloom/output/overlay resources and the old chain,
then flushes deferred destruction. A worker that has not finished within two
seconds rejects the creation attempt without partially tearing down its state.
The new device receives fresh hooks (including the skinning shader fix), shader
programs, grass tables and GPU queries. The renderer Present hook, Engine
detours, archive cache and frame-trace session persist. A different output
window is treated as an auxiliary chain and bypasses the primary output policy.

Options are initialized once per session: re-reading the probe options on
each device would allocate a new frame ring and lose pending trace rows.
Cleanup, flushing and shader rebuilding run only at device recreation; no
new checks, locks or flushes are added to the normal frame/draw paths.

The off-game regression fixture calls the production creation import four
times with alternating native buffer counts, presents through the audited
renderer wrapper, and checks FP16, old-chain/grass lifetimes, replacement
shader device ownership, auxiliary-window isolation and continuous CSV frame
ordinals. It also inspects every required visual device/context hook and both
renderer draw targets after each creation, while confirming that native Draw
and DrawIndexed slots remain untouched. A separate rejected-renderer fixture
checks that optional visual rollback preserves the unconditional skinning hook.

The installation review corrected publication ordering in the shared slot
helper: the original function pointer is published before its hook becomes
callable. The helper rejects exhausted tracking storage and competing slot
writes, and installation verifies the recorded slots once before reporting
success. Recreation joins the unmap worker fully before reusing its queue and
lock. These operations add no per-frame checks or locks.

The local VSync-off/on capture (`cache/runs/recreation-vsync-off-on`) confirms
two successful replacement devices: all three generations report FP16 HDR,
20 successful visual patches, renderer draw installation and ready shader
programs. All 5,828 CSV frames remain consecutive. Grass reports 484,692 crossed
draws out of 484,734, with no seed/twin failures. This and the production-DLL
fixtures exercise CrossOver; native Windows validation remains outstanding.

The gameplay hitch before the toggles is frame 3316, about 45.2 seconds into
the capture, lasting 376.304 ms. It contains 69 synchronous main-thread resource
loads (174.246 ms): 44 textures (154.258 ms), 22 meshes (18.579 ms), two shaders
(1.329 ms) and one other resource (0.080 ms). Archive counters record 49 reads,
35,155 KiB requested and 129.497 ms of aggregate decompression across threads;
these counters can overlap resource-loading timings and must not be added.
The cache has two hits and 169 stores/evictions. Draw submission also consumes
172.240 ms, mostly in the second geometry invocation. Present is only 0.051 ms,
directional shadows 4.750 ms and grass crossing 0.153 ms. This identifies a
resource-loading burst plus expensive draw submission, not a VSync wait. The
capture has no F12 resource-name report for that frame, so specific filenames
cannot be attributed from it. The earlier intermittent five-second shadow
stalls did not recur; their cause is still unresolved.

This is a compatibility improvement, not a demonstrated fix for the user's
reported Frostbite slowdown. The user subsequently reported that OBS was
running and that the frame rate was better without it. Their VSync-on setup
does not use the new tearing Present flag.

DXGI requirements:
- https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays
- https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present
- https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd
