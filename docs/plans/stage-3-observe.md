# Stage 3 — Observe the samplers, and the buffers

**Goal:** see the `-FLT_MAX` sampler with our own eyes, in full, and learn which
pass owns it. Fix nothing.

**Precondition:** Stage 2's gate met.

This is the stage that decides whether H-A is right. DXMT's warning tells us a
sampler exists with an unrepresentable border colour; it does not tell us what
that sampler is *for*, and everything in Stage 4 depends on knowing.

## The patch

One vtable data-write on the device returned by Stage 2:
`ID3D11Device::CreateSamplerState`. Patch it at device-creation time, before the
render thread exists — never mid-frame (Risk 4). `VirtualProtect` around the
write; the sibling's `src/patch.{cpp,h}` has the primitive.

Confirm the slot index against the real `ID3D11Device` layout rather than
trusting a remembered number.

## What to log, per call

The complete `D3D11_SAMPLER_DESC`, one line each:

- `Filter` — in particular whether it is a **comparison** filter, which would
  mark it as a shadow sampler outright
- `AddressU`, `AddressV`, `AddressW`
- `BorderColor[4]`, printed with enough precision to recognise `-FLT_MAX`
- `ComparisonFunc`, `MinLOD`, `MaxLOD`, `MipLODBias`, `MaxAnisotropy`
- a sequence number, and the returned sampler pointer

Then count: how many samplers are created in total, how many use
`ADDRESS_BORDER`, and how many carry a border colour Metal cannot represent.
If it is exactly one, Stage 4 is a one-line change. If it is several, Stage 4
needs a policy.

## While we are in here — constant buffers, for H-B1

Nearly free, and it feeds Defect B a stage early. Also patch
`ID3D11Device::CreateBuffer` and log, for every buffer with
`D3D11_BIND_CONSTANT_BUFFER`: the `ByteWidth`, the `Usage`, the `CPUAccessFlags`.

`D3DCOMPILER_43.dll` is already in the process (`substrate.md`), so `D3DReflect`
is available. If it can be used without disturbing anything, reflect each shader's
declared constant-buffer sizes and log any that **exceed** a width the game
actually created. That is the mismatch DXVK's app profile exists to paper over,
and a single logged instance of it would promote H-B1 from inference to
observation.

Still log only. Padding a buffer has a real hazard attached — `UpdateSubresource`
with a null destination box copies the whole resource and would read past the end
of the game's own source pointer — and that belongs in Stage 5 with its own plan,
not smuggled in here.

## Gate

- Our log contains the `-FLT_MAX` sampler with its complete description, and we
  can say what pass it belongs to — or say honestly that we cannot tell yet.
- We have counts: total samplers, border-address samplers, unrepresentable ones.
- The constant-buffer widths are logged, with any reflection mismatches called
  out.
- The game is unharmed and the flicker is unchanged.

## Outcome

*(fill in at the end of the stage)*
