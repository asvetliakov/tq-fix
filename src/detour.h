#pragma once

#include <windows.h>
#include <stdint.h>

namespace tq {
namespace detour {

// Writing into another module's .text, with the verification that makes it
// survivable.
//
// Lifted out of src/grass.cpp, which held the only copy, and extended for the
// Engine.dll instrumentation with the two things that copy lacks: a verify
// length separate from the stolen length, and a call-site retargeter.
//
// The separate verify length is not a refinement, it is the whole safety
// argument. `Region::LoadLevel`, `Archive::ReadFromFile`, the archive block
// inflate and `Region::GetEntitiesInFrustum` all open with the identical
// `55 8b ec 83 e4 f8`, so matching a six-byte prologue proves nothing about
// which function -- let alone which build -- is under the pointer. Engine
// hooks verify 16-24 bytes and steal 6-7 of them.

// push imm32; ret -- an absolute six-byte branch, so nothing depends on where
// the patched module and winmm.dll land relative to one another.
const SIZE_T kBranchSize = 6;
const SIZE_T kMaxStolen = 8;
// Long enough for a call site plus the instructions that give it its context;
// the longest in use is a 33-byte run of resource-manager sweeps.
const SIZE_T kMaxSignature = 48;
const unsigned kMaxRelocations = 4;

bool readable(const void* address, SIZE_T bytes);
bool moduleText(HMODULE module, BYTE** begin, SIZE_T* size);
void absoluteBranch(BYTE* code, const void* destination);
bool writeBytes(BYTE* address, const BYTE* expected, const BYTE* replacement,
                SIZE_T bytes);

// A dword inside a signature that the loader rewrote for the module's actual
// base: an absolute operand, a structured-exception handler, an import slot.
// Comparing it literally fails under ASLR and masking it out weakens the
// match, so it is compared against `module + rva`, which is neither.
struct Relocation {
    unsigned offset;   // where the dword starts inside the signature
    DWORD rva;         // what it has to resolve to
};

// `bytes` carries a placeholder at every byte a relocation covers; `length`
// must contain each relocation whole.
struct Signature {
    const BYTE* bytes;
    SIZE_T length;
    const Relocation* relocations;
    unsigned relocationCount;
};

bool matches(HMODULE module, const void* address, const Signature& signature);

struct Detour {
    BYTE* entry;
    BYTE* trampoline;      // null when the target was replaced outright
    SIZE_T stolen;
    BYTE original[kMaxStolen];
    BYTE patched[kMaxStolen];
    bool installed;
};

// Copies the stolen bytes into an executable trampoline, branches from its end
// back into the body, then overwrites the entry. Nothing is written until the
// signature matches, so a build whose prologue differs is left alone.
//
// `trampoline` is written *before* the entry is patched and cleared again if
// the patch fails, so pass the variable the replacement calls through rather
// than a local: that leaves no window in which a call arriving on another
// thread reaches the hook before the hook has anything to call.
bool attach(Detour& detour, HMODULE module, void* target,
            const Signature& verify, SIZE_T stolen, const void* replacement,
            void** trampoline);

// For a target too small to trampoline. `Region::WaitForLoadingToFinish` is
// seven bytes and two of them are a relative jump into itself, so copying them
// anywhere else changes what they mean; the replacement has to implement the
// whole function rather than call through to it. Bytes past the branch are
// filled with nop so the restore compare stays exact.
bool replace(Detour& detour, HMODULE module, void* target,
             const Signature& verify, SIZE_T length, const void* replacement);

// Puts the entry back, but only if it still holds what we wrote.
void detach(Detour& detour);

// A retargeted call site. `target` is the cell an `FF 15` site is repointed
// at, so it has to outlive the patch: keep the CallPatch itself static.
struct CallPatch {
    void* target;
    BYTE* operand;
    uint32_t original;
    uint32_t replacement;
    bool installed;
};

// Retargets the call at `window + callOffset`, having first verified the whole
// window against `signature`. Handles `E8 rel32` by rewriting the
// displacement, and `FF 15 disp32` by repointing the operand at `patch.target`
// -- never by writing through the import slot, so every other caller of the
// same import is left exactly as it was.
//
// `expectedTarget` is what the call resolves to today, and it is the check
// that carries the safety: bytes can match by coincidence, a destination
// cannot. Four bytes change, and restoreCall puts back those four.
bool patchCall(CallPatch& patch, HMODULE module, void* window,
               const Signature& signature, unsigned callOffset,
               const void* expectedTarget, const void* replacement);

void restoreCall(CallPatch& patch);

}  // namespace detour
}  // namespace tq
